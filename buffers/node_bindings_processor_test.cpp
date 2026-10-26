#include "gtest/gtest.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/Node/NodeBindingsProcessor.hpp"
#include "MayaFlux/Nodes/Conduit/Constant.hpp"
#include "MayaFlux/Nodes/Generators/Sine.hpp"
#include "MayaFlux/Portal/Graphics/ShaderFoundry.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::NodeBindingsProcessor;
    using Buffers::ShaderConfig;
    using Buffers::VKBuffer;
    using BindingMode = NodeBindingsProcessor::ProcessingMode;

    constexpr const char* k_push_constant_shader = R"(#version 460
layout(local_size_x = 1) in;
layout(push_constant) uniform Constants { float a; float b; float c; float d; } pc;
layout(set = 0, binding = 0, std430) buffer Data { float values[]; } data;
void main() {
    data.values[0] = pc.a + pc.b + pc.c + pc.d;
}
)";

    class NodeBindingsShaderFile {
    public:
        explicit NodeBindingsShaderFile(const void* owner)
            : m_path(std::filesystem::temp_directory_path()
                  / ("mayaflux_node_bindings_" + std::to_string(reinterpret_cast<std::uintptr_t>(owner)) + ".comp"))
        {
            std::ofstream file(m_path);
            file << k_push_constant_shader;
        }

        NodeBindingsShaderFile(const NodeBindingsShaderFile&) = delete;
        NodeBindingsShaderFile& operator=(const NodeBindingsShaderFile&) = delete;
        NodeBindingsShaderFile(NodeBindingsShaderFile&&) = delete;
        NodeBindingsShaderFile& operator=(NodeBindingsShaderFile&&) = delete;

        ~NodeBindingsShaderFile()
        {
            std::error_code error;
            std::filesystem::remove(m_path, error);
        }

        [[nodiscard]] std::string path() const { return m_path.string(); }

    private:
        std::filesystem::path m_path;
    };

    class NodeBindingsProcessorDeviceTest : public VulkanDeviceTest {
    protected:
        void SetUp() override
        {
            VulkanDeviceTest::SetUp();
            if (IsSkipped())
                return;

            Portal::Graphics::get_shader_foundry().shutdown();
            auto borrowed = std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) {});
            ASSERT_TRUE(Portal::Graphics::get_shader_foundry().initialize(borrowed));

            shader = std::make_unique<NodeBindingsShaderFile>(this);

            ShaderConfig config { shader->path() };
            config.stage = Portal::Graphics::ShaderStage::COMPUTE;
            processor = std::make_shared<NodeBindingsProcessor>(std::move(config));

            target = std::make_shared<VKBuffer>(
                sizeof(float) * 4, VKBuffer::Usage::HOST_STORAGE, Kakshya::DataModality::UNKNOWN);
            auto* service = Registry::BackendRegistry::instance()
                                .get_service<Registry::Service::BufferService>();
            ASSERT_NE(service, nullptr);
            service->initialize_buffer(target);
        }

        void TearDown() override
        {
            auto* service = Registry::BackendRegistry::instance()
                                .get_service<Registry::Service::BufferService>();
            processor.reset();
            if (target && target->is_initialized() && service)
                service->destroy_buffer(target);
            target.reset();
            shader.reset();

            Portal::Graphics::get_shader_foundry().shutdown();
            VulkanDeviceTest::TearDown();
        }

        void tick() { processor->processing_function(target); }

        auto& staged() { return target->get_pipeline_context().push_constant_bindings; }

        template <typename T>
        std::optional<T> staged_value(uint32_t offset)
        {
            for (const auto& entry : staged()) {
                if (entry.offset == offset && entry.data.size() == sizeof(T)) {
                    T value {};
                    std::memcpy(&value, entry.data.data(), sizeof(value));
                    return value;
                }
            }
            return std::nullopt;
        }

        std::unique_ptr<NodeBindingsShaderFile> shader;
        std::shared_ptr<NodeBindingsProcessor> processor;
        std::shared_ptr<VKBuffer> target;
    };

    // ------------------------------------------------------------------
    // Staging
    // ------------------------------------------------------------------

    TEST_F(NodeBindingsProcessorDeviceTest, AFloatBindingStagesTheNodeValueAtItsOffsetUnderItsName)
    {
        auto node = std::make_shared<Nodes::Constant>(2.5);
        processor->bind_node("gain", node, 8);

        tick();

        ASSERT_EQ(staged().size(), 1U);
        EXPECT_EQ(staged().front().offset, 8U);
        EXPECT_EQ(staged().front().name, "gain");
        EXPECT_EQ(staged_value<float>(8), 2.5F);
    }

    TEST_F(NodeBindingsProcessorDeviceTest, ADoubleSizedBindingStagesAFullDouble)
    {
        auto node = std::make_shared<Nodes::Constant>(1.25);
        processor->bind_node("wide", node, 0, sizeof(double));

        tick();

        EXPECT_EQ(staged_value<double>(0), 1.25);
    }

    TEST_F(NodeBindingsProcessorDeviceTest, ASizeOtherThanFloatOrDoubleStagesZeroedBytesOfThatSize)
    {
        auto node = std::make_shared<Nodes::Constant>(7.0);
        processor->bind_node("odd", node, 4, 2);

        tick();

        ASSERT_EQ(staged().size(), 1U);
        EXPECT_EQ(staged().front().data, (std::vector<uint8_t> { 0, 0 }));
    }

    TEST_F(NodeBindingsProcessorDeviceTest, SeveralBindingsStageOneEntryEach)
    {
        auto first = std::make_shared<Nodes::Constant>(1.0);
        auto second = std::make_shared<Nodes::Constant>(2.0);
        auto wide = std::make_shared<Nodes::Constant>(3.0);
        processor->bind_node("first", first, 8);
        processor->bind_node("second", second, 12);
        processor->bind_node("wide", wide, 0, sizeof(double));

        tick();

        EXPECT_EQ(staged().size(), 3U);
        EXPECT_EQ(staged_value<float>(8), 1.0F);
        EXPECT_EQ(staged_value<float>(12), 2.0F);
        EXPECT_EQ(staged_value<double>(0), 3.0);
    }

    TEST_F(NodeBindingsProcessorDeviceTest, AnEntryIsRewrittenEveryTickNotDuplicated)
    {
        auto node = std::make_shared<Nodes::Constant>(1.0);
        processor->bind_node("gain", node, 8);

        tick();
        node->set_constant(-4.5);
        tick();
        tick();

        ASSERT_EQ(staged().size(), 1U);
        EXPECT_EQ(staged_value<float>(8), -4.5F);
    }

    TEST_F(NodeBindingsProcessorDeviceTest, BindingsAtOneOffsetShareOneStagedEntry)
    {
        auto first = std::make_shared<Nodes::Constant>(1.0);
        auto second = std::make_shared<Nodes::Constant>(2.0);
        processor->bind_node("first", first, 8);
        processor->bind_node("second", second, 8);

        tick();

        EXPECT_EQ(staged().size(), 1U);
    }

    TEST_F(NodeBindingsProcessorDeviceTest, AnEntryAlreadyStagedAtTheOffsetIsOverwrittenAndKeepsItsName)
    {
        staged().push_back(Portal::Graphics::PushConstantBindingInfo {
            .offset = 8, .data = std::vector<uint8_t>(sizeof(float), 0xFF), .name = "other" });
        auto node = std::make_shared<Nodes::Constant>(6.0);
        processor->bind_node("gain", node, 8);

        tick();

        ASSERT_EQ(staged().size(), 1U);
        EXPECT_EQ(staged().front().name, "other");
        EXPECT_EQ(staged_value<float>(8), 6.0F);
    }

    TEST_F(NodeBindingsProcessorDeviceTest, WithNoBindingsNothingIsStaged)
    {
        tick();

        EXPECT_TRUE(staged().empty());
    }

    TEST_F(NodeBindingsProcessorDeviceTest, ANullNodeStillStagesItsSlotAsZeroes)
    {
        processor->bind_node("missing", nullptr, 8);

        tick();

        ASSERT_EQ(staged().size(), 1U);
        EXPECT_EQ(staged_value<float>(8), 0.0F);
    }

    // ------------------------------------------------------------------
    // Processing modes
    // ------------------------------------------------------------------

    TEST_F(NodeBindingsProcessorDeviceTest, ExternalModeReadsTheNodesLastOutputInsteadOfProcessingIt)
    {
        auto node = std::make_shared<Nodes::Generator::Sine>(440.0F, 1.0);
        for (int i = 0; i < 10; ++i)
            node->process_sample();
        const double last = node->get_last_output();
        processor->bind_node("gain", node, 8, sizeof(float), BindingMode::EXTERNAL);

        tick();
        tick();
        EXPECT_EQ(staged_value<float>(8), static_cast<float>(last));
        EXPECT_DOUBLE_EQ(node->get_last_output(), last);

        processor->set_processing_mode("gain", BindingMode::INTERNAL);
        tick();
        EXPECT_NE(node->get_last_output(), last);
        EXPECT_EQ(staged_value<float>(8), static_cast<float>(node->get_last_output()));
    }

    TEST_F(NodeBindingsProcessorDeviceTest, ProcessingModesDefaultToInternalAndCanBeSetPerBindingOrForAll)
    {
        auto node = std::make_shared<Nodes::Constant>(1.0);
        processor->bind_node("first", node, 8);
        processor->bind_node("second", node, 12);

        EXPECT_EQ(processor->get_processing_mode("first"), BindingMode::INTERNAL);
        EXPECT_EQ(processor->get_processing_mode("second"), BindingMode::INTERNAL);

        processor->set_processing_mode("first", BindingMode::EXTERNAL);
        EXPECT_EQ(processor->get_processing_mode("first"), BindingMode::EXTERNAL);
        EXPECT_EQ(processor->get_processing_mode("second"), BindingMode::INTERNAL);

        processor->set_processing_mode(BindingMode::EXTERNAL);
        EXPECT_EQ(processor->get_processing_mode("first"), BindingMode::EXTERNAL);
        EXPECT_EQ(processor->get_processing_mode("second"), BindingMode::EXTERNAL);

        EXPECT_EQ(processor->get_processing_mode("missing"), BindingMode::INTERNAL);
        EXPECT_NO_THROW(processor->set_processing_mode("missing", BindingMode::EXTERNAL));
        EXPECT_FALSE(processor->has_binding("missing"));
    }

    TEST_F(NodeBindingsProcessorDeviceTest, ABindingModeIsKeptWhenTheBindingIsGiven)
    {
        auto node = std::make_shared<Nodes::Constant>(1.0);
        processor->bind_node("gain", node, 8, sizeof(float), BindingMode::EXTERNAL);

        EXPECT_EQ(processor->get_processing_mode("gain"), BindingMode::EXTERNAL);
    }

    // ------------------------------------------------------------------
    // Bookkeeping
    // ------------------------------------------------------------------

    TEST_F(NodeBindingsProcessorDeviceTest, BindingNamesAndMembershipFollowBindAndUnbind)
    {
        auto node = std::make_shared<Nodes::Constant>(1.0);
        processor->bind_node("first", node, 8);
        processor->bind_node("second", node, 12);

        auto names = processor->get_binding_names();
        std::ranges::sort(names);
        EXPECT_EQ(names, (std::vector<std::string> { "first", "second" }));
        EXPECT_TRUE(processor->has_binding("first"));
        EXPECT_FALSE(processor->has_binding("missing"));

        processor->unbind_node("first");

        EXPECT_FALSE(processor->has_binding("first"));
        EXPECT_EQ(processor->get_binding_names(), std::vector<std::string> { "second" });
        EXPECT_NO_THROW(processor->unbind_node("first"));
        EXPECT_NO_THROW(processor->unbind_node("missing"));
    }

    TEST_F(NodeBindingsProcessorDeviceTest, ABindingStopsSamplingItsNodeOnceUnbound)
    {
        auto node = std::make_shared<Nodes::Generator::Sine>(440.0F, 1.0);
        processor->bind_node("gain", node, 8);
        tick();
        const double after_first = node->get_last_output();

        processor->unbind_node("gain");
        tick();

        EXPECT_DOUBLE_EQ(node->get_last_output(), after_first);
    }

    TEST_F(NodeBindingsProcessorDeviceTest, BindingAnExistingNameReplacesItsNodeOffsetAndSize)
    {
        auto first = std::make_shared<Nodes::Constant>(1.0);
        auto second = std::make_shared<Nodes::Constant>(9.0);
        processor->bind_node("gain", first, 8);

        processor->bind_node("gain", second, 12);
        tick();

        EXPECT_EQ(processor->get_binding_names(), std::vector<std::string> { "gain" });
        ASSERT_EQ(staged().size(), 1U);
        EXPECT_EQ(staged().front().offset, 12U);
        EXPECT_EQ(staged_value<float>(12), 9.0F);
    }

}
}
