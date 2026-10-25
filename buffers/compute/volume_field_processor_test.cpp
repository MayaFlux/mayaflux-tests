#include "gtest/gtest.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "volume_grid_fixture.hpp"

#include "MayaFlux/Buffers/State/VolumeFieldProcessor.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::VolumeFieldProcessor;
    using Buffers::VolumeGridBuffer;

    constexpr const char* k_field_shader = R"(#version 460
layout(local_size_x = 8, local_size_y = 8, local_size_z = 4) in;
layout(set = 0, binding = 0, std430) readonly buffer Source { float values[]; } source_field;
layout(set = 0, binding = 1, std430) writeonly buffer Target { float values[]; } target_field;
layout(push_constant) uniform Params {
    uvec4 dimensions;
    vec4 cell;
    vec4 extra;
} pc;
void main() {
    uvec3 c = gl_GlobalInvocationID.xyz;
    if (any(greaterThanEqual(c, pc.dimensions.xyz))) return;
    uint i = (c.z * pc.dimensions.y + c.y) * pc.dimensions.x + c.x;
    target_field.values[i] = source_field.values[i] + float(pc.dimensions.w)
        + pc.cell.x + pc.cell.y + pc.cell.z + pc.cell.w + pc.extra.x;
}
)";

    class TemporaryVolumeShader {
    public:
        explicit TemporaryVolumeShader(const void* owner)
            : m_path(std::filesystem::temp_directory_path()
                  / ("mayaflux_volume_field_"
                      + std::to_string(reinterpret_cast<std::uintptr_t>(owner)) + ".comp"))
        {
            std::ofstream file(m_path);
            file << k_field_shader;
        }

        ~TemporaryVolumeShader()
        {
            std::error_code error;
            std::filesystem::remove(m_path, error);
        }

        TemporaryVolumeShader(const TemporaryVolumeShader&) = delete;
        TemporaryVolumeShader& operator=(const TemporaryVolumeShader&) = delete;

        [[nodiscard]] std::string path() const { return m_path.string(); }

    private:
        std::filesystem::path m_path;
    };

    class VolumeFieldProbe final : public VolumeFieldProcessor {
    public:
        VolumeFieldProbe(std::string source, std::string target, const std::string& shader_path,
            bool swap_target = true)
            : VolumeFieldProcessor({
                  { .name = "source_field", .binding = 0, .field = std::move(source),
                      .access = FieldAccess::READ },
                  { .name = "target_field", .binding = 1, .field = target,
                      .access = FieldAccess::WRITE },
              }, shader_path)
        {
            reserve_param_size(48U);
            if (swap_target)
                add_swap_field(std::move(target));
        }

        void set_words(uint32_t word3, float word7, float tail)
        {
            write_lattice_word3(word3);
            write_lattice_word7(word7);
            write_param_tail(32U, &tail, sizeof(tail));
        }

        void write_tail_at(size_t offset, float value)
        {
            write_param_tail(offset, &value, sizeof(value));
        }

        void refresh_lattice() { write_lattice_params(); }
    };

    template <typename T>
    T word_at(const std::vector<uint8_t>& bytes, size_t offset)
    {
        T value {};
        std::memcpy(&value, bytes.data() + offset, sizeof(T));
        return value;
    }

    class VolumeFieldProcessorDeviceTest : public VolumeGridDeviceFixture {
    protected:
        static std::shared_ptr<VolumeFieldProbe> attach_probe(
            const std::shared_ptr<VolumeGridBuffer>& volume,
            const TemporaryVolumeShader& shader,
            std::string source, std::string target, bool swap_target = true)
        {
            auto probe = std::make_shared<VolumeFieldProbe>(
                std::move(source), std::move(target), shader.path(), swap_target);
            volume->set_default_processor(probe);
            return probe;
        }
    };

    TEST_F(VolumeFieldProcessorDeviceTest, RegistersNamedStorageBindingsInSetZero)
    {
        TemporaryVolumeShader shader(this);
        VolumeFieldProbe probe("source", "target", shader.path());
        const auto& bindings = probe.get_config().bindings;

        ASSERT_EQ(bindings.size(), 2U);
        EXPECT_EQ(bindings.at("source_field").set, 0U);
        EXPECT_EQ(bindings.at("source_field").binding, 0U);
        EXPECT_EQ(bindings.at("source_field").type, vk::DescriptorType::eStorageBuffer);
        EXPECT_EQ(bindings.at("target_field").set, 0U);
        EXPECT_EQ(bindings.at("target_field").binding, 1U);
        EXPECT_EQ(bindings.at("target_field").type, vk::DescriptorType::eStorageBuffer);
        EXPECT_EQ(probe.get_config().push_constant_size, 48U);
    }

    TEST_F(VolumeFieldProcessorDeviceTest, MissingFieldsRejectAttachmentAndDispatch)
    {
        TemporaryVolumeShader shader(this);
        auto volume = attach({ { "source", sizeof(float) } });
        auto probe = attach_probe(volume, shader, "source", "missing");

        EXPECT_EQ(probe->get_volume(), nullptr);
        volume->process_default();
        EXPECT_FALSE(probe->is_pipeline_ready());
        EXPECT_EQ(volume->get_field_names(), (std::vector<std::string> { "source" }));
    }

    TEST_F(VolumeFieldProcessorDeviceTest, ReadWriteScratchFieldRejectsAttachment)
    {
        TemporaryVolumeShader shader(this);
        auto volume = attach({ { "scratch", sizeof(float), false } });
        const auto original = volume->read_handle("scratch");
        auto probe = attach_probe(volume, shader, "scratch", "scratch");

        EXPECT_EQ(probe->get_volume(), nullptr);
        volume->process_default();
        EXPECT_FALSE(probe->is_pipeline_ready());
        EXPECT_EQ(volume->read_handle("scratch"), original);
    }

    TEST_F(VolumeFieldProcessorDeviceTest, SeparateWriteOnlyScratchFieldIsAccepted)
    {
        TemporaryVolumeShader shader(this);
        auto volume = attach({
            { "source", sizeof(float) },
            { "scratch", sizeof(float), false },
        });
        seed_raw(volume, "source", std::vector<float>(8U, 2.0F));
        const auto scratch_handle = volume->read_handle("scratch");
        auto probe = attach_probe(volume, shader, "source", "scratch", false);
        probe->set_words(0U, 0.0F, 0.0F);

        ASSERT_EQ(probe->get_volume(), volume);
        volume->process_default();
        EXPECT_EQ(read<float>(volume, "scratch"), std::vector<float>(8U, 8.0F));
        EXPECT_EQ(volume->read_handle("scratch"), scratch_handle);
    }

    TEST_F(VolumeFieldProcessorDeviceTest, DispatchRoundsUpEveryLatticeAxis)
    {
        TemporaryVolumeShader shader(this);
        auto lattice = small_volume_lattice();
        lattice.resolution = { 9U, 10U, 5U };
        lattice.bounds = { .min = glm::vec3(0.0F), .max = glm::vec3(9.0F, 10.0F, 5.0F) };
        auto volume = attach({ { "density", sizeof(float) } }, lattice);
        auto probe = attach_probe(volume, shader, "density", "density");
        probe->set_words(0U, 0.0F, 0.0F);

        const auto& dispatch = probe->get_dispatch_config();
        EXPECT_EQ(dispatch.mode, Buffers::ShaderDispatchConfig::DispatchMode::MANUAL);
        EXPECT_EQ(dispatch.workgroup_x, 8U);
        EXPECT_EQ(dispatch.workgroup_y, 8U);
        EXPECT_EQ(dispatch.workgroup_z, 4U);
        EXPECT_EQ(dispatch.group_count_x, 2U);
        EXPECT_EQ(dispatch.group_count_y, 2U);
        EXPECT_EQ(dispatch.group_count_z, 2U);

        seed_raw(volume, "density", std::vector<float>(volume->get_cell_count(), 1.0F));
        volume->process_default();
        const auto values = read<float>(volume, "density");
        ASSERT_EQ(values.size(), 450U);
        EXPECT_EQ(values.front(), 4.0F);
        EXPECT_EQ(values.back(), 4.0F);
    }

    TEST_F(VolumeFieldProcessorDeviceTest, PushConstantPrefixKeepsSubclassWordsAndTail)
    {
        TemporaryVolumeShader shader(this);
        auto volume = attach({ { "density", sizeof(float) } });
        auto probe = attach_probe(volume, shader, "density", "density");
        probe->set_words(7U, 3.5F, 11.0F);
        probe->refresh_lattice();
        const auto& data = probe->get_push_constant_data();

        ASSERT_GE(data.size(), 48U);
        EXPECT_EQ(word_at<uint32_t>(data, 0U), 2U);
        EXPECT_EQ(word_at<uint32_t>(data, 4U), 2U);
        EXPECT_EQ(word_at<uint32_t>(data, 8U), 2U);
        EXPECT_EQ(word_at<uint32_t>(data, 12U), 7U);
        EXPECT_EQ(word_at<float>(data, 16U), 2.0F);
        EXPECT_EQ(word_at<float>(data, 20U), 2.0F);
        EXPECT_EQ(word_at<float>(data, 24U), 2.0F);
        EXPECT_EQ(word_at<float>(data, 28U), 3.5F);
        EXPECT_EQ(word_at<float>(data, 32U), 11.0F);
    }

    TEST_F(VolumeFieldProcessorDeviceTest, ParameterTailRejectsPrefixOverlap)
    {
        TemporaryVolumeShader shader(this);
        auto volume = attach({ { "density", sizeof(float) } });
        auto probe = attach_probe(volume, shader, "density", "density");
        const auto original = probe->get_push_constant_data();

        probe->write_tail_at(28U, 19.0F);

        EXPECT_EQ(probe->get_push_constant_data(), original);
    }

    TEST_F(VolumeFieldProcessorDeviceTest, DispatchReadsThenWritesAndSwapsExactlyOnce)
    {
        TemporaryVolumeShader shader(this);
        auto volume = attach({ { "density", sizeof(float) } });
        seed_raw(volume, "density", std::vector<float>(8U, 1.0F));
        auto probe = attach_probe(volume, shader, "density", "density");
        probe->set_words(2U, 3.0F, 4.0F);
        const auto initial_read = volume->read_handle("density");
        const auto initial_write = volume->write_handle("density");

        volume->process_default();
        EXPECT_EQ(volume->read_handle("density"), initial_write);
        EXPECT_EQ(read<float>(volume, "density"), std::vector<float>(8U, 16.0F));

        volume->process_default();
        EXPECT_EQ(volume->read_handle("density"), initial_read);
        EXPECT_EQ(read<float>(volume, "density"), std::vector<float>(8U, 31.0F));
    }

    TEST_F(VolumeFieldProcessorDeviceTest, DescriptorReadsTheNewSourceGeneration)
    {
        TemporaryVolumeShader shader(this);
        auto volume = attach({
            { "source", sizeof(float) },
            { "target", sizeof(float) },
        });
        seed_raw(volume, "source", std::vector<float>(8U, 1.0F));
        auto probe = attach_probe(volume, shader, "source", "target");
        probe->set_words(0U, 0.0F, 0.0F);

        volume->process_default();
        EXPECT_EQ(read<float>(volume, "target"), std::vector<float>(8U, 7.0F));

        volume->swap_field("source");
        seed_raw(volume, "source", std::vector<float>(8U, 5.0F));
        volume->process_default();
        EXPECT_EQ(read<float>(volume, "target"), std::vector<float>(8U, 11.0F));
    }

    TEST_F(VolumeFieldProcessorDeviceTest, InterleavedVolumesKeepTheirFieldBindings)
    {
        TemporaryVolumeShader shader(this);
        auto first = attach({ { "density", sizeof(float) } });
        auto second = attach({ { "density", sizeof(float) } });
        seed_raw(first, "density", std::vector<float>(8U, 1.0F));
        seed_raw(second, "density", std::vector<float>(8U, 10.0F));
        auto first_probe = attach_probe(first, shader, "density", "density");
        auto second_probe = attach_probe(second, shader, "density", "density");
        first_probe->set_words(0U, 0.0F, 0.0F);
        second_probe->set_words(0U, 0.0F, 0.0F);

        first->process_default();
        second->process_default();
        first->process_default();

        EXPECT_EQ(read<float>(first, "density"), std::vector<float>(8U, 13.0F));
        EXPECT_EQ(read<float>(second, "density"), std::vector<float>(8U, 16.0F));
    }

    TEST_F(VolumeFieldProcessorDeviceTest, DeferredDispatchKeepsBindingsUntilCompletion)
    {
        TemporaryVolumeShader shader(this);
        auto volume = attach({ { "density", sizeof(float) } });
        seed_raw(volume, "density", std::vector<float>(8U, 1.0F));
        auto probe = attach_probe(volume, shader, "density", "density");
        probe->set_words(0U, 0.0F, 0.0F);
        probe->set_deferred_submission(true);
        const auto original_read = volume->read_handle("density");

        volume->process_default();

        ASSERT_TRUE(probe->is_dispatch_pending());
        EXPECT_NE(volume->read_handle("density"), original_read);
        EXPECT_TRUE(probe->resolve_pending_dispatch(true));
        EXPECT_FALSE(probe->is_dispatch_pending());
        EXPECT_EQ(read<float>(volume, "density"), std::vector<float>(8U, 7.0F));
        probe->set_deferred_submission(false);
    }

} // namespace
} // namespace MayaFlux::Test
