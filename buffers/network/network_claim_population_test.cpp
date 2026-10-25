#include "network_hash_fixture.hpp"

#include "MayaFlux/Buffers/Network/MutationClaimProcessor.hpp"
#include "MayaFlux/Buffers/Network/PopulationProcessor.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::ClaimAccumulateProcessor;
    using Buffers::ClaimFlattenProcessor;
    using Buffers::ClaimInitProcessor;
    using Buffers::ClaimProcessor;
    using Buffers::ClaimSwallowProcessor;
    using Buffers::ClaimTransferProcessor;
    using Buffers::MutationConfig;
    using Buffers::PopulationConfig;
    using Buffers::PopulationInitProcessor;
    using Buffers::PopulationSpawnProcessor;
    using Nodes::Network::PhysicsOperator;

    constexpr float k_absorb = 0.12F;
    constexpr size_t k_size_word = 7;

    struct ClaimOracle {
        std::vector<uint32_t> raw;
        std::vector<uint32_t> root;
        uint32_t events {};
    };

    ClaimOracle expected_claims(
        const std::vector<glm::vec3>& points,
        const std::vector<float>& radius,
        const std::vector<bool>& alive = {},
        const std::vector<uint32_t>& cluster = {},
        bool cross_cluster = true)
    {
        const auto n = static_cast<uint32_t>(points.size());
        ClaimOracle result;
        result.raw.resize(n);
        std::ranges::iota(result.raw, 0U);

        for (uint32_t i = 0; i < n; ++i) {
            if (!alive.empty() && !alive[i])
                continue;
            for (uint32_t j = i + 1; j < n; ++j) {
                if (!alive.empty() && !alive[j])
                    continue;
                if (!cross_cluster && !cluster.empty() && cluster[i] != cluster[j])
                    continue;
                if (glm::length(points[j] - points[i]) < radius[i]) {
                    result.raw[j] = std::min(result.raw[j], i);
                    ++result.events;
                }
            }
        }

        result.root.resize(n);
        for (uint32_t j = 0; j < n; ++j) {
            uint32_t r = j;
            while (result.raw[r] != r)
                r = result.raw[r];
            result.root[j] = r;
        }
        return result;
    }

    std::vector<float> uniform_radius(size_t count, float radius)
    {
        return std::vector<float>(count, radius);
    }

    std::vector<uint32_t> members_per_root(const ClaimOracle& oracle, const std::vector<uint32_t>& cluster = {})
    {
        std::vector<uint32_t> counts(oracle.root.size(), 0U);
        for (uint32_t i = 0; i < oracle.root.size(); ++i) {
            const uint32_t root = oracle.root[i];
            if (root == i)
                continue;
            if (!cluster.empty() && cluster[root] != cluster[i])
                continue;
            ++counts[root];
        }
        return counts;
    }

    class NetworkClaimTest : public NetworkHashTest {
    protected:
        void prepare(SpatialFieldConfig field_config = {}, float absorb = k_absorb)
        {
            op = std::make_shared<GpuFieldOperator>(Kakshya::VertexLayout::for_points(), field_config);
            claim = MutationConfig { .hash = config, .absorb_radius = absorb };
            claim.declare_fields(buffer);
            write_state<float>("mutation_accreted_mass", std::vector<float>(k_particles, 0.0F));
        }

        void add_claim(bool flatten = true)
        {
            add_hash_build();
            add(std::make_shared<ClaimInitProcessor>(claim));
            add(std::make_shared<ClaimProcessor>(claim, op, false));
            if (flatten)
                add(std::make_shared<ClaimFlattenProcessor>(claim));
        }

        void assign_alternating_clusters()
        {
            clusters.resize(k_particles);
            for (uint32_t i = 0; i < k_particles; ++i)
                clusters[i] = i % 2;
            write_state<uint32_t>("hash_cluster_id", clusters);
        }

        void lay_chain(uint32_t length)
        {
            for (uint32_t i = 0; i < length; ++i)
                poke_position(i, { -0.95F + 0.1F * static_cast<float>(i), 0.0F, 0.0F });
        }

        [[nodiscard]] PhysicsOperator* physics() const
        {
            return dynamic_cast<PhysicsOperator*>(network->get_operator());
        }

        MutationConfig claim {};
        std::shared_ptr<GpuFieldOperator> op;
        std::vector<uint32_t> clusters;
    };

    // Claim protocol

    TEST_F(NetworkClaimTest, InitResetsOwnershipAndCounters)
    {
        prepare();
        write_state<uint32_t>("mutation_claimed_by", std::vector<uint32_t>(k_particles, 9U));
        write_state<uint32_t>("mutation_swallow_count", std::vector<uint32_t>(k_particles, 9U));
        write_state<uint32_t>("mutation_claim_events", { 9U });
        add(std::make_shared<ClaimInitProcessor>(claim));

        run_stages();

        std::vector<uint32_t> identity(k_particles);
        std::ranges::iota(identity, 0U);
        EXPECT_EQ(read_state<uint32_t>("mutation_claimed_by"), identity);
        EXPECT_EQ(read_state<uint32_t>("mutation_swallow_count"), std::vector<uint32_t>(k_particles, 0U));
        EXPECT_EQ(read_state<uint32_t>("mutation_claim_events"), std::vector<uint32_t> { 0U });
    }

    TEST_F(NetworkClaimTest, TheLowestIndexInRangeClaimsAParticle)
    {
        prepare();
        add_claim(false);

        run_stages();

        const auto expected = expected_claims(positions, uniform_radius(k_particles, k_absorb));
        ASSERT_GT(expected.events, 0U);
        EXPECT_EQ(read_state<uint32_t>("mutation_claimed_by"), expected.raw);
        EXPECT_EQ(read_state<uint32_t>("mutation_claim_events")[0], expected.events);
    }

    TEST_F(NetworkClaimTest, ClaimsStayInsideTheirClusterByDefault)
    {
        prepare();
        assign_alternating_clusters();
        add_claim(false);

        run_stages();

        const auto expected = expected_claims(
            positions, uniform_radius(k_particles, k_absorb), {}, clusters, false);
        EXPECT_EQ(read_state<uint32_t>("mutation_claimed_by"), expected.raw);
        const auto owners = read_state<uint32_t>("mutation_claimed_by");
        for (uint32_t i = 0; i < k_particles; ++i)
            EXPECT_EQ(clusters[owners[i]], clusters[i]);
    }

    TEST_F(NetworkClaimTest, AccreteMassWidensTheCaptureRadiusOfItsOwner)
    {
        prepare(SpatialFieldConfig { .capture_growth = 0.5F });
        std::vector<float> mass(k_particles, 0.0F);
        mass[0] = 8.0F;
        write_state<float>("mutation_accreted_mass", mass);
        add_claim(false);

        run_stages();

        std::vector<float> radius = uniform_radius(k_particles, k_absorb);
        radius[0] = std::max(k_absorb, std::cbrt(8.0F) * 0.5F);
        const auto expected = expected_claims(positions, radius);
        EXPECT_EQ(read_state<uint32_t>("mutation_claimed_by"), expected.raw);
        EXPECT_EQ(read_state<uint32_t>("mutation_claim_events")[0], expected.events);
    }

    TEST_F(NetworkClaimTest, FlattenResolvesEveryChainToItsRoot)
    {
        prepare();
        lay_chain(20);
        add_claim();

        run_stages();

        const auto expected = expected_claims(positions, uniform_radius(k_particles, k_absorb));
        const auto owners = read_state<uint32_t>("mutation_claimed_by");
        EXPECT_EQ(owners, expected.root);
        for (uint32_t i = 0; i < 20; ++i)
            EXPECT_EQ(owners[i], 0U) << "chain member " << i;
    }

    // Accumulate, swallow, transfer

    TEST_F(NetworkClaimTest, AccumulateCountsSwallowedMembersAtTheirRoot)
    {
        prepare();
        lay_chain(20);
        add_claim();
        add(std::make_shared<ClaimAccumulateProcessor>(claim, nullptr, 0U, op, false));

        run_stages();

        const auto expected = expected_claims(positions, uniform_radius(k_particles, k_absorb));
        EXPECT_EQ(read_state<uint32_t>("mutation_swallow_count"), members_per_root(expected));
        EXPECT_GE(read_state<uint32_t>("mutation_swallow_count")[0], 19U);
    }

    TEST_F(NetworkClaimTest, SwallowSizesRootsAndFoldsMembersOntoThem)
    {
        prepare();
        lay_chain(20);
        add_claim();
        add(std::make_shared<ClaimAccumulateProcessor>(claim, nullptr, 0U, op, false));
        add(std::make_shared<ClaimSwallowProcessor>(claim, op, false));

        run_stages();

        const auto expected = expected_claims(positions, uniform_radius(k_particles, k_absorb));
        const auto counts = members_per_root(expected);
        const auto floats = floats_of(read_vertices(static_cast<size_t>(k_particles) * stride_words * 4));
        const auto field = [&](uint32_t i, size_t word) {
            return floats[static_cast<size_t>(i) * stride_words + word];
        };

        for (uint32_t i = 0; i < k_particles; ++i) {
            const uint32_t root = expected.root[i];
            const float size = std::clamp(10.0F + static_cast<float>(counts[root]) * 0.6F, 10.0F, 70.0F);
            const glm::vec3 heat = heat_of((size - 10.0F) / 60.0F);
            if (root == i) {
                EXPECT_NEAR(field(i, k_size_word), size, 1e-4F) << "root " << i;
                EXPECT_NEAR(field(i, k_color_word), heat.x, 1e-4F) << "root " << i;
                EXPECT_NEAR(field(i, k_color_word + 2), heat.z, 1e-4F) << "root " << i;
                continue;
            }
            EXPECT_EQ(field(i, 0), positions[root].x) << "member " << i;
            EXPECT_EQ(field(i, 1), positions[root].y) << "member " << i;
            EXPECT_EQ(field(i, 2), positions[root].z) << "member " << i;
            EXPECT_NEAR(field(i, k_color_word), heat.x * 0.08F, 1e-4F) << "member " << i;
        }
    }

    TEST_F(NetworkClaimTest, TransferAdoptsTheRootsClusterAndAccumulateIgnoresCrossClusterMembers)
    {
        prepare(SpatialFieldConfig { .cross_cluster = true, .transfer_on_claim = true });
        assign_alternating_clusters();
        add_claim();
        add(std::make_shared<ClaimAccumulateProcessor>(claim, nullptr, 0U, op, true));
        add(std::make_shared<ClaimTransferProcessor>(claim));

        run_stages();

        const auto expected = expected_claims(positions, uniform_radius(k_particles, k_absorb));
        EXPECT_EQ(read_state<uint32_t>("mutation_swallow_count"), members_per_root(expected, clusters));

        const auto adopted = read_state<uint32_t>("hash_cluster_id");
        bool crossed = false;
        for (uint32_t i = 0; i < k_particles; ++i) {
            EXPECT_EQ(adopted[i], clusters[expected.root[i]]) << "particle " << i;
            crossed |= clusters[expected.root[i]] != clusters[i];
        }
        EXPECT_TRUE(crossed);
    }

    // CPU bond synchronisation

    TEST_F(NetworkClaimTest, ClaimsAdoptBondsAndPublishTheAccretedMass)
    {
        prepare();
        auto* physics_op = physics();
        ASSERT_NE(physics_op, nullptr);
        add_claim();
        add(std::make_shared<ClaimAccumulateProcessor>(claim, physics_op, 0U, op, false));

        run_stages();

        EXPECT_GT(physics_op->bond_count(), 0U);
        const auto mass = physics_op->get_accreted_mass_span();
        ASSERT_EQ(mass.size(), k_particles);
        EXPECT_EQ(read_state<float>("mutation_accreted_mass"), std::vector<float>(mass.begin(), mass.end()));
    }

    TEST_F(NetworkClaimTest, NoClaimsClearTheBonds)
    {
        prepare(SpatialFieldConfig {}, 1e-5F);
        auto* physics_op = physics();
        ASSERT_NE(physics_op, nullptr);
        std::vector<uint32_t> roots(k_particles);
        std::ranges::iota(roots, 0U);
        std::ranges::fill(std::span(roots).first(10), 0U);
        physics_op->sync_bonds_from_claims(roots);
        ASSERT_GT(physics_op->bond_count(), 0U);
        add_claim();
        add(std::make_shared<ClaimAccumulateProcessor>(claim, physics_op, 0U, op, false));

        run_stages();

        EXPECT_EQ(read_state<uint32_t>("mutation_claim_events")[0], 0U);
        EXPECT_EQ(physics_op->bond_count(), 0U);
    }

    // Population dynamics

    constexpr uint32_t k_live = 200;
    constexpr uint32_t k_total = 300;

    class NetworkPopulationTest : public NetworkBufferDeviceTest {
    protected:
        void SetUp() override
        {
            NetworkBufferDeviceTest::SetUp();
            if (IsSkipped())
                return;

            auto particles = std::make_shared<Nodes::Network::ParticleNetwork>(
                k_live, glm::vec3(-1.0F), glm::vec3(1.0F),
                Kinesis::SpatialDistribution::RANDOM_VOLUME);
            particles->create_operator<PhysicsOperator>();
            attach(particles, 1.0F);

            const auto made = SpatialHashConfig::from_network(buffer, k_cell);
            ASSERT_TRUE(made.has_value());
            config = *made;
            stride_words = config.stride_words;
            config.particle_count = k_total;
            buffer->resize(static_cast<size_t>(k_total) * stride_words * 4, true);

            config.declare_fields(buffer);
            write_state<uint32_t>("hash_cluster_id", std::vector<uint32_t>(k_total, 0U));
            population = PopulationConfig { .hash = config, .live_count = k_live };
            population.declare_fields(buffer);

            buffer->process_default();
            live_bytes = read_vertices(static_cast<size_t>(k_live) * stride_words * 4);
            poison_reserve();
        }

        void poison_reserve()
        {
            const size_t offset = static_cast<size_t>(k_live) * stride_words * 4;
            const std::vector<uint8_t> bytes(static_cast<size_t>(k_total - k_live) * stride_words * 4, 0xAB);
            auto binding = buffer->get_processor()->get_binding(buffer->get_binding_name());
            Buffers::upload_to_gpu(bytes.data(), bytes.size(), buffer, binding->staging_buffer, offset);
        }

        void add(const std::shared_ptr<Buffers::BufferProcessor>& processor)
        {
            ASSERT_TRUE(buffer->get_processing_chain()->add_processor(processor, buffer));
        }

        void run_stages()
        {
            buffer->get_processing_chain()->process_complete(buffer);
        }

        void add_gated_hash_build()
        {
            add(std::make_shared<Buffers::HashClearProcessor>(config));
            add(std::make_shared<Buffers::HashCountProcessor>(config, true));
            add(std::make_shared<Buffers::HashScanProcessor>(config));
            add(std::make_shared<Buffers::HashScatterProcessor>(config, true));
        }

        [[nodiscard]] std::vector<glm::vec3> live_positions() const
        {
            const auto floats = floats_of(live_bytes);
            std::vector<glm::vec3> out(k_live);
            for (uint32_t i = 0; i < k_live; ++i) {
                const size_t base = static_cast<size_t>(i) * stride_words;
                out[i] = { floats[base], floats[base + 1], floats[base + 2] };
            }
            return out;
        }

        [[nodiscard]] std::vector<uint32_t> live_neighbour_counts() const
        {
            const auto points = live_positions();
            std::vector<uint32_t> counts(k_live, 0U);
            for (uint32_t i = 0; i < k_live; ++i) {
                for (uint32_t j = 0; j < k_live; ++j) {
                    if (j != i && glm::length(points[j] - points[i]) < k_cell)
                        ++counts[i];
                }
            }
            return counts;
        }

        [[nodiscard]] std::vector<uint8_t> record(const std::vector<uint8_t>& bytes, uint32_t index) const
        {
            const size_t width = static_cast<size_t>(stride_words) * 4;
            return { bytes.begin() + static_cast<std::ptrdiff_t>(index * width),
                bytes.begin() + static_cast<std::ptrdiff_t>((index + 1) * width) };
        }

        [[nodiscard]] std::vector<uint8_t> all_records()
        {
            return read_vertices(static_cast<size_t>(k_total) * stride_words * 4);
        }

        SpatialHashConfig config {};
        PopulationConfig population {};
        uint32_t stride_words {};
        std::vector<uint8_t> live_bytes;
    };

    TEST_F(NetworkPopulationTest, InitMarksLiveSlotsClearsTheReserveAndSeedsTheCursor)
    {
        add(std::make_shared<PopulationInitProcessor>(population));

        run_stages();

        const auto alive = read_state<uint32_t>("mutation_alive");
        for (uint32_t i = 0; i < k_total; ++i)
            EXPECT_EQ(alive[i], i < k_live ? 1U : 0U) << "slot " << i;
        EXPECT_EQ(read_state<uint32_t>("mutation_spawn_cursor"), std::vector<uint32_t> { k_live });

        const auto bytes = all_records();
        const size_t live_size = live_bytes.size();
        EXPECT_EQ(std::vector<uint8_t>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(live_size)), live_bytes);
        EXPECT_TRUE(std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(live_size), bytes.end(),
            [](uint8_t b) { return b == 0; }));
    }

    TEST_F(NetworkPopulationTest, InitRunsOnlyOnce)
    {
        add(std::make_shared<PopulationInitProcessor>(population));
        run_stages();
        write_state<uint32_t>("mutation_alive", std::vector<uint32_t>(k_total, 7U));
        write_state<uint32_t>("mutation_spawn_cursor", { 55U });

        run_stages();

        EXPECT_EQ(read_state<uint32_t>("mutation_alive"), std::vector<uint32_t>(k_total, 7U));
        EXPECT_EQ(read_state<uint32_t>("mutation_spawn_cursor"), std::vector<uint32_t> { 55U });
    }

    TEST_F(NetworkPopulationTest, SpawnCopiesDenseParentsIntoDistinctReserveSlots)
    {
        const auto neighbours = live_neighbour_counts();
        auto ranked = neighbours;
        std::ranges::sort(ranked, std::greater<> {});
        const auto threshold = static_cast<float>(ranked[39]);
        const auto spawners = static_cast<uint32_t>(std::ranges::count_if(
            neighbours, [&](uint32_t n) { return static_cast<float>(n) >= threshold; }));
        ASSERT_GT(spawners, 0U);
        ASSERT_LT(spawners, k_total - k_live);

        const auto op = std::make_shared<GpuFieldOperator>(
            Kakshya::VertexLayout::for_points(), SpatialFieldConfig { .spawn_density_threshold = threshold });
        add(std::make_shared<PopulationInitProcessor>(population));
        add_gated_hash_build();
        add(std::make_shared<PopulationSpawnProcessor>(population, op));

        run_stages();

        const uint32_t cursor = read_state<uint32_t>("mutation_spawn_cursor")[0];
        ASSERT_GE(cursor, k_live + spawners);
        const uint32_t filled = std::min(cursor, k_total);
        const auto alive = read_state<uint32_t>("mutation_alive");
        for (uint32_t i = 0; i < k_total; ++i)
            EXPECT_EQ(alive[i], i < filled ? 1U : 0U) << "slot " << i;

        const auto bytes = all_records();
        std::set<std::vector<uint8_t>> spawner_records;
        for (uint32_t i = 0; i < k_live; ++i) {
            EXPECT_EQ(record(bytes, i), record(live_bytes, i)) << "parent " << i;
            if (static_cast<float>(neighbours[i]) >= threshold)
                spawner_records.insert(record(live_bytes, i));
        }

        std::set<std::vector<uint8_t>> spawned_records;
        for (uint32_t slot = k_live; slot < filled; ++slot) {
            spawned_records.insert(record(bytes, slot));
            EXPECT_TRUE(spawner_records.contains(record(bytes, slot))) << "slot " << slot;
        }
        EXPECT_EQ(spawned_records, spawner_records);
    }

    TEST_F(NetworkPopulationTest, SpawnStopsAtTheReserveCapacity)
    {
        const auto neighbours = live_neighbour_counts();
        const auto spawners = static_cast<uint32_t>(std::ranges::count_if(
            neighbours, [](uint32_t n) { return n >= 1U; }));
        ASSERT_GT(spawners, k_total - k_live);

        const auto op = std::make_shared<GpuFieldOperator>(
            Kakshya::VertexLayout::for_points(), SpatialFieldConfig { .spawn_density_threshold = 1.0F });
        add(std::make_shared<PopulationInitProcessor>(population));
        add_gated_hash_build();
        add(std::make_shared<PopulationSpawnProcessor>(population, op));

        run_stages();

        const auto alive = read_state<uint32_t>("mutation_alive");
        EXPECT_EQ(alive, std::vector<uint32_t>(k_total, 1U));
        EXPECT_GE(read_state<uint32_t>("mutation_spawn_cursor")[0], k_live + spawners);
    }

    TEST_F(NetworkPopulationTest, GatedAccumulateDeactivatesSwallowedMembersAndZeroesTheirSize)
    {
        const auto points = live_positions();
        const auto op = std::make_shared<GpuFieldOperator>(Kakshya::VertexLayout::for_points());
        const MutationConfig claim { .hash = config, .absorb_radius = k_absorb };
        claim.declare_fields(buffer);
        write_state<float>("mutation_accreted_mass", std::vector<float>(k_total, 0.0F));

        add(std::make_shared<PopulationInitProcessor>(population));
        add_gated_hash_build();
        add(std::make_shared<ClaimInitProcessor>(claim));
        add(std::make_shared<ClaimProcessor>(claim, op, true));
        add(std::make_shared<ClaimFlattenProcessor>(claim));
        add(std::make_shared<ClaimAccumulateProcessor>(claim, nullptr, k_live, op, false));

        run_stages();

        std::vector<bool> alive(k_total, false);
        std::fill_n(alive.begin(), k_live, true);
        auto oracle_points = points;
        oracle_points.resize(k_total, glm::vec3(0.0F));
        const auto expected = expected_claims(oracle_points, uniform_radius(k_total, k_absorb), alive);
        ASSERT_GT(expected.events, 0U);

        const auto flags = read_state<uint32_t>("mutation_alive");
        const auto floats = floats_of(all_records());
        for (uint32_t i = 0; i < k_total; ++i) {
            const bool swallowed = i < k_live && expected.root[i] != i;
            const bool live_root = i < k_live && !swallowed;
            EXPECT_EQ(flags[i], live_root ? 1U : 0U) << "slot " << i;
            if (swallowed)
                EXPECT_EQ(floats[static_cast<size_t>(i) * stride_words + k_size_word], 0.0F) << "slot " << i;
        }
    }

} // namespace
} // namespace MayaFlux::Test
