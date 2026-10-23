#include "gtest/gtest.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "../../support/scripted_window.hpp"
#include "../../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/Forma/FormaBuffer.hpp"
#include "MayaFlux/Buffers/Shaders/RenderProcessor.hpp"
#include "MayaFlux/Buffers/Staging/StagingUtils.hpp"
#include "MayaFlux/Kakshya/NDData/VertexFormats.hpp"
#include "MayaFlux/Kakshya/NDData/VertexLayout.hpp"
#include "MayaFlux/Portal/Forma/Element.hpp"
#include "MayaFlux/Portal/Forma/Layer.hpp"
#include "MayaFlux/Portal/Forma/Target.hpp"
#include "MayaFlux/Portal/Graphics/ShaderFoundry.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::FormaBuffer;
    using Buffers::ProcessingToken;
    using Portal::Forma::Element;
    using Portal::Forma::FormaTarget;
    using Portal::Forma::Layer;
    using Portal::Graphics::DrawRun;
    using Portal::Graphics::PrimitiveTopology;

    const size_t k_target_stride = Kakshya::VertexLayout::for_meshes().stride_bytes;

    std::vector<uint8_t> target_vertices(size_t count, uint8_t fill)
    {
        return std::vector<uint8_t>(count * k_target_stride, fill);
    }

    std::vector<uint8_t> target_concat(std::initializer_list<std::vector<uint8_t>> parts)
    {
        std::vector<uint8_t> out;
        for (const auto& part : parts) {
            out.insert(out.end(), part.begin(), part.end());
        }
        return out;
    }

    TEST(FormaTargetTest, AnEmptyHandleIsFalseAndEveryCallIsHarmless)
    {
        FormaTarget target;

        EXPECT_FALSE(target);
        EXPECT_EQ(target.buffer(), nullptr);
        EXPECT_EQ(target.slice(), 0U);
        EXPECT_EQ(target.operator->(), nullptr);
        EXPECT_TRUE(target == nullptr);
        EXPECT_NO_THROW(target.submit(target_vertices(1, 1)));
        EXPECT_NO_THROW(target.bind_texture(0, nullptr));
        EXPECT_NO_THROW(target.set_visible(false));
        EXPECT_NO_THROW(target.bring_to_front(3));
        EXPECT_NO_THROW(target.send_to_back(-3));
        EXPECT_NO_THROW(target.retire());
    }

    TEST(FormaTargetTest, ANullBufferMakesAnEmptyHandle)
    {
        const FormaTarget from_null_buffer { std::shared_ptr<FormaBuffer> {} };
        const FormaTarget from_nullptr = nullptr;

        EXPECT_FALSE(from_null_buffer);
        EXPECT_FALSE(from_nullptr);
    }

    TEST(FormaTargetTest, ABufferConvertsToAHandleOnTheWholeBuffer)
    {
        auto buffer = std::make_shared<FormaBuffer>(240, PrimitiveTopology::TRIANGLE_LIST);

        const FormaTarget target = buffer;

        EXPECT_TRUE(target);
        EXPECT_EQ(target.buffer(), buffer);
        EXPECT_EQ(target.slice(), 0U);
        EXPECT_TRUE(target == buffer);
        EXPECT_EQ(target->topology(), PrimitiveTopology::TRIANGLE_LIST);
        EXPECT_EQ(&*target, buffer.get());
    }

    TEST(FormaTargetTest, HandlesCompareByBufferAndSlice)
    {
        auto buffer = std::make_shared<FormaBuffer>(240, PrimitiveTopology::TRIANGLE_LIST);
        auto other = std::make_shared<FormaBuffer>(240, PrimitiveTopology::TRIANGLE_LIST);

        const FormaTarget first { buffer, 1 };
        const FormaTarget same { buffer, 1 };
        const FormaTarget second_slice { buffer, 2 };
        const FormaTarget other_buffer { other, 1 };

        EXPECT_TRUE(first == same);
        EXPECT_TRUE(first != second_slice);
        EXPECT_TRUE(first != other_buffer);
        EXPECT_EQ(first.slice(), 1U);
    }

    TEST(FormaTargetTest, SliceOfANullBufferIsAnEmptyHandle)
    {
        EXPECT_FALSE(FormaTarget::slice_of(nullptr));
    }

    TEST(FormaTargetTest, SliceOfABufferWithoutProcessorsIsAnEmptyHandle)
    {
        auto buffer = std::make_shared<FormaBuffer>(240, PrimitiveTopology::TRIANGLE_LIST);

        EXPECT_FALSE(FormaTarget::slice_of(buffer));
    }

    class FormaTargetDeviceTest : public VulkanDeviceTest {
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

        bool ready(PrimitiveTopology topology = PrimitiveTopology::TRIANGLE_LIST, size_t vertex_capacity = 16)
        {
            buffer = std::make_shared<FormaBuffer>(k_target_stride * vertex_capacity, topology);
            auto* service = Registry::BackendRegistry::instance()
                                .get_service<Registry::Service::BufferService>();
            if (!service)
                return false;
            service->initialize_buffer(buffer);
            if (!buffer->is_initialized())
                return false;
            buffer->setup_processors(ProcessingToken::GRAPHICS_BACKEND);
            window = std::make_shared<ScriptedWindow>(128, 128);
            buffer->setup_rendering({ .target_window = window });
            return buffer->get_default_processor() != nullptr;
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

    TEST_F(FormaTargetDeviceTest, SliceOfAddsAnEmptySliceDrawnAfterTheExistingOnes)
    {
        ASSERT_TRUE(ready());
        const auto existing = buffer->add_slice(target_vertices(2, 1));

        const auto target = FormaTarget::slice_of(buffer);

        ASSERT_TRUE(target);
        EXPECT_EQ(target.buffer(), buffer);
        EXPECT_NE(target.slice(), 0U);
        EXPECT_NE(target.slice(), existing);
        EXPECT_EQ(buffer->slice_count(), 2U);

        target.submit(target_vertices(1, 7));
        buffer->process_default();

        EXPECT_EQ(vertex_count(), 3U);
        EXPECT_EQ(read_bytes(3 * k_target_stride), target_concat({ target_vertices(2, 1), target_vertices(1, 7) }));
    }

    TEST_F(FormaTargetDeviceTest, SlicesOfOneBufferAreWrittenAndHiddenIndependently)
    {
        ASSERT_TRUE(ready());
        const auto first = FormaTarget::slice_of(buffer);
        const auto middle = FormaTarget::slice_of(buffer);
        const auto last = FormaTarget::slice_of(buffer);
        first.submit(target_vertices(2, 1));
        middle.submit(target_vertices(3, 2));
        last.submit(target_vertices(1, 3));
        buffer->process_default();

        EXPECT_EQ(vertex_count(), 6U);
        EXPECT_EQ(read_bytes(6 * k_target_stride),
            target_concat({ target_vertices(2, 1), target_vertices(3, 2), target_vertices(1, 3) }));

        middle.set_visible(false);
        buffer->process_default();

        ASSERT_EQ(runs().size(), 2U);
        EXPECT_EQ(runs()[0].vertex_offset, 0U);
        EXPECT_EQ(runs()[0].vertex_count, 2U);
        EXPECT_EQ(runs()[1].vertex_offset, 5U);
        EXPECT_EQ(runs()[1].vertex_count, 1U);
    }

    TEST_F(FormaTargetDeviceTest, RetiringTheLastSliceRetiresTheBuffer)
    {
        ASSERT_TRUE(ready());
        const auto first = FormaTarget::slice_of(buffer);
        const auto second = FormaTarget::slice_of(buffer);

        first.retire();
        EXPECT_FALSE(buffer->needs_removal());
        EXPECT_EQ(buffer->slice_count(), 1U);

        second.retire();
        EXPECT_TRUE(buffer->needs_removal());
        EXPECT_EQ(buffer->slice_count(), 0U);
    }

    TEST_F(FormaTargetDeviceTest, ASubmitOnAWholeBufferHandleWritesTheDefaultSlice)
    {
        ASSERT_TRUE(ready());
        const FormaTarget target = buffer;

        target.submit(target_vertices(2, 1));
        buffer->process_default();

        EXPECT_EQ(vertex_count(), 2U);
        EXPECT_EQ(read_bytes(2 * k_target_stride), target_vertices(2, 1));
    }

    TEST_F(FormaTargetDeviceTest, ASubmitOnASliceHandleUpdatesOnlyThatSlice)
    {
        ASSERT_TRUE(ready());
        const auto a = buffer->add_slice(target_vertices(2, 1));
        const auto b = buffer->add_slice(target_vertices(1, 2));
        const FormaTarget second { buffer, b };
        buffer->process_default();

        second.submit(target_vertices(1, 9));
        buffer->process_default();

        EXPECT_NE(a, 0U);
        EXPECT_EQ(vertex_count(), 3U);
        EXPECT_EQ(read_bytes(3 * k_target_stride), target_concat({ target_vertices(2, 1), target_vertices(1, 9) }));
    }

    TEST_F(FormaTargetDeviceTest, TypedSubmitOverloadsForwardThroughTheHandle)
    {
        ASSERT_TRUE(ready());
        const FormaTarget target = buffer;

        const std::array<Kakshya::MeshVertex, 3> triangle {
            Kakshya::MeshVertex { .position = { 0.F, 0.F, 0.F } },
            Kakshya::MeshVertex { .position = { 1.F, 0.F, 0.F } },
            Kakshya::MeshVertex { .position = { 0.F, 1.F, 0.F } },
        };
        target.submit(triangle);
        buffer->process_default();

        std::vector<uint8_t> expected(sizeof(triangle));
        std::memcpy(expected.data(), triangle.data(), expected.size());
        EXPECT_EQ(vertex_count(), 3U);
        EXPECT_EQ(read_bytes(expected.size()), expected);

        const Kakshya::MeshVertex single { .position = { 0.5F, 0.5F, 0.F } };
        target.submit(single);
        buffer->process_default();

        std::vector<uint8_t> expected_single(sizeof(single));
        std::memcpy(expected_single.data(), &single, expected_single.size());
        EXPECT_EQ(vertex_count(), 1U);
        EXPECT_EQ(read_bytes(expected_single.size()), expected_single);
    }

    TEST_F(FormaTargetDeviceTest, ASliceHandleHidesAndShowsOnlyItsSlice)
    {
        ASSERT_TRUE(ready());
        buffer->add_slice(target_vertices(2, 1));
        const auto b = buffer->add_slice(target_vertices(1, 2));
        buffer->add_slice(target_vertices(2, 3));
        const FormaTarget middle { buffer, b };
        buffer->process_default();

        middle.set_visible(false);
        buffer->process_default();
        ASSERT_EQ(runs().size(), 2U);
        EXPECT_EQ(runs()[0].vertex_offset, 0U);
        EXPECT_EQ(runs()[0].vertex_count, 2U);
        EXPECT_EQ(runs()[1].vertex_offset, 3U);
        EXPECT_EQ(runs()[1].vertex_count, 2U);

        middle.set_visible(true);
        buffer->process_default();
        EXPECT_TRUE(runs().empty());
        EXPECT_EQ(vertex_count(), 5U);
    }

    TEST_F(FormaTargetDeviceTest, AWholeBufferHandleHidesThroughTheRenderProcessor)
    {
        ASSERT_TRUE(ready());
        const FormaTarget target = buffer;
        const auto render = buffer->get_render_processor();
        ASSERT_NE(render, nullptr);

        target.set_visible(false);
        EXPECT_FALSE(render->is_visible(buffer));

        target.set_visible(true);
        EXPECT_TRUE(render->is_visible(buffer));
    }

    TEST_F(FormaTargetDeviceTest, SliceHandlesReorderRunsWithinTheirBuffer)
    {
        ASSERT_TRUE(ready());
        const auto a = buffer->add_slice(target_vertices(1, 1));
        buffer->add_slice(target_vertices(1, 2));
        const auto c = buffer->add_slice(target_vertices(1, 3));
        buffer->process_default();

        FormaTarget { buffer, a }.bring_to_front(5);
        buffer->process_default();
        ASSERT_EQ(runs().size(), 2U);
        EXPECT_EQ(runs()[0].vertex_offset, 1U);
        EXPECT_EQ(runs()[0].vertex_count, 2U);
        EXPECT_EQ(runs()[1].vertex_offset, 0U);

        FormaTarget { buffer, c }.send_to_back(-5);
        buffer->process_default();
        ASSERT_FALSE(runs().empty());
        EXPECT_EQ(runs().front().vertex_offset, 2U);
    }

    TEST_F(FormaTargetDeviceTest, AWholeBufferHandleWritesItsPriorityAndASliceHandleDoesNot)
    {
        ASSERT_TRUE(ready());
        const FormaTarget whole = buffer;
        const auto a = buffer->add_slice(target_vertices(1, 1));
        const FormaTarget slice { buffer, a };

        whole.bring_to_front(7);
        EXPECT_EQ(buffer->get_render_config().draw_priority, 7);

        whole.send_to_back(-3);
        EXPECT_EQ(buffer->get_render_config().draw_priority, -3);

        slice.bring_to_front(99);
        EXPECT_EQ(buffer->get_render_config().draw_priority, -3);
    }

    TEST_F(FormaTargetDeviceTest, RetireMarksAWholeBufferAndRemovesOnlyASlice)
    {
        ASSERT_TRUE(ready());
        const auto a = buffer->add_slice(target_vertices(2, 1));
        const auto b = buffer->add_slice(target_vertices(1, 2));
        buffer->process_default();

        FormaTarget { buffer, a }.retire();
        buffer->process_default();

        EXPECT_FALSE(buffer->needs_removal());
        EXPECT_EQ(vertex_count(), 1U);
        EXPECT_EQ(read_bytes(k_target_stride), target_vertices(1, 2));
        EXPECT_NE(b, 0U);

        FormaTarget { buffer }.retire();
        EXPECT_TRUE(buffer->needs_removal());
    }

    TEST_F(FormaTargetDeviceTest, ALayerRoutesVisibilityAndRemovalToTheSliceOfAnElement)
    {
        ASSERT_TRUE(ready());
        const auto a = buffer->add_slice(target_vertices(2, 1));
        const auto b = buffer->add_slice(target_vertices(1, 2));
        const auto c = buffer->add_slice(target_vertices(2, 3));
        buffer->process_default();

        Layer layer;
        Element first;
        first.with_buffer(FormaTarget { buffer, a });
        Element middle;
        middle.with_buffer(FormaTarget { buffer, b });
        Element last;
        last.with_buffer(FormaTarget { buffer, c });
        static_cast<void>(layer.add(std::move(first)));
        const uint32_t middle_id = layer.add(std::move(middle));
        static_cast<void>(layer.add(std::move(last)));

        ASSERT_TRUE(layer.set_visible(middle_id, false));
        buffer->process_default();
        EXPECT_EQ(runs().size(), 2U);

        ASSERT_TRUE(layer.set_visible(middle_id, true));
        buffer->process_default();
        EXPECT_TRUE(runs().empty());

        ASSERT_TRUE(layer.remove(middle_id));
        buffer->process_default();
        EXPECT_FALSE(buffer->needs_removal());
        EXPECT_EQ(vertex_count(), 4U);
        EXPECT_EQ(read_bytes(4 * k_target_stride), target_concat({ target_vertices(2, 1), target_vertices(2, 3) }));
    }

    TEST_F(FormaTargetDeviceTest, ALayerRaisesASliceElementWithinItsBuffer)
    {
        ASSERT_TRUE(ready());
        const auto a = buffer->add_slice(target_vertices(1, 1));
        const auto b = buffer->add_slice(target_vertices(1, 2));
        const auto c = buffer->add_slice(target_vertices(1, 3));
        buffer->process_default();

        Layer layer;
        Element first;
        first.with_buffer(FormaTarget { buffer, a });
        Element second;
        second.with_buffer(FormaTarget { buffer, b });
        Element third;
        third.with_buffer(FormaTarget { buffer, c });
        const uint32_t first_id = layer.add(std::move(first));
        static_cast<void>(layer.add(std::move(second)));
        static_cast<void>(layer.add(std::move(third)));

        ASSERT_TRUE(layer.bring_to_front(first_id));
        buffer->process_default();

        ASSERT_EQ(runs().size(), 2U);
        EXPECT_EQ(runs()[0].vertex_offset, 1U);
        EXPECT_EQ(runs()[0].vertex_count, 2U);
        EXPECT_EQ(runs()[1].vertex_offset, 0U);
        EXPECT_EQ(runs()[1].vertex_count, 1U);
    }

    TEST_F(FormaTargetDeviceTest, ALayerStillDrivesAWholeBufferElementExactlyAsBefore)
    {
        ASSERT_TRUE(ready());
        buffer->submit(target_vertices(2, 1));
        buffer->process_default();

        Layer layer;
        Element element;
        element.with_buffer(buffer);
        const uint32_t id = layer.add(std::move(element));
        const auto render = buffer->get_render_processor();
        ASSERT_NE(render, nullptr);

        ASSERT_TRUE(layer.set_visible(id, false));
        EXPECT_FALSE(render->is_visible(buffer));
        ASSERT_TRUE(layer.set_visible(id, true));
        EXPECT_TRUE(render->is_visible(buffer));

        ASSERT_TRUE(layer.bring_to_front(id));
        EXPECT_GT(buffer->get_render_config().draw_priority, 0);

        ASSERT_TRUE(layer.remove(id));
        EXPECT_TRUE(buffer->needs_removal());
    }

}
}
