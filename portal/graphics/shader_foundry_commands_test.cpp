#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include <stdexcept>
#include <string_view>

#include "../../support/vulkan_test_support.hpp"

#include "MayaFlux/Core/Backends/Graphics/Vulkan/VKCommandManager.hpp"
#include "MayaFlux/Core/Backends/Graphics/Vulkan/VKImage.hpp"
#include "MayaFlux/Portal/Graphics/ShaderFoundry.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test {
namespace {

    using Portal::Graphics::DescriptorBufferWrite;
    using Portal::Graphics::DescriptorImageWrite;
    using Portal::Graphics::INVALID_COMMAND_BUFFER;
    using Portal::Graphics::INVALID_DESCRIPTOR_SET;
    using Portal::Graphics::INVALID_FENCE;
    using Portal::Graphics::INVALID_SEMAPHORE;
    using Portal::Graphics::ShaderFoundry;

    using CommandType = ShaderFoundry::CommandBufferType;

    constexpr uint32_t k_shader_foundry_commands_words = 16;
    constexpr size_t k_shader_foundry_commands_bytes = k_shader_foundry_commands_words * sizeof(uint32_t);
    constexpr uint32_t k_pattern = 0xDEADBEEF;

    ShaderFoundry& shader_foundry_commands_foundry()
    {
        return Portal::Graphics::get_shader_foundry();
    }

    class ShaderFoundryDeviceTest : public ::testing::Test {
    protected:
        static void SetUpTestSuite()
        {
            if (!vulkan_unavailable_reason().empty()) {
                return;
            }
            auto backend = std::make_shared<Core::VulkanBackend>();
            if (backend->initialize(headless_graphics_config())) {
                s_backend = std::move(backend);
            }
        }

        static void TearDownTestSuite()
        {
            s_backend.reset();
        }

        void SetUp() override
        {
            if (!s_backend) {
                GTEST_SKIP() << "No usable Vulkan device: " << vulkan_unavailable_reason();
            }
            shader_foundry_commands_foundry().shutdown();
            ASSERT_TRUE(shader_foundry_commands_foundry().initialize(s_backend));
        }

        void TearDown() override
        {
            shader_foundry_commands_foundry().shutdown();
        }

        static const std::shared_ptr<Core::VulkanBackend>& backend() { return s_backend; }

        static vk::Device device() { return s_backend->get_context().get_device(); }

        static std::unique_ptr<RawVulkanBuffer> host_buffer()
        {
            return std::make_unique<RawVulkanBuffer>(*s_backend, k_shader_foundry_commands_bytes,
                vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc
                    | vk::BufferUsageFlagBits::eStorageBuffer,
                true);
        }

        static std::vector<uint32_t> words(const RawVulkanBuffer& buffer)
        {
            std::vector<uint32_t> out(k_shader_foundry_commands_words);
            std::memcpy(out.data(), buffer.mapped(), k_shader_foundry_commands_bytes);
            return out;
        }

        static void clear(const RawVulkanBuffer& buffer)
        {
            std::memset(buffer.mapped(), 0, k_shader_foundry_commands_bytes);
        }

        class StorageLayout {
        public:
            StorageLayout()
            {
                const vk::DescriptorSetLayoutBinding binding(
                    0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute);
                const vk::DescriptorSetLayoutCreateInfo info({}, 1, &binding);
                m_layout = device().createDescriptorSetLayout(info);
            }

            ~StorageLayout() { device().destroyDescriptorSetLayout(m_layout); }

            StorageLayout(const StorageLayout&) = delete;
            StorageLayout& operator=(const StorageLayout&) = delete;
            StorageLayout(StorageLayout&&) = delete;
            StorageLayout& operator=(StorageLayout&&) = delete;

            [[nodiscard]] vk::DescriptorSetLayout get() const { return m_layout; }

        private:
            vk::DescriptorSetLayout m_layout;
        };

    private:
        static inline std::shared_ptr<Core::VulkanBackend> s_backend;
    };

    // ------------------------------------------------------------------
    // Descriptors
    // ------------------------------------------------------------------

    TEST_F(ShaderFoundryDeviceTest, AllocatedDescriptorSetsHaveDistinctIdsAndHandles)
    {
        StorageLayout layout;

        const auto first = shader_foundry_commands_foundry().allocate_descriptor_set(layout.get());
        const auto second = shader_foundry_commands_foundry().allocate_descriptor_set(layout.get());

        EXPECT_NE(first, INVALID_DESCRIPTOR_SET);
        EXPECT_NE(second, INVALID_DESCRIPTOR_SET);
        EXPECT_NE(first, second);
        EXPECT_TRUE(shader_foundry_commands_foundry().get_descriptor_set(first));
        EXPECT_NE(shader_foundry_commands_foundry().get_descriptor_set(first), shader_foundry_commands_foundry().get_descriptor_set(second));
    }

    TEST_F(ShaderFoundryDeviceTest, UnknownDescriptorSetIdThrows)
    {
        EXPECT_THROW(static_cast<void>(shader_foundry_commands_foundry().get_descriptor_set(9999)), std::invalid_argument);
    }

    TEST_F(ShaderFoundryDeviceTest, BufferWritesToAKnownSetAreAcceptedAndUnknownSetsAreSkipped)
    {
        StorageLayout layout;
        const auto set = shader_foundry_commands_foundry().allocate_descriptor_set(layout.get());
        const auto buffer = host_buffer();

        EXPECT_NO_THROW(shader_foundry_commands_foundry().update_descriptor_buffer(
            set, 0, vk::DescriptorType::eStorageBuffer, buffer->buffer(), 0, k_shader_foundry_commands_bytes));
        EXPECT_NO_THROW(shader_foundry_commands_foundry().update_descriptor_buffer(
            9999, 0, vk::DescriptorType::eStorageBuffer, buffer->buffer(), 0, k_shader_foundry_commands_bytes));

        const std::vector<DescriptorBufferWrite> writes {
            { .descriptor_set_id = 9999, .binding = 0, .type = vk::DescriptorType::eStorageBuffer, .buffer = buffer->buffer(), .offset = 0, .size = k_shader_foundry_commands_bytes },
            { .descriptor_set_id = set, .binding = 0, .type = vk::DescriptorType::eStorageBuffer, .buffer = buffer->buffer(), .offset = 0, .size = k_shader_foundry_commands_bytes }
        };
        EXPECT_NO_THROW(shader_foundry_commands_foundry().update_descriptor_buffers(writes));
        EXPECT_NO_THROW(shader_foundry_commands_foundry().update_descriptor_buffers({}));
    }

    TEST_F(ShaderFoundryDeviceTest, ImageWritesToAnUnknownSetAreSkipped)
    {
        EXPECT_NO_THROW(shader_foundry_commands_foundry().update_descriptor_image(
            9999, 0, vk::ImageView {}, vk::Sampler {}, vk::ImageLayout::eShaderReadOnlyOptimal));
        EXPECT_NO_THROW(shader_foundry_commands_foundry().update_descriptor_storage_image(
            9999, 0, vk::ImageView {}, vk::ImageLayout::eGeneral));

        const std::vector<DescriptorImageWrite> writes {
            { .descriptor_set_id = 9999, .binding = 0, .type = vk::DescriptorType::eStorageImage, .image_view = vk::ImageView {}, .sampler = vk::Sampler {}, .layout = vk::ImageLayout::eGeneral }
        };
        EXPECT_NO_THROW(shader_foundry_commands_foundry().update_descriptor_images(writes));
        EXPECT_NO_THROW(shader_foundry_commands_foundry().update_descriptor_images({}));
    }

    TEST_F(ShaderFoundryDeviceTest, ShutdownForgetsEveryDescriptorSet)
    {
        StorageLayout layout;
        const auto set = shader_foundry_commands_foundry().allocate_descriptor_set(layout.get());
        ASSERT_TRUE(shader_foundry_commands_foundry().get_descriptor_set(set));

        shader_foundry_commands_foundry().shutdown();
        ASSERT_TRUE(shader_foundry_commands_foundry().initialize(backend()));

        EXPECT_THROW(static_cast<void>(shader_foundry_commands_foundry().get_descriptor_set(set)), std::invalid_argument);
    }

    // ------------------------------------------------------------------
    // Command buffers
    // ------------------------------------------------------------------

    struct TypeCase {
        std::string_view name;
        CommandType type;
    };

    class ShaderFoundryCommandTypeTest : public ShaderFoundryDeviceTest, public ::testing::WithParamInterface<TypeCase> { };

    MAYAFLUX_TEST_WITH(TEST_P, ShaderFoundryCommandTypeTest, BeginCommandsGivesADistinctBufferThatCanBeSubmitted, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        const auto first = shader_foundry_commands_foundry().begin_commands(GetParam().type);
        const auto second = shader_foundry_commands_foundry().begin_commands(GetParam().type);

        EXPECT_NE(first, INVALID_COMMAND_BUFFER);
        EXPECT_NE(second, INVALID_COMMAND_BUFFER);
        EXPECT_NE(first, second);
        EXPECT_TRUE(shader_foundry_commands_foundry().get_command_buffer(first));
        EXPECT_NE(shader_foundry_commands_foundry().get_command_buffer(first), shader_foundry_commands_foundry().get_command_buffer(second));

        shader_foundry_commands_foundry().submit_and_wait(first);
        shader_foundry_commands_foundry().submit_and_wait(second);
        EXPECT_FALSE(shader_foundry_commands_foundry().get_command_buffer(first));
    }

    INSTANTIATE_TEST_SUITE_P(Types, ShaderFoundryCommandTypeTest,
        ::testing::Values(
            TypeCase { "Graphics", CommandType::GRAPHICS },
            TypeCase { "Compute", CommandType::COMPUTE }),
        [](const ::testing::TestParamInfo<TypeCase>& info) { return std::string(info.param.name); });

    TEST_F(ShaderFoundryDeviceTest, EndCommandsSucceedsOnceAndFailsForUnknownOrFinishedBuffers)
    {
        const auto id = shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);

        EXPECT_TRUE(shader_foundry_commands_foundry().end_commands(id));
        EXPECT_FALSE(shader_foundry_commands_foundry().end_commands(id));
        EXPECT_FALSE(shader_foundry_commands_foundry().end_commands(9999));
        EXPECT_FALSE(shader_foundry_commands_foundry().get_command_buffer(9999));
    }

    TEST_F(ShaderFoundryDeviceTest, SubmitAndWaitRunsTheRecordedWorkAndForgetsTheBuffer)
    {
        const auto buffer = host_buffer();
        clear(*buffer);
        const auto id = shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);
        shader_foundry_commands_foundry().get_command_buffer(id).fillBuffer(buffer->buffer(), 0, k_shader_foundry_commands_bytes, k_pattern);

        shader_foundry_commands_foundry().submit_and_wait(id);

        EXPECT_EQ(words(*buffer), std::vector<uint32_t>(k_shader_foundry_commands_words, k_pattern));
        EXPECT_FALSE(shader_foundry_commands_foundry().get_command_buffer(id));
    }

    TEST_F(ShaderFoundryDeviceTest, SubmitAndWaitOfAnUnknownBufferIsIgnored)
    {
        EXPECT_NO_THROW(shader_foundry_commands_foundry().submit_and_wait(9999));
    }

    TEST_F(ShaderFoundryDeviceTest, AsyncSubmitSignalsItsFenceAndReleaseFreesTheBuffer)
    {
        const auto buffer = host_buffer();
        clear(*buffer);
        const auto id = shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);
        shader_foundry_commands_foundry().get_command_buffer(id).fillBuffer(buffer->buffer(), 0, k_shader_foundry_commands_bytes, k_pattern);

        const auto fence = shader_foundry_commands_foundry().submit_async(id);
        ASSERT_NE(fence, INVALID_FENCE);
        shader_foundry_commands_foundry().wait_for_fence(fence);

        EXPECT_TRUE(shader_foundry_commands_foundry().is_fence_signaled(fence));
        EXPECT_EQ(words(*buffer), std::vector<uint32_t>(k_shader_foundry_commands_words, k_pattern));
        EXPECT_TRUE(shader_foundry_commands_foundry().get_command_buffer(id));

        shader_foundry_commands_foundry().release_fence(fence);

        EXPECT_FALSE(shader_foundry_commands_foundry().get_command_buffer(id));
        EXPECT_FALSE(shader_foundry_commands_foundry().is_fence_signaled(fence));
        EXPECT_NO_THROW(shader_foundry_commands_foundry().release_fence(fence));
    }

    TEST_F(ShaderFoundryDeviceTest, WaitForFencesWaitsForEveryFence)
    {
        const auto a = host_buffer();
        const auto b = host_buffer();
        clear(*a);
        clear(*b);
        const auto cmd_a = shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);
        shader_foundry_commands_foundry().get_command_buffer(cmd_a).fillBuffer(a->buffer(), 0, k_shader_foundry_commands_bytes, 1U);
        const auto cmd_b = shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);
        shader_foundry_commands_foundry().get_command_buffer(cmd_b).fillBuffer(b->buffer(), 0, k_shader_foundry_commands_bytes, 2U);
        const auto fence_a = shader_foundry_commands_foundry().submit_async(cmd_a);
        const auto fence_b = shader_foundry_commands_foundry().submit_async(cmd_b);
        ASSERT_NE(fence_a, INVALID_FENCE);
        ASSERT_NE(fence_b, INVALID_FENCE);

        shader_foundry_commands_foundry().wait_for_fences({ fence_a, fence_b });

        EXPECT_TRUE(shader_foundry_commands_foundry().is_fence_signaled(fence_a));
        EXPECT_TRUE(shader_foundry_commands_foundry().is_fence_signaled(fence_b));
        EXPECT_EQ(words(*a), std::vector<uint32_t>(k_shader_foundry_commands_words, 1U));
        EXPECT_EQ(words(*b), std::vector<uint32_t>(k_shader_foundry_commands_words, 2U));
        shader_foundry_commands_foundry().release_fence(fence_a);
        shader_foundry_commands_foundry().release_fence(fence_b);
    }

    TEST_F(ShaderFoundryDeviceTest, ALeaseHeldOnACommandBufferIsReleasedWhenASynchronousSubmitCompletes)
    {
        Portal::Graphics::SlotArena arena;
        ASSERT_TRUE(arena.initialize(64, Portal::Graphics::BufferUsageHint::UNIFORM, 2));

        const auto id = shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);
        shader_foundry_commands_foundry().hold_until_complete(id, arena.lease(0));
        EXPECT_EQ(arena.acquire(), 1U);

        shader_foundry_commands_foundry().submit_and_wait(id);

        EXPECT_EQ(arena.acquire(), 0U);
        arena.release();
    }

    TEST_F(ShaderFoundryDeviceTest, ALeaseSurvivesReleasingTheFenceUntilTheSubmissionCompletes)
    {
        Portal::Graphics::SlotArena arena;
        ASSERT_TRUE(arena.initialize(64, Portal::Graphics::BufferUsageHint::UNIFORM, 2));
        const auto buffer = host_buffer();

        const auto id = shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);
        shader_foundry_commands_foundry().get_command_buffer(id).fillBuffer(buffer->buffer(), 0, k_shader_foundry_commands_bytes, k_pattern);
        shader_foundry_commands_foundry().hold_until_complete(id, arena.lease(0));

        shader_foundry_commands_foundry().release_fence(shader_foundry_commands_foundry().submit_async(id));
        backend()->wait_idle();
        shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);

        EXPECT_EQ(arena.acquire(), 0U);
        arena.release();
    }

    TEST_F(ShaderFoundryDeviceTest, ALeaseOnACommandBufferRetiredWithoutSubmitIsReleasedWithIt)
    {
        Portal::Graphics::SlotArena arena;
        ASSERT_TRUE(arena.initialize(64, Portal::Graphics::BufferUsageHint::UNIFORM, 2));

        const auto id = shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);
        shader_foundry_commands_foundry().hold_until_complete(id, arena.lease(0));
        EXPECT_EQ(arena.acquire(), 1U);

        const std::array<Portal::Graphics::CommandBufferID, 1> ids { id };
        shader_foundry_commands_foundry().retire_after_submit(ids);
        backend()->wait_idle();
        shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);

        EXPECT_EQ(arena.acquire(), 0U);
        arena.release();
    }

    MAYAFLUX_TEST_WITH(TEST_F, ShaderFoundryDeviceTest, RecordedImageReadSurvivesCleanupBeforeSubmit, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan, ::MayaFlux::Test::Meta::Capability::vulkan_validation>() }))
    {
        auto& resources = backend()->get_resource_manager();
        auto image = std::make_shared<Core::VKImage>(4, 4, 1, vk::Format::eR8G8B8A8Unorm,
            Core::VKImage::Usage::TEXTURE_2D);
        resources.initialize_image(image);
        const std::array<uint8_t, 64> expected = [] {
            std::array<uint8_t, 64> bytes {};
            std::iota(bytes.begin(), bytes.end(), uint8_t { 23 });
            return bytes;
        }();
        resources.upload_image_data(image, expected.data(), expected.size());
        resources.transition_image_layout(image, vk::ImageLayout::eTransferSrcOptimal, Core::SubmitMode::IMMEDIATE);
        RawVulkanBuffer destination(*backend(), expected.size(), vk::BufferUsageFlagBits::eTransferDst, true);
        std::memset(destination.mapped(), 0, expected.size());

        auto allocation = resources.retain_image_allocation(image);
        ASSERT_NE(allocation, nullptr);
        const std::weak_ptr<void> weak = allocation;
        const auto id = shader_foundry_commands_foundry().begin_commands(CommandType::GRAPHICS);
        shader_foundry_commands_foundry().hold_image(id, image);
        allocation.reset();
        vk::BufferImageCopy region {};
        region.imageSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
        region.imageExtent = vk::Extent3D { 4, 4, 1 };
        auto cmd = shader_foundry_commands_foundry().get_command_buffer(id);
        cmd.copyImageToBuffer(image->get_image(), vk::ImageLayout::eTransferSrcOptimal, destination.buffer(), 1, &region);
        const vk::MemoryBarrier2 to_host {
            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
            vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead
        };
        vk::DependencyInfo dependency {};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &to_host;
        cmd.pipelineBarrier2(dependency);

        resources.cleanup_image(image);
        EXPECT_FALSE(image->is_initialized());
        EXPECT_FALSE(weak.expired());
        shader_foundry_commands_foundry().submit_and_wait(id);
        backend()->get_command_manager().collect();

        EXPECT_EQ(std::memcmp(destination.mapped(), expected.data(), expected.size()), 0);
        EXPECT_TRUE(weak.expired());
    }

    TEST_F(ShaderFoundryDeviceTest, HoldingAnEmptyLeaseOrOnAnUnknownBufferHoldsNothing)
    {
        Portal::Graphics::SlotArena arena;
        ASSERT_TRUE(arena.initialize(64, Portal::Graphics::BufferUsageHint::UNIFORM, 1));

        const auto id = shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);
        EXPECT_NO_THROW(shader_foundry_commands_foundry().hold_until_complete(id, Portal::Graphics::SlotLease {}));
        EXPECT_NO_THROW(shader_foundry_commands_foundry().hold_until_complete(9999, arena.lease(0)));

        EXPECT_EQ(arena.acquire(), 0U);
        EXPECT_EQ(arena.slot_count(), 1U);
        arena.release();
    }

    TEST_F(ShaderFoundryDeviceTest, SubmittingAnUnknownBufferGivesNoFenceOrSemaphore)
    {
        EXPECT_EQ(shader_foundry_commands_foundry().submit_async(9999), INVALID_FENCE);
        EXPECT_EQ(shader_foundry_commands_foundry().submit_with_signal(9999), INVALID_SEMAPHORE);
        EXPECT_FALSE(shader_foundry_commands_foundry().is_fence_signaled(9999));
        EXPECT_NO_THROW(shader_foundry_commands_foundry().wait_for_fence(9999));
        EXPECT_NO_THROW(shader_foundry_commands_foundry().wait_for_fences({ 9999 }));
    }

    TEST_F(ShaderFoundryDeviceTest, SubmitWithSignalGivesASemaphoreWithAHandle)
    {
        const auto id = shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);

        const auto semaphore = shader_foundry_commands_foundry().submit_with_signal(id);

        ASSERT_NE(semaphore, INVALID_SEMAPHORE);
        EXPECT_TRUE(shader_foundry_commands_foundry().get_semaphore_handle(semaphore));
        EXPECT_FALSE(shader_foundry_commands_foundry().get_semaphore_handle(9999));
        device().waitIdle();
    }

    TEST_F(ShaderFoundryDeviceTest, BeginCommandsWithWaitRefusesAnUnknownSemaphoreAndAcceptsAKnownOne)
    {
        const auto producer = shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);
        const auto semaphore = shader_foundry_commands_foundry().submit_with_signal(producer);
        ASSERT_NE(semaphore, INVALID_SEMAPHORE);

        EXPECT_EQ(shader_foundry_commands_foundry().begin_commands_with_wait(CommandType::COMPUTE, 9999, vk::PipelineStageFlagBits2::eTransfer),
            INVALID_COMMAND_BUFFER);

        const auto consumer = shader_foundry_commands_foundry().begin_commands_with_wait(
            CommandType::COMPUTE, semaphore, vk::PipelineStageFlagBits2::eTransfer);
        EXPECT_NE(consumer, INVALID_COMMAND_BUFFER);
        shader_foundry_commands_foundry().submit_and_wait(consumer);
        device().waitIdle();
    }

    TEST_F(ShaderFoundryDeviceTest, AWaitingCommandBufferRunsAfterItsProducer)
    {
        const auto buffer = host_buffer();
        clear(*buffer);

        const auto producer = shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);
        shader_foundry_commands_foundry().get_command_buffer(producer).fillBuffer(buffer->buffer(), 0, k_shader_foundry_commands_bytes, k_pattern);
        const auto semaphore = shader_foundry_commands_foundry().submit_with_signal(producer);
        ASSERT_NE(semaphore, INVALID_SEMAPHORE);

        constexpr uint32_t consumer_pattern = 0x5A5A5A5AU;
        const auto consumer = shader_foundry_commands_foundry().begin_commands_with_wait(
            CommandType::COMPUTE, semaphore, vk::PipelineStageFlagBits2::eTransfer);
        ASSERT_NE(consumer, INVALID_COMMAND_BUFFER);
        shader_foundry_commands_foundry().get_command_buffer(consumer).fillBuffer(buffer->buffer(), 0, k_shader_foundry_commands_bytes, consumer_pattern);
        shader_foundry_commands_foundry().submit_and_wait(consumer);

        EXPECT_EQ(words(*buffer), std::vector<uint32_t>(k_shader_foundry_commands_words, consumer_pattern));
        device().waitIdle();
    }

    TEST_F(ShaderFoundryDeviceTest, ReleaseAfterSubmitRunsOnceThePendingSubmissionsComplete)
    {
        const auto id = shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);
        const auto fence = shader_foundry_commands_foundry().submit_async(id);
        ASSERT_NE(fence, INVALID_FENCE);

        bool released = false;
        shader_foundry_commands_foundry().release_after_submit([&released] { released = true; });

        shader_foundry_commands_foundry().wait_for_fence(fence);
        shader_foundry_commands_foundry().release_fence(fence);

        const auto next = shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);
        EXPECT_TRUE(released);
        shader_foundry_commands_foundry().submit_and_wait(next);
    }

    TEST_F(ShaderFoundryDeviceTest, WaitForFencesCoversGraphicsAndComputeSubmissions)
    {
        const auto a = host_buffer();
        const auto b = host_buffer();
        clear(*a);
        clear(*b);
        const auto graphics = shader_foundry_commands_foundry().begin_commands(CommandType::GRAPHICS);
        shader_foundry_commands_foundry().get_command_buffer(graphics).fillBuffer(a->buffer(), 0, k_shader_foundry_commands_bytes, 3U);
        const auto compute = shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);
        shader_foundry_commands_foundry().get_command_buffer(compute).fillBuffer(b->buffer(), 0, k_shader_foundry_commands_bytes, 4U);
        const auto graphics_fence = shader_foundry_commands_foundry().submit_async(graphics);
        const auto compute_fence = shader_foundry_commands_foundry().submit_async(compute);
        ASSERT_NE(graphics_fence, INVALID_FENCE);
        ASSERT_NE(compute_fence, INVALID_FENCE);

        shader_foundry_commands_foundry().wait_for_fences({ graphics_fence, compute_fence });

        EXPECT_TRUE(shader_foundry_commands_foundry().is_fence_signaled(graphics_fence));
        EXPECT_TRUE(shader_foundry_commands_foundry().is_fence_signaled(compute_fence));
        EXPECT_EQ(words(*a), std::vector<uint32_t>(k_shader_foundry_commands_words, 3U));
        EXPECT_EQ(words(*b), std::vector<uint32_t>(k_shader_foundry_commands_words, 4U));
        shader_foundry_commands_foundry().release_fence(graphics_fence);
        shader_foundry_commands_foundry().release_fence(compute_fence);
    }

    TEST_F(ShaderFoundryDeviceTest, RetireAfterSubmitFreesBuffersOnceTheirSubmissionCompletes)
    {
        const auto id = shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);
        const auto semaphore = shader_foundry_commands_foundry().submit_with_signal(id);
        ASSERT_NE(semaphore, INVALID_SEMAPHORE);

        const std::array<Portal::Graphics::CommandBufferID, 1> ids { id };
        shader_foundry_commands_foundry().retire_after_submit(ids);
        backend()->get_command_manager().wait(backend()->get_command_manager().submitted());

        const auto next = shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);
        EXPECT_FALSE(shader_foundry_commands_foundry().get_command_buffer(id));
        shader_foundry_commands_foundry().submit_and_wait(next);
    }

    TEST_F(ShaderFoundryDeviceTest, RetireAfterSubmitFreesBuffersThatWereNeverSubmitted)
    {
        const auto id = shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);
        shader_foundry_commands_foundry().end_commands(id);

        const std::array<Portal::Graphics::CommandBufferID, 1> ids { id };
        shader_foundry_commands_foundry().retire_after_submit(ids);

        const auto next = shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);
        EXPECT_FALSE(shader_foundry_commands_foundry().get_command_buffer(id));
        shader_foundry_commands_foundry().submit_and_wait(next);
    }

    TEST_F(ShaderFoundryDeviceTest, RetireAfterSubmitHoldsBuffersWhileTheQueueIsBlocked)
    {
        TimelineGate gate(device());
        const std::array<vk::SemaphoreSubmitInfo, 1> waits { gate.closed() };
        auto& commands = backend()->get_command_manager();

        const auto point = commands.submit(shader_foundry_commands_foundry().get_graphics_queue(), {}, waits);
        ASSERT_TRUE(point);

        const auto id = shader_foundry_commands_foundry().begin_commands(CommandType::GRAPHICS);
        shader_foundry_commands_foundry().end_commands(id);
        const std::array<Portal::Graphics::CommandBufferID, 1> ids { id };
        shader_foundry_commands_foundry().retire_after_submit(ids);

        const auto held = shader_foundry_commands_foundry().begin_commands(CommandType::GRAPHICS);
        EXPECT_TRUE(shader_foundry_commands_foundry().get_command_buffer(id));
        shader_foundry_commands_foundry().end_commands(held);

        gate.open();
        commands.wait(commands.submitted());

        const auto next = shader_foundry_commands_foundry().begin_commands(CommandType::GRAPHICS);
        EXPECT_FALSE(shader_foundry_commands_foundry().get_command_buffer(id));
        shader_foundry_commands_foundry().end_commands(next);
    }

    TEST_F(ShaderFoundryDeviceTest, RetireAfterSubmitIgnoresUnknownIds)
    {
        const std::array<Portal::Graphics::CommandBufferID, 1> ids { 9999 };
        EXPECT_NO_THROW(shader_foundry_commands_foundry().retire_after_submit(ids));
        EXPECT_NO_THROW(shader_foundry_commands_foundry().retire_after_submit({}));
    }

    TEST_F(ShaderFoundryDeviceTest, ReleaseAfterSubmitWaitsForWorkPendingOnEveryQueue)
    {
        TimelineGate gate(device());
        const std::array<vk::SemaphoreSubmitInfo, 1> waits { gate.closed() };
        auto& commands = backend()->get_command_manager();

        const auto compute = commands.submit(shader_foundry_commands_foundry().get_compute_queue(), {}, waits);
        const auto graphics = commands.submit(shader_foundry_commands_foundry().get_graphics_queue(), {}, waits);
        ASSERT_TRUE(compute);
        ASSERT_TRUE(graphics);

        auto released = std::make_shared<bool>(false);
        shader_foundry_commands_foundry().release_after_submit([released] { *released = true; });

        const auto held = shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);
        EXPECT_FALSE(*released);
        shader_foundry_commands_foundry().end_commands(held);

        gate.open();
        commands.wait(commands.submitted());

        const auto next = shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);
        EXPECT_TRUE(*released);
        shader_foundry_commands_foundry().end_commands(next);
    }

    TEST_F(ShaderFoundryDeviceTest, ReleaseAfterSubmitRunsImmediatelyWhenNothingIsPending)
    {
        backend()->wait_idle();

        auto released = std::make_shared<bool>(false);
        shader_foundry_commands_foundry().release_after_submit([released] { *released = true; });

        EXPECT_TRUE(*released);
    }

    TEST_F(ShaderFoundryDeviceTest, ReleaseAfterSubmitLeavesOpenDeferredBatchUntouched)
    {
        backend()->wait_idle();
        auto& commands = backend()->get_command_manager();
        backend()->get_resource_manager().record_deferred_commands([](vk::CommandBuffer) { });
        ASSERT_TRUE(commands.has_deferred_commands());

        bool released = false;
        shader_foundry_commands_foundry().release_after_submit([&released] { released = true; });
        EXPECT_TRUE(commands.has_deferred_commands());
        EXPECT_TRUE(released);

        shader_foundry_commands_foundry().flush_deferred();
        commands.wait(commands.submitted());
        commands.collect();
    }

    TEST_F(ShaderFoundryDeviceTest, WaitSubmittedLeavesDeferredWorkUnsubmitted)
    {
        auto output = host_buffer();
        clear(*output);
        auto& commands = backend()->get_command_manager();
        backend()->get_resource_manager().record_deferred_commands([&output](vk::CommandBuffer cmd) {
            cmd.fillBuffer(output->buffer(), 0, k_shader_foundry_commands_bytes, k_pattern);
            const vk::MemoryBarrier2 to_host {
                vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead
            };
            vk::DependencyInfo dependency {};
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &to_host;
            cmd.pipelineBarrier2(dependency);
        });

        shader_foundry_commands_foundry().wait_submitted();
        EXPECT_TRUE(commands.has_deferred_commands());

        shader_foundry_commands_foundry().flush_deferred();
        commands.wait(commands.submitted());
        EXPECT_EQ(words(*output), std::vector<uint32_t>(k_shader_foundry_commands_words, k_pattern));
    }

    TEST_F(ShaderFoundryDeviceTest, EveryQueueKindHasAQueue)
    {
        EXPECT_TRUE(shader_foundry_commands_foundry().get_graphics_queue());
        EXPECT_TRUE(shader_foundry_commands_foundry().get_compute_queue());
        EXPECT_TRUE(shader_foundry_commands_foundry().get_transfer_queue());
    }

    TEST_F(ShaderFoundryDeviceTest, ShutdownFreesBuffersThatWereNeverSubmitted)
    {
        const auto id = shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);
        ASSERT_TRUE(shader_foundry_commands_foundry().get_command_buffer(id));

        shader_foundry_commands_foundry().shutdown();
        ASSERT_TRUE(shader_foundry_commands_foundry().initialize(backend()));

        EXPECT_FALSE(shader_foundry_commands_foundry().get_command_buffer(id));
    }

    TEST_F(ShaderFoundryDeviceTest, StopFreesRecordingBuffersButKeepsTheFoundryInitialized)
    {
        const auto id = shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);
        ASSERT_TRUE(shader_foundry_commands_foundry().get_command_buffer(id));

        shader_foundry_commands_foundry().stop();

        EXPECT_FALSE(shader_foundry_commands_foundry().get_command_buffer(id));
        EXPECT_TRUE(shader_foundry_commands_foundry().is_initialized());
    }

    // ------------------------------------------------------------------
    // Barriers and timestamps
    // ------------------------------------------------------------------

    TEST_F(ShaderFoundryDeviceTest, BufferBarrierOrdersAFillBeforeACopyOfTheSameBuffer)
    {
        const auto source = host_buffer();
        const auto target = host_buffer();
        clear(*source);
        clear(*target);
        const auto id = shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);
        auto cmd = shader_foundry_commands_foundry().get_command_buffer(id);
        cmd.fillBuffer(source->buffer(), 0, k_shader_foundry_commands_bytes, k_pattern);

        shader_foundry_commands_foundry().buffer_barrier(id, source->buffer(),
            vk::AccessFlagBits2::eTransferWrite, vk::AccessFlagBits2::eTransferRead,
            vk::PipelineStageFlagBits2::eTransfer, vk::PipelineStageFlagBits2::eTransfer);

        const vk::BufferCopy region(0, 0, k_shader_foundry_commands_bytes);
        cmd.copyBuffer(source->buffer(), target->buffer(), region);
        shader_foundry_commands_foundry().submit_and_wait(id);

        EXPECT_EQ(words(*target), std::vector<uint32_t>(k_shader_foundry_commands_words, k_pattern));
    }

    TEST_F(ShaderFoundryDeviceTest, BarriersOnAnUnknownCommandBufferAreIgnored)
    {
        const auto buffer = host_buffer();

        EXPECT_NO_THROW(shader_foundry_commands_foundry().buffer_barrier(9999, buffer->buffer(),
            vk::AccessFlagBits2::eTransferWrite, vk::AccessFlagBits2::eTransferRead,
            vk::PipelineStageFlagBits2::eTransfer, vk::PipelineStageFlagBits2::eTransfer));
        EXPECT_NO_THROW(shader_foundry_commands_foundry().image_barrier(9999, vk::Image {},
            vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
            vk::AccessFlags2 {}, vk::AccessFlagBits2::eShaderRead,
            vk::PipelineStageFlagBits2::eNone, vk::PipelineStageFlagBits2::eComputeShader));
    }

    TEST_F(ShaderFoundryDeviceTest, TimestampsOfASubmittedBufferGiveAValidDuration)
    {
        const auto buffer = host_buffer();
        const auto id = shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);
        shader_foundry_commands_foundry().begin_timestamp(id, "fill");
        shader_foundry_commands_foundry().get_command_buffer(id).fillBuffer(buffer->buffer(), 0, k_shader_foundry_commands_bytes, k_pattern);
        shader_foundry_commands_foundry().end_timestamp(id, "fill");

        const auto fence = shader_foundry_commands_foundry().submit_async(id);
        ASSERT_NE(fence, INVALID_FENCE);
        shader_foundry_commands_foundry().wait_for_fence(fence);
        const auto result = shader_foundry_commands_foundry().get_timestamp_result(id, "fill");

        EXPECT_TRUE(result.valid);
        EXPECT_EQ(result.label, "fill");
        shader_foundry_commands_foundry().release_fence(fence);
    }

    TEST_F(ShaderFoundryDeviceTest, TimestampsAreReadableAfterSubmitAndWait)
    {
        const auto buffer = host_buffer();
        const auto id = shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);
        shader_foundry_commands_foundry().begin_timestamp(id, "fill");
        shader_foundry_commands_foundry().get_command_buffer(id).fillBuffer(buffer->buffer(), 0, k_shader_foundry_commands_bytes, k_pattern);
        shader_foundry_commands_foundry().end_timestamp(id, "fill");

        shader_foundry_commands_foundry().submit_and_wait(id);

        EXPECT_TRUE(shader_foundry_commands_foundry().get_timestamp_result(id, "fill").valid);
    }

    TEST_F(ShaderFoundryDeviceTest, TimestampResultsForUnknownBuffersOrLabelsAreInvalid)
    {
        const auto id = shader_foundry_commands_foundry().begin_commands(CommandType::COMPUTE);
        shader_foundry_commands_foundry().begin_timestamp(id, "known");

        EXPECT_FALSE(shader_foundry_commands_foundry().get_timestamp_result(9999, "known").valid);
        EXPECT_FALSE(shader_foundry_commands_foundry().get_timestamp_result(id, "other").valid);
        EXPECT_NO_THROW(shader_foundry_commands_foundry().begin_timestamp(9999, "x"));
        EXPECT_NO_THROW(shader_foundry_commands_foundry().end_timestamp(9999, "x"));
        EXPECT_NO_THROW(shader_foundry_commands_foundry().end_timestamp(id, "never_begun"));
        shader_foundry_commands_foundry().submit_and_wait(id);
    }

    TEST_F(ShaderFoundryDeviceTest, FlushDeferredSubmitsPendingCommandsOnce)
    {
        backend()->get_resource_manager().record_deferred_commands([](vk::CommandBuffer) { });

        const auto point = shader_foundry_commands_foundry().flush_deferred();
        EXPECT_TRUE(point);
        EXPECT_FALSE(shader_foundry_commands_foundry().flush_deferred());
        EXPECT_FALSE(backend()->get_command_manager().has_deferred_commands());
        backend()->get_command_manager().wait(point);
    }

} // namespace
} // namespace MayaFlux::Test
