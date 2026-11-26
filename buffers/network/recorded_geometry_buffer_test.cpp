#include "network_buffer_fixture.hpp"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Buffers/Network/RecordedGeometryBuffer.hpp"
#include "MayaFlux/Buffers/Network/SpatialHashProcessor.hpp"
#include "MayaFlux/Kakshya/NDData/VertexFormats.hpp"
#include "MayaFlux/Kinesis/Tendency/DualField.hpp"
#include "MayaFlux/Kinesis/Tendency/ShaderCompat.hpp"
#include "MayaFlux/Nodes/Network/Operators/GpuFieldOperator.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test {
namespace {

    using Buffers::RecordedGeometryBuffer;
    using Kakshya::SpatialAttribute;
    using Kakshya::SpatialData;
    using Kakshya::SpatialScope;
    using Kakshya::Vertex;
    using Kinesis::FieldTarget;
    using Kinesis::SamplerBounds;
    using Nodes::Network::GpuFieldOperator;
    using Nodes::Network::SpatialFieldConfig;
    using Portal::Graphics::PrimitiveTopology;

    namespace fields {

        using namespace MayaFlux::ShaderCompat;

        const auto recorded_geometry_buffer_shift_x = MF_FIELD(recorded_geometry_buffer_shift_x, [](vec3 p) -> vec3 {
            return p * 0.0f + vec3(0.25f, 0.0f, 0.0f);
        });

    } // namespace fields

    constexpr float k_cell_size = 0.5F;
    constexpr uint32_t k_capacity = 64;

    SamplerBounds unit_bounds() { return SamplerBounds { glm::vec3(-1.0F), glm::vec3(1.0F) }; }

    uint32_t cell_of(const glm::vec3& p)
    {
        glm::ivec3 c = glm::ivec3(glm::floor((p - glm::vec3(-1.0F)) / k_cell_size));
        c = glm::clamp(c, glm::ivec3(0), glm::ivec3(3));
        return static_cast<uint32_t>(c.x) + static_cast<uint32_t>(c.y) * 4U + static_cast<uint32_t>(c.z) * 16U;
    }

    std::shared_ptr<const SpatialData> scattered(uint32_t count, uint32_t seed = 11U)
    {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> spread(-0.99F, 0.99F);
        auto frame = std::make_shared<SpatialData>();
        frame->topology = PrimitiveTopology::POINT_LIST;
        for (uint32_t i = 0; i < count; ++i) {
            frame->positions.emplace_back(spread(rng), spread(rng), spread(rng));
        }
        return frame;
    }

    std::shared_ptr<const SpatialData> strips(const std::vector<int32_t>& counts)
    {
        auto frame = std::make_shared<SpatialData>();
        frame->topology = PrimitiveTopology::LINE_STRIP;
        frame->vertex_counts_per_curve = counts;
        const auto total = std::accumulate(counts.begin(), counts.end(), 0);
        for (int i = 0; i < total; ++i) {
            frame->positions.emplace_back(static_cast<float>(i) * 0.1F, 0.0F, 0.0F);
        }
        return frame;
    }

    std::vector<uint32_t> expected_counts(const SpatialData& frame, size_t kept)
    {
        std::vector<uint32_t> counts(64, 0U);
        for (size_t i = 0; i < std::min(kept, frame.positions.size()); ++i) {
            ++counts[cell_of(frame.positions[i])];
        }
        return counts;
    }

    std::shared_ptr<GpuFieldOperator> density_op()
    {
        return std::make_shared<GpuFieldOperator>(
            Kakshya::VertexLayout::for_points(),
            SpatialFieldConfig { .cell_size = k_cell_size, .density_color = true });
    }

    // What needs no device

    MAYAFLUX_TEST_WITH(TEST, RecordedGeometryBufferContractTest, ItIsAStateFieldBufferSizedFromItsCapacity, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        static_assert(std::is_base_of_v<Buffers::StateFieldBuffer, RecordedGeometryBuffer>);

        const RecordedGeometryBuffer buffer(10, PrimitiveTopology::LINE_STRIP, unit_bounds());

        EXPECT_EQ(buffer.capacity(), 10U);
        EXPECT_EQ(buffer.topology(), PrimitiveTopology::LINE_STRIP);
        EXPECT_EQ(buffer.get_size_bytes(), 10U * sizeof(Vertex));
    }

    MAYAFLUX_TEST_WITH(TEST, RecordedGeometryBufferContractTest, ZeroCapacityAndTriangleTopologiesAreRefused, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        EXPECT_THROW(RecordedGeometryBuffer(0, PrimitiveTopology::POINT_LIST, unit_bounds()), std::invalid_argument);
        EXPECT_THROW(RecordedGeometryBuffer(4, PrimitiveTopology::TRIANGLE_LIST, unit_bounds()), std::invalid_argument);
    }

    MAYAFLUX_TEST_WITH(TEST, RecordedGeometryBufferContractTest, TheNewestFrameWrittenIsTheCurrentOne, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        RecordedGeometryBuffer buffer(8, PrimitiveTopology::POINT_LIST, unit_bounds());
        EXPECT_EQ(buffer.current_frame(), nullptr);

        const auto first = scattered(3);
        const auto second = scattered(5);
        buffer.write_frame(first);
        buffer.write_frame(second);

        EXPECT_EQ(buffer.current_frame(), second);
    }

    // The device fixture

    class RecordedGeometryDeviceTest : public NetworkBufferDeviceTest {
    protected:
        void TearDown() override
        {
            if (recorded) {
                if (auto chain = recorded->get_processing_chain()) {
                    if (auto render = recorded->get_render_processor()) {
                        chain->remove_processor_anywhere(render, recorded);
                    }
                }
                recorded->set_default_processor(nullptr);
                if (recorded->is_initialized() && service) {
                    service->destroy_buffer(recorded);
                }
                recorded.reset();
            }
            NetworkBufferDeviceTest::TearDown();
        }

        void make(uint32_t capacity,
            PrimitiveTopology topology = PrimitiveTopology::POINT_LIST,
            std::shared_ptr<GpuFieldOperator> op = nullptr)
        {
            recorded = std::make_shared<RecordedGeometryBuffer>(capacity, topology, unit_bounds(), std::move(op));
            service->initialize_buffer(recorded);
            ASSERT_TRUE(recorded->is_initialized());
            recorded->setup_processors(Buffers::ProcessingToken::GRAPHICS_BACKEND);
        }

        void draw_into_window()
        {
            window = std::make_shared<ScriptedWindow>(320, 240);
            recorded->setup_rendering(Portal::Graphics::RenderConfig { .target_window = window });
        }

        void run()
        {
            recorded->process_default();
            recorded->get_processing_chain()->process_complete(recorded);
        }

        [[nodiscard]] std::vector<Vertex> read_records(size_t count)
        {
            std::vector<uint8_t> bytes(recorded->get_size_bytes());
            Buffers::download_from_gpu(recorded, bytes.data(), bytes.size());
            std::vector<Vertex> records(count);
            std::memcpy(records.data(), bytes.data(), count * sizeof(Vertex));
            return records;
        }

        template <typename T>
        [[nodiscard]] std::vector<T> read_field(const std::string& name)
        {
            std::vector<T> values(recorded->get_state_bytes(name) / sizeof(T));
            std::shared_ptr<Buffers::VKBuffer> staging;
            Buffers::download_back_buffer(
                recorded->read_state_slot(name), values.data(), values.size() * sizeof(T), staging);
            return values;
        }

        [[nodiscard]] static uint32_t total_of(const std::vector<uint32_t>& counts)
        {
            return std::accumulate(counts.begin(), counts.end(), 0U);
        }

        std::shared_ptr<RecordedGeometryBuffer> recorded;
    };

    // Writing frames

    TEST_F(RecordedGeometryDeviceTest, AFrameIsWrittenAsWholeVertexRecords)
    {
        make(8);
        auto frame = std::make_shared<SpatialData>(*scattered(3));
        frame->attributes.push_back(SpatialAttribute { .name = "color", .scope = SpatialScope::Constant, .values = Kakshya::DataVariant { std::vector<glm::vec4> { { 0.5F, 0.25F, 0.125F, 1.0F } } } });
        frame->attributes.push_back(SpatialAttribute { .name = "scalar", .scope = SpatialScope::Varying, .values = Kakshya::DataVariant { std::vector<float> { 1.0F, 2.0F, 3.0F } } });

        recorded->write_frame(frame);
        recorded->process_default();

        const auto records = read_records(3);
        for (size_t i = 0; i < 3; ++i) {
            EXPECT_EQ(records[i].position, frame->positions[i]) << "record " << i;
            EXPECT_EQ(records[i].color, glm::vec4(0.5F, 0.25F, 0.125F, 1.0F)) << "record " << i;
            EXPECT_EQ(records[i].scalar, static_cast<float>(i + 1)) << "record " << i;
            EXPECT_EQ(records[i].normal, glm::vec3(0.0F, 0.0F, 1.0F)) << "record " << i;
        }
    }

    TEST_F(RecordedGeometryDeviceTest, ANewFrameReplacesTheVertices)
    {
        make(8);
        recorded->write_frame(scattered(8, 1U));
        recorded->process_default();
        const auto second = scattered(5, 2U);

        recorded->write_frame(second);
        recorded->process_default();

        const auto records = read_records(5);
        for (size_t i = 0; i < 5; ++i) {
            EXPECT_EQ(records[i].position, second->positions[i]) << "record " << i;
        }
    }

    TEST_F(RecordedGeometryDeviceTest, AFrameBiggerThanTheCapacityKeepsItsFirstVertices)
    {
        make(8, PrimitiveTopology::POINT_LIST, density_op());
        const auto frame = scattered(12);

        recorded->write_frame(frame);
        run();

        const auto records = read_records(8);
        for (size_t i = 0; i < 8; ++i) {
            EXPECT_EQ(records[i].position, frame->positions[i]) << "record " << i;
        }
        const auto counts = read_field<uint32_t>("hash_cell_count");
        EXPECT_EQ(counts, expected_counts(*frame, 8));
        EXPECT_EQ(read_field<uint32_t>("mutation_alive"), std::vector<uint32_t>(8, 1U));
    }

    TEST_F(RecordedGeometryDeviceTest, TheClusterIdsOfAFrameAreWrittenWithIt)
    {
        make(8, PrimitiveTopology::POINT_LIST, density_op());
        auto frame = std::make_shared<SpatialData>(*scattered(4));
        frame->attributes.push_back(SpatialAttribute { .name = "cluster", .scope = SpatialScope::Varying, .values = Kakshya::DataVariant { std::vector<uint32_t> { 3U, 1U, 4U, 1U } } });

        recorded->write_frame(frame);
        recorded->process_default();

        const auto ids = read_field<uint32_t>("hash_cluster_id");
        ASSERT_EQ(ids.size(), 8U);
        EXPECT_EQ(std::vector<uint32_t>(ids.begin(), ids.begin() + 4), (std::vector<uint32_t> { 3U, 1U, 4U, 1U }));
    }

    // Frames of different sizes through the stages

    TEST_F(RecordedGeometryDeviceTest, WithNoFrameWrittenNothingIsLive)
    {
        make(k_capacity, PrimitiveTopology::POINT_LIST, density_op());

        run();

        EXPECT_EQ(read_field<uint32_t>("mutation_alive"), std::vector<uint32_t>(k_capacity, 0U));
        EXPECT_EQ(total_of(read_field<uint32_t>("hash_cell_count")), 0U);
    }

    TEST_F(RecordedGeometryDeviceTest, TheHashFollowsTheFrameAndASmallerFrameKillsTheTail)
    {
        make(k_capacity, PrimitiveTopology::POINT_LIST, density_op());
        const auto big = scattered(k_capacity, 1U);
        const auto small = scattered(16, 2U);

        recorded->write_frame(big);
        run();
        EXPECT_EQ(read_field<uint32_t>("hash_cell_count"), expected_counts(*big, k_capacity));

        recorded->write_frame(small);
        run();

        EXPECT_EQ(read_field<uint32_t>("hash_cell_count"), expected_counts(*small, 16));
        std::vector<uint32_t> alive(k_capacity, 0U);
        std::fill_n(alive.begin(), 16, 1U);
        EXPECT_EQ(read_field<uint32_t>("mutation_alive"), alive);
    }

    TEST_F(RecordedGeometryDeviceTest, AnEmptyFrameLeavesNothingLive)
    {
        make(k_capacity, PrimitiveTopology::POINT_LIST, density_op());
        recorded->write_frame(scattered(k_capacity));
        run();

        recorded->write_frame(std::make_shared<SpatialData>());
        run();

        EXPECT_EQ(total_of(read_field<uint32_t>("hash_cell_count")), 0U);
        EXPECT_EQ(read_field<uint32_t>("mutation_alive"), std::vector<uint32_t>(k_capacity, 0U));
    }

    TEST_F(RecordedGeometryDeviceTest, AFrameOfAnotherTopologyIsNotDrawn)
    {
        make(k_capacity, PrimitiveTopology::POINT_LIST, density_op());

        recorded->write_frame(strips({ 3, 2 }));
        run();

        EXPECT_EQ(total_of(read_field<uint32_t>("hash_cell_count")), 0U);
    }

    TEST_F(RecordedGeometryDeviceTest, ANullFrameShowsNothing)
    {
        make(k_capacity, PrimitiveTopology::POINT_LIST, density_op());
        recorded->write_frame(scattered(10));
        run();

        recorded->write_frame(nullptr);
        run();

        EXPECT_EQ(total_of(read_field<uint32_t>("hash_cell_count")), 0U);
    }

    TEST_F(RecordedGeometryDeviceTest, TheNewestFrameFromAnotherThreadWins)
    {
        make(k_capacity, PrimitiveTopology::POINT_LIST, density_op());

        std::thread supplier([this] {
            for (uint32_t n = 1; n <= 40; ++n) {
                recorded->write_frame(scattered(n, n));
            }
        });
        supplier.join();
        run();

        EXPECT_EQ(total_of(read_field<uint32_t>("hash_cell_count")), 40U);
    }

    // The field pass over a held frame

    TEST_F(RecordedGeometryDeviceTest, AHeldFrameIsRestoredBeforeEveryFieldPassSoItDoesNotAccumulate)
    {
        auto op = std::make_shared<GpuFieldOperator>(Kakshya::VertexLayout::for_points());
        op->bind(FieldTarget::POSITION, fields::recorded_geometry_buffer_shift_x);
        make(k_capacity, PrimitiveTopology::POINT_LIST, op);
        const auto frame = scattered(k_capacity);
        recorded->write_frame(frame);

        run();
        run();
        run();

        const auto records = read_records(k_capacity);
        for (size_t i = 0; i < k_capacity; ++i) {
            EXPECT_NEAR(records[i].position.x, frame->positions[i].x + 0.25F, 1e-5F) << "record " << i;
            EXPECT_EQ(records[i].position.y, frame->positions[i].y) << "record " << i;
        }
    }

    // Drawing

    TEST_F(RecordedGeometryDeviceTest, TheCurvesOfAStripAreDrawnAsSeparateSpans)
    {
        make(16, PrimitiveTopology::LINE_STRIP);
        draw_into_window();

        recorded->write_frame(strips({ 3, 2, 4 }));
        recorded->process_default();

        const auto& runs = recorded->get_render_processor()->get_runs();
        ASSERT_EQ(runs.size(), 3U);
        EXPECT_EQ(runs[0].topology, PrimitiveTopology::LINE_STRIP);
        EXPECT_EQ(runs[0].vertex_offset, 0U);
        EXPECT_EQ(runs[0].vertex_count, 3U);
        EXPECT_EQ(runs[1].vertex_offset, 3U);
        EXPECT_EQ(runs[1].vertex_count, 2U);
        EXPECT_EQ(runs[2].vertex_offset, 5U);
        EXPECT_EQ(runs[2].vertex_count, 4U);
    }

    TEST_F(RecordedGeometryDeviceTest, ACurveThatRunsPastTheCapacityIsCutAtIt)
    {
        make(6, PrimitiveTopology::LINE_STRIP);
        draw_into_window();

        recorded->write_frame(strips({ 3, 5, 2 }));
        recorded->process_default();

        const auto& runs = recorded->get_render_processor()->get_runs();
        ASSERT_EQ(runs.size(), 2U);
        EXPECT_EQ(runs[1].vertex_offset, 3U);
        EXPECT_EQ(runs[1].vertex_count, 3U);
    }

    TEST_F(RecordedGeometryDeviceTest, APointFrameIsOneSpan)
    {
        make(16);
        draw_into_window();

        recorded->write_frame(scattered(7));
        recorded->process_default();

        EXPECT_TRUE(recorded->get_render_processor()->get_runs().empty());
    }

    TEST_F(RecordedGeometryDeviceTest, AFrameLoadedBeforeTheBufferIsDrawnIsAppliedWhenItIs)
    {
        make(16, PrimitiveTopology::LINE_STRIP);
        recorded->write_frame(strips({ 2, 2 }));
        recorded->process_default();

        draw_into_window();

        EXPECT_EQ(recorded->get_render_processor()->get_runs().size(), 2U);
    }

    TEST_F(RecordedGeometryDeviceTest, AnEmptyFrameHidesTheBufferAndTheNextOneShowsItAgain)
    {
        make(16);
        draw_into_window();
        const auto render = recorded->get_render_processor();

        recorded->write_frame(scattered(5));
        recorded->process_default();
        EXPECT_TRUE(render->is_visible(recorded));

        recorded->write_frame(std::make_shared<SpatialData>());
        recorded->process_default();
        EXPECT_FALSE(render->is_visible(recorded));

        recorded->write_frame(scattered(3));
        recorded->process_default();
        EXPECT_TRUE(render->is_visible(recorded));
    }

    // Stages the buffer cannot wire

    TEST_F(RecordedGeometryDeviceTest, ALayoutThatIsNotTheVertexRecordWiresNothing)
    {
        auto layout = Kakshya::VertexLayout::for_points();
        layout.stride_bytes = 64;
        auto op = std::make_shared<GpuFieldOperator>(layout, SpatialFieldConfig { .cell_size = k_cell_size, .density_color = true });
        op->bind(FieldTarget::POSITION, fields::recorded_geometry_buffer_shift_x);

        make(k_capacity, PrimitiveTopology::POINT_LIST, op);

        EXPECT_TRUE(recorded->get_processing_chain()->get_processors(recorded).empty());
        EXPECT_FALSE(recorded->has_state("hash_cell_count"));
    }

    TEST_F(RecordedGeometryDeviceTest, ADensityConfigWithoutACellSizeWiresOnlyTheFieldPass)
    {
        auto op = std::make_shared<GpuFieldOperator>(
            Kakshya::VertexLayout::for_points(), SpatialFieldConfig { .density_color = true });
        op->bind(FieldTarget::POSITION, fields::recorded_geometry_buffer_shift_x);

        make(k_capacity, PrimitiveTopology::POINT_LIST, op);

        EXPECT_EQ(recorded->get_processing_chain()->get_processors(recorded).size(), 1U);
        EXPECT_FALSE(recorded->has_state("hash_cell_count"));
    }

} // namespace
} // namespace MayaFlux::Test
