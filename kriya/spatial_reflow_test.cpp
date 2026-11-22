#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../buffers/network/network_buffer_fixture.hpp"
#include "../support/io_roundtrip_support.hpp"
#include "../support/scoped_working_dir.hpp"

#include "MayaFlux/Buffers/Network/RecordedGeometryBuffer.hpp"
#include "MayaFlux/IO/Spatial/SpatialSequence.hpp"
#include "MayaFlux/Kakshya/NDData/VertexFormats.hpp"
#include "MayaFlux/Kriya/SpatialReflow.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan, ::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {
namespace {

    using Buffers::RecordedGeometryBuffer;
    using Kakshya::SpatialData;
    using Kakshya::Vertex;
    using Kriya::FrameContext;
    using Kriya::SpatialReflowBuilder;
    using Portal::Graphics::PrimitiveTopology;
    using RoundTrip::path_in_cwd;

    std::vector<glm::vec3> points(size_t count, float offset = 0.0F)
    {
        std::vector<glm::vec3> out;
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const auto f = static_cast<float>(i);
            out.emplace_back(f * 0.1F + offset, 0.05F * f, -0.1F * f);
        }
        return out;
    }

    std::shared_ptr<const SpatialData> frame_of(size_t count, float offset = 0.0F)
    {
        auto frame = std::make_shared<SpatialData>();
        frame->topology = PrimitiveTopology::POINT_LIST;
        frame->positions = points(count, offset);
        return frame;
    }

    bool write_clouds(const std::string& path, const std::vector<std::vector<glm::vec3>>& frames)
    {
        IO::SpatialCache cache;
        if (!cache.open(path)) {
            return false;
        }
        for (const auto& positions : frames) {
            const IO::SpatialSample sample { .topology = PrimitiveTopology::POINT_LIST, .positions = positions };
            if (!cache.write("swarm", sample)) {
                return false;
            }
        }
        cache.close();
        return true;
    }

    class SpatialReflowDeviceTest : public NetworkBufferDeviceTest {
    protected:
        void TearDown() override
        {
            if (recorded) {
                recorded->set_default_processor(nullptr);
                if (recorded->is_initialized() && service) {
                    service->destroy_buffer(recorded);
                }
                recorded.reset();
            }
            NetworkBufferDeviceTest::TearDown();
        }

        void make(uint32_t capacity)
        {
            recorded = std::make_shared<RecordedGeometryBuffer>(
                capacity, PrimitiveTopology::POINT_LIST, Kinesis::SamplerBounds {});
            service->initialize_buffer(recorded);
            ASSERT_TRUE(recorded->is_initialized());
            recorded->setup_processors(Buffers::ProcessingToken::GRAPHICS_BACKEND);
        }

        [[nodiscard]] std::vector<Vertex> read_records(size_t count)
        {
            std::vector<uint8_t> bytes(recorded->get_size_bytes());
            Buffers::download_from_gpu(recorded, bytes.data(), bytes.size());
            std::vector<Vertex> records(count);
            std::memcpy(records.data(), bytes.data(), count * sizeof(Vertex));
            return records;
        }

        void expect_positions(const std::vector<glm::vec3>& expected)
        {
            const auto records = read_records(expected.size());
            for (size_t i = 0; i < expected.size(); ++i) {
                EXPECT_EQ(records[i].position, expected[i]) << "record " << i;
            }
        }

        ScopedWorkingDir m_files;
        std::shared_ptr<RecordedGeometryBuffer> recorded;
    };

    TEST_F(SpatialReflowDeviceTest, ASinkShowsEachFrameItIsGivenInTheBuffer)
    {
        make(8);
        const auto sink = Kriya::spatial_sink(recorded);

        sink(frame_of(3), FrameContext {});
        recorded->process_default();
        expect_positions(points(3));

        sink(frame_of(5, 1.0F), FrameContext {});
        recorded->process_default();
        expect_positions(points(5, 1.0F));
    }

    TEST_F(SpatialReflowDeviceTest, ANullFrameLeavesTheBufferAsItWas)
    {
        make(8);
        const auto sink = Kriya::spatial_sink(recorded);
        const auto shown = frame_of(4);
        sink(shown, FrameContext {});

        sink(nullptr, FrameContext {});

        EXPECT_EQ(recorded->current_frame(), shown);
    }

    TEST(SpatialSinkTest, ASinkOutlivingItsBufferWritesNothingAndDoesNotKeepItAlive)
    {
        std::weak_ptr<RecordedGeometryBuffer> weak;
        Kriya::SpatialReflow::Sink sink;
        {
            auto buffer = std::make_shared<RecordedGeometryBuffer>(
                4, PrimitiveTopology::POINT_LIST, Kinesis::SamplerBounds {});
            weak = buffer;
            sink = Kriya::spatial_sink(buffer);
        }

        EXPECT_TRUE(weak.expired());
        EXPECT_NO_THROW(sink(frame_of(2), FrameContext {}));
    }

    TEST_F(SpatialReflowDeviceTest, AReflowPlaysAnArchiveStreamIntoTheBuffer)
    {
        const auto path = path_in_cwd("swarm.abc");
        ASSERT_TRUE(write_clouds(path, { points(4), points(5, 1.0F), points(6, 2.0F) }));
        const auto frames = IO::SpatialFileFrames::create(path);
        ASSERT_NE(frames, nullptr);
        const auto capacity = frames->max_vertex_count();
        ASSERT_EQ(capacity, std::optional<uint32_t> { 6 });

        make(*capacity);
        auto reflow = SpatialReflowBuilder(frames, Kriya::spatial_sink(recorded), 1.0).build();
        ASSERT_NE(reflow, nullptr);

        reflow->advance();
        recorded->process_default();
        expect_positions(points(4));

        reflow->advance();
        reflow->advance();
        recorded->process_default();
        expect_positions(points(6, 2.0F));
        EXPECT_EQ(reflow->writes(), 3U);
    }

} // namespace
} // namespace MayaFlux::Test
