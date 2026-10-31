#include "gtest/gtest.h"


#include "../../support/native_window_test_support.hpp"
#include "../../support/scripted_window.hpp"
#include "../../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/Network/MeshNetworkBuffer.hpp"
#include "MayaFlux/Buffers/Root/RootGraphicsBuffer.hpp"
#include "MayaFlux/Buffers/Shaders/RenderProcessor.hpp"
#include "MayaFlux/Buffers/Staging/StagingUtils.hpp"
#include "MayaFlux/Nodes/Network/MeshNetwork.hpp"
#include "MayaFlux/Nodes/Network/Operators/MeshFieldOperator.hpp"
#include "MayaFlux/Nodes/Network/Operators/MeshTransformOperator.hpp"
#include "MayaFlux/Portal/Graphics/ComputePress.hpp"
#include "MayaFlux/Portal/Graphics/RenderFlow.hpp"
#include "MayaFlux/Portal/Graphics/ShaderFoundry.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"
#include "MayaFlux/Registry/Service/DisplayService.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::MeshNetworkBuffer;
    using Buffers::ProcessingToken;
    using Nodes::MeshVertex;
    using Nodes::GpuSync::MeshWriterNode;
    using Nodes::Network::MeshNetwork;
    MeshVertex vertex(
        glm::vec3 position,
        glm::vec3 color,
        float weight,
        glm::vec2 uv,
        glm::vec3 normal,
        glm::vec3 tangent)
    {
        MeshVertex result;
        result.position = position;
        result.color = glm::vec4(color, 1.0F);
        result.weight = weight;
        result.uv = uv;
        result.normal = normal;
        result.tangent = glm::vec4(tangent, 1.0F);
        return result;
    }

    std::shared_ptr<MeshWriterNode> make_mesh(
        std::vector<MeshVertex> vertices,
        std::vector<uint32_t> indices)
    {
        auto node = std::make_shared<MeshWriterNode>(vertices.size());
        node->set_mesh(vertices, indices);
        node->compute_frame();
        return node;
    }

    std::vector<MeshVertex> triangle(float weight, glm::vec3 color)
    {
        return {
            vertex({ -0.25F, -0.25F, 0.0F }, color, weight, { 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 1.0F, 0.0F, 0.0F }),
            vertex({ 0.25F, -0.25F, 0.0F }, color, weight, { 1.0F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 1.0F, 0.0F, 0.0F }),
            vertex({ 0.0F, 0.25F, 0.0F }, color, weight, { 0.5F, 1.0F }, { 0.0F, 0.0F, 1.0F }, { 1.0F, 0.0F, 0.0F })
        };
    }

    std::vector<uint8_t> bytes_of(std::span<const MeshVertex> vertices)
    {
        const auto* begin = reinterpret_cast<const uint8_t*>(vertices.data());
        return { begin, begin + vertices.size_bytes() };
    }

    std::vector<uint32_t> indices_from_bytes(const std::vector<uint8_t>& bytes)
    {
        std::vector<uint32_t> indices(bytes.size() / sizeof(uint32_t));
        std::memcpy(indices.data(), bytes.data(), bytes.size());
        return indices;
    }

    void* vulkan_handle(vk::Buffer buffer)
    {
        return static_cast<void*>(static_cast<VkBuffer>(buffer));
    }

    class MeshNetworkBufferDeviceTest : public VulkanDeviceTest {
    protected:
        static void SetUpTestSuite()
        {
            VulkanDeviceTest::SetUpTestSuite();
            if (!vulkan_unavailable_reason().empty())
                return;

            auto borrowed = std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) { });
            auto& foundry = Portal::Graphics::get_shader_foundry();
            foundry.shutdown();
            s_foundry_ready = foundry.initialize(borrowed);
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

            service = Registry::BackendRegistry::instance()
                          .get_service<Registry::Service::BufferService>();
            ASSERT_NE(service, nullptr);
        }

        void TearDown() override
        {
            if (buffer) {
                if (auto render = buffer->get_render_processor()) {
                    if (auto chain = buffer->get_processing_chain())
                        chain->remove_processor_anywhere(render, buffer);
                }
                buffer->set_default_processor(nullptr);
                if (buffer->is_initialized() && service)
                    service->destroy_buffer(buffer);
            }
            buffer.reset();
            network.reset();
            window.reset();
        }

        void attach(std::shared_ptr<MeshNetwork> value)
        {
            network = std::move(value);
            buffer = std::make_shared<MeshNetworkBuffer>(network);
            service->initialize_buffer(buffer);
            ASSERT_TRUE(buffer->is_initialized());
            buffer->setup_processors(ProcessingToken::GRAPHICS_BACKEND);
            ASSERT_NE(buffer->get_mesh_processor(), nullptr);
            buffer->process_default();
        }

        void process_dirty_network()
        {
            for (auto& slot : network->slots())
                slot.dirty = true;
            buffer->process_default();
        }

        std::vector<uint8_t> read_vertices(size_t byte_count)
        {
            std::vector<uint8_t> bytes(buffer->get_size_bytes());
            Buffers::download_from_gpu(buffer, bytes.data(), bytes.size());
            bytes.resize(byte_count);
            return bytes;
        }

        std::vector<uint8_t> read_raw_buffer(vk::Buffer source, size_t byte_count)
        {
            RawVulkanBuffer destination(
                backend(), byte_count, vk::BufferUsageFlagBits::eTransferDst, true);
            service->copy_buffer(
                vulkan_handle(source), vulkan_handle(destination.buffer()), byte_count, 0, 0);
            const auto* data = static_cast<const uint8_t*>(destination.mapped());
            return { data, data + byte_count };
        }

        std::vector<uint8_t> read_index_bytes(size_t byte_count)
        {
            return read_raw_buffer(buffer->get_index_buffer(), byte_count);
        }

        void setup_rendering()
        {
            window = std::make_shared<ScriptedWindow>(320, 240);
            buffer->setup_rendering(MeshNetworkBuffer::RenderConfig { .target_window = window });
        }

        static inline bool s_foundry_ready {};
        Registry::Service::BufferService* service {};
        std::shared_ptr<MeshNetwork> network;
        std::shared_ptr<MeshNetworkBuffer> buffer;
        std::shared_ptr<ScriptedWindow> window;
    };

    TEST_F(MeshNetworkBufferDeviceTest, UploadsEveryVertexAttributeAndRebasesEachSlotsIndices)
    {
        auto first_vertices = std::vector<MeshVertex> {
            vertex({ 1, 2, 3 }, { 0.1F, 0.2F, 0.3F }, 0.125F, { 0.2F, 0.4F }, { 0, 0, 1 }, { 1, 0, 0 }),
            vertex({ 4, 5, 6 }, { 0.4F, 0.5F, 0.6F }, 0.25F, { 0.6F, 0.8F }, { 0, 1, 0 }, { 0, 1, 0 }),
            vertex({ 7, 8, 9 }, { 0.7F, 0.8F, 0.9F }, 0.5F, { 1.0F, 0.3F }, { 1, 0, 0 }, { 0, 0, 1 })
        };
        auto second_vertices = std::vector<MeshVertex> {
            vertex({ -1, -2, -3 }, { 0.9F, 0.2F, 0.1F }, 0.625F, { 0.1F, 0.9F }, { 0, -1, 0 }, { -1, 0, 0 }),
            vertex({ -4, -5, -6 }, { 0.6F, 0.5F, 0.4F }, 0.75F, { 0.7F, 0.2F }, { -1, 0, 0 }, { 0, -1, 0 }),
            vertex({ -7, -8, -9 }, { 0.3F, 0.8F, 0.7F }, 1.0F, { 0.4F, 0.6F }, { 0, 0, -1 }, { 0, 0, -1 })
        };

        auto network = std::make_shared<MeshNetwork>();
        network->add_slot("front", make_mesh(first_vertices, { 2, 0, 1 }));
        network->add_slot("back", make_mesh(second_vertices, { 1, 2, 0 }));
        attach(network);

        std::vector<MeshVertex> expected_vertices = first_vertices;
        expected_vertices.insert(expected_vertices.end(), second_vertices.begin(), second_vertices.end());
        EXPECT_EQ(read_vertices(expected_vertices.size() * sizeof(MeshVertex)), bytes_of(expected_vertices));
        EXPECT_EQ(indices_from_bytes(read_index_bytes(6U * sizeof(uint32_t))),
            (std::vector<uint32_t> { 2, 0, 1, 4, 5, 3 }));

        const auto layout = buffer->get_vertex_layout();
        ASSERT_TRUE(layout.has_value());
        EXPECT_EQ(layout->stride_bytes, sizeof(MeshVertex));
        EXPECT_EQ(layout->vertex_count, 6U);
        EXPECT_FLOAT_EQ(expected_vertices[0].weight, 0.125F);
        EXPECT_FLOAT_EQ(expected_vertices[3].weight, 0.625F);
    }

    TEST_F(MeshNetworkBufferDeviceTest, InitialAttachUploadsAndPublishesTheTransformAndVertexSlotSsboData)
    {
        auto network = std::make_shared<MeshNetwork>();
        network->add_slot("root", make_mesh(triangle(0.2F, { 1, 0, 0 }), { 0, 1, 2 }));
        network->add_slot("child", make_mesh(triangle(0.8F, { 0, 1, 0 }), { 0, 1, 2 }), 0U);
        network->get_slot(0).local_transform[3] = glm::vec4(-0.5F, 0.25F, 0.0F, 1.0F);
        network->get_slot(1).local_transform[3] = glm::vec4(1.0F, -0.5F, 0.0F, 1.0F);
        network->process_batch(1);
        attach(network);

        const auto& bindings = buffer->get_engine_context().ssbo_bindings;
        ASSERT_EQ(bindings.size(), 2U);
        const auto models = std::ranges::find_if(bindings, [](const auto& binding) {
            return binding.binding == 1;
        });
        const auto slots = std::ranges::find_if(bindings, [](const auto& binding) {
            return binding.binding == 2;
        });
        ASSERT_NE(models, bindings.end());
        ASSERT_NE(slots, bindings.end());

        std::array<glm::mat4, 2> uploaded_models;
        const auto model_bytes = read_raw_buffer(models->buffer_info.buffer, models->buffer_info.range);
        ASSERT_EQ(model_bytes.size(), sizeof(uploaded_models));
        std::memcpy(uploaded_models.data(), model_bytes.data(), model_bytes.size());
        EXPECT_FLOAT_EQ(uploaded_models[0][3].x, -0.5F);
        EXPECT_FLOAT_EQ(uploaded_models[0][3].y, 0.25F);
        EXPECT_FLOAT_EQ(uploaded_models[1][3].x, 0.5F);
        EXPECT_FLOAT_EQ(uploaded_models[1][3].y, -0.25F);

        const auto slot_bytes = read_raw_buffer(slots->buffer_info.buffer, slots->buffer_info.range);
        EXPECT_EQ(indices_from_bytes(slot_bytes), (std::vector<uint32_t> { 0, 0, 0, 1, 1, 1 }));
    }

    TEST_F(MeshNetworkBufferDeviceTest, ProcessedNetworkFieldsReachVertexAndTransformSsboUploads)
    {
        auto root_mesh = make_mesh(triangle(0.1F, { 1, 0, 0 }), { 0, 1, 2 });
        auto child_mesh = make_mesh(triangle(0.2F, { 0, 1, 0 }), { 0, 1, 2 });
        auto network = std::make_shared<MeshNetwork>();
        network->add_slot("root", root_mesh);
        network->add_slot("child", child_mesh, 0U);
        network->get_slot(1).local_transform[3] = glm::vec4(0.5F, 1.0F, 0.0F, 1.0F);

        auto transforms = network->create_operator<Nodes::Network::MeshTransformOperator>();
        transforms->bind(0, [](float) {
            glm::mat4 transform(1.0F);
            transform[3] = glm::vec4(2.0F, 0.0F, 0.0F, 1.0F);
            return transform;
        });
        auto fields = network->get_operator_chain()->emplace<Nodes::Network::MeshFieldOperator>();
        fields->bind(0, Kinesis::FieldTarget::SCALAR,
            Kinesis::SpatialField { .fn = [](const glm::vec3&) { return 0.25F; } });
        fields->bind(1, Kinesis::FieldTarget::SCALAR,
            Kinesis::SpatialField { .fn = [](const glm::vec3&) { return 0.75F; } });
        fields->bind(1, Kinesis::FieldTarget::POSITION,
            Kinesis::VectorField { .fn = [](const glm::vec3&) { return glm::vec3(0.0F, 0.25F, 0.0F); } });

        attach(network);
        network->process_batch(1);
        buffer->process_default();

        const auto& bindings = buffer->get_engine_context().ssbo_bindings;
        ASSERT_EQ(bindings.size(), 2U);
        const auto model_binding = std::ranges::find_if(bindings, [](const auto& binding) {
            return binding.set == 0 && binding.binding == 1;
        });
        const auto slot_binding = std::ranges::find_if(bindings, [](const auto& binding) {
            return binding.set == 0 && binding.binding == 2;
        });
        ASSERT_NE(model_binding, bindings.end());
        ASSERT_NE(slot_binding, bindings.end());
        EXPECT_EQ(model_binding->type, vk::DescriptorType::eStorageBuffer);
        EXPECT_EQ(model_binding->buffer_info.range, 2U * sizeof(glm::mat4));
        EXPECT_EQ(slot_binding->type, vk::DescriptorType::eStorageBuffer);
        EXPECT_EQ(slot_binding->buffer_info.range, 6U * sizeof(uint32_t));

        std::vector<glm::mat4> models(2);
        const auto model_bytes = read_raw_buffer(model_binding->buffer_info.buffer, model_binding->buffer_info.range);
        ASSERT_EQ(model_bytes.size(), models.size() * sizeof(glm::mat4));
        std::memcpy(models.data(), model_bytes.data(), model_bytes.size());
        EXPECT_FLOAT_EQ(models[0][3].x, 2.0F);
        EXPECT_FLOAT_EQ(models[0][3].y, 0.0F);
        EXPECT_FLOAT_EQ(models[1][3].x, 2.5F);
        EXPECT_FLOAT_EQ(models[1][3].y, 1.0F);

        const auto slot_bytes = read_raw_buffer(slot_binding->buffer_info.buffer, slot_binding->buffer_info.range);
        EXPECT_EQ(indices_from_bytes(slot_bytes), (std::vector<uint32_t> { 0, 0, 0, 1, 1, 1 }));

        std::vector<MeshVertex> uploaded(6);
        const auto vertex_bytes = read_vertices(uploaded.size() * sizeof(MeshVertex));
        std::memcpy(uploaded.data(), vertex_bytes.data(), vertex_bytes.size());
        for (size_t i = 0; i < 3; ++i)
            EXPECT_FLOAT_EQ(uploaded[i].weight, 0.25F);
        for (size_t i = 3; i < 6; ++i) {
            EXPECT_FLOAT_EQ(uploaded[i].weight, 0.75F);
            EXPECT_FLOAT_EQ(uploaded[i].position.y, triangle(0.2F, { 0, 1, 0 })[i - 3].position.y + 0.25F);
        }
    }

    TEST_F(MeshNetworkBufferDeviceTest, AddingALargeSlotGrowsBothGpuStreamsAndRebasesItsIndices)
    {
        auto network = std::make_shared<MeshNetwork>();
        network->add_slot("initial", make_mesh(triangle(0.2F, { 1, 1, 1 }), { 0, 1, 2 }));
        attach(network);
        const auto old_vertex_capacity = buffer->get_size_bytes();
        const auto old_index_capacity = buffer->get_index_buffer_size();

        std::vector<MeshVertex> added_vertices(36);
        std::vector<uint32_t> added_indices(36);
        for (uint32_t i = 0; i < added_vertices.size(); ++i) {
            const auto f = static_cast<float>(i);
            added_vertices[i] = vertex({ f, f + 1.0F, -f }, { 0.2F, 0.4F, 0.6F }, f / 36.0F,
                { f / 36.0F, 1.0F - f / 36.0F }, { 0, 0, 1 }, { 1, 0, 0 });
            added_indices[i] = i;
        }
        network->add_slot("large", make_mesh(added_vertices, added_indices));
        buffer->process_default();

        EXPECT_GT(buffer->get_size_bytes(), old_vertex_capacity);
        EXPECT_GT(buffer->get_index_buffer_size(), old_index_capacity);

        std::vector<MeshVertex> expected_vertices = triangle(0.2F, { 1, 1, 1 });
        expected_vertices.insert(expected_vertices.end(), added_vertices.begin(), added_vertices.end());
        EXPECT_EQ(read_vertices(expected_vertices.size() * sizeof(MeshVertex)), bytes_of(expected_vertices));

        std::vector<uint32_t> expected_indices { 0, 1, 2 };
        for (uint32_t i = 0; i < added_indices.size(); ++i)
            expected_indices.push_back(i + 3);
        EXPECT_EQ(indices_from_bytes(read_index_bytes(expected_indices.size() * sizeof(uint32_t))), expected_indices);

        const auto& bindings = buffer->get_engine_context().ssbo_bindings;
        const auto slot_binding = std::ranges::find_if(bindings, [](const auto& binding) {
            return binding.binding == 2;
        });
        ASSERT_NE(slot_binding, bindings.end());
        const size_t slot_map_size = expected_vertices.size() * sizeof(uint32_t);
        EXPECT_GE(slot_binding->buffer_info.range, slot_map_size);
        const auto slot_bytes = read_raw_buffer(slot_binding->buffer_info.buffer, slot_map_size);
        std::vector<uint32_t> expected_slots { 0, 0, 0 };
        expected_slots.insert(expected_slots.end(), added_vertices.size(), 1U);
        EXPECT_EQ(indices_from_bytes(slot_bytes), expected_slots);
    }

    TEST_F(MeshNetworkBufferDeviceTest, DirtySlotGeometryReplacesUploadedVerticesAndIndices)
    {
        auto original = triangle(0.2F, { 1, 0, 0 });
        auto node = make_mesh(original, { 0, 1, 2 });
        auto network = std::make_shared<MeshNetwork>();
        network->add_slot("editable", node);
        network->add_slot("retained", make_mesh(triangle(0.8F, { 0, 1, 0 }), { 0, 1, 2 }));
        attach(network);

        auto replacement = triangle(0.6F, { 0, 0, 1 });
        for (auto& item : replacement)
            item.position.x += 0.4F;
        const std::array<uint32_t, 3> replacement_indices { 2, 1, 0 };
        node->set_mesh(replacement, replacement_indices);
        node->compute_frame();
        buffer->process_default();

        std::vector<MeshVertex> expected_vertices = replacement;
        const auto retained = triangle(0.8F, { 0, 1, 0 });
        expected_vertices.insert(expected_vertices.end(), retained.begin(), retained.end());
        EXPECT_EQ(read_vertices(expected_vertices.size() * sizeof(MeshVertex)), bytes_of(expected_vertices));
        EXPECT_EQ(indices_from_bytes(read_index_bytes(6U * sizeof(uint32_t))),
            (std::vector<uint32_t> { 2, 1, 0, 3, 4, 5 }));
    }

    TEST_F(MeshNetworkBufferDeviceTest, RenderProcessorUsesMeshNetworkVertexInputsAndEngineDescriptorBindings)
    {
        auto network = std::make_shared<MeshNetwork>();
        network->add_slot("one", make_mesh(triangle(0.3F, { 1, 0, 0 }), { 0, 1, 2 }));
        network->add_slot("two", make_mesh(triangle(0.8F, { 0, 1, 0 }), { 0, 1, 2 }));
        attach(network);
        setup_rendering();

        const auto render = buffer->get_render_processor();
        ASSERT_NE(render, nullptr);
        EXPECT_EQ(render->get_config().shader_path, "mesh_network.vert.spv");

        const auto& bindings = render->get_config().bindings;
        const auto model = bindings.find("modelMatrices");
        const auto slot = bindings.find("slotIndices");
        ASSERT_NE(model, bindings.end());
        ASSERT_NE(slot, bindings.end());
        EXPECT_EQ(model->second.set, 0U);
        EXPECT_EQ(model->second.binding, 1U);
        EXPECT_EQ(model->second.type, vk::DescriptorType::eStorageBuffer);
        EXPECT_EQ(slot->second.set, 0U);
        EXPECT_EQ(slot->second.binding, 2U);
        EXPECT_EQ(slot->second.type, vk::DescriptorType::eStorageBuffer);

        process_dirty_network();
    }

    TEST_F(MeshNetworkBufferDeviceTest, MillingUsesAnExplicitExtendedLayoutAndUploadsSlotIdentityWithEachVertex)
    {
        auto network = std::make_shared<MeshNetwork>();
        const auto first = triangle(0.3F, { 1, 0, 0 });
        const auto second = triangle(0.8F, { 0, 1, 0 });
        network->add_slot("first", make_mesh(first, { 0, 1, 2 }));
        network->add_slot("second", make_mesh(second, { 0, 1, 2 }));
        attach(network);

        window = std::make_shared<ScriptedWindow>(320, 240);
        buffer->setup_rendering(MeshNetworkBuffer::RenderConfig {
            .target_window = window,
            .polygon_mode = Portal::Graphics::PolygonMode::LINE,
            .triangulate = true });
        buffer->process_default();

        const auto render = buffer->get_render_processor();
        ASSERT_NE(render, nullptr);
        EXPECT_TRUE(render->is_triangulate());
        EXPECT_EQ(render->get_config().shader_path, "mesh_network.vert.spv");
        const auto& bindings = render->get_config().bindings;
        EXPECT_NE(bindings.find("modelMatrices"), bindings.end());
        const auto slot = bindings.find("slotIndices");
        ASSERT_NE(slot, bindings.end());
        EXPECT_EQ(slot->second.set, 0U);
        EXPECT_EQ(slot->second.binding, 2U);
        EXPECT_EQ(slot->second.type, vk::DescriptorType::eStorageBuffer);

        const auto layout = buffer->get_vertex_layout();
        ASSERT_TRUE(layout.has_value());
        EXPECT_EQ(layout->stride_bytes, sizeof(MeshVertex) + 2 * sizeof(float));
        ASSERT_EQ(layout->attributes.size(), 8U);
        EXPECT_EQ(layout->attributes[6].offset_in_vertex, sizeof(MeshVertex));
        EXPECT_EQ(layout->attributes[7].offset_in_vertex, sizeof(MeshVertex) + sizeof(float));
        EXPECT_EQ(layout->vertex_count, 6U);

        struct MilledSourceVertex {
            MeshVertex mesh;
            float slot_low;
            float slot_high;
        };
        static_assert(sizeof(MilledSourceVertex) == sizeof(MeshVertex) + 2 * sizeof(float));
        const auto bytes = read_vertices(6U * sizeof(MilledSourceVertex));
        std::vector<MilledSourceVertex> uploaded(6);
        std::memcpy(uploaded.data(), bytes.data(), bytes.size());
        for (size_t i = 0; i < 3; ++i) {
            EXPECT_EQ(std::memcmp(&uploaded[i].mesh, &first[i], sizeof(MeshVertex)), 0);
            EXPECT_FLOAT_EQ(uploaded[i].slot_low, 0.0F);
            EXPECT_FLOAT_EQ(uploaded[i].slot_high, 0.0F);
            EXPECT_EQ(std::memcmp(&uploaded[i + 3].mesh, &second[i], sizeof(MeshVertex)), 0);
            EXPECT_FLOAT_EQ(uploaded[i + 3].slot_low, 1.0F);
            EXPECT_FLOAT_EQ(uploaded[i + 3].slot_high, 0.0F);
        }
    }

    TEST_F(MeshNetworkBufferDeviceTest, DefaultTextureBindingRegistersTheNetworkSamplerSlot)
    {
        auto network = std::make_shared<MeshNetwork>();
        network->add_slot("textured", make_mesh(triangle(0.3F, { 1, 1, 1 }), { 0, 1, 2 }));
        attach(network);

        window = std::make_shared<ScriptedWindow>(320, 240);
        buffer->setup_rendering(MeshNetworkBuffer::RenderConfig {
            .target_window = window,
            .default_texture_binding = "albedoMap" });

        const auto render = buffer->get_render_processor();
        ASSERT_NE(render, nullptr);
        const auto& config = render->get_config();
        EXPECT_EQ(config.shader_path, "mesh_network.vert.spv");
        const auto sampler = config.bindings.find("albedoMap");
        ASSERT_NE(sampler, config.bindings.end());
        EXPECT_EQ(sampler->second.set, 0U);
        EXPECT_EQ(sampler->second.binding, 3U);
        EXPECT_EQ(sampler->second.type, vk::DescriptorType::eCombinedImageSampler);
    }

#if defined(MAYAFLUX_TEST_NATIVE_WINDOWS)

    class MeshNetworkBufferPresentationTest : public PresentingVulkanTest {
    protected:
        void SetUp() override
        {
            PresentingVulkanTest::SetUp();
            if (IsSkipped())
                return;

            Portal::Graphics::get_render_flow().shutdown();
            Portal::Graphics::get_compute_press().shutdown();
            Portal::Graphics::get_shader_foundry().shutdown();
            auto borrowed = std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) { });
            ASSERT_TRUE(Portal::Graphics::get_shader_foundry().initialize(borrowed));
            foundry_ready = true;
            ASSERT_TRUE(Portal::Graphics::get_compute_press().initialize());
            compute_ready = true;
            ASSERT_TRUE(Portal::Graphics::get_render_flow().initialize());
            flow_ready = true;

            window = make_window({ 0.0F, 0.0F, 0.0F, 1.0F });
            ASSERT_NE(window, nullptr);
            ASSERT_TRUE(backend().register_window(window));
            service = Registry::BackendRegistry::instance()
                          .get_service<Registry::Service::BufferService>();
            ASSERT_NE(service, nullptr);

            network = std::make_shared<MeshNetwork>();
            auto left = make_mesh(triangle(0.2F, { 1, 0, 0 }), { 0, 1, 2 });
            auto right = make_mesh(triangle(0.8F, { 0, 1, 0 }), { 0, 1, 2 });
            network->add_slot("left", left);
            network->add_slot("right", right);
            network->get_slot(0).local_transform[3] = glm::vec4(-0.5F, 0.0F, 0.0F, 1.0F);
            network->get_slot(1).local_transform[3] = glm::vec4(0.5F, 0.0F, 0.0F, 1.0F);
            network->process_batch(1);

            buffer = std::make_shared<MeshNetworkBuffer>(network);
            service->initialize_buffer(buffer);
            ASSERT_TRUE(buffer->is_initialized());
            buffer->setup_processors(ProcessingToken::GRAPHICS_BACKEND);
            root = std::make_shared<Buffers::RootGraphicsBuffer>();
            root->initialize();
            root->add_child_buffer(buffer);
        }

        void TearDown() override
        {
            if (buffer) {
                if (auto render = buffer->get_render_processor()) {
                    if (auto chain = buffer->get_processing_chain())
                        chain->remove_processor_anywhere(render, buffer);
                }
                buffer->set_default_processor(nullptr);
            }
            root.reset();
            if (buffer && buffer->is_initialized() && service)
                service->destroy_buffer(buffer);
            buffer.reset();
            network.reset();
            if (flow_ready)
                Portal::Graphics::get_render_flow().shutdown();
            if (compute_ready)
                Portal::Graphics::get_compute_press().shutdown();
            if (foundry_ready)
                Portal::Graphics::get_shader_foundry().shutdown();
            PresentingVulkanTest::TearDown();
        }

        std::array<uint8_t, 4> pixel_at_ndc(float x, float y)
        {
            auto* display = Registry::BackendRegistry::instance()
                                .get_service<Registry::Service::DisplayService>();
            EXPECT_NE(display, nullptr);
            if (!display)
                return {};

            uint32_t width = 0;
            uint32_t height = 0;
            display->get_swapchain_extent(window, width, height);
            std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
            if (pixels.empty())
                return {};
            EXPECT_TRUE(display->readback_swapchain_region(
                window, pixels.data(), 0, 0, width, height, pixels.size()));

            const auto px = std::min(static_cast<uint32_t>((x + 1.0F) * 0.5F * width), width - 1);
            const auto py = std::min(static_cast<uint32_t>((1.0F - y) * 0.5F * height), height - 1);
            const size_t offset = (static_cast<size_t>(py) * width + px) * 4;
            return { pixels[offset], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3] };
        }

        uint8_t max_channel_near_ndc(float x, float y, size_t channel)
        {
            uint8_t result = 0;
            for (const float dx : { -0.006F, 0.0F, 0.006F }) {
                for (const float dy : { -0.006F, 0.0F, 0.006F })
                    result = std::max(result, pixel_at_ndc(x + dx, y + dy)[channel]);
            }
            return result;
        }

        std::shared_ptr<Core::Window> window;
        std::shared_ptr<MeshNetwork> network;
        std::shared_ptr<MeshNetworkBuffer> buffer;
        std::shared_ptr<Buffers::RootGraphicsBuffer> root;
        Registry::Service::BufferService* service {};
        bool foundry_ready {};
        bool compute_ready {};
        bool flow_ready {};
    };

    TEST_F(MeshNetworkBufferPresentationTest, DefaultShadersDrawSlotColorsAtTheirUploadedWorldTransforms)
    {
        buffer->setup_rendering(MeshNetworkBuffer::RenderConfig { .target_window = window });
        const auto render = buffer->get_render_processor();
        ASSERT_NE(render, nullptr);
        EXPECT_EQ(render->get_config().shader_path, "mesh_network.vert.spv");

        root->process_default();
        Buffers::PresentProcessor presenter;
        presenter.processing_function(root);

        const auto left = pixel_at_ndc(-0.5F, 0.0F);
        EXPECT_GT(left[2], 200U);
        EXPECT_LT(left[1], 40U);
        EXPECT_LT(left[0], 40U);
        const auto right = pixel_at_ndc(0.5F, 0.0F);
        EXPECT_LT(right[0], 40U);
        EXPECT_GT(right[1], 200U);
        EXPECT_LT(right[2], 40U);
    }

    TEST_F(MeshNetworkBufferPresentationTest, MilledLineModeDrawsWireframesForEachTransformedSlot)
    {
        buffer->setup_rendering(MeshNetworkBuffer::RenderConfig {
            .target_window = window,
            .polygon_mode = Portal::Graphics::PolygonMode::LINE,
            .triangulate = true });
        auto render = buffer->get_render_processor();
        ASSERT_NE(render, nullptr);
        EXPECT_EQ(render->get_config().shader_path, "mesh_network.vert.spv");

        root->process_default();
        Buffers::PresentProcessor presenter;
        presenter.processing_function(root);
        EXPECT_EQ(render->milled_vertex_count(), 6U);

        EXPECT_GT(max_channel_near_ndc(-0.5F, -0.25F, 2), 200U);
        EXPECT_GT(max_channel_near_ndc(0.5F, -0.25F, 1), 200U);
        const auto left_interior = pixel_at_ndc(-0.5F, 0.0F);
        EXPECT_LT(left_interior[0], 40U);
        EXPECT_LT(left_interior[1], 40U);
    }

    TEST_F(MeshNetworkBufferPresentationTest, MilledPointModeDrawsVertexMarkersForEachTransformedSlot)
    {
        buffer->setup_rendering(MeshNetworkBuffer::RenderConfig {
            .target_window = window,
            .polygon_mode = Portal::Graphics::PolygonMode::POINT,
            .triangulate = true });
        auto render = buffer->get_render_processor();
        ASSERT_NE(render, nullptr);
        EXPECT_EQ(render->get_config().shader_path, "mesh_network.vert.spv");
        auto mill_spec = render->get_mill_spec();
        mill_spec.point_scale = 0.12F;
        mill_spec.use_vertex_extent = false;
        render->set_mill_spec(mill_spec);

        root->process_default();
        Buffers::PresentProcessor presenter;
        presenter.processing_function(root);
        EXPECT_EQ(render->milled_vertex_count(), 36U);

        const auto left_marker = pixel_at_ndc(-0.75F, -0.25F);
        EXPECT_GT(left_marker[2], 200U);
        const auto right_marker = pixel_at_ndc(0.25F, -0.25F);
        EXPECT_GT(right_marker[1], 200U);
        const auto left_interior = pixel_at_ndc(-0.5F, 0.0F);
        EXPECT_LT(left_interior[0], 40U);
        EXPECT_LT(left_interior[1], 40U);
    }

#endif

} // namespace
} // namespace MayaFlux::Test
