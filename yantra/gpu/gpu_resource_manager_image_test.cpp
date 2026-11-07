#include "gtest/gtest.h"

#include "../../support/gpu_dispatch_test_support.hpp"

#include "MayaFlux/Core/Backends/Graphics/Vulkan/VKImage.hpp"
#include "MayaFlux/Portal/Graphics/SamplerForge.hpp"
#include "MayaFlux/Portal/Graphics/TextureLoom.hpp"

namespace MayaFlux::Test {
namespace {

    using namespace GpuDispatch;
    using Yantra::GpuDispatchRecorder;
    using Yantra::GpuResourceManager;

    constexpr uint32_t k_width = 16;
    constexpr size_t k_row_bytes = static_cast<size_t>(k_width) * 4;
    constexpr std::array<uint32_t, 3> k_one { 1, 1, 1 };

    constexpr std::string_view k_paint_source = R"(#version 450
layout(local_size_x = 16) in;
layout(set = 0, binding = 0, rgba8) uniform writeonly image2D out_image;
void main()
{
    imageStore(out_image, ivec2(gl_GlobalInvocationID.x, 0), vec4(1.0));
}
)";

    Portal::Graphics::GpuBufferBinding storage_image_binding(uint32_t binding)
    {
        Portal::Graphics::GpuBufferBinding out;
        out.set = 0;
        out.binding = binding;
        out.element_type = Portal::Graphics::GpuBufferBinding::ElementType::IMAGE_STORAGE;
        return out;
    }

    template <bool Dedicated>
    class GpuImageFixture : public GpuDispatchFixture<Dedicated> {
    protected:
        void SetUp() override
        {
            GpuDispatchFixture<Dedicated>::SetUp();
            if (this->IsSkipped() || this->HasFatalFailure()) {
                return;
            }
            ASSERT_TRUE(Portal::Graphics::get_texture_manager().initialize(this->backend()));
            ASSERT_TRUE(Portal::Graphics::SamplerForge::instance().initialize(this->backend()));
        }

        void TearDown() override
        {
            if (this->backend()) {
                Portal::Graphics::SamplerForge::instance().shutdown();
                Portal::Graphics::get_texture_manager().shutdown();
            }
            GpuDispatchFixture<Dedicated>::TearDown();
        }
    };

    template <typename Fixture>
    class GpuImageManagerTest : public Fixture {
    };

    using ImageHardwareOptions = ::testing::Types<GpuImageFixture<true>, GpuImageFixture<false>>;
    TYPED_TEST_SUITE(GpuImageManagerTest, ImageHardwareOptions);

    TYPED_TEST(GpuImageManagerTest, TransitionImageUpdatesTheTrackedLayoutWithAndWithoutARecording)
    {
        for (const bool recorded : { false, true }) {
            SCOPED_TRACE(recorded ? "recorded" : "immediate");
            auto recording = std::make_shared<GpuDispatchRecorder>(false);
            GpuResourceManager manager(false);
            manager.set_recording(recorded ? recording : nullptr);
            auto image = Portal::Graphics::get_texture_manager().create_storage_image(k_width, 1);
            ASSERT_NE(image, nullptr);

            manager.transition_image(image, vk::ImageLayout::eGeneral);
            EXPECT_EQ(image->get_current_layout(), vk::ImageLayout::eGeneral);
            manager.transition_image(image, vk::ImageLayout::eShaderReadOnlyOptimal);
            EXPECT_EQ(image->get_current_layout(), vk::ImageLayout::eShaderReadOnlyOptimal);

            if (recorded) {
                EXPECT_TRUE(recording->is_open());
                recording->wait();
            }
        }
    }

    TYPED_TEST(GpuImageManagerTest, TransitionToTheCurrentLayoutDoesNotOpenARecording)
    {
        auto recording = std::make_shared<GpuDispatchRecorder>(false);
        GpuResourceManager manager(false);
        manager.set_recording(recording);
        auto image = Portal::Graphics::get_texture_manager().create_storage_image(k_width, 1);
        ASSERT_NE(image, nullptr);

        manager.transition_image(image, vk::ImageLayout::eShaderReadOnlyOptimal);
        recording->wait();
        ASSERT_FALSE(recording->is_open());

        manager.transition_image(image, vk::ImageLayout::eShaderReadOnlyOptimal);
        EXPECT_FALSE(recording->is_open());
    }

    TYPED_TEST(GpuImageManagerTest, IsolatedManagerRefusesImageTransitions)
    {
        GpuResourceManager manager(true);
        auto image = Portal::Graphics::get_texture_manager().create_storage_image(k_width, 1);
        ASSERT_NE(image, nullptr);

        EXPECT_THROW(manager.transition_image(image, vk::ImageLayout::eGeneral), std::invalid_argument);
    }

    TYPED_TEST(GpuImageManagerTest, StorageImageBoundThroughBindImagesBatchIsWrittenByTheKernel)
    {
        for (const bool recorded : { false, true }) {
            SCOPED_TRACE(recorded ? "recorded" : "immediate");
            auto recording = std::make_shared<GpuDispatchRecorder>(false);
            GpuResourceManager manager(false);
            manager.set_recording(recorded ? recording : nullptr);

            const auto shader = load_compute(k_paint_source);
            ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);
            const auto binding = storage_image_binding(0);
            ASSERT_TRUE(manager.initialise("paint", compute_config(shader, 0), { binding }));

            auto image = Portal::Graphics::get_texture_manager().create_storage_image(k_width, 1);
            ASSERT_NE(image, nullptr);
            manager.transition_image(image, vk::ImageLayout::eGeneral);
            manager.bind_images_batch("paint", { GpuResourceManager::ImageBind {
                                                   .index = 0,
                                                   .image = image,
                                                   .sampler = vk::Sampler {},
                                                   .spec = binding,
                                               } });
            manager.record_stage("paint", k_one, {}, {});
            if (recorded) {
                recording->wait();
            }

            std::vector<uint8_t> pixels(k_row_bytes, 0);
            Portal::Graphics::get_texture_manager().download_data(image, pixels.data(), pixels.size());
            EXPECT_EQ(pixels, std::vector<uint8_t>(k_row_bytes, 255));
        }
    }

} // namespace
} // namespace MayaFlux::Test
