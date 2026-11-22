#include "network_buffer_fixture.hpp"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Buffers/Network/MutationClaimProcessor.hpp"
#include "MayaFlux/Buffers/Network/PopulationProcessor.hpp"
#include "MayaFlux/Buffers/Network/SpatialFieldWiring.hpp"
#include "MayaFlux/Buffers/Network/SpatialHashProcessor.hpp"
#include "MayaFlux/Buffers/Shaders/VertexFieldProcessor.hpp"
#include "MayaFlux/Buffers/State/StateFieldBuffer.hpp"
#include "MayaFlux/Kinesis/Tendency/DualField.hpp"
#include "MayaFlux/Kinesis/Tendency/ShaderCompat.hpp"
#include "MayaFlux/Kinesis/VertexSampler.hpp"
#include "MayaFlux/Nodes/Network/Operators/GpuFieldOperator.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test {
namespace {

    using Buffers::BufferProcessingChain;
    using Buffers::SpatialHashConfig;
    using Buffers::StateFieldBuffer;
    using Kinesis::FieldTarget;
    using Kinesis::SamplerBounds;
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
     * @brief Vertex storage and nothing that feeds it: a buffer with no network behind it.
     */
    class WiringProbeBuffer final : public StateFieldBuffer {
    public:
        explicit WiringProbeBuffer(size_t byte_count)
            : StateFieldBuffer(byte_count, Usage::VERTEX, Kakshya::DataModality::VERTICES_3D)
        {
        }

        void setup_processors(Buffers::ProcessingToken token) override
        {
            auto chain = get_processing_chain();
            if (!chain) {
                chain = std::make_shared<BufferProcessingChain>();
                set_processing_chain(chain);
            }
            chain->set_preferred_token(token);
        }
    };

    std::string stage_name(const std::shared_ptr<Buffers::BufferProcessor>& processor)
    {
        const auto* raw = processor.get();
        if (dynamic_cast<const Buffers::PopulationInitProcessor*>(raw))
            return "PopulationInit";
        if (dynamic_cast<const Buffers::HashClearProcessor*>(raw))
            return "HashClear";
        if (dynamic_cast<const Buffers::HashCountProcessor*>(raw))
            return "HashCount";
        if (dynamic_cast<const Buffers::HashScanProcessor*>(raw))
            return "HashScan";
        if (dynamic_cast<const Buffers::HashScatterProcessor*>(raw))
            return "HashScatter";
        if (dynamic_cast<const Buffers::HashDensityColorProcessor*>(raw))
            return "HashDensityColor";
        if (dynamic_cast<const Buffers::ClaimInitProcessor*>(raw))
            return "ClaimInit";
        if (dynamic_cast<const Buffers::ClaimProcessor*>(raw))
            return "Claim";
        if (dynamic_cast<const Buffers::ClaimFlattenProcessor*>(raw))
            return "ClaimFlatten";
        if (dynamic_cast<const Buffers::ClaimAccumulateProcessor*>(raw))
            return "ClaimAccumulate";
        if (dynamic_cast<const Buffers::ClaimSwallowProcessor*>(raw))
            return "ClaimSwallow";
        if (dynamic_cast<const Buffers::ClaimTransferProcessor*>(raw))
            return "ClaimTransfer";
        if (dynamic_cast<const Buffers::PopulationSpawnProcessor*>(raw))
            return "PopulationSpawn";
        if (dynamic_cast<const Buffers::VertexFieldProcessor*>(raw))
            return "VertexField";
        return "Other";
    }

    using Names = std::vector<std::string>;

    SamplerBounds unit_bounds() { return SamplerBounds { glm::vec3(-1.0F), glm::vec3(1.0F) }; }

    std::optional<SpatialHashConfig> probe_hash(uint32_t count = k_records)
    {
        return SpatialHashConfig::from_bounds(
            unit_bounds(), Kakshya::VertexLayout::for_points(), count, k_cell_size);
    }

    std::vector<uint32_t> counting_ids(uint32_t count)
    {
        std::vector<uint32_t> ids(count);
        std::iota(ids.begin(), ids.end(), 0U);
        return ids;
    }

    uint32_t cell_of(const glm::vec3& p)
    {
        glm::ivec3 c = glm::ivec3(glm::floor((p - glm::vec3(-1.0F)) / k_cell_size));
        c = glm::clamp(c, glm::ivec3(0), glm::ivec3(3));
        return static_cast<uint32_t>(c.x) + static_cast<uint32_t>(c.y) * 4U + static_cast<uint32_t>(c.z) * 16U;
    }

    // SpatialHashConfig::from_bounds: no device, no buffer

    MAYAFLUX_TEST_WITH(TEST, SpatialHashFromBoundsContractTest, TheGridCoversTheBoundsInWholeCells, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        const auto config = probe_hash();

        ASSERT_TRUE(config.has_value());
        EXPECT_EQ(config->grid_min, glm::vec3(-1.0F));
        EXPECT_EQ(config->grid_dims, glm::uvec3(4U));
        EXPECT_EQ(config->cell_count(), 64U);
        EXPECT_EQ(config->cell_size, k_cell_size);
        EXPECT_EQ(config->particle_count, k_records);
        EXPECT_EQ(config->stride_words, k_stride_words);
        EXPECT_EQ(config->position_word_offset, 0U);
    }

    MAYAFLUX_TEST_WITH(TEST, SpatialHashFromBoundsContractTest, AnExtentThatIsNotAWholeNumberOfCellsRoundsUp, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        const SamplerBounds bounds { glm::vec3(0.0F), glm::vec3(2.2F, 0.4F, 1.0F) };

        const auto config = SpatialHashConfig::from_bounds(
            bounds, Kakshya::VertexLayout::for_points(), 10, k_cell_size);

        ASSERT_TRUE(config.has_value());
        EXPECT_EQ(config->grid_dims, glm::uvec3(5U, 1U, 2U));
        EXPECT_EQ(config->grid_min, glm::vec3(0.0F));
    }

    MAYAFLUX_TEST_WITH(TEST, SpatialHashFromBoundsContractTest, AFlatAxisStillGetsOneCell, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        const SamplerBounds bounds { glm::vec3(0.0F), glm::vec3(1.0F, 0.0F, 1.0F) };

        const auto config = SpatialHashConfig::from_bounds(
            bounds, Kakshya::VertexLayout::for_points(), 10, k_cell_size);

        ASSERT_TRUE(config.has_value());
        EXPECT_EQ(config->grid_dims, glm::uvec3(2U, 1U, 2U));
    }

    MAYAFLUX_TEST_WITH(TEST, SpatialHashFromBoundsContractTest, ANonPositiveCellSizeGivesNoConfig, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        const auto layout = Kakshya::VertexLayout::for_points();

        EXPECT_FALSE(SpatialHashConfig::from_bounds(unit_bounds(), layout, k_records, 0.0F).has_value());
        EXPECT_FALSE(SpatialHashConfig::from_bounds(unit_bounds(), layout, k_records, -1.0F).has_value());
    }

    MAYAFLUX_TEST_WITH(TEST, SpatialHashFromBoundsContractTest, AStrideThatIsNotAWholeNumberOfWordsGivesNoConfig, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        auto layout = Kakshya::VertexLayout::for_points();

        layout.stride_bytes = 0;
        EXPECT_FALSE(SpatialHashConfig::from_bounds(unit_bounds(), layout, k_records, k_cell_size).has_value());

        layout.stride_bytes = 66;
        EXPECT_FALSE(SpatialHashConfig::from_bounds(unit_bounds(), layout, k_records, k_cell_size).has_value());
    }

    MAYAFLUX_TEST_WITH(TEST, SpatialHashFromBoundsContractTest, ALayoutWithNoWordAlignedPositionGivesNoConfig, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        auto misaligned = Kakshya::VertexLayout::for_points();
        misaligned.attributes.front().offset_in_vertex = 2;
        EXPECT_FALSE(SpatialHashConfig::from_bounds(unit_bounds(), misaligned, k_records, k_cell_size).has_value());

        auto none = Kakshya::VertexLayout::for_points();
        none.attributes.clear();
        EXPECT_FALSE(SpatialHashConfig::from_bounds(unit_bounds(), none, k_records, k_cell_size).has_value());
    }

    MAYAFLUX_TEST_WITH(TEST, SpatialHashFromBoundsContractTest, ThePositionOffsetFollowsTheLayout, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        auto layout = Kakshya::VertexLayout::for_points();
        layout.attributes.front().offset_in_vertex = 8;

        const auto config = SpatialHashConfig::from_bounds(unit_bounds(), layout, k_records, k_cell_size);

        ASSERT_TRUE(config.has_value());
        EXPECT_EQ(config->position_word_offset, 2U);
    }

    // wire_field_stages: what it needs no device to say

    MAYAFLUX_TEST_WITH(TEST, SpatialFieldWiringContractTest, ADensityOrAbsorbConfigWantsTheSpatialStages, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        EXPECT_FALSE(Buffers::wants_spatial_stages(SpatialFieldConfig {}));
        EXPECT_FALSE(Buffers::wants_spatial_stages(SpatialFieldConfig { .cell_size = 0.5F }));
        EXPECT_FALSE(Buffers::wants_spatial_stages(SpatialFieldConfig { .reserve_fraction = 0.5F, .transfer_on_claim = true }));
        EXPECT_TRUE(Buffers::wants_spatial_stages(SpatialFieldConfig { .density_color = true }));
        EXPECT_TRUE(Buffers::wants_spatial_stages(SpatialFieldConfig { .absorb_radius = 0.1F }));
    }

    MAYAFLUX_TEST_WITH(TEST, SpatialFieldWiringContractTest, ANullBufferOrANullOperatorIsLeftAlone, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        const auto probe = std::make_shared<WiringProbeBuffer>(256);
        const auto op = std::make_shared<GpuFieldOperator>(
            Kakshya::VertexLayout::for_points(), SpatialFieldConfig { .cell_size = 0.5F, .density_color = true });

        EXPECT_NO_THROW(Buffers::wire_field_stages(nullptr, op, probe_hash(), {}, nullptr));
        EXPECT_NO_THROW(Buffers::wire_field_stages(probe, nullptr, probe_hash(), {}, nullptr));

        EXPECT_FALSE(probe->has_state("hash_cell_count"));
    }

    // wire_field_stages over a buffer with no network

    class SpatialFieldWiringProbeTest : public NetworkBufferDeviceTest {
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
            probe = std::make_shared<WiringProbeBuffer>(static_cast<size_t>(records) * k_stride_words * 4);
            service->initialize_buffer(probe);
            ASSERT_TRUE(probe->is_initialized());
            probe->setup_processors(Buffers::ProcessingToken::GRAPHICS_BACKEND);
            ASSERT_NE(probe->get_processing_chain(), nullptr);
        }

        [[nodiscard]] static std::shared_ptr<GpuFieldOperator> make_op(
            SpatialFieldConfig config, bool bind_field = true)
        {
            auto op = std::make_shared<GpuFieldOperator>(Kakshya::VertexLayout::for_points(), config);
            if (bind_field) {
                op->bind(FieldTarget::POSITION, fields::shift_x);
            }
            return op;
        }

        void wire(const std::shared_ptr<GpuFieldOperator>& op,
            const std::optional<SpatialHashConfig>& hash = probe_hash(),
            const std::vector<uint32_t>& cluster_ids = {})
        {
            Buffers::wire_field_stages(probe, op, hash, cluster_ids, nullptr);
        }

        [[nodiscard]] Names stages() const
        {
            Names names;
            for (const auto& processor : probe->get_processing_chain()->get_processors(probe)) {
                names.push_back(stage_name(processor));
            }
            return names;
        }

        static std::vector<glm::vec3> scattered(uint32_t count)
        {
            std::mt19937 rng(11U);
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
            std::vector<float> words(positions.size() * k_stride_words, 0.0F);
            for (size_t i = 0; i < positions.size(); ++i) {
                words[i * k_stride_words] = positions[i].x;
                words[i * k_stride_words + 1] = positions[i].y;
                words[i * k_stride_words + 2] = positions[i].z;
            }
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

        void run_stages()
        {
            probe->get_processing_chain()->process_complete(probe);
        }

        [[nodiscard]] std::vector<uint32_t> expected_counts() const
        {
            std::vector<uint32_t> counts(64, 0U);
            for (const auto& p : positions_in_use) {
                ++counts[cell_of(p)];
            }
            return counts;
        }

        std::shared_ptr<WiringProbeBuffer> probe;
        uint32_t record_count {};
        std::vector<glm::vec3> positions_in_use;
    };

    // Chain composition

    TEST_F(SpatialFieldWiringProbeTest, TheFullConfigurationWiresEveryStageInOrderWithTheFieldPassLast)
    {
        make_probe();

        wire(make_op(SpatialFieldConfig {
            .cell_size = 0.5F,
            .absorb_radius = 0.1F,
            .density_color = true,
            .reserve_fraction = 0.5F,
            .transfer_on_claim = true }));

        EXPECT_EQ(stages(), (Names {
                                "PopulationInit", "HashClear", "HashCount", "HashScan", "HashScatter",
                                "HashDensityColor", "ClaimInit", "Claim", "ClaimFlatten", "ClaimAccumulate",
                                "ClaimSwallow", "ClaimTransfer", "PopulationSpawn", "VertexField" }));
    }

    TEST_F(SpatialFieldWiringProbeTest, DensityColourAloneBuildsTheHashAndColoursBeforeTheFieldPass)
    {
        make_probe();

        wire(make_op(SpatialFieldConfig { .cell_size = 0.5F, .density_color = true }));

        EXPECT_EQ(stages(), (Names { "HashClear", "HashCount", "HashScan", "HashScatter",
                                "HashDensityColor", "VertexField" }));
    }

    TEST_F(SpatialFieldWiringProbeTest, AbsorbWithoutSwallowOrTransferStopsAfterAccumulate)
    {
        make_probe();

        wire(make_op(SpatialFieldConfig { .cell_size = 0.5F, .absorb_radius = 0.1F, .cosmetic_swallow = false }, false));

        EXPECT_EQ(stages(), (Names { "HashClear", "HashCount", "HashScan", "HashScatter",
                                "ClaimInit", "Claim", "ClaimFlatten", "ClaimAccumulate" }));
    }

    TEST_F(SpatialFieldWiringProbeTest, BindingsAloneWireOnlyTheFieldPassAndNeedNoHash)
    {
        make_probe();

        wire(make_op(SpatialFieldConfig {}), std::nullopt);

        EXPECT_EQ(stages(), (Names { "VertexField" }));
        EXPECT_FALSE(probe->has_state("hash_cell_count"));
    }

    TEST_F(SpatialFieldWiringProbeTest, NoBindingsMeansNoFieldPass)
    {
        make_probe();

        wire(make_op(SpatialFieldConfig { .cell_size = 0.5F, .density_color = true }, false));

        const auto names = stages();
        EXPECT_EQ(std::ranges::count(names, "VertexField"), 0);
        EXPECT_EQ(names.back(), "HashDensityColor");
    }

    TEST_F(SpatialFieldWiringProbeTest, WithoutAHashTheStagesAreSkippedButTheFieldPassIsStillWired)
    {
        make_probe();

        wire(make_op(SpatialFieldConfig { .cell_size = 0.5F, .absorb_radius = 0.1F, .density_color = true }), std::nullopt);

        EXPECT_EQ(stages(), (Names { "VertexField" }));
        EXPECT_FALSE(probe->has_state("hash_cell_count"));
    }

    TEST_F(SpatialFieldWiringProbeTest, TransferWithoutAbsorbIsDisabledAndWiresNoClaims)
    {
        make_probe();

        wire(make_op(SpatialFieldConfig { .cell_size = 0.5F, .density_color = true, .transfer_on_claim = true }));

        const auto names = stages();
        EXPECT_EQ(std::ranges::count(names, "ClaimTransfer"), 0);
        EXPECT_EQ(std::ranges::count(names, "Claim"), 0);
        EXPECT_EQ(std::ranges::count(names, "HashDensityColor"), 1);
    }

    TEST_F(SpatialFieldWiringProbeTest, ReserveWithoutAbsorbIsDisabledAndLeavesTheBufferAlone)
    {
        make_probe();

        wire(make_op(SpatialFieldConfig { .cell_size = 0.5F, .density_color = true, .reserve_fraction = 0.5F }));

        const auto names = stages();
        EXPECT_EQ(std::ranges::count(names, "PopulationInit"), 0);
        EXPECT_EQ(std::ranges::count(names, "PopulationSpawn"), 0);
        EXPECT_FALSE(probe->has_state("mutation_alive"));
        EXPECT_EQ(probe->get_size_bytes(), k_records * k_stride_words * 4);
    }

    // Reserve capacity and cluster ids

    TEST_F(SpatialFieldWiringProbeTest, ReserveCapacityGrowsTheBufferAndSizesEveryFieldToTheTotal)
    {
        make_probe();

        wire(make_op(SpatialFieldConfig { .cell_size = 0.5F, .absorb_radius = 0.1F, .reserve_fraction = 0.5F }));

        constexpr uint32_t total = k_records + k_records / 2;
        EXPECT_GE(probe->get_size_bytes(), total * k_stride_words * 4);
        EXPECT_EQ(probe->get_state_bytes("hash_cluster_id"), total * sizeof(uint32_t));
        EXPECT_EQ(probe->get_state_bytes("hash_particle_index"), total * sizeof(uint32_t));
        EXPECT_EQ(probe->get_state_bytes("mutation_alive"), total * sizeof(uint32_t));
        EXPECT_EQ(probe->get_state_bytes("mutation_spawn_cursor"), sizeof(uint32_t));
    }

    TEST_F(SpatialFieldWiringProbeTest, TheReserveTakesClusterIdZeroAndTheLiveRecordsKeepTheirs)
    {
        make_probe();

        wire(make_op(SpatialFieldConfig { .cell_size = 0.5F, .absorb_radius = 0.1F, .reserve_fraction = 0.5F }),
            probe_hash(), counting_ids(k_records));

        auto expected = counting_ids(k_records);
        expected.resize(k_records + k_records / 2, 0U);
        EXPECT_EQ(probe_read<uint32_t>("hash_cluster_id"), expected);
    }

    TEST_F(SpatialFieldWiringProbeTest, WithoutAReserveEveryRecordKeepsItsClusterId)
    {
        make_probe();

        wire(make_op(SpatialFieldConfig { .cell_size = 0.5F, .density_color = true }),
            probe_hash(), counting_ids(k_records));

        EXPECT_EQ(probe_read<uint32_t>("hash_cluster_id"), counting_ids(k_records));
    }

    TEST_F(SpatialFieldWiringProbeTest, NoClusterIdsGivesEveryRecordZero)
    {
        make_probe();

        wire(make_op(SpatialFieldConfig { .cell_size = 0.5F, .density_color = true }));

        EXPECT_EQ(probe_read<uint32_t>("hash_cluster_id"), std::vector<uint32_t>(k_records, 0U));
    }

    TEST_F(SpatialFieldWiringProbeTest, ClusterIdsPastTheLiveCountAreIgnored)
    {
        make_probe();

        wire(make_op(SpatialFieldConfig { .cell_size = 0.5F, .density_color = true }),
            probe_hash(), counting_ids(k_records + 10));

        EXPECT_EQ(probe_read<uint32_t>("hash_cluster_id"), counting_ids(k_records));
    }

    // Dispatch over the wired chain

    TEST_F(SpatialFieldWiringProbeTest, TheWiredHashBucketsTheRecordsBeforeTheFieldPassMovesThem)
    {
        make_probe();
        fill(scattered(k_records));
        const auto before = read_words();

        wire(make_op(SpatialFieldConfig { .cell_size = 0.5F, .density_color = true }));
        run_stages();

        const auto counts = probe_read<uint32_t>("hash_cell_count");
        EXPECT_EQ(counts, expected_counts());
        EXPECT_EQ(std::accumulate(counts.begin(), counts.end(), 0U), k_records);

        const auto after = read_words();
        for (uint32_t i = 0; i < k_records; ++i) {
            const size_t base = static_cast<size_t>(i) * k_stride_words;
            EXPECT_NEAR(after[base], before[base] + 0.25F, 1e-5F) << "record " << i;
            EXPECT_EQ(after[base + 1], before[base + 1]) << "record " << i;
            EXPECT_EQ(after[base + 2], before[base + 2]) << "record " << i;
        }
    }

    TEST_F(SpatialFieldWiringProbeTest, TheReserveRecordsStayOutOfTheWiredHash)
    {
        make_probe();

        wire(make_op(SpatialFieldConfig { .cell_size = 0.5F, .absorb_radius = 0.01F, .reserve_fraction = 0.5F }, false));
        fill(scattered(k_records));
        run_stages();

        const auto counts = probe_read<uint32_t>("hash_cell_count");
        EXPECT_EQ(std::accumulate(counts.begin(), counts.end(), 0U), k_records);

        std::vector<uint32_t> alive(k_records + k_records / 2, 0U);
        std::fill_n(alive.begin(), k_records, 1U);
        EXPECT_EQ(probe_read<uint32_t>("mutation_alive"), alive);
    }

    // The same config derived by hand and by the network

    class SpatialHashFromBoundsDeviceTest : public NetworkBufferDeviceTest { };

    TEST_F(SpatialHashFromBoundsDeviceTest, ExplicitValuesGiveTheConfigTheNetworkDerives)
    {
        auto particles = make_particles(k_records);
        particles->create_operator<Nodes::Network::PhysicsOperator>();
        attach(particles, 1.0F);

        const auto derived = SpatialHashConfig::from_network(buffer, k_cell_size);
        const auto explicit_config = SpatialHashConfig::from_bounds(
            unit_bounds(), Kakshya::VertexLayout::for_points(), k_records, k_cell_size);

        ASSERT_TRUE(derived.has_value());
        ASSERT_TRUE(explicit_config.has_value());
        EXPECT_EQ(explicit_config->grid_min, derived->grid_min);
        EXPECT_EQ(explicit_config->grid_dims, derived->grid_dims);
        EXPECT_EQ(explicit_config->cell_size, derived->cell_size);
        EXPECT_EQ(explicit_config->particle_count, derived->particle_count);
        EXPECT_EQ(explicit_config->stride_words, derived->stride_words);
        EXPECT_EQ(explicit_config->position_word_offset, derived->position_word_offset);
    }

} // namespace
} // namespace MayaFlux::Test
