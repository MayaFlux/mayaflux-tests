#include "gtest/gtest.h"


#include "../support/scripted_window.hpp"
#include "../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/Forma/FormaBuffer.hpp"
#include "MayaFlux/Buffers/Forma/FormaProcessor.hpp"
#include "MayaFlux/Buffers/Shaders/RenderProcessor.hpp"
#include "MayaFlux/Buffers/Staging/StagingUtils.hpp"
#include "MayaFlux/Kakshya/NDData/VertexLayout.hpp"
#include "MayaFlux/Portal/Graphics/ShaderFoundry.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::FormaBuffer;
    using Buffers::ProcessingToken;
    using Portal::Graphics::DrawRun;
    using Portal::Graphics::PrimitiveTopology;

    const size_t k_stride = Kakshya::VertexLayout::for_meshes().stride_bytes;

    std::vector<uint8_t> vertices(size_t count, uint8_t fill)
    {
        return std::vector<uint8_t>(count * k_stride, fill);
    }

    std::vector<uint8_t> concat(std::initializer_list<std::vector<uint8_t>> parts)
    {
        std::vector<uint8_t> out;
        for (const auto& part : parts) {
            out.insert(out.end(), part.begin(), part.end());
        }
        return out;
    }

    ::testing::AssertionResult is_run(const DrawRun& run, PrimitiveTopology topology, uint32_t offset, uint32_t count)
    {
        if (run.topology == topology && run.vertex_offset == offset && run.vertex_count == count) {
            return ::testing::AssertionSuccess();
        }
        return ::testing::AssertionFailure()
            << "run {offset " << run.vertex_offset << ", count " << run.vertex_count << "} expected {offset "
            << offset << ", count " << count << "}";
    }

    class FormaSliceDeviceTest : public VulkanDeviceTest {
    protected:
        static void SetUpTestSuite()
        {
            VulkanDeviceTest::SetUpTestSuite();
            if (!vulkan_unavailable_reason().empty())
                return;

            auto borrowed = std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) {});
            Portal::Graphics::get_shader_foundry().shutdown();
            s_foundry_ready = Portal::Graphics::get_shader_foundry().initialize(borrowed);
        }

        static void TearDownTestSuite()
        {
            if (s_foundry_ready)
                Portal::Graphics::get_shader_foundry().shutdown();
            s_foundry_ready = false;
            VulkanDeviceTest::TearDownTestSuite();
        }

        void SetUp() override
        {
            VulkanDeviceTest::SetUp();
            if (IsSkipped())
                return;
            ASSERT_TRUE(s_foundry_ready);
        }

        void TearDown() override
        {
            if (buffer) {
                if (auto render = buffer->get_render_processor())
                    buffer->get_processing_chain()->remove_processor_anywhere(render, buffer);
                buffer->set_default_processor(nullptr);
                if (buffer->is_initialized()) {
                    auto* service = Registry::BackendRegistry::instance()
                                        .get_service<Registry::Service::BufferService>();
                    if (service)
                        service->destroy_buffer(buffer);
                }
            }
            buffer.reset();
            window.reset();
        }

        bool make_buffer(size_t capacity, PrimitiveTopology topology)
        {
            buffer = std::make_shared<FormaBuffer>(capacity, topology);
            auto* service = Registry::BackendRegistry::instance()
                                .get_service<Registry::Service::BufferService>();
            if (!service)
                return false;
            service->initialize_buffer(buffer);
            return buffer->is_initialized();
        }

        bool attach()
        {
            if (!buffer)
                return false;
            buffer->setup_processors(ProcessingToken::GRAPHICS_BACKEND);
            return buffer->get_default_processor() != nullptr;
        }

        void attach_rendering()
        {
            window = std::make_shared<ScriptedWindow>(128, 128);
            buffer->setup_rendering({ .target_window = window });
        }

        bool ready(PrimitiveTopology topology = PrimitiveTopology::TRIANGLE_LIST, size_t vertex_capacity = 16)
        {
            return make_buffer(k_stride * vertex_capacity, topology) && attach();
        }

        [[nodiscard]] uint32_t vertex_count() const
        {
            const auto layout = buffer->get_vertex_layout();
            return layout ? layout->vertex_count : 0;
        }

        [[nodiscard]] std::vector<uint8_t> read_bytes(size_t count) const
        {
            std::vector<uint8_t> bytes(buffer->get_size_bytes());
            Buffers::download_from_gpu(buffer, bytes.data(), bytes.size());
            bytes.resize(count);
            return bytes;
        }

        [[nodiscard]] const std::vector<DrawRun>& runs() const
        {
            return buffer->get_render_processor()->get_runs();
        }

        std::shared_ptr<FormaBuffer> buffer;
        std::shared_ptr<ScriptedWindow> window;

    private:
        static inline bool s_foundry_ready {};
    };

    TEST_F(FormaSliceDeviceTest, SlicesAddedBeforeTheProcessorExistAreRefusedHarmlessly)
    {
        ASSERT_TRUE(make_buffer(k_stride * 4, PrimitiveTopology::TRIANGLE_LIST));

        EXPECT_EQ(buffer->add_slice(vertices(1, 1)), 0U);
        EXPECT_NO_THROW(buffer->update_slice(1, vertices(1, 2)));
        EXPECT_NO_THROW(buffer->remove_slice(1));
        EXPECT_NO_THROW(buffer->set_slice_visible(1, false));
        EXPECT_NO_THROW(buffer->bring_slice_to_front(1));
        EXPECT_NO_THROW(buffer->send_slice_to_back(1));
    }

    TEST_F(FormaSliceDeviceTest, AddedSlicesUploadContiguouslyAndDrawAsOneFullRun)
    {
        ASSERT_TRUE(ready());
        attach_rendering();

        const auto a = buffer->add_slice(vertices(2, 1));
        const auto b = buffer->add_slice(vertices(3, 2));
        buffer->process_default();

        EXPECT_NE(a, 0U);
        EXPECT_NE(b, 0U);
        EXPECT_NE(a, b);
        EXPECT_EQ(vertex_count(), 5U);
        EXPECT_EQ(read_bytes(5 * k_stride), concat({ vertices(2, 1), vertices(3, 2) }));
        EXPECT_TRUE(runs().empty());
    }

    TEST_F(FormaSliceDeviceTest, SubmitWritesTheDefaultSliceBesideAddedOnes)
    {
        ASSERT_TRUE(ready());
        attach_rendering();

        buffer->submit(vertices(2, 1));
        buffer->add_slice(vertices(1, 2));
        buffer->process_default();

        EXPECT_EQ(vertex_count(), 3U);
        EXPECT_EQ(read_bytes(3 * k_stride), concat({ vertices(2, 1), vertices(1, 2) }));
    }

    TEST_F(FormaSliceDeviceTest, ResubmittingResizesTheDefaultSliceAndShiftsTheOthers)
    {
        ASSERT_TRUE(ready());
        attach_rendering();
        buffer->submit(vertices(2, 1));
        buffer->add_slice(vertices(1, 2));
        buffer->process_default();

        buffer->submit(vertices(4, 9));
        buffer->process_default();

        EXPECT_EQ(vertex_count(), 5U);
        EXPECT_EQ(read_bytes(5 * k_stride), concat({ vertices(4, 9), vertices(1, 2) }));
    }

    TEST_F(FormaSliceDeviceTest, UpdatingASliceInPlaceChangesOnlyItsBytes)
    {
        ASSERT_TRUE(ready());
        attach_rendering();
        buffer->add_slice(vertices(2, 1));
        const auto b = buffer->add_slice(vertices(2, 2));
        buffer->add_slice(vertices(2, 3));
        buffer->process_default();

        buffer->update_slice(b, vertices(2, 7));
        buffer->process_default();

        EXPECT_EQ(vertex_count(), 6U);
        EXPECT_EQ(read_bytes(6 * k_stride), concat({ vertices(2, 1), vertices(2, 7), vertices(2, 3) }));
        EXPECT_TRUE(runs().empty());
    }

    TEST_F(FormaSliceDeviceTest, ResizingASliceShiftsLaterBytes)
    {
        ASSERT_TRUE(ready());
        attach_rendering();
        buffer->add_slice(vertices(2, 1));
        const auto b = buffer->add_slice(vertices(1, 2));
        buffer->add_slice(vertices(2, 3));
        buffer->process_default();

        buffer->update_slice(b, vertices(3, 8));
        buffer->process_default();

        EXPECT_EQ(vertex_count(), 7U);
        EXPECT_EQ(read_bytes(7 * k_stride), concat({ vertices(2, 1), vertices(3, 8), vertices(2, 3) }));
    }

    TEST_F(FormaSliceDeviceTest, HidingAMiddleSliceSplitsTheDrawIntoTwoRuns)
    {
        ASSERT_TRUE(ready());
        attach_rendering();
        buffer->add_slice(vertices(2, 1));
        const auto b = buffer->add_slice(vertices(1, 2));
        buffer->add_slice(vertices(2, 3));
        buffer->process_default();

        buffer->set_slice_visible(b, false);
        buffer->process_default();

        ASSERT_EQ(runs().size(), 2U);
        EXPECT_TRUE(is_run(runs()[0], PrimitiveTopology::TRIANGLE_LIST, 0, 2));
        EXPECT_TRUE(is_run(runs()[1], PrimitiveTopology::TRIANGLE_LIST, 3, 2));
        EXPECT_EQ(vertex_count(), 5U);
        EXPECT_EQ(read_bytes(5 * k_stride), concat({ vertices(2, 1), vertices(1, 2), vertices(2, 3) }));
    }

    TEST_F(FormaSliceDeviceTest, ShowingTheSliceAgainRestoresASingleFullRun)
    {
        ASSERT_TRUE(ready());
        attach_rendering();
        buffer->add_slice(vertices(2, 1));
        const auto b = buffer->add_slice(vertices(1, 2));
        buffer->add_slice(vertices(2, 3));
        buffer->process_default();
        buffer->set_slice_visible(b, false);
        buffer->process_default();
        ASSERT_EQ(runs().size(), 2U);

        buffer->set_slice_visible(b, true);
        buffer->process_default();

        EXPECT_TRUE(runs().empty());
        EXPECT_EQ(vertex_count(), 5U);
    }

    TEST_F(FormaSliceDeviceTest, BringingASliceToFrontReordersRunsWithoutMovingBytes)
    {
        ASSERT_TRUE(ready());
        attach_rendering();
        const auto a = buffer->add_slice(vertices(1, 1));
        buffer->add_slice(vertices(1, 2));
        buffer->add_slice(vertices(1, 3));
        buffer->process_default();

        buffer->bring_slice_to_front(a);
        buffer->process_default();

        ASSERT_EQ(runs().size(), 2U);
        EXPECT_TRUE(is_run(runs()[0], PrimitiveTopology::TRIANGLE_LIST, 1, 2));
        EXPECT_TRUE(is_run(runs()[1], PrimitiveTopology::TRIANGLE_LIST, 0, 1));
        EXPECT_EQ(read_bytes(3 * k_stride), concat({ vertices(1, 1), vertices(1, 2), vertices(1, 3) }));
    }

    TEST_F(FormaSliceDeviceTest, SendingASliceToTheBackReordersRunsWithoutMovingBytes)
    {
        ASSERT_TRUE(ready());
        attach_rendering();
        buffer->add_slice(vertices(1, 1));
        buffer->add_slice(vertices(1, 2));
        const auto c = buffer->add_slice(vertices(1, 3));
        buffer->process_default();

        buffer->send_slice_to_back(c);
        buffer->process_default();

        ASSERT_EQ(runs().size(), 2U);
        EXPECT_TRUE(is_run(runs()[0], PrimitiveTopology::TRIANGLE_LIST, 2, 1));
        EXPECT_TRUE(is_run(runs()[1], PrimitiveTopology::TRIANGLE_LIST, 0, 2));
        EXPECT_EQ(read_bytes(3 * k_stride), concat({ vertices(1, 1), vertices(1, 2), vertices(1, 3) }));
    }

    TEST_F(FormaSliceDeviceTest, RemovingASliceCompactsTheBytesAndDrawsOneRun)
    {
        ASSERT_TRUE(ready());
        attach_rendering();
        buffer->add_slice(vertices(2, 1));
        const auto b = buffer->add_slice(vertices(1, 2));
        buffer->add_slice(vertices(2, 3));
        buffer->process_default();

        buffer->remove_slice(b);
        buffer->process_default();

        EXPECT_EQ(vertex_count(), 4U);
        EXPECT_EQ(read_bytes(4 * k_stride), concat({ vertices(2, 1), vertices(2, 3) }));
        EXPECT_TRUE(runs().empty());
    }

    TEST_F(FormaSliceDeviceTest, HidingEverySliceStopsDrawingAndShowingOneResumes)
    {
        ASSERT_TRUE(ready());
        attach_rendering();
        const auto a = buffer->add_slice(vertices(2, 1));
        const auto b = buffer->add_slice(vertices(2, 2));
        buffer->process_default();

        buffer->set_slice_visible(a, false);
        buffer->set_slice_visible(b, false);
        buffer->process_default();

        EXPECT_EQ(vertex_count(), 0U);
        EXPECT_TRUE(runs().empty());

        buffer->set_slice_visible(b, true);
        buffer->process_default();

        EXPECT_GT(vertex_count(), 0U);
        EXPECT_TRUE(runs().empty());
        EXPECT_EQ(read_bytes(4 * k_stride), concat({ vertices(2, 1), vertices(2, 2) }));
    }

    TEST_F(FormaSliceDeviceTest, StripSlicesAreNeverJoinedIntoOneRun)
    {
        ASSERT_TRUE(ready(PrimitiveTopology::TRIANGLE_STRIP));
        attach_rendering();
        buffer->add_slice(vertices(3, 1));
        buffer->add_slice(vertices(4, 2));
        buffer->process_default();

        ASSERT_EQ(runs().size(), 2U);
        EXPECT_TRUE(is_run(runs()[0], PrimitiveTopology::TRIANGLE_STRIP, 0, 3));
        EXPECT_TRUE(is_run(runs()[1], PrimitiveTopology::TRIANGLE_STRIP, 3, 4));
        EXPECT_EQ(vertex_count(), 7U);
    }

    TEST_F(FormaSliceDeviceTest, TriangulatingBufferKeepsSlicesOfMixedTopologyAsSeparateRuns)
    {
        ASSERT_TRUE(ready());
        window = std::make_shared<ScriptedWindow>(128, 128);
        Portal::Graphics::RenderConfig config;
        config.target_window = window;
        config.triangulate = true;
        buffer->setup_rendering(config);

        buffer->add_slice(vertices(3, 1));
        buffer->add_slice(vertices(4, 2), PrimitiveTopology::TRIANGLE_STRIP);
        buffer->add_slice(vertices(2, 3), PrimitiveTopology::LINE_LIST);
        buffer->process_default();

        ASSERT_EQ(runs().size(), 3U);
        EXPECT_TRUE(is_run(runs()[0], PrimitiveTopology::TRIANGLE_LIST, 0, 3));
        EXPECT_TRUE(is_run(runs()[1], PrimitiveTopology::TRIANGLE_STRIP, 3, 4));
        EXPECT_TRUE(is_run(runs()[2], PrimitiveTopology::LINE_LIST, 7, 2));
        EXPECT_EQ(vertex_count(), 9U);
        EXPECT_EQ(read_bytes(9 * k_stride),
            concat({ vertices(3, 1), vertices(4, 2), vertices(2, 3) }));
    }

    TEST_F(FormaSliceDeviceTest, RunsWaitForARenderProcessorAndAreAppliedWhenItAppears)
    {
        ASSERT_TRUE(ready());
        buffer->add_slice(vertices(2, 1));
        const auto b = buffer->add_slice(vertices(1, 2));
        buffer->add_slice(vertices(2, 3));
        buffer->set_slice_visible(b, false);
        buffer->process_default();

        ASSERT_EQ(buffer->get_render_processor(), nullptr);
        EXPECT_EQ(vertex_count(), 5U);

        attach_rendering();
        buffer->process_default();

        ASSERT_NE(buffer->get_render_processor(), nullptr);
        ASSERT_EQ(runs().size(), 2U);
        EXPECT_TRUE(is_run(runs()[0], PrimitiveTopology::TRIANGLE_LIST, 0, 2));
        EXPECT_TRUE(is_run(runs()[1], PrimitiveTopology::TRIANGLE_LIST, 3, 2));
    }

    TEST_F(FormaSliceDeviceTest, UnknownSliceIdsLeaveTheBufferUnchanged)
    {
        ASSERT_TRUE(ready());
        attach_rendering();
        buffer->add_slice(vertices(2, 1));
        buffer->process_default();

        buffer->update_slice(999, vertices(1, 5));
        buffer->remove_slice(999);
        buffer->set_slice_visible(999, false);
        buffer->bring_slice_to_front(999);
        buffer->send_slice_to_back(999);
        buffer->process_default();

        EXPECT_EQ(vertex_count(), 2U);
        EXPECT_EQ(read_bytes(2 * k_stride), vertices(2, 1));
        EXPECT_TRUE(runs().empty());
    }

    TEST_F(FormaSliceDeviceTest, SlicesBeyondCapacityGrowTheBufferAndKeepEveryByte)
    {
        ASSERT_TRUE(ready(PrimitiveTopology::TRIANGLE_LIST, 1));
        attach_rendering();

        buffer->add_slice(vertices(3, 1));
        buffer->add_slice(vertices(4, 2));
        buffer->process_default();

        EXPECT_GE(buffer->get_size_bytes(), 7 * k_stride);
        EXPECT_EQ(vertex_count(), 7U);
        EXPECT_EQ(read_bytes(7 * k_stride), concat({ vertices(3, 1), vertices(4, 2) }));
    }

    TEST_F(FormaSliceDeviceTest, ManySlicesWithSomeHiddenProduceTheExpectedRunList)
    {
        constexpr size_t count = 60;
        ASSERT_TRUE(ready(PrimitiveTopology::TRIANGLE_LIST, count));
        attach_rendering();
        std::vector<FormaBuffer::SliceId> ids;
        ids.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            ids.push_back(buffer->add_slice(vertices(1, static_cast<uint8_t>(i))));
        }
        buffer->process_default();

        for (size_t i = 0; i < count; i += 3) {
            buffer->set_slice_visible(ids[i], false);
        }
        buffer->process_default();

        ASSERT_EQ(runs().size(), 20U);
        for (size_t r = 0; r < runs().size(); ++r) {
            EXPECT_TRUE(is_run(runs()[r], PrimitiveTopology::TRIANGLE_LIST, static_cast<uint32_t>(r * 3 + 1), 2)) << "run " << r;
        }
        EXPECT_EQ(vertex_count(), count);
    }

}
}
