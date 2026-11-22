#include "network_buffer_fixture.hpp"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Buffers/Network/MutationClaimProcessor.hpp"
#include "MayaFlux/Buffers/Network/NetworkTextureBuffer.hpp"
#include "MayaFlux/Buffers/Network/PopulationProcessor.hpp"
#include "MayaFlux/Buffers/Network/SpatialHashProcessor.hpp"
#include "MayaFlux/Buffers/Shaders/VertexFieldProcessor.hpp"
#include "MayaFlux/Buffers/Staging/DataReadProcessor.hpp"
#include "MayaFlux/Buffers/Staging/DataWriteProcessor.hpp"
#include "MayaFlux/Buffers/State/StateFieldBuffer.hpp"
#include "MayaFlux/Kakshya/Utils/DataUtils.hpp"
#include "MayaFlux/Kinesis/Tendency/DualField.hpp"
#include "MayaFlux/Kinesis/Tendency/ShaderCompat.hpp"
#include "MayaFlux/Nodes/Network/Operators/GpuFieldOperator.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test {
namespace {

    using Buffers::BufferProcessingChain;
    using Buffers::DataReadProcessor;
    using Buffers::DataWriteProcessor;
    using Buffers::MutationConfig;
    using Buffers::NetworkGeometryBuffer;
    using Buffers::PopulationConfig;
    using Buffers::SpatialHashConfig;
    using Buffers::StateFieldBuffer;
    using Kinesis::FieldTarget;
    using Nodes::Network::GpuFieldOperator;
    using Nodes::Network::SpatialFieldConfig;

    namespace fields {

        using namespace MayaFlux::ShaderCompat;

        const auto shift_x = MF_FIELD(shift_x, [](vec3 p) -> vec3 {
            return p * 0.0f + vec3(0.25f, 0.0f, 0.0f);
        });

    } // namespace fields

    constexpr uint32_t k_records = 64;
    constexpr uint32_t k_stride_words = 17;
    constexpr float k_cell_size = 0.5F;

    /**
     * @brief The smallest concrete StateFieldBuffer: vertex storage and nothing that feeds it.
     *
     * Stands for any buffer other than a NetworkGeometryBuffer that wants the spatial
     * stages. The tests fill its storage directly, which is all a frame supplier does.
     */
    class ProbeBuffer final : public StateFieldBuffer {
    public:
        explicit ProbeBuffer(size_t byte_count)
            : StateFieldBuffer(byte_count, Usage::VERTEX, Kakshya::DataModality::VERTICES_3D)
        {
        }
    };

    glm::uvec3 cell_dims() { return glm::uvec3(4U); }

    uint32_t cell_of(const glm::vec3& p)
    {
        glm::ivec3 c = glm::ivec3(glm::floor((p - glm::vec3(-1.0F)) / k_cell_size));
        c = glm::clamp(c, glm::ivec3(0), glm::ivec3(cell_dims()) - glm::ivec3(1));
        return static_cast<uint32_t>(c.x) + static_cast<uint32_t>(c.y) * 4U + static_cast<uint32_t>(c.z) * 16U;
    }

    std::vector<uint8_t> bytes_of(const std::vector<uint32_t>& values)
    {
        std::vector<uint8_t> bytes(values.size() * sizeof(uint32_t));
        std::memcpy(bytes.data(), values.data(), bytes.size());
        return bytes;
    }

    SpatialHashConfig explicit_config(uint32_t particle_count)
    {
        return SpatialHashConfig {
            .grid_min = glm::vec3(-1.0F),
            .grid_dims = cell_dims(),
            .cell_size = k_cell_size,
            .particle_count = particle_count,
            .stride_words = k_stride_words,
            .position_word_offset = 0,
        };
    }

    // The contract: what the base is, and what it needs no device to say

    MAYAFLUX_TEST_WITH(TEST, StateFieldBufferContractTest, TheNetworkBuffersAreStateFieldBuffers, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        static_assert(std::is_base_of_v<Buffers::VKBuffer, StateFieldBuffer>);
        static_assert(std::is_base_of_v<StateFieldBuffer, NetworkGeometryBuffer>);
        static_assert(std::is_base_of_v<StateFieldBuffer, Buffers::NetworkTextureBuffer>);
        SUCCEED();
    }

    MAYAFLUX_TEST_WITH(TEST, StateFieldBufferContractTest, AnUnregisteredBufferHasNoStateAndNoClusterIds, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        ProbeBuffer probe(256);

        EXPECT_FALSE(probe.has_state("anything"));
        EXPECT_EQ(probe.get_state_bytes("anything"), 0U);
        EXPECT_FALSE(probe.ensure_cluster_ids());
    }

    MAYAFLUX_TEST_WITH(TEST, StateFieldBufferContractTest, TheBaseSizesItsOwnStorageFromTheConstructor, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        const ProbeBuffer probe(1024);

        EXPECT_EQ(probe.get_size_bytes(), 1024U);
        EXPECT_EQ(probe.get_modality(), Kakshya::DataModality::VERTICES_3D);
    }

    // The device fixture: a probe buffer beside the network buffer the shared fixture owns

    class StateFieldBufferTest : public NetworkBufferDeviceTest {
    protected:
        void TearDown() override
        {
            if (probe) {
                if (probe->is_initialized() && service) {
                    service->destroy_buffer(probe);
                }
                probe.reset();
            }
            NetworkBufferDeviceTest::TearDown();
        }

        void make_probe(uint32_t records = k_records)
        {
            record_count = records;
            probe = std::make_shared<ProbeBuffer>(static_cast<size_t>(records) * k_stride_words * 4);
            service->initialize_buffer(probe);
            ASSERT_TRUE(probe->is_initialized());
            auto chain = probe->get_processing_chain();
            if (!chain) {
                chain = std::make_shared<BufferProcessingChain>();
                probe->set_processing_chain(chain);
            }
            chain->set_preferred_token(Buffers::ProcessingToken::GRAPHICS_BACKEND);
            ASSERT_EQ(probe->get_processing_chain(), chain);
        }

        /** @brief One vertex record per position: position in the first three words, the rest zero. */
        static std::vector<float> records_at(const std::vector<glm::vec3>& positions)
        {
            std::vector<float> words(positions.size() * k_stride_words, 0.0F);
            for (size_t i = 0; i < positions.size(); ++i) {
                words[i * k_stride_words] = positions[i].x;
                words[i * k_stride_words + 1] = positions[i].y;
                words[i * k_stride_words + 2] = positions[i].z;
            }
            return words;
        }

        static std::vector<glm::vec3> scattered(uint32_t count)
        {
            std::mt19937 rng(7U);
            std::uniform_real_distribution<float> spread(-0.99F, 0.99F);
            std::vector<glm::vec3> positions(count);
            for (auto& p : positions) {
                p = { spread(rng), spread(rng), spread(rng) };
            }
            return positions;
        }

        void fill(const std::vector<glm::vec3>& positions)
        {
            positions_in_use = positions;
            const auto words = records_at(positions);
            Buffers::upload_to_gpu(words.data(), words.size() * sizeof(float), probe);
        }

        [[nodiscard]] std::vector<float> read_words()
        {
            std::vector<float> words(static_cast<size_t>(record_count) * k_stride_words);
            Buffers::download_from_gpu(probe, words.data(), words.size() * sizeof(float));
            return words;
        }

        template <typename T>
        [[nodiscard]] std::vector<T> probe_read(const std::string& name)
        {
            std::vector<T> values(probe->get_state_bytes(name) / sizeof(T));
            std::shared_ptr<Buffers::VKBuffer> staging;
            Buffers::download_back_buffer(
                probe->read_state_slot(name), values.data(), values.size() * sizeof(T), staging);
            return values;
        }

        template <typename T>
        void probe_write(const std::string& name, const std::vector<T>& values)
        {
            std::shared_ptr<Buffers::VKBuffer> staging;
            Buffers::upload_back_buffer(
                probe->write_state_slot(name), values.data(), values.size() * sizeof(T), staging);
        }

        void add(const std::shared_ptr<Buffers::BufferProcessor>& processor)
        {
            ASSERT_TRUE(probe->get_processing_chain()->add_processor(processor, probe));
        }

        void run_stages()
        {
            probe->get_processing_chain()->process_complete(probe);
        }

        void add_hash_build(const SpatialHashConfig& config, bool gate_alive = false)
        {
            add(std::make_shared<Buffers::HashClearProcessor>(config));
            add(std::make_shared<Buffers::HashCountProcessor>(config, gate_alive));
            add(std::make_shared<Buffers::HashScanProcessor>(config));
            add(std::make_shared<Buffers::HashScatterProcessor>(config, gate_alive));
        }

        [[nodiscard]] std::vector<uint32_t> expected_counts(const std::vector<bool>& alive = {}) const
        {
            std::vector<uint32_t> counts(64, 0U);
            for (size_t i = 0; i < positions_in_use.size(); ++i) {
                if (alive.empty() || alive[i]) {
                    ++counts[cell_of(positions_in_use[i])];
                }
            }
            return counts;
        }

        std::shared_ptr<ProbeBuffer> probe;
        uint32_t record_count {};
        std::vector<glm::vec3> positions_in_use;
    };

    // Declaring state

    TEST_F(StateFieldBufferTest, DeclareStateRejectsEmptyNamesZeroSizesAndDuplicates)
    {
        make_probe();

        EXPECT_FALSE(probe->declare_state("", 4, 4));
        EXPECT_FALSE(probe->declare_state("no_elements", 0, 4));
        EXPECT_FALSE(probe->declare_state("no_stride", 4, 0));
        EXPECT_TRUE(probe->declare_state("field", 4, 4));
        EXPECT_FALSE(probe->declare_state("field", 8, 4));
        EXPECT_EQ(probe->get_state_bytes("field"), 16U);
    }

    TEST_F(StateFieldBufferTest, ADiscardedDeclarationLeavesNoFieldBehind)
    {
        make_probe();

        ASSERT_FALSE(probe->declare_state("no_stride", 4, 0));

        EXPECT_FALSE(probe->has_state("no_stride"));
        EXPECT_EQ(probe->get_state_bytes("no_stride"), 0U);
    }

    TEST_F(StateFieldBufferTest, StateBytesAreTheElementCountTimesTheStride)
    {
        make_probe();
        ASSERT_TRUE(probe->declare_state("vec3s", 3, 12, false));
        ASSERT_TRUE(probe->declare_state("words", 7, 4, true));

        EXPECT_EQ(probe->get_state_bytes("vec3s"), 36U);
        EXPECT_EQ(probe->get_state_bytes("words"), 28U);
    }

    TEST_F(StateFieldBufferTest, DeclaringAppendsOneBackBufferSlotForASingleSlotFieldAndTwoForADoubleBufferedOne)
    {
        make_probe();
        const size_t before = probe->get_buffer_resources().back_buffers.size();

        ASSERT_TRUE(probe->declare_state("single", 4, 4, false));
        EXPECT_EQ(probe->get_buffer_resources().back_buffers.size(), before + 1);

        ASSERT_TRUE(probe->declare_state("pair", 4, 4, true));
        EXPECT_EQ(probe->get_buffer_resources().back_buffers.size(), before + 3);
    }

    TEST_F(StateFieldBufferTest, ARejectedDeclarationAllocatesNothing)
    {
        make_probe();
        ASSERT_TRUE(probe->declare_state("field", 4, 4, true));
        const size_t before = probe->get_buffer_resources().back_buffers.size();

        EXPECT_FALSE(probe->declare_state("field", 4, 4, true));
        EXPECT_FALSE(probe->declare_state("", 4, 4, true));
        EXPECT_FALSE(probe->declare_state("empty", 0, 4, true));

        EXPECT_EQ(probe->get_buffer_resources().back_buffers.size(), before);
    }

    TEST_F(StateFieldBufferTest, UndeclaredStateResolvesToNothing)
    {
        make_probe();

        EXPECT_FALSE(probe->has_state("missing"));
        EXPECT_EQ(probe->get_state_bytes("missing"), 0U);
        EXPECT_FALSE(probe->read_state_handle("missing"));
        EXPECT_FALSE(probe->write_state_handle("missing"));
        EXPECT_FALSE(probe->read_state_slot("missing").buffer);
        EXPECT_FALSE(probe->write_state_slot("missing").buffer);
        EXPECT_NO_THROW(probe->swap_state("missing"));
    }

    // Slots and swapping

    TEST_F(StateFieldBufferTest, SingleSlotStateReadsAndWritesTheSameHandleAndIgnoresSwap)
    {
        make_probe();
        ASSERT_TRUE(probe->declare_state("single", 4, 4, false));

        const auto handle = probe->read_state_handle("single");
        EXPECT_TRUE(handle);
        EXPECT_EQ(handle, probe->write_state_handle("single"));

        probe->swap_state("single");
        EXPECT_EQ(handle, probe->read_state_handle("single"));
        EXPECT_EQ(handle, probe->write_state_handle("single"));
    }

    TEST_F(StateFieldBufferTest, DoubleBufferedStateExchangesReadAndWriteOnSwap)
    {
        make_probe();
        ASSERT_TRUE(probe->declare_state("ping", 4, 4, true));

        const auto read_before = probe->read_state_handle("ping");
        const auto write_before = probe->write_state_handle("ping");
        EXPECT_NE(read_before, write_before);

        probe->swap_state("ping");
        EXPECT_EQ(probe->read_state_handle("ping"), write_before);
        EXPECT_EQ(probe->write_state_handle("ping"), read_before);

        probe->swap_state("ping");
        EXPECT_EQ(probe->read_state_handle("ping"), read_before);
        EXPECT_EQ(probe->write_state_handle("ping"), write_before);
    }

    TEST_F(StateFieldBufferTest, ReadAndWriteSlotsCarryTheSameHandlesAsTheHandleAccessors)
    {
        make_probe();
        ASSERT_TRUE(probe->declare_state("ping", 4, 4, true));

        EXPECT_EQ(probe->read_state_slot("ping").buffer, probe->read_state_handle("ping"));
        EXPECT_EQ(probe->write_state_slot("ping").buffer, probe->write_state_handle("ping"));

        probe->swap_state("ping");
        EXPECT_EQ(probe->read_state_slot("ping").buffer, probe->read_state_handle("ping"));
        EXPECT_EQ(probe->write_state_slot("ping").buffer, probe->write_state_handle("ping"));
    }

    TEST_F(StateFieldBufferTest, StateFieldsKeepIndependentStorage)
    {
        make_probe();
        ASSERT_TRUE(probe->declare_state("first", 4, sizeof(uint32_t), false));
        ASSERT_TRUE(probe->declare_state("second", 4, sizeof(uint32_t), false));

        probe_write<uint32_t>("first", { 1, 2, 3, 4 });
        probe_write<uint32_t>("second", { 9, 8, 7, 6 });

        EXPECT_EQ(probe_read<uint32_t>("first"), (std::vector<uint32_t> { 1, 2, 3, 4 }));
        EXPECT_EQ(probe_read<uint32_t>("second"), (std::vector<uint32_t> { 9, 8, 7, 6 }));
    }

    TEST_F(StateFieldBufferTest, WhatIsWrittenToTheWriteSlotIsReadFromTheReadSlotOnlyAfterTheSwap)
    {
        make_probe();
        ASSERT_TRUE(probe->declare_state("ping", 4, sizeof(uint32_t), true));
        probe_write<uint32_t>("ping", { 0, 0, 0, 0 });
        probe->swap_state("ping");

        probe_write<uint32_t>("ping", { 5, 6, 7, 8 });
        EXPECT_EQ(probe_read<uint32_t>("ping"), (std::vector<uint32_t> { 0, 0, 0, 0 }));

        probe->swap_state("ping");
        EXPECT_EQ(probe_read<uint32_t>("ping"), (std::vector<uint32_t> { 5, 6, 7, 8 }));
    }

    TEST_F(StateFieldBufferTest, SwappingOneFieldLeavesAnotherUntouched)
    {
        make_probe();
        ASSERT_TRUE(probe->declare_state("a", 4, 4, true));
        ASSERT_TRUE(probe->declare_state("b", 4, 4, true));
        const auto b_read = probe->read_state_handle("b");
        const auto b_write = probe->write_state_handle("b");

        probe->swap_state("a");

        EXPECT_EQ(probe->read_state_handle("b"), b_read);
        EXPECT_EQ(probe->write_state_handle("b"), b_write);
    }

    TEST_F(StateFieldBufferTest, DeclaringFieldsLeavesTheVertexStorageAlone)
    {
        make_probe();
        fill(scattered(k_records));
        const auto before = read_words();

        ASSERT_TRUE(probe->declare_state("one", 16, 4, false));
        ASSERT_TRUE(probe->declare_state("two", 16, 4, true));
        probe_write<uint32_t>("one", std::vector<uint32_t>(16, 0xFFFFFFFFU));

        EXPECT_EQ(read_words(), before);
    }

    // Cluster ids

    TEST_F(StateFieldBufferTest, TheBaseReportsAClusterFieldItWasGivenAndNeverDeclaresOne)
    {
        make_probe();

        EXPECT_FALSE(probe->ensure_cluster_ids());
        EXPECT_FALSE(probe->has_state("hash_cluster_id"));

        ASSERT_TRUE(probe->declare_state("hash_cluster_id", k_records, sizeof(uint32_t), false));
        const auto handle = probe->read_state_handle("hash_cluster_id");

        EXPECT_TRUE(probe->ensure_cluster_ids());
        EXPECT_TRUE(probe->ensure_cluster_ids());
        EXPECT_EQ(probe->read_state_handle("hash_cluster_id"), handle);
    }

    // The network buffer keeps its route

    TEST_F(StateFieldBufferTest, ANetworkBufferSharesItsStateThroughTheBasePointer)
    {
        attach(make_particles(48));
        std::shared_ptr<StateFieldBuffer> base = buffer;

        ASSERT_TRUE(base->declare_state("shared", 4, sizeof(uint32_t), false));

        EXPECT_TRUE(buffer->has_state("shared"));
        EXPECT_EQ(buffer->get_state_bytes("shared"), 16U);
        EXPECT_EQ(buffer->read_state_handle("shared"), base->read_state_handle("shared"));
        EXPECT_FALSE(buffer->declare_state("shared", 4, sizeof(uint32_t), false));
    }

    TEST_F(StateFieldBufferTest, ClusterIdsThroughTheBasePointerReachTheNetworkDerivation)
    {
        auto particles = make_particles(48);
        particles->create_operator<Nodes::Network::PhysicsOperator>();
        attach(particles);
        std::shared_ptr<StateFieldBuffer> base = buffer;

        EXPECT_TRUE(base->ensure_cluster_ids());

        EXPECT_TRUE(buffer->has_state("hash_cluster_id"));
        EXPECT_EQ(buffer->get_state_bytes("hash_cluster_id"), 48 * sizeof(uint32_t));
        EXPECT_EQ(read_state<uint32_t>("hash_cluster_id"), std::vector<uint32_t>(48, 0U));
    }

    TEST_F(StateFieldBufferTest, ANetworkBufferWithoutAnOperatorStillCannotDeriveClusterIdsThroughTheBase)
    {
        attach(make_particles(48));
        std::shared_ptr<StateFieldBuffer> base = buffer;

        EXPECT_FALSE(base->ensure_cluster_ids());
        EXPECT_FALSE(buffer->has_state("hash_cluster_id"));
    }

    // The spatial hash over a buffer that is not a network buffer

    TEST_F(StateFieldBufferTest, DeclareFieldsSizesEveryHashFieldOnAnyStateFieldBuffer)
    {
        make_probe();
        explicit_config(k_records).declare_fields(probe);

        EXPECT_EQ(probe->get_state_bytes("hash_cell_count"), 64 * sizeof(uint32_t));
        EXPECT_EQ(probe->get_state_bytes("hash_cell_start"), 64 * sizeof(uint32_t));
        EXPECT_EQ(probe->get_state_bytes("hash_cell_cursor"), 64 * sizeof(uint32_t));
        EXPECT_EQ(probe->get_state_bytes("hash_particle_index"), k_records * sizeof(uint32_t));
        EXPECT_EQ(probe->get_state_bytes("hash_cluster_id"), k_records * sizeof(uint32_t));
    }

    TEST_F(StateFieldBufferTest, DeclareFieldsKeepsAClusterFieldThatIsAlreadyThere)
    {
        make_probe();
        ASSERT_TRUE(probe->declare_state("hash_cluster_id", k_records, sizeof(uint32_t), false));
        probe_write<uint32_t>("hash_cluster_id", std::vector<uint32_t>(k_records, 3U));
        const auto handle = probe->read_state_handle("hash_cluster_id");

        explicit_config(k_records).declare_fields(probe);

        EXPECT_EQ(probe->read_state_handle("hash_cluster_id"), handle);
        EXPECT_EQ(probe_read<uint32_t>("hash_cluster_id"), std::vector<uint32_t>(k_records, 3U));
    }

    TEST_F(StateFieldBufferTest, HashStagesBucketEveryRecordOfAProbeBuffer)
    {
        make_probe();
        fill(scattered(k_records));
        const auto config = explicit_config(k_records);
        config.declare_fields(probe);
        add_hash_build(config);

        run_stages();

        const auto counts = probe_read<uint32_t>("hash_cell_count");
        EXPECT_EQ(counts, expected_counts());
        EXPECT_EQ(std::accumulate(counts.begin(), counts.end(), 0U), k_records);
    }

    TEST_F(StateFieldBufferTest, HashScanOverAProbeBufferIsAnExclusivePrefixSum)
    {
        make_probe();
        fill(scattered(k_records));
        const auto config = explicit_config(k_records);
        config.declare_fields(probe);
        add_hash_build(config);

        run_stages();

        const auto counts = expected_counts();
        std::vector<uint32_t> starts(64, 0U);
        std::exclusive_scan(counts.begin(), counts.end(), starts.begin(), 0U);
        EXPECT_EQ(probe_read<uint32_t>("hash_cell_start"), starts);
    }

    TEST_F(StateFieldBufferTest, HashScatterOverAProbeBufferListsExactlyTheRecordsOfEachCell)
    {
        make_probe();
        fill(scattered(k_records));
        const auto config = explicit_config(k_records);
        config.declare_fields(probe);
        add_hash_build(config);

        run_stages();

        const auto counts = probe_read<uint32_t>("hash_cell_count");
        const auto starts = probe_read<uint32_t>("hash_cell_start");
        const auto index = probe_read<uint32_t>("hash_particle_index");

        std::vector<std::set<uint32_t>> expected(64);
        for (uint32_t i = 0; i < k_records; ++i) {
            expected[cell_of(positions_in_use[i])].insert(i);
        }
        for (uint32_t c = 0; c < 64; ++c) {
            const std::set<uint32_t> listed(index.begin() + starts[c], index.begin() + starts[c] + counts[c]);
            EXPECT_EQ(listed, expected[c]) << "cell " << c;
        }
    }

    TEST_F(StateFieldBufferTest, TheHashFollowsRecordsThatMoveBetweenCycles)
    {
        make_probe();
        auto positions = scattered(k_records);
        fill(positions);
        const auto config = explicit_config(k_records);
        config.declare_fields(probe);
        add_hash_build(config);
        run_stages();

        positions[0] = { 0.9F, 0.9F, 0.9F };
        positions[1] = { -0.9F, -0.9F, -0.9F };
        fill(positions);
        run_stages();

        EXPECT_EQ(probe_read<uint32_t>("hash_cell_count"), expected_counts());
    }

    TEST_F(StateFieldBufferTest, AStageAttachedToANetworkBufferHoldsItAsItsStateFieldBuffer)
    {
        auto particles = make_particles(32);
        particles->create_operator<Nodes::Network::PhysicsOperator>();
        attach(particles, 1.0F);
        const auto config = explicit_config(32);
        config.declare_fields(buffer);
        const auto clear = std::make_shared<Buffers::HashClearProcessor>(config);

        EXPECT_TRUE(buffer->get_processing_chain()->add_processor(clear, buffer));
        EXPECT_NE(clear->get_network_buffer(), nullptr);
        EXPECT_EQ(clear->get_network_buffer().get(), static_cast<StateFieldBuffer*>(buffer.get()));
    }

    // The claim protocol over a probe buffer

    TEST_F(StateFieldBufferTest, ClaimsOverAProbeBufferResolveAClosePairToTheLowerIndex)
    {
        constexpr uint32_t count = 8;
        make_probe(count);
        std::vector<glm::vec3> positions(count);
        for (uint32_t i = 0; i < count; ++i) {
            positions[i] = { -0.9F + 0.22F * static_cast<float>(i), 0.0F, 0.0F };
        }
        positions[4] = { positions[3].x + 0.05F, 0.0F, 0.0F };
        fill(positions);

        const auto config = explicit_config(count);
        config.declare_fields(probe);
        const MutationConfig claim_config { .hash = config, .absorb_radius = 0.1F };
        claim_config.declare_fields(probe);
        auto op = std::make_shared<GpuFieldOperator>(
            Kakshya::VertexLayout::for_points(), SpatialFieldConfig { .cell_size = k_cell_size, .absorb_radius = 0.1F });

        add_hash_build(config);
        add(std::make_shared<Buffers::ClaimInitProcessor>(claim_config));
        add(std::make_shared<Buffers::ClaimProcessor>(claim_config, op));
        add(std::make_shared<Buffers::ClaimFlattenProcessor>(claim_config));
        add(std::make_shared<Buffers::ClaimAccumulateProcessor>(claim_config, nullptr, 0, op));

        run_stages();

        EXPECT_EQ(probe_read<uint32_t>("mutation_claimed_by"),
            (std::vector<uint32_t> { 0, 1, 2, 3, 3, 5, 6, 7 }));
        EXPECT_EQ(probe_read<uint32_t>("mutation_swallow_count"),
            (std::vector<uint32_t> { 0, 0, 0, 1, 0, 0, 0, 0 }));
    }

    TEST_F(StateFieldBufferTest, ClaimsOverAProbeBufferLeaveWellSeparatedRecordsAlone)
    {
        constexpr uint32_t count = 8;
        make_probe(count);
        std::vector<glm::vec3> positions(count);
        for (uint32_t i = 0; i < count; ++i) {
            positions[i] = { -0.9F + 0.22F * static_cast<float>(i), 0.0F, 0.0F };
        }
        fill(positions);

        const auto config = explicit_config(count);
        config.declare_fields(probe);
        const MutationConfig claim_config { .hash = config, .absorb_radius = 0.1F };
        claim_config.declare_fields(probe);
        auto op = std::make_shared<GpuFieldOperator>(
            Kakshya::VertexLayout::for_points(), SpatialFieldConfig { .cell_size = k_cell_size, .absorb_radius = 0.1F });

        add_hash_build(config);
        add(std::make_shared<Buffers::ClaimInitProcessor>(claim_config));
        add(std::make_shared<Buffers::ClaimProcessor>(claim_config, op));
        add(std::make_shared<Buffers::ClaimFlattenProcessor>(claim_config));
        add(std::make_shared<Buffers::ClaimAccumulateProcessor>(claim_config, nullptr, 0, op));

        run_stages();

        EXPECT_EQ(probe_read<uint32_t>("mutation_claimed_by"),
            (std::vector<uint32_t> { 0, 1, 2, 3, 4, 5, 6, 7 }));
        EXPECT_EQ(probe_read<uint32_t>("mutation_swallow_count"), std::vector<uint32_t>(count, 0U));
    }

    // Population over a probe buffer

    TEST_F(StateFieldBufferTest, PopulationInitSplitsLiveFromReserveAndClearsTheReserveRecords)
    {
        constexpr uint32_t total = 8;
        constexpr uint32_t live = 6;
        make_probe(total);
        fill(scattered(total));

        const auto config = explicit_config(total);
        config.declare_fields(probe);
        const PopulationConfig population { .hash = config, .live_count = live };
        population.declare_fields(probe);
        add(std::make_shared<Buffers::PopulationInitProcessor>(population));

        run_stages();

        EXPECT_EQ(probe_read<uint32_t>("mutation_alive"),
            (std::vector<uint32_t> { 1, 1, 1, 1, 1, 1, 0, 0 }));
        EXPECT_EQ(probe_read<uint32_t>("mutation_spawn_cursor"), (std::vector<uint32_t> { live }));

        const auto words = read_words();
        for (uint32_t i = live; i < total; ++i) {
            for (uint32_t w = 0; w < k_stride_words; ++w) {
                EXPECT_EQ(words[i * k_stride_words + w], 0.0F) << "record " << i << " word " << w;
            }
        }
        for (uint32_t i = 0; i < live; ++i) {
            EXPECT_NE(words[i * k_stride_words], 0.0F) << "record " << i;
        }
    }

    TEST_F(StateFieldBufferTest, PopulationInitRunsOnlyOnce)
    {
        constexpr uint32_t total = 8;
        make_probe(total);
        fill(scattered(total));
        const auto config = explicit_config(total);
        config.declare_fields(probe);
        const PopulationConfig population { .hash = config, .live_count = 6 };
        population.declare_fields(probe);
        add(std::make_shared<Buffers::PopulationInitProcessor>(population));
        run_stages();

        probe_write<uint32_t>("mutation_alive", std::vector<uint32_t>(total, 0U));
        run_stages();

        EXPECT_EQ(probe_read<uint32_t>("mutation_alive"), std::vector<uint32_t>(total, 0U));
    }

    TEST_F(StateFieldBufferTest, AGatedHashSkipsTheDeadReserveRecords)
    {
        constexpr uint32_t total = 8;
        constexpr uint32_t live = 6;
        make_probe(total);
        fill(scattered(total));

        const auto config = explicit_config(total);
        config.declare_fields(probe);
        const PopulationConfig population { .hash = config, .live_count = live };
        population.declare_fields(probe);
        add(std::make_shared<Buffers::PopulationInitProcessor>(population));
        add_hash_build(config, true);

        run_stages();

        const auto counts = probe_read<uint32_t>("hash_cell_count");
        EXPECT_EQ(std::accumulate(counts.begin(), counts.end(), 0U), live);
        const auto index = probe_read<uint32_t>("hash_particle_index");
        const auto starts = probe_read<uint32_t>("hash_cell_start");
        std::set<uint32_t> listed;
        for (uint32_t c = 0; c < 64; ++c) {
            listed.insert(index.begin() + starts[c], index.begin() + starts[c] + counts[c]);
        }
        EXPECT_EQ(listed, (std::set<uint32_t> { 0, 1, 2, 3, 4, 5 }));
    }

    // The field read and write processors

    TEST_F(StateFieldBufferTest, DataWriteProcessorWritesANamedFieldOfAProbeBuffer)
    {
        make_probe();
        ASSERT_TRUE(probe->declare_state("target", 4, sizeof(uint32_t), false));
        auto writer = std::make_shared<DataWriteProcessor>();
        add(writer);

        writer->write_field("target", Kakshya::DataVariant(bytes_of({ 11, 22, 33, 44 })));
        run_stages();

        EXPECT_EQ(probe_read<uint32_t>("target"), (std::vector<uint32_t> { 11, 22, 33, 44 }));
    }

    TEST_F(StateFieldBufferTest, DataWriteProcessorWritesTheSlotTheNextStageReads)
    {
        make_probe();
        ASSERT_TRUE(probe->declare_state("target", 4, sizeof(uint32_t), true));
        probe_write<uint32_t>("target", { 0, 0, 0, 0 });
        probe->swap_state("target");
        auto writer = std::make_shared<DataWriteProcessor>();
        add(writer);

        writer->write_field("target", Kakshya::DataVariant(bytes_of({ 1, 2, 3, 4 })));
        run_stages();

        EXPECT_EQ(probe_read<uint32_t>("target"), (std::vector<uint32_t> { 1, 2, 3, 4 }));
    }

    TEST_F(StateFieldBufferTest, DataWriteProcessorRefusesADataSizeThatIsNotTheFieldSize)
    {
        make_probe();
        ASSERT_TRUE(probe->declare_state("target", 4, sizeof(uint32_t), false));
        probe_write<uint32_t>("target", { 5, 5, 5, 5 });
        auto writer = std::make_shared<DataWriteProcessor>();
        add(writer);

        writer->write_field("target", Kakshya::DataVariant(bytes_of({ 1, 2 })));
        run_stages();

        EXPECT_EQ(probe_read<uint32_t>("target"), (std::vector<uint32_t> { 5, 5, 5, 5 }));
    }

    TEST_F(StateFieldBufferTest, DataWriteProcessorLeavesAnUnknownFieldAndTheOthersAlone)
    {
        make_probe();
        ASSERT_TRUE(probe->declare_state("target", 4, sizeof(uint32_t), false));
        probe_write<uint32_t>("target", { 5, 5, 5, 5 });
        auto writer = std::make_shared<DataWriteProcessor>();
        add(writer);

        writer->write_field("missing", Kakshya::DataVariant(bytes_of({ 1, 2, 3, 4 })));
        run_stages();

        EXPECT_FALSE(probe->has_state("missing"));
        EXPECT_EQ(probe_read<uint32_t>("target"), (std::vector<uint32_t> { 5, 5, 5, 5 }));
    }

    TEST_F(StateFieldBufferTest, DataWriteProcessorStillWritesAStateFieldOfANetworkBuffer)
    {
        auto particles = make_particles(32);
        particles->create_operator<Nodes::Network::PhysicsOperator>();
        attach(particles, 1.0F);
        ASSERT_TRUE(buffer->declare_state("target", 4, sizeof(uint32_t), false));
        auto writer = std::make_shared<DataWriteProcessor>();
        ASSERT_TRUE(buffer->get_processing_chain()->add_processor(writer, buffer));

        writer->write_field("target", Kakshya::DataVariant(bytes_of({ 7, 8, 9, 10 })));
        buffer->get_processing_chain()->process_complete(buffer);

        EXPECT_EQ(read_state<uint32_t>("target"), (std::vector<uint32_t> { 7, 8, 9, 10 }));
    }

    TEST_F(StateFieldBufferTest, DataReadProcessorReadsANamedFieldOfAProbeBuffer)
    {
        make_probe();
        ASSERT_TRUE(probe->declare_state("source", 4, sizeof(uint32_t), false));
        probe_write<uint32_t>("source", { 21, 22, 23, 24 });
        auto reader = std::make_shared<DataReadProcessor>();
        add(reader);

        reader->read_field("source");
        run_stages();

        ASSERT_TRUE(reader->has_result());
        const auto result = reader->resolve_bytes();
        ASSERT_TRUE(result.has_value());
        const auto bytes = Kakshya::convert_variant<uint8_t>(*result);
        ASSERT_EQ(bytes.size(), 4 * sizeof(uint32_t));
        std::vector<uint32_t> values(4);
        std::memcpy(values.data(), bytes.data(), bytes.size());
        EXPECT_EQ(values, (std::vector<uint32_t> { 21, 22, 23, 24 }));
    }

    TEST_F(StateFieldBufferTest, DataReadProcessorReadsTheCurrentReadSlotAfterASwap)
    {
        make_probe();
        ASSERT_TRUE(probe->declare_state("source", 4, sizeof(uint32_t), true));
        probe_write<uint32_t>("source", { 1, 1, 1, 1 });
        probe->swap_state("source");
        probe_write<uint32_t>("source", { 2, 2, 2, 2 });
        probe->swap_state("source");
        auto reader = std::make_shared<DataReadProcessor>();
        add(reader);

        reader->read_field("source");
        run_stages();

        const auto result = reader->resolve_bytes();
        ASSERT_TRUE(result.has_value());
        const auto bytes = Kakshya::convert_variant<uint8_t>(*result);
        std::vector<uint32_t> values(4);
        ASSERT_EQ(bytes.size(), values.size() * sizeof(uint32_t));
        std::memcpy(values.data(), bytes.data(), bytes.size());
        EXPECT_EQ(values, (std::vector<uint32_t> { 2, 2, 2, 2 }));
    }

    TEST_F(StateFieldBufferTest, DataReadProcessorGivesNoResultForAnUnknownField)
    {
        make_probe();
        auto reader = std::make_shared<DataReadProcessor>();
        add(reader);

        reader->read_field("missing");
        run_stages();

        EXPECT_FALSE(reader->has_result());
    }

    // The field pass over a probe buffer

    TEST_F(StateFieldBufferTest, TheFieldPassDisplacesEveryRecordOfAProbeBuffer)
    {
        make_probe();
        fill(scattered(k_records));
        const auto before = read_words();
        auto op = std::make_shared<GpuFieldOperator>(Kakshya::VertexLayout::for_points());
        op->bind(FieldTarget::POSITION, fields::shift_x);
        add(std::make_shared<Buffers::VertexFieldProcessor>(op));

        run_stages();

        const auto after = read_words();
        for (uint32_t i = 0; i < k_records; ++i) {
            const size_t base = static_cast<size_t>(i) * k_stride_words;
            EXPECT_NEAR(after[base], before[base] + 0.25F, 1e-5F) << "record " << i;
            EXPECT_EQ(after[base + 1], before[base + 1]) << "record " << i;
            EXPECT_EQ(after[base + 2], before[base + 2]) << "record " << i;
        }
    }

    TEST_F(StateFieldBufferTest, AClusterScopedBindingMovesOnlyItsClusterOfAProbeBuffer)
    {
        make_probe();
        fill(scattered(k_records));
        const auto before = read_words();
        ASSERT_TRUE(probe->declare_state("hash_cluster_id", k_records, sizeof(uint32_t), false));
        std::vector<uint32_t> clusters(k_records);
        for (uint32_t i = 0; i < k_records; ++i) {
            clusters[i] = i % 2U;
        }
        probe_write<uint32_t>("hash_cluster_id", clusters);

        auto op = std::make_shared<GpuFieldOperator>(Kakshya::VertexLayout::for_points());
        op->bind(FieldTarget::POSITION, fields::shift_x, 1U);
        add(std::make_shared<Buffers::VertexFieldProcessor>(op));

        run_stages();

        const auto after = read_words();
        for (uint32_t i = 0; i < k_records; ++i) {
            const size_t base = static_cast<size_t>(i) * k_stride_words;
            const float shift = clusters[i] == 1U ? 0.25F : 0.0F;
            EXPECT_NEAR(after[base], before[base] + shift, 1e-5F) << "record " << i;
        }
    }

    TEST_F(StateFieldBufferTest, AClusterScopedBindingOverAProbeWithoutClusterIdsMovesNothing)
    {
        make_probe();
        fill(scattered(k_records));
        const auto before = read_words();
        auto op = std::make_shared<GpuFieldOperator>(Kakshya::VertexLayout::for_points());
        op->bind(FieldTarget::POSITION, fields::shift_x, 1U);
        add(std::make_shared<Buffers::VertexFieldProcessor>(op));

        run_stages();

        EXPECT_EQ(read_words(), before);
        EXPECT_FALSE(probe->has_state("hash_cluster_id"));
    }

} // namespace
} // namespace MayaFlux::Test
