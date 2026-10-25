#include "network_hash_fixture.hpp"

namespace MayaFlux::Test {
namespace {


    // Configuration

    TEST_F(NetworkHashTest, ConfigIsDerivedFromTheNetworkBoundsAndTheOperatorLayout)
    {
        EXPECT_EQ(config.grid_min, glm::vec3(-1.0F));
        EXPECT_EQ(config.grid_dims, glm::uvec3(4U));
        EXPECT_EQ(config.cell_count(), 64U);
        EXPECT_EQ(config.cell_size, k_cell);
        EXPECT_EQ(config.particle_count, k_particles);
        EXPECT_EQ(config.stride_words, 17U);
        EXPECT_EQ(config.position_word_offset, 0U);
    }

    TEST_F(NetworkHashTest, NonPositiveCellSizeGivesNoConfig)
    {
        EXPECT_FALSE(SpatialHashConfig::from_network(buffer, 0.0F).has_value());
        EXPECT_FALSE(SpatialHashConfig::from_network(buffer, -1.0F).has_value());
    }

    TEST_F(NetworkHashTest, ANetworkThatIsNotAParticleOrPointCloudGivesNoConfig)
    {
        auto assembly = std::make_shared<Nodes::Network::AssemblyNetwork>();
        assembly->add_geometry(std::vector<glm::vec3> { { 0, 0, 0 }, { 1, 1, 1 } });
        auto other = std::make_shared<Buffers::NetworkGeometryBuffer>(assembly);

        EXPECT_FALSE(SpatialHashConfig::from_network(other, k_cell).has_value());
    }

    TEST_F(NetworkHashTest, DeclareFieldsSizesEveryHashField)
    {
        EXPECT_EQ(buffer->get_state_bytes("hash_cell_count"), 64 * sizeof(uint32_t));
        EXPECT_EQ(buffer->get_state_bytes("hash_cell_start"), 64 * sizeof(uint32_t));
        EXPECT_EQ(buffer->get_state_bytes("hash_cell_cursor"), 64 * sizeof(uint32_t));
        EXPECT_EQ(buffer->get_state_bytes("hash_particle_index"), k_particles * sizeof(uint32_t));
        EXPECT_EQ(buffer->get_state_bytes("hash_cluster_id"), k_particles * sizeof(uint32_t));
    }

    // Build stages

    TEST_F(NetworkHashTest, ClearZeroesTheHistogram)
    {
        write_state<uint32_t>("hash_cell_count", std::vector<uint32_t>(64, 7U));
        add(std::make_shared<HashClearProcessor>(config));

        run_stages();

        EXPECT_EQ(read_state<uint32_t>("hash_cell_count"), std::vector<uint32_t>(64, 0U));
    }

    TEST_F(NetworkHashTest, CountBucketsEveryParticleIntoItsCell)
    {
        add(std::make_shared<HashClearProcessor>(config));
        add(std::make_shared<HashCountProcessor>(config));

        run_stages();

        const auto counts = read_state<uint32_t>("hash_cell_count");
        EXPECT_EQ(counts, expected_counts());
        EXPECT_EQ(std::accumulate(counts.begin(), counts.end(), 0U), k_particles);
    }

    TEST_F(NetworkHashTest, CountClampsOutOfBoundsParticlesToTheEdgeCells)
    {
        poke_position(0, { 100.0F, 100.0F, 100.0F });
        poke_position(1, { -100.0F, -100.0F, -100.0F });
        add(std::make_shared<HashClearProcessor>(config));
        add(std::make_shared<HashCountProcessor>(config));

        run_stages();

        const auto counts = read_state<uint32_t>("hash_cell_count");
        EXPECT_EQ(counts, expected_counts());
        EXPECT_GE(counts[63], 1U);
        EXPECT_GE(counts[0], 1U);
    }

    TEST_F(NetworkHashTest, ScanIsAnExclusivePrefixSumAndSeedsTheCursor)
    {
        add(std::make_shared<HashClearProcessor>(config));
        add(std::make_shared<HashCountProcessor>(config));
        add(std::make_shared<HashScanProcessor>(config));

        run_stages();

        const auto counts = expected_counts();
        std::vector<uint32_t> starts(64, 0U);
        std::exclusive_scan(counts.begin(), counts.end(), starts.begin(), 0U);
        EXPECT_EQ(read_state<uint32_t>("hash_cell_start"), starts);
        EXPECT_EQ(read_state<uint32_t>("hash_cell_cursor"), starts);
    }

    TEST_F(NetworkHashTest, ScatterListsExactlyTheParticlesOfEachCell)
    {
        add_hash_build();

        run_stages();

        const auto counts = read_state<uint32_t>("hash_cell_count");
        const auto starts = read_state<uint32_t>("hash_cell_start");
        const auto index = read_state<uint32_t>("hash_particle_index");

        std::vector<std::set<uint32_t>> expected(grid.cell_count());
        for (uint32_t i = 0; i < k_particles; ++i)
            expected[grid.cell_of(positions[i])].insert(i);

        for (uint32_t c = 0; c < grid.cell_count(); ++c) {
            const std::set<uint32_t> listed(index.begin() + starts[c], index.begin() + starts[c] + counts[c]);
            EXPECT_EQ(listed, expected[c]) << "cell " << c;
        }

        auto sorted = index;
        std::ranges::sort(sorted);
        std::vector<uint32_t> identity(k_particles);
        std::ranges::iota(identity, 0U);
        EXPECT_EQ(sorted, identity);
    }

    TEST_F(NetworkHashTest, RebuildingEveryCycleGivesTheSameHash)
    {
        add_hash_build();

        run_stages();
        const auto first_counts = read_state<uint32_t>("hash_cell_count");
        const auto first_starts = read_state<uint32_t>("hash_cell_start");
        run_stages();

        EXPECT_EQ(read_state<uint32_t>("hash_cell_count"), first_counts);
        EXPECT_EQ(read_state<uint32_t>("hash_cell_start"), first_starts);
    }

    TEST_F(NetworkHashTest, AliveGateKeepsDeadParticlesOutOfEveryCell)
    {
        std::vector<bool> alive(k_particles);
        for (uint32_t i = 0; i < k_particles; ++i)
            alive[i] = i % 3 != 0;
        declare_alive(alive);
        add_hash_build(true);

        run_stages();

        const auto counts = read_state<uint32_t>("hash_cell_count");
        EXPECT_EQ(counts, expected_counts(alive));

        const auto starts = read_state<uint32_t>("hash_cell_start");
        const auto index = read_state<uint32_t>("hash_particle_index");
        for (uint32_t c = 0; c < grid.cell_count(); ++c) {
            for (uint32_t k = 0; k < counts[c]; ++k) {
                const uint32_t particle = index[static_cast<size_t>(starts[c]) + k];
                EXPECT_TRUE(alive[particle]) << "dead particle " << particle << " in cell " << c;
                EXPECT_EQ(grid.cell_of(positions[particle]), c);
            }
        }
    }

    // Density colouring

    TEST_F(NetworkHashTest, DensityColorMatchesTheCpuNeighbourCount)
    {
        const auto op = std::make_shared<GpuFieldOperator>(
            Kakshya::VertexLayout::for_points(), SpatialFieldConfig { .density_color = true });
        add_hash_build();
        add(std::make_shared<HashDensityColorProcessor>(config, op));

        run_stages();

        const float saturation = op->get_field_config().density_saturation_count;
        const auto colors = read_colors();
        for (uint32_t i = 0; i < k_particles; ++i) {
            const float density = std::clamp(static_cast<float>(neighbours_of(i)) / saturation, 0.0F, 1.0F);
            const auto expected = heat_of(density);
            EXPECT_NEAR(colors[i].x, expected.x, 1e-4F) << "particle " << i;
            EXPECT_NEAR(colors[i].y, expected.y, 1e-4F) << "particle " << i;
            EXPECT_NEAR(colors[i].z, expected.z, 1e-4F) << "particle " << i;
        }
    }

    TEST_F(NetworkHashTest, DensityColorFollowsASaturationChangeWithoutRebuilding)
    {
        const auto op = std::make_shared<GpuFieldOperator>(
            Kakshya::VertexLayout::for_points(), SpatialFieldConfig { .density_color = true });
        add_hash_build();
        add(std::make_shared<HashDensityColorProcessor>(config, op));
        run_stages();
        const auto before = read_colors();

        op->set_density_saturation_count(96.0F);
        run_stages();

        const auto after = read_colors();
        EXPECT_NE(after, before);
        for (uint32_t i = 0; i < k_particles; ++i) {
            const auto expected = heat_of(std::clamp(static_cast<float>(neighbours_of(i)) / 96.0F, 0.0F, 1.0F));
            EXPECT_NEAR(after[i].x, expected.x, 1e-4F) << "particle " << i;
            EXPECT_NEAR(after[i].z, expected.z, 1e-4F) << "particle " << i;
        }
    }

} // namespace
} // namespace MayaFlux::Test
