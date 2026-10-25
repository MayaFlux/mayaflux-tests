#include "gtest/gtest.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "volume_grid_fixture.hpp"

#include "MayaFlux/Buffers/Shaders/RenderProcessor.hpp"
#include "MayaFlux/Buffers/State/RaymarchBuffer.hpp"
#include "MayaFlux/Buffers/State/RaymarchProcessor.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::RaymarchBuffer;
    using Buffers::RaymarchProcessor;
    using Buffers::VolumeGridBuffer;

    constexpr const char* k_fragment = "march_params";

    class VolumeRaymarchDeviceTest : public VolumeGridDeviceFixture {
    protected:
        void TearDown() override
        {
            for (auto& marcher : marchers) {
                if (auto chain = marcher->get_processing_chain()) {
                    const auto processors = chain->get_processors(marcher);
                    for (const auto& processor : processors) {
                        chain->remove_processor_anywhere(processor, marcher);
                    }
                    if (auto processor = chain->get_final_processor(marcher)) {
                        chain->remove_processor_anywhere(processor, marcher);
                    }
                }
                marcher->set_default_processor(nullptr);
                marcher->set_processing_chain(nullptr, true);
                if (marcher->is_initialized() && service) {
                    service->destroy_buffer(marcher);
                }
            }
            marchers.clear();
            VolumeGridDeviceFixture::TearDown();
        }

        std::shared_ptr<VolumeGridBuffer> field_volume()
        {
            return attach({ scalar_decl("density") }, unit_cell_lattice());
        }

        static std::shared_ptr<RaymarchProcessor> processor_for(
            const std::shared_ptr<VolumeGridBuffer>& field, uint32_t binding = 1U)
        {
            const size_t bytes = field->get_field_bytes("density");
            return std::make_shared<RaymarchProcessor>(
                [field] { return field->read_handle("density"); },
                bytes, unit_cell_lattice(), binding);
        }

        static const Portal::Graphics::PushConstantBindingInfo* fragment(
            const std::shared_ptr<VolumeGridBuffer>& canvas)
        {
            const auto& fragments = canvas->get_pipeline_context().push_constant_bindings;
            const auto it = std::ranges::find_if(fragments, [](const auto& entry) {
                return entry.name == k_fragment;
            });
            return it == fragments.end() ? nullptr : &*it;
        }

        static const Portal::Graphics::DescriptorBindingInfo* descriptor(
            const std::shared_ptr<VolumeGridBuffer>& canvas, uint32_t set, uint32_t binding)
        {
            const auto& staged = canvas->get_engine_context().ssbo_bindings;
            const auto it = std::ranges::find_if(staged, [&](const auto& entry) {
                return entry.set == set && entry.binding == binding;
            });
            return it == staged.end() ? nullptr : &*it;
        }

        std::vector<std::shared_ptr<RaymarchBuffer>> marchers;
    };

    TEST_F(VolumeRaymarchDeviceTest, AttachStagesTheFieldAsAnEngineSsboAndTheParameterFragment)
    {
        auto field = field_volume();
        auto canvas = attach_empty();
        auto march = processor_for(field);

        march->on_attach(canvas);

        EXPECT_TRUE(canvas->get_pipeline_context().descriptor_buffer_bindings.empty());
        const auto* staged = descriptor(canvas, 0U, 1U);
        ASSERT_NE(staged, nullptr);
        EXPECT_EQ(staged->type, vk::DescriptorType::eStorageBuffer);
        EXPECT_EQ(staged->buffer_info.buffer, field->read_handle("density"));
        EXPECT_EQ(staged->buffer_info.offset, 0U);
        EXPECT_EQ(staged->buffer_info.range, field->get_field_bytes("density"));
        EXPECT_EQ(staged->count, 1U);

        const auto* params = fragment(canvas);
        ASSERT_NE(params, nullptr);
        EXPECT_EQ(params->offset, 0U);
        EXPECT_EQ(params->data.size(), sizeof(RaymarchProcessor::MarchParams));
    }

    TEST_F(VolumeRaymarchDeviceTest, ParameterBlockStartsFromTheLatticeAndTheDefaults)
    {
        auto field = field_volume();
        auto canvas = attach_empty();
        processor_for(field)->on_attach(canvas);

        const auto* params = fragment(canvas);
        ASSERT_NE(params, nullptr);
        const auto& bytes = params->data;
        EXPECT_EQ(constant_at<uint32_t>(bytes, 0U), 4U);
        EXPECT_EQ(constant_at<uint32_t>(bytes, 4U), 4U);
        EXPECT_EQ(constant_at<uint32_t>(bytes, 8U), 4U);
        EXPECT_EQ(constant_at<uint32_t>(bytes, 12U), 128U);
        EXPECT_EQ(constant_at<float>(bytes, 16U), 0.0F);
        EXPECT_EQ(constant_at<float>(bytes, 28U), 0.5F);
        EXPECT_EQ(constant_at<float>(bytes, 32U), 1.0F);
        EXPECT_EQ(constant_at<float>(bytes, 36U), 1.0F);
        EXPECT_EQ(constant_at<float>(bytes, 40U), 1.0F);
        EXPECT_EQ(constant_at<float>(bytes, 44U), 1.0F);
        EXPECT_EQ(constant_at<float>(bytes, 48U), 0.35F);
        EXPECT_EQ(constant_at<float>(bytes, 60U), 12.0F);
        EXPECT_EQ(constant_at<float>(bytes, 64U), 1.0F);
        EXPECT_EQ(constant_at<float>(bytes, 76U), 2.2F);
        EXPECT_EQ(constant_at<float>(bytes, 80U), 0.02F);
    }

    TEST_F(VolumeRaymarchDeviceTest, BindingZeroFallsBackToBindingOne)
    {
        auto field = field_volume();
        auto canvas = attach_empty();

        processor_for(field, 0U)->on_attach(canvas);

        EXPECT_EQ(descriptor(canvas, 0U, 0U), nullptr);
        EXPECT_NE(descriptor(canvas, 0U, 1U), nullptr);
    }

    TEST_F(VolumeRaymarchDeviceTest, ANullHandleStagesNothingUntilTheSourceResolves)
    {
        auto field = field_volume();
        auto canvas = attach_empty();
        bool resolve = false;
        auto march = std::make_shared<RaymarchProcessor>(
            [&] { return resolve ? field->read_handle("density") : vk::Buffer {}; },
            field->get_field_bytes("density"), unit_cell_lattice());

        march->on_attach(canvas);
        EXPECT_EQ(descriptor(canvas, 0U, 1U), nullptr);
        EXPECT_NE(fragment(canvas), nullptr);

        resolve = true;
        march->processing_function(canvas);
        ASSERT_NE(descriptor(canvas, 0U, 1U), nullptr);
        EXPECT_EQ(descriptor(canvas, 0U, 1U)->buffer_info.buffer, field->read_handle("density"));
    }

    TEST_F(VolumeRaymarchDeviceTest, ProcessingRestagesTheHandleAndSettingsWithoutDuplicates)
    {
        auto first = field_volume();
        auto second = field_volume();
        auto canvas = attach_empty();
        auto current = first;
        auto march = std::make_shared<RaymarchProcessor>(
            [&current] { return current->read_handle("density"); },
            first->get_field_bytes("density"), unit_cell_lattice());
        march->on_attach(canvas);

        current = second;
        march->set_density_scale(3.0F);
        march->set_absorption(5.0F);
        march->set_emission(7.0F);
        march->set_threshold(0.5F);
        march->set_emission_ramp(glm::vec3(0.1F, 0.2F, 0.3F), glm::vec3(0.4F, 0.5F, 0.6F));
        march->processing_function(canvas);

        const auto& staged = canvas->get_engine_context().ssbo_bindings;
        const auto& fragments = canvas->get_pipeline_context().push_constant_bindings;
        EXPECT_EQ(staged.size(), 1U);
        EXPECT_EQ(fragments.size(), 1U);
        EXPECT_EQ(descriptor(canvas, 0U, 1U)->buffer_info.buffer, second->read_handle("density"));

        const auto& bytes = fragment(canvas)->data;
        EXPECT_EQ(constant_at<float>(bytes, 44U), 3.0F);
        EXPECT_EQ(constant_at<float>(bytes, 60U), 5.0F);
        EXPECT_EQ(constant_at<float>(bytes, 76U), 7.0F);
        EXPECT_EQ(constant_at<float>(bytes, 80U), 0.5F);
        EXPECT_EQ(constant_at<float>(bytes, 48U), 0.1F);
        EXPECT_EQ(constant_at<float>(bytes, 52U), 0.2F);
        EXPECT_EQ(constant_at<float>(bytes, 56U), 0.3F);
        EXPECT_EQ(constant_at<float>(bytes, 64U), 0.4F);
        EXPECT_EQ(constant_at<float>(bytes, 68U), 0.5F);
        EXPECT_EQ(constant_at<float>(bytes, 72U), 0.6F);
    }

    TEST_F(VolumeRaymarchDeviceTest, UnresolvedSourceKeepsThePreviousStaging)
    {
        auto field = field_volume();
        auto canvas = attach_empty();
        bool resolve = true;
        auto march = std::make_shared<RaymarchProcessor>(
            [&] { return resolve ? field->read_handle("density") : vk::Buffer {}; },
            field->get_field_bytes("density"), unit_cell_lattice());
        march->on_attach(canvas);
        march->set_density_scale(9.0F);

        resolve = false;
        march->processing_function(canvas);

        EXPECT_EQ(descriptor(canvas, 0U, 1U)->buffer_info.buffer, field->read_handle("density"));
        EXPECT_EQ(constant_at<float>(fragment(canvas)->data, 44U), 1.0F);
    }

    TEST_F(VolumeRaymarchDeviceTest, ZeroMaxStepsFollowsTheLatticeDiagonalAndStepScale)
    {
        auto field = field_volume();
        auto canvas = attach_empty();
        auto march = processor_for(field);
        march->on_attach(canvas);

        march->configure({});
        march->processing_function(canvas);
        EXPECT_EQ(constant_at<uint32_t>(fragment(canvas)->data, 12U), 15U);

        march->set_step_scale(0.25F);
        march->processing_function(canvas);
        EXPECT_EQ(constant_at<uint32_t>(fragment(canvas)->data, 12U), 29U);
        EXPECT_EQ(constant_at<float>(fragment(canvas)->data, 28U), 0.25F);

        march->set_max_steps(7U);
        march->set_step_scale(0.5F);
        march->processing_function(canvas);
        EXPECT_EQ(constant_at<uint32_t>(fragment(canvas)->data, 12U), 7U);
    }

    TEST_F(VolumeRaymarchDeviceTest, ConfigureWritesEverySettingAndExplicitStepsWin)
    {
        auto field = field_volume();
        auto canvas = attach_empty();
        auto march = processor_for(field);
        march->on_attach(canvas);

        march->configure({
            .max_steps = 64U,
            .step_scale = 0.75F,
            .density_scale = 2.0F,
            .absorption = 4.0F,
            .emission = 6.0F,
            .cool = glm::vec3(0.0F, 0.1F, 0.2F),
            .hot = glm::vec3(0.8F, 0.9F, 1.0F),
            .threshold = 0.25F,
        });
        march->processing_function(canvas);

        const auto& bytes = fragment(canvas)->data;
        EXPECT_EQ(constant_at<uint32_t>(bytes, 12U), 64U);
        EXPECT_EQ(constant_at<float>(bytes, 28U), 0.75F);
        EXPECT_EQ(constant_at<float>(bytes, 44U), 2.0F);
        EXPECT_EQ(constant_at<float>(bytes, 60U), 4.0F);
        EXPECT_EQ(constant_at<float>(bytes, 76U), 6.0F);
        EXPECT_EQ(constant_at<float>(bytes, 52U), 0.1F);
        EXPECT_EQ(constant_at<float>(bytes, 72U), 1.0F);
        EXPECT_EQ(constant_at<float>(bytes, 80U), 0.25F);

        march->set_step_scale(0.5F);
        march->processing_function(canvas);
        EXPECT_EQ(constant_at<uint32_t>(fragment(canvas)->data, 12U), 64U);
    }

    TEST_F(VolumeRaymarchDeviceTest, BufferWiresTheMarchStageAndTheRenderProcessor)
    {
        auto field = field_volume();
        const size_t bytes = field->get_field_bytes("density");
        auto marcher = std::make_shared<RaymarchBuffer>(
            unit_cell_lattice(),
            [field] { return field->read_handle("density"); }, bytes);
        marchers.push_back(marcher);
        service->initialize_buffer(marcher);
        ASSERT_TRUE(marcher->is_initialized());

        EXPECT_EQ(marcher->get_lattice().resolution, glm::uvec3(4U));
        EXPECT_EQ(marcher->march_processor(), nullptr);

        marcher->setup_processors(Buffers::ProcessingToken::GRAPHICS_BACKEND);
        const auto march = marcher->march_processor();
        ASSERT_NE(march, nullptr);
        const auto processors = marcher->get_processing_chain()->get_processors(marcher);
        EXPECT_TRUE(std::ranges::any_of(processors, [&march](const auto& processor) {
            return processor == march;
        }));

        auto window = std::make_shared<ScriptedWindow>(320, 240);
        windows.push_back(window);
        marcher->setup_rendering({ .target_window = window });
        ASSERT_NE(marcher->get_render_processor(), nullptr);

        const auto& bindings = marcher->get_render_processor()->get_config().bindings;
        ASSERT_TRUE(bindings.contains("volume_field"));
        EXPECT_EQ(bindings.at("volume_field").set, 0U);
        EXPECT_EQ(bindings.at("volume_field").binding, RaymarchProcessor::k_field_binding);
        EXPECT_EQ(bindings.at("volume_field").type, vk::DescriptorType::eStorageBuffer);
    }

} // namespace
} // namespace MayaFlux::Test
