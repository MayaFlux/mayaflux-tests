#pragma once

#include "gtest/gtest.h"


#include "../../support/scripted_window.hpp"
#include "../../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/State/VolumeGridBuffer.hpp"
#include "MayaFlux/Portal/Graphics/ComputePress.hpp"
#include "MayaFlux/Portal/Graphics/ShaderFoundry.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"

namespace MayaFlux::Test {

inline Kinesis::Lattice3D small_volume_lattice()
{
    return {
        .resolution = { 2U, 2U, 2U },
        .bounds = { .min = { -2.0F, -2.0F, -2.0F }, .max = { 2.0F, 2.0F, 2.0F } },
    };
}

inline Kinesis::Lattice3D unit_cell_lattice()
{
    return {
        .resolution = { 4U, 4U, 4U },
        .bounds = { .min = { 0.0F, 0.0F, 0.0F }, .max = { 4.0F, 4.0F, 4.0F } },
    };
}

inline Buffers::VolumeGridBuffer::FieldDecl scalar_decl(std::string name, bool double_buffered = true)
{
    return { .name = std::move(name), .stride_bytes = sizeof(float), .double_buffered = double_buffered };
}

inline Buffers::VolumeGridBuffer::FieldDecl vector_decl(std::string name, bool double_buffered = true)
{
    return { .name = std::move(name), .stride_bytes = sizeof(glm::vec4), .double_buffered = double_buffered };
}

template <typename T>
T constant_at(const std::vector<uint8_t>& bytes, size_t offset)
{
    T value {};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

class VolumeGridDeviceFixture : public VulkanDeviceTest {
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
        if (s_foundry_ready) {
            auto& press = Portal::Graphics::get_compute_press();
            press.shutdown();
            s_compute_ready = press.initialize();
        }
    }

    static void TearDownTestSuite()
    {
        if (s_compute_ready)
            Portal::Graphics::get_compute_press().shutdown();
        if (s_foundry_ready)
            Portal::Graphics::get_shader_foundry().shutdown();
        s_compute_ready = false;
        s_foundry_ready = false;
        VulkanDeviceTest::TearDownTestSuite();
    }

    void SetUp() override
    {
        VulkanDeviceTest::SetUp();
        if (IsSkipped())
            return;
        ASSERT_TRUE(s_foundry_ready);
        ASSERT_TRUE(s_compute_ready);
        service = Registry::BackendRegistry::instance()
                      .get_service<Registry::Service::BufferService>();
        ASSERT_NE(service, nullptr);
    }

    void TearDown() override
    {
        for (auto& volume : volumes) {
            if (auto chain = volume->get_processing_chain()) {
                const auto processors = chain->get_processors(volume);
                for (const auto& processor : processors)
                    chain->remove_processor_anywhere(processor, volume);
                if (auto processor = chain->get_preprocessor(volume))
                    chain->remove_processor_anywhere(processor, volume);
                if (auto processor = chain->get_postprocessor(volume))
                    chain->remove_processor_anywhere(processor, volume);
                if (auto processor = chain->get_final_processor(volume))
                    chain->remove_processor_anywhere(processor, volume);
            }
            volume->set_default_processor(nullptr);
            volume->set_processing_chain(nullptr, true);
            if (volume->is_initialized() && service)
                service->destroy_buffer(volume);
        }
        volumes.clear();
        windows.clear();
    }

    std::shared_ptr<Buffers::VolumeGridBuffer> attach(
        std::vector<Buffers::VolumeGridBuffer::FieldDecl> fields,
        Kinesis::Lattice3D lattice = small_volume_lattice(),
        std::optional<Buffers::VolumeGridBuffer::SurfaceConfig> surface = std::nullopt)
    {
        auto volume = std::make_shared<Buffers::VolumeGridBuffer>(
            lattice, std::move(fields), std::move(surface));
        service->initialize_buffer(volume);
        EXPECT_TRUE(volume->is_initialized());
        volumes.push_back(volume);
        return volume;
    }

    std::shared_ptr<Buffers::VolumeGridBuffer> attach_empty(
        Kinesis::Lattice3D lattice = small_volume_lattice(),
        std::optional<Buffers::VolumeGridBuffer::SurfaceConfig> surface = std::nullopt)
    {
        auto volume = std::make_shared<Buffers::VolumeGridBuffer>(lattice, std::move(surface));
        service->initialize_buffer(volume);
        EXPECT_TRUE(volume->is_initialized());
        volumes.push_back(volume);
        return volume;
    }

    std::shared_ptr<Buffers::VolumeGridBuffer> attach_surface(
        std::vector<Buffers::VolumeGridBuffer::FieldDecl> fields,
        Buffers::VolumeGridBuffer::SurfaceConfig surface,
        Kinesis::Lattice3D lattice = small_volume_lattice())
    {
        auto volume = attach(std::move(fields), lattice, surface);
        volume->setup_processors(Buffers::ProcessingToken::GRAPHICS_BACKEND);
        auto window = std::make_shared<ScriptedWindow>(320, 240);
        windows.push_back(window);
        volume->setup_rendering({ .target_window = window });
        EXPECT_NE(volume->surface_processor(), nullptr);
        EXPECT_NE(volume->mesh_processor(), nullptr);
        EXPECT_NE(volume->get_render_processor(), nullptr);
        return volume;
    }

    /**
     * @brief Blocks until the field's in-flight seed upload has landed.
     *
     * Seeding is an asynchronous staged copy that is only resolved by the next
     * transfer on the volume. Tests that dispatch straight after seeding call
     * this so the dispatch reads the seeded values.
     */
    static void settle(const std::shared_ptr<Buffers::VolumeGridBuffer>& volume,
        const std::string& name)
    {
        std::vector<uint8_t> scratch(volume->get_field_bytes(name));
        volume->read_field(name, scratch.data(), scratch.size());
    }

    template <typename T>
    static void seed_raw(const std::shared_ptr<Buffers::VolumeGridBuffer>& volume,
        const std::string& name, const std::vector<T>& values)
    {
        volume->seed_raw(name, values.data(), values.size() * sizeof(T));
        settle(volume, name);
    }

    template <typename T>
    static void fill(const std::shared_ptr<Buffers::VolumeGridBuffer>& volume,
        const std::string& name, const T& value)
    {
        seed_raw(volume, name, std::vector<T>(volume->get_cell_count(), value));
    }

    static void expect_scalars(const std::vector<float>& actual,
        const std::vector<float>& expected, float tolerance = 1e-5F)
    {
        ASSERT_EQ(actual.size(), expected.size());
        for (size_t i = 0; i < actual.size(); ++i)
            EXPECT_NEAR(actual[i], expected[i], tolerance) << "cell " << i;
    }

    static void expect_vectors(const std::vector<glm::vec4>& actual,
        const std::vector<glm::vec4>& expected, float tolerance = 1e-5F)
    {
        ASSERT_EQ(actual.size(), expected.size());
        for (size_t i = 0; i < actual.size(); ++i) {
            for (int c = 0; c < 4; ++c)
                EXPECT_NEAR(actual[i][c], expected[i][c], tolerance)
                    << "cell " << i << " component " << c;
        }
    }

    template <typename T>
    static std::vector<T> read(const std::shared_ptr<Buffers::VolumeGridBuffer>& volume,
        const std::string& name)
    {
        std::vector<T> values(volume->get_cell_count());
        volume->read_field(name, values.data(), values.size() * sizeof(T));
        return values;
    }

    static inline bool s_foundry_ready {};
    static inline bool s_compute_ready {};
    Registry::Service::BufferService* service {};
    std::vector<std::shared_ptr<Buffers::VolumeGridBuffer>> volumes;
    std::vector<std::shared_ptr<ScriptedWindow>> windows;
};

} // namespace MayaFlux::Test
