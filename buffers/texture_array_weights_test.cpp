#include "gtest/gtest.h"

#include <array>
#include <memory>
#include <vector>

#include "../support/scripted_window.hpp"
#include "../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/Staging/StagingUtils.hpp"
#include "MayaFlux/Buffers/Textures/TextureArrayBuffer.hpp"
#include "MayaFlux/Portal/Graphics/SamplerForge.hpp"
#include "MayaFlux/Portal/Graphics/ShaderFoundry.hpp"
#include "MayaFlux/Portal/Graphics/TextureLoom.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::ProcessingToken;
    using Buffers::TextureArrayBuffer;
    using Portal::Graphics::ImageFormat;
    using Portal::Graphics::RenderConfig;

    class TextureArrayWeightsTest : public VulkanDeviceTest {
    protected:
        static void SetUpTestSuite()
        {
            VulkanDeviceTest::SetUpTestSuite();
            if (!vulkan_unavailable_reason().empty())
                return;

            s_backend_ref = std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) { });
            auto& foundry = Portal::Graphics::get_shader_foundry();
            foundry.shutdown();
            s_foundry_ready = foundry.initialize(s_backend_ref);
            if (!s_foundry_ready)
                return;

            auto& loom = Portal::Graphics::get_texture_manager();
            loom.shutdown();
            s_texture_ready = loom.initialize(s_backend_ref);
        }

        static void TearDownTestSuite()
        {
            Portal::Graphics::SamplerForge::instance().shutdown();
            if (s_texture_ready)
                Portal::Graphics::get_texture_manager().shutdown();
            if (s_foundry_ready)
                Portal::Graphics::get_shader_foundry().shutdown();
            s_texture_ready = false;
            s_foundry_ready = false;
            s_backend_ref.reset();
            VulkanDeviceTest::TearDownTestSuite();
        }

        void SetUp() override
        {
            VulkanDeviceTest::SetUp();
            if (IsSkipped())
                return;
            ASSERT_TRUE(s_foundry_ready);
            ASSERT_TRUE(s_texture_ready);

            Portal::Graphics::SamplerForge::instance().shutdown();
            ASSERT_TRUE(Portal::Graphics::SamplerForge::instance().initialize(s_backend_ref));

            service =Registry::BackendRegistry::instance()
                          .get_service<Registry::Service::BufferService>();
            ASSERT_NE(service, nullptr);

            window = std::make_shared<ScriptedWindow>(64, 64);
        }

        void TearDown() override
        {
            for (auto& array : arrays) {
                if (auto weights = array->get_weights_buffer(); weights && weights->is_initialized() && service)
                    service->destroy_buffer(weights);
            }
            arrays.clear();
            window.reset();
        }

        std::shared_ptr<TextureArrayBuffer> make_array(uint32_t layers, bool rendered = true)
        {
            auto array = std::make_shared<TextureArrayBuffer>(4U, 2U, ImageFormat::RGBA8, layers);
            arrays.push_back(array);
            if (rendered)
                render(*array);
            return array;
        }

        void render(TextureArrayBuffer& array)
        {
            array.setup_processors(ProcessingToken::GRAPHICS_BACKEND);
            RenderConfig config;
            config.target_window = window;
            array.setup_rendering(config);
        }

        static std::vector<float> read_weights(const TextureArrayBuffer& array)
        {
            std::vector<float> values;
            const auto buffer = array.get_weights_buffer();
            if (buffer)
                Buffers::download_from_gpu(buffer, values);
            return values;
        }

        Registry::Service::BufferService* service { nullptr };
        std::shared_ptr<ScriptedWindow> window;
        std::vector<std::shared_ptr<TextureArrayBuffer>> arrays;

        static inline std::shared_ptr<Core::VulkanBackend> s_backend_ref;
        static inline bool s_foundry_ready { false };
        static inline bool s_texture_ready { false };
    };

    TEST_F(TextureArrayWeightsTest, WeightsBufferHasOneFloatPerLayerAndStartsAtOne)
    {
        const auto array = make_array(40);

        ASSERT_NE(array->get_weights_buffer(), nullptr);
        EXPECT_TRUE(array->get_weights_buffer()->is_initialized());

        const auto values = read_weights(*array);
        ASSERT_EQ(values.size(), 40U);
        for (const float value : values)
            EXPECT_FLOAT_EQ(value, 1.0F);
    }

    TEST_F(TextureArrayWeightsTest, OneLayerArrayStillHasTheHistoryShareSlot)
    {
        const auto array = make_array(1);

        const auto values = read_weights(*array);
        ASSERT_EQ(values.size(), 2U);
        EXPECT_FLOAT_EQ(values[1], 1.0F);
    }

    TEST_F(TextureArrayWeightsTest, DeviceAddressIsNonZeroAndSurvivesWeightWrites)
    {
        const auto array = make_array(4);
        const auto buffer = array->get_weights_buffer();
        ASSERT_NE(buffer, nullptr);

        const uint64_t address = buffer->get_device_address();
        EXPECT_NE(address, 0U);

        const std::array<float, 2> weights { 0.5F, 0.25F };
        array->set_weights(weights);

        EXPECT_EQ(array->get_weights_buffer(), buffer);
        EXPECT_EQ(buffer->get_device_address(), address);
    }

    TEST_F(TextureArrayWeightsTest, SetWeightsWritesTheFirstLayersAndLeavesTheRest)
    {
        const auto array = make_array(4);

        const std::array<float, 3> weights { 0.5F, 0.25F, 0.125F };
        array->set_weights(weights);

        const auto values = read_weights(*array);
        ASSERT_EQ(values.size(), 4U);
        EXPECT_FLOAT_EQ(values[0], 0.5F);
        EXPECT_FLOAT_EQ(values[1], 0.25F);
        EXPECT_FLOAT_EQ(values[2], 0.125F);
        EXPECT_FLOAT_EQ(values[3], 1.0F);
    }

    TEST_F(TextureArrayWeightsTest, SetWeightPastTheOldThirtySlotLimitReachesTheBuffer)
    {
        const auto array = make_array(40);

        array->set_weight(35, 0.75F);

        const auto values = read_weights(*array);
        ASSERT_EQ(values.size(), 40U);
        EXPECT_FLOAT_EQ(values[35], 0.75F);
        EXPECT_FLOAT_EQ(values[34], 1.0F);
    }

    TEST_F(TextureArrayWeightsTest, SetWeightOutsideTheLayerCountIsIgnored)
    {
        const auto array = make_array(4);

        array->set_weight(4, 0.5F);

        const auto values = read_weights(*array);
        ASSERT_EQ(values.size(), 4U);
        for (const float value : values)
            EXPECT_FLOAT_EQ(value, 1.0F);
    }

    TEST_F(TextureArrayWeightsTest, WeightSetBeforeRenderingIsUploadedWhenTheBufferIsCreated)
    {
        const auto array = make_array(4, false);

        array->set_weight(2, 0.3F);
        EXPECT_EQ(array->get_weights_buffer(), nullptr);

        render(*array);

        const auto values = read_weights(*array);
        ASSERT_EQ(values.size(), 4U);
        EXPECT_FLOAT_EQ(values[2], 0.3F);
        EXPECT_FLOAT_EQ(values[0], 1.0F);
    }

}
} // namespace MayaFlux::Test
