#include "network_buffer_fixture.hpp"

#include "MayaFlux/Buffers/Network/MutationClaimProcessor.hpp"
#include "MayaFlux/Buffers/Network/PopulationProcessor.hpp"
#include "MayaFlux/Buffers/Network/SpatialHashProcessor.hpp"
#include "MayaFlux/Buffers/Shaders/VertexFieldProcessor.hpp"
#include "MayaFlux/Kinesis/Tendency/DualField.hpp"
#include "MayaFlux/Kinesis/Tendency/ShaderCompat.hpp"
#include "MayaFlux/Nodes/Network/AssemblyNetwork.hpp"
#include "MayaFlux/Nodes/Network/Operators/GpuFieldOperator.hpp"
#include "MayaFlux/Nodes/Network/PointCloudNetwork.hpp"

namespace MayaFlux::Test {
namespace {

    using Kinesis::FieldTarget;
    using Nodes::Network::GpuFieldOperator;
    using Nodes::Network::ParticleNetwork;
    using Nodes::Network::PhysicsOperator;
    using Nodes::Network::PointCloudNetwork;
    using Nodes::Network::SpatialFieldConfig;

    namespace fields {

        using namespace MayaFlux::ShaderCompat;

        const auto shift_x = MF_FIELD(shift_x, [](vec3 p) -> vec3 {
            return p * 0.0f + vec3(0.25f, 0.0f, 0.0f);
        });

        const auto tint_red = MF_FIELD(tint_red, [](vec3 p) -> vec3 {
            return p * 0.0f + vec3(1.0f, 0.0f, 0.0f);
        });

    } // namespace fields

    constexpr uint32_t k_count = 64;
    constexpr size_t k_stride_words = 17;

    std::string name_of(const std::shared_ptr<Buffers::BufferProcessor>& processor)
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

    class NetworkFieldWiringTest : public NetworkBufferDeviceTest {
    protected:
        void wire_particles(SpatialFieldConfig field_config, bool bind_field = true)
        {
            auto particles = std::make_shared<ParticleNetwork>(
                k_count, glm::vec3(-1.0F), glm::vec3(1.0F),
                Kinesis::SpatialDistribution::RANDOM_VOLUME);
            particles->create_operator<PhysicsOperator>();
            field_op = particles->get_operator_chain()->emplace<GpuFieldOperator>(
                Kakshya::VertexLayout::for_points(), field_config);
            if (bind_field)
                field_op->bind(FieldTarget::POSITION, fields::shift_x);
            attach(particles, 1.0F);
        }

        [[nodiscard]] Names stages() const
        {
            Names names;
            for (const auto& processor : buffer->get_processing_chain()->get_processors(buffer))
                names.push_back(name_of(processor));
            return names;
        }

        [[nodiscard]] std::shared_ptr<Buffers::VertexFieldProcessor> field_pass() const
        {
            for (const auto& processor : buffer->get_processing_chain()->get_processors(buffer)) {
                if (auto pass = std::dynamic_pointer_cast<Buffers::VertexFieldProcessor>(processor))
                    return pass;
            }
            return nullptr;
        }

        [[nodiscard]] std::vector<float> read_words(uint32_t records)
        {
            const auto bytes = read_vertices(static_cast<size_t>(records) * k_stride_words * 4);
            std::vector<float> words(bytes.size() / sizeof(float));
            std::memcpy(words.data(), bytes.data(), bytes.size());
            return words;
        }

        std::shared_ptr<GpuFieldOperator> field_op;
    };

    // Chain composition

    TEST_F(NetworkFieldWiringTest, TheFullConfigurationWiresEveryStageInOrderWithTheFieldPassLast)
    {
        wire_particles(SpatialFieldConfig {
            .cell_size = 0.5F,
            .absorb_radius = 0.1F,
            .density_color = true,
            .reserve_fraction = 0.5F,
            .transfer_on_claim = true });

        EXPECT_EQ(stages(), (Names {
                                "PopulationInit", "HashClear", "HashCount", "HashScan", "HashScatter",
                                "HashDensityColor", "ClaimInit", "Claim", "ClaimFlatten", "ClaimAccumulate",
                                "ClaimSwallow", "ClaimTransfer", "PopulationSpawn", "VertexField" }));
    }

    TEST_F(NetworkFieldWiringTest, DensityColourAloneBuildsTheHashAndColoursBeforeTheFieldPass)
    {
        wire_particles(SpatialFieldConfig { .cell_size = 0.5F, .density_color = true });

        EXPECT_EQ(stages(), (Names { "HashClear", "HashCount", "HashScan", "HashScatter",
                                "HashDensityColor", "VertexField" }));
    }

    TEST_F(NetworkFieldWiringTest, AbsorbWithoutSwallowOrTransferStopsAfterAccumulate)
    {
        wire_particles(SpatialFieldConfig {
                           .cell_size = 0.5F, .absorb_radius = 0.1F, .cosmetic_swallow = false },
            false);

        EXPECT_EQ(stages(), (Names { "HashClear", "HashCount", "HashScan", "HashScatter",
                                "ClaimInit", "Claim", "ClaimFlatten", "ClaimAccumulate" }));
    }

    TEST_F(NetworkFieldWiringTest, BindingsAloneWireOnlyTheFieldPass)
    {
        wire_particles(SpatialFieldConfig {});

        EXPECT_EQ(stages(), (Names { "VertexField" }));
    }

    TEST_F(NetworkFieldWiringTest, NoBindingsMeansNoFieldPass)
    {
        wire_particles(SpatialFieldConfig { .cell_size = 0.5F, .density_color = true }, false);

        const auto names = stages();
        EXPECT_EQ(std::ranges::count(names, "VertexField"), 0);
        EXPECT_EQ(names.back(), "HashDensityColor");
    }

    TEST_F(NetworkFieldWiringTest, TransferWithoutAbsorbIsDisabledAndWiresNoClaims)
    {
        wire_particles(SpatialFieldConfig { .cell_size = 0.5F, .density_color = true, .transfer_on_claim = true });

        const auto names = stages();
        EXPECT_EQ(std::ranges::count(names, "ClaimTransfer"), 0);
        EXPECT_EQ(std::ranges::count(names, "Claim"), 0);
        EXPECT_EQ(std::ranges::count(names, "HashDensityColor"), 1);
    }

    TEST_F(NetworkFieldWiringTest, ReserveWithoutAbsorbIsDisabledAndLeavesTheBufferAlone)
    {
        wire_particles(SpatialFieldConfig { .cell_size = 0.5F, .density_color = true, .reserve_fraction = 0.5F });

        const auto names = stages();
        EXPECT_EQ(std::ranges::count(names, "PopulationInit"), 0);
        EXPECT_EQ(std::ranges::count(names, "PopulationSpawn"), 0);
        EXPECT_FALSE(buffer->has_state("mutation_alive"));
        EXPECT_EQ(buffer->get_size_bytes(), k_count * k_stride_words * 4);
    }

    TEST_F(NetworkFieldWiringTest, ANonPhysicsPrimaryNeedsAnExplicitCellSize)
    {
        auto cloud = std::make_shared<PointCloudNetwork>(
            k_count, glm::vec3(-1.0F), glm::vec3(1.0F), Kinesis::SpatialDistribution::RANDOM_VOLUME);
        cloud->initialize();
        field_op = cloud->get_operator_chain()->emplace<GpuFieldOperator>(
            Kakshya::VertexLayout::for_points(), SpatialFieldConfig { .density_color = true });
        field_op->bind(FieldTarget::POSITION, fields::shift_x);
        attach(cloud);

        EXPECT_EQ(stages(), (Names { "VertexField" }));
    }

    TEST_F(NetworkFieldWiringTest, NetworksThatAreNotFieldCompatibleAreNotWired)
    {
        auto assembly = std::make_shared<Nodes::Network::AssemblyNetwork>();
        assembly->add_geometry(std::vector<glm::vec3> { { 0, 0, 0 }, { 1, 0, 0 } });
        field_op = assembly->get_operator_chain()->emplace<GpuFieldOperator>(
            Kakshya::VertexLayout::for_points(), SpatialFieldConfig { .cell_size = 0.5F, .density_color = true });
        field_op->bind(FieldTarget::POSITION, fields::shift_x);
        attach(assembly);

        EXPECT_TRUE(stages().empty());
    }

    TEST_F(NetworkFieldWiringTest, ReserveCapacityGrowsTheBufferAndPadsTheClusterIds)
    {
        wire_particles(SpatialFieldConfig { .cell_size = 0.5F, .absorb_radius = 0.1F, .reserve_fraction = 0.5F });

        constexpr uint32_t total = k_count + k_count / 2;
        EXPECT_GE(buffer->get_size_bytes(), total * k_stride_words * 4);
        EXPECT_EQ(buffer->get_state_bytes("hash_cluster_id"), total * sizeof(uint32_t));
        EXPECT_EQ(buffer->get_state_bytes("hash_particle_index"), total * sizeof(uint32_t));
        EXPECT_EQ(buffer->get_state_bytes("mutation_alive"), total * sizeof(uint32_t));
        EXPECT_EQ(buffer->get_state_bytes("mutation_spawn_cursor"), sizeof(uint32_t));
        EXPECT_EQ(read_state<uint32_t>("hash_cluster_id"), std::vector<uint32_t>(total, 0U));
    }

    // Field dispatch

    TEST_F(NetworkFieldWiringTest, TheFieldPassDisplacesEveryRecordByTheBoundField)
    {
        wire_particles(SpatialFieldConfig {});
        buffer->process_default();
        const auto before = read_words(k_count);

        buffer->get_processing_chain()->process_complete(buffer);

        const auto after = read_words(k_count);
        for (uint32_t i = 0; i < k_count; ++i) {
            const size_t base = i * k_stride_words;
            EXPECT_NEAR(after[base], before[base] + 0.25F, 1e-5F) << "record " << i;
            EXPECT_EQ(after[base + 1], before[base + 1]) << "record " << i;
            EXPECT_EQ(after[base + 2], before[base + 2]) << "record " << i;
        }
    }

    TEST_F(NetworkFieldWiringTest, AnExplicitVertexRangeLimitsTheDispatch)
    {
        wire_particles(SpatialFieldConfig {});
        buffer->process_default();
        const auto before = read_words(k_count);
        const auto pass = field_pass();
        ASSERT_NE(pass, nullptr);
        pass->set_vertex_range(10, 5);

        buffer->get_processing_chain()->process_complete(buffer);

        const auto after = read_words(k_count);
        for (uint32_t i = 0; i < k_count; ++i) {
            const size_t base = i * k_stride_words;
            const float shift = (i >= 10 && i < 15) ? 0.25F : 0.0F;
            EXPECT_NEAR(after[base], before[base] + shift, 1e-5F) << "record " << i;
        }

        pass->clear_vertex_range();
        buffer->process_default();
        const auto restored = read_words(k_count);
        buffer->get_processing_chain()->process_complete(buffer);
        const auto all = read_words(k_count);
        EXPECT_NEAR(all[0], restored[0] + 0.25F, 1e-5F);
        EXPECT_NEAR(all[(k_count - 1) * k_stride_words], restored[(k_count - 1) * k_stride_words] + 0.25F, 1e-5F);
    }

    TEST_F(NetworkFieldWiringTest, RebindingAfterWiringIsPickedUpOnTheNextDispatch)
    {
        wire_particles(SpatialFieldConfig {});
        buffer->process_default();
        buffer->get_processing_chain()->process_complete(buffer);

        field_op->bind(FieldTarget::COLOR, fields::tint_red);
        buffer->process_default();
        buffer->get_processing_chain()->process_complete(buffer);

        const auto words = read_words(k_count);
        constexpr size_t color_word = 3;
        for (uint32_t i = 0; i < k_count; ++i) {
            const size_t base = i * k_stride_words + color_word;
            EXPECT_EQ(words[base], 1.0F) << "record " << i;
            EXPECT_EQ(words[base + 1], 0.0F) << "record " << i;
            EXPECT_EQ(words[base + 2], 0.0F) << "record " << i;
        }
    }

} // namespace
} // namespace MayaFlux::Test
