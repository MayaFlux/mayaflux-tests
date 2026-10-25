#include "gtest/gtest.h"

#include <cstdint>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "volume_grid_fixture.hpp"

#include "MayaFlux/Buffers/State/AdvectProcessor.hpp"
#include "MayaFlux/Buffers/State/BuoyancyProcessor.hpp"
#include "MayaFlux/Buffers/State/DiffuseProcessor.hpp"
#include "MayaFlux/Buffers/State/DivergenceProcessor.hpp"
#include "MayaFlux/Buffers/State/PressureProcessor.hpp"
#include "MayaFlux/Buffers/State/SolenoidalProcessor.hpp"
#include "MayaFlux/Buffers/State/WallProcessor.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::VolumeGridBuffer;

    constexpr const char* k_advect_scalar = "volume_advect_scalar.comp.spv";
    constexpr const char* k_buoyancy = "volume_buoyancy.comp.spv";
    constexpr const char* k_diffuse = "volume_diffuse_vector.comp.spv";
    constexpr const char* k_divergence = "volume_divergence.comp.spv";
    constexpr const char* k_pressure = "volume_pressure_jacobi.comp.spv";
    constexpr const char* k_solenoidal = "volume_solenoidal.comp.spv";
    constexpr const char* k_wall = "volume_wall.comp.spv";

    constexpr uint32_t k_cells = 64U;

    uint32_t axis_x(size_t index) { return static_cast<uint32_t>(index % 4U); }
    uint32_t axis_y(size_t index) { return static_cast<uint32_t>((index / 4U) % 4U); }
    uint32_t axis_z(size_t index) { return static_cast<uint32_t>(index / 16U); }
    size_t cell_at(uint32_t x, uint32_t y, uint32_t z) { return (static_cast<size_t>(z) * 4U + y) * 4U + x; }

    template <typename Config>
    void expect_binding(const Config& config, const std::string& name, uint32_t binding)
    {
        ASSERT_TRUE(config.bindings.contains(name)) << name;
        EXPECT_EQ(config.bindings.at(name).set, 0U) << name;
        EXPECT_EQ(config.bindings.at(name).binding, binding) << name;
        EXPECT_EQ(config.bindings.at(name).type, vk::DescriptorType::eStorageBuffer) << name;
    }

    class VolumeFlowDeviceTest : public VolumeGridDeviceFixture {
    protected:
        std::shared_ptr<VolumeGridBuffer> grid(std::vector<VolumeGridBuffer::FieldDecl> fields)
        {
            return attach(std::move(fields), unit_cell_lattice());
        }

        template <typename Stage>
        static void run(const std::shared_ptr<VolumeGridBuffer>& volume,
            const std::shared_ptr<Stage>& stage)
        {
            volume->set_default_processor(stage);
            volume->process_default();
        }

        static std::vector<float> indices()
        {
            std::vector<float> values(k_cells);
            std::iota(values.begin(), values.end(), 0.0F);
            return values;
        }

        VolumeGridBuffer::FlowConfig basic_flow(const std::shared_ptr<VolumeGridBuffer>& volume)
        {
            VolumeGridBuffer::FlowConfig config;
            config.velocity = volume->declare_vector("velocity");
            config.divergence = volume->declare_scratch("divergence");
            config.pressure = volume->declare_scalar("pressure");
            return config;
        }

        std::shared_ptr<VolumeGridBuffer> flow_volume()
        {
            auto volume = attach_empty(unit_cell_lattice());
            volume->setup_processors(Buffers::ProcessingToken::GRAPHICS_BACKEND);
            return volume;
        }
    };

    TEST_F(VolumeFlowDeviceTest, AdvectBindsVelocityAndCarriedFieldInSetZero)
    {
        Buffers::AdvectProcessor stage("velocity", "smoke", k_advect_scalar);
        const auto& config = stage.get_config();

        ASSERT_EQ(config.bindings.size(), 3U);
        expect_binding(config, "velocity_in", 0U);
        expect_binding(config, "carried_in", 1U);
        expect_binding(config, "carried_out", 2U);
    }

    TEST_F(VolumeFlowDeviceTest, AdvectWritesStepAndDissipationIntoItsTail)
    {
        auto volume = grid({ vector_decl("velocity"), scalar_decl("smoke") });
        auto stage = std::make_shared<Buffers::AdvectProcessor>("velocity", "smoke", k_advect_scalar);
        volume->set_default_processor(stage);

        const auto& data = stage->get_push_constant_data();
        ASSERT_GE(data.size(), 48U);
        EXPECT_EQ(stage->get_config().push_constant_size, 48U);
        EXPECT_EQ(constant_at<uint32_t>(data, 0U), 4U);
        EXPECT_EQ(constant_at<float>(data, 16U), 1.0F);

        stage->set_time_step(0.25F);
        stage->set_dissipation(0.5F);
        EXPECT_EQ(constant_at<float>(data, 28U), 0.25F);
        EXPECT_EQ(constant_at<float>(data, 32U), 0.5F);
    }

    TEST_F(VolumeFlowDeviceTest, AdvectWithZeroVelocityOnlyScalesByDissipation)
    {
        auto volume = grid({ vector_decl("velocity"), scalar_decl("smoke") });
        fill(volume, "velocity", glm::vec4(0.0F));
        seed_raw(volume, "smoke", indices());
        auto stage = std::make_shared<Buffers::AdvectProcessor>("velocity", "smoke", k_advect_scalar);
        stage->set_time_step(0.5F);
        stage->set_dissipation(0.5F);
        const auto initial_read = volume->read_handle("smoke");

        run(volume, stage);

        EXPECT_NE(volume->read_handle("smoke"), initial_read);
        std::vector<float> expected = indices();
        for (auto& value : expected) {
            value *= 0.5F;
        }
        expect_scalars(read<float>(volume, "smoke"), expected);
    }

    TEST_F(VolumeFlowDeviceTest, AdvectCarriesTheFieldDownstreamByTheBacktrace)
    {
        auto volume = grid({ vector_decl("velocity"), scalar_decl("smoke") });
        fill(volume, "velocity", glm::vec4(1.0F, 0.0F, 0.0F, 0.0F));
        std::vector<float> source(k_cells);
        for (size_t i = 0; i < source.size(); ++i) {
            source[i] = static_cast<float>(axis_x(i));
        }
        seed_raw(volume, "smoke", source);
        auto stage = std::make_shared<Buffers::AdvectProcessor>("velocity", "smoke", k_advect_scalar);
        stage->set_time_step(1.0F);
        stage->set_dissipation(1.0F);

        run(volume, stage);

        std::vector<float> expected(k_cells);
        for (size_t i = 0; i < expected.size(); ++i) {
            expected[i] = axis_x(i) == 0U ? 0.0F : static_cast<float>(axis_x(i) - 1U);
        }
        expect_scalars(read<float>(volume, "smoke"), expected);
    }

    TEST_F(VolumeFlowDeviceTest, BuoyancyBindsTemperatureDensityAndVelocity)
    {
        Buffers::BuoyancyProcessor stage("temperature", "density", "velocity",
            glm::vec3(0.0F, 1.0F, 0.0F), k_buoyancy);
        const auto& config = stage.get_config();

        ASSERT_EQ(config.bindings.size(), 4U);
        expect_binding(config, "temperature_in", 0U);
        expect_binding(config, "density_in", 1U);
        expect_binding(config, "velocity_in", 2U);
        expect_binding(config, "velocity_out", 3U);
    }

    TEST_F(VolumeFlowDeviceTest, BuoyancyWritesEverySettingIntoItsTail)
    {
        auto volume = grid({ scalar_decl("temperature"), scalar_decl("density"), vector_decl("velocity") });
        auto stage = std::make_shared<Buffers::BuoyancyProcessor>("temperature", "density", "velocity",
            glm::vec3(0.0F, 1.0F, 0.0F), k_buoyancy);
        volume->set_default_processor(stage);
        stage->set_time_step(0.5F);
        stage->set_direction(glm::vec3(1.0F, 2.0F, 3.0F));
        stage->set_ambient(4.0F);
        stage->set_temperature_gain(5.0F);
        stage->set_density_gain(6.0F);

        const auto& data = stage->get_push_constant_data();
        ASSERT_GE(data.size(), 64U);
        EXPECT_EQ(stage->get_config().push_constant_size, 64U);
        EXPECT_EQ(stage->get_time_step(), 0.5F);
        EXPECT_EQ(constant_at<float>(data, 28U), 0.5F);
        EXPECT_EQ(constant_at<float>(data, 32U), 1.0F);
        EXPECT_EQ(constant_at<float>(data, 36U), 2.0F);
        EXPECT_EQ(constant_at<float>(data, 40U), 3.0F);
        EXPECT_EQ(constant_at<float>(data, 44U), 4.0F);
        EXPECT_EQ(constant_at<float>(data, 48U), 5.0F);
        EXPECT_EQ(constant_at<float>(data, 52U), 6.0F);
    }

    TEST_F(VolumeFlowDeviceTest, BuoyancyAddsNetLiftAlongTheDirectionAndKeepsTheFourthComponent)
    {
        auto volume = grid({ scalar_decl("temperature"), scalar_decl("density"), vector_decl("velocity") });
        fill(volume, "temperature", 3.0F);
        fill(volume, "density", 1.0F);
        fill(volume, "velocity", glm::vec4(0.0F, 0.5F, 0.0F, 7.0F));
        auto stage = std::make_shared<Buffers::BuoyancyProcessor>("temperature", "density", "velocity",
            glm::vec3(0.0F, 1.0F, 0.0F), k_buoyancy);
        stage->set_time_step(0.5F);
        stage->set_ambient(1.0F);
        stage->set_temperature_gain(2.0F);
        stage->set_density_gain(1.0F);

        run(volume, stage);

        expect_vectors(read<glm::vec4>(volume, "velocity"),
            std::vector<glm::vec4>(k_cells, glm::vec4(0.0F, 2.0F, 0.0F, 7.0F)));
    }

    TEST_F(VolumeFlowDeviceTest, DiffuseBindsScratchAsSourceAndTargetAsPingPongPair)
    {
        Buffers::DiffuseProcessor stage("velocity", "scratch", k_diffuse);
        const auto& config = stage.get_config();

        ASSERT_EQ(config.bindings.size(), 3U);
        expect_binding(config, "source", 0U);
        expect_binding(config, "target_a", 1U);
        expect_binding(config, "target_b", 2U);
        EXPECT_EQ(stage.get_iteration_count(), 20U);
    }

    TEST_F(VolumeFlowDeviceTest, DiffuseCoefficientIsRateTimesStep)
    {
        auto volume = grid({ vector_decl("velocity"), vector_decl("scratch", false) });
        auto stage = std::make_shared<Buffers::DiffuseProcessor>("velocity", "scratch", k_diffuse, 4U);
        volume->set_default_processor(stage);
        stage->set_rate(3.0F);
        stage->set_time_step(0.5F);

        EXPECT_EQ(stage->get_iteration_count(), 4U);
        EXPECT_EQ(constant_at<float>(stage->get_push_constant_data(), 28U), 1.5F);
    }

    TEST_F(VolumeFlowDeviceTest, DiffuseOneIterationSpreadsASpikeAndKeepsTheSourceInScratch)
    {
        auto volume = grid({ vector_decl("velocity"), vector_decl("scratch", false) });
        std::vector<glm::vec4> spike(k_cells, glm::vec4(0.0F));
        spike[cell_at(1U, 1U, 1U)] = glm::vec4(1.0F, 0.0F, 0.0F, 0.0F);
        seed_raw(volume, "velocity", spike);
        auto stage = std::make_shared<Buffers::DiffuseProcessor>("velocity", "scratch", k_diffuse, 1U);
        stage->set_rate(1.0F);
        stage->set_time_step(0.5F);
        const auto initial_read = volume->read_handle("velocity");

        run(volume, stage);

        EXPECT_NE(volume->read_handle("velocity"), initial_read);
        std::vector<glm::vec4> expected(k_cells, glm::vec4(0.0F));
        expected[cell_at(1U, 1U, 1U)].x = 0.25F;
        for (const auto& neighbour : { cell_at(0U, 1U, 1U), cell_at(2U, 1U, 1U),
                 cell_at(1U, 0U, 1U), cell_at(1U, 2U, 1U),
                 cell_at(1U, 1U, 0U), cell_at(1U, 1U, 2U) }) {
            expected[neighbour].x = 0.125F;
        }
        expect_vectors(read<glm::vec4>(volume, "velocity"), expected);
        expect_vectors(read<glm::vec4>(volume, "scratch"), spike);
    }

    TEST_F(VolumeFlowDeviceTest, DiffuseEvenIterationCountEndsOnTheOriginalSlot)
    {
        auto volume = grid({ vector_decl("velocity"), vector_decl("scratch", false) });
        const glm::vec4 uniform(0.5F, -1.0F, 2.0F, 3.0F);
        fill(volume, "velocity", uniform);
        auto stage = std::make_shared<Buffers::DiffuseProcessor>("velocity", "scratch", k_diffuse, 2U);
        stage->set_rate(1.0F);
        stage->set_time_step(0.5F);
        const auto initial_read = volume->read_handle("velocity");

        run(volume, stage);

        EXPECT_EQ(volume->read_handle("velocity"), initial_read);
        expect_vectors(read<glm::vec4>(volume, "velocity"), std::vector<glm::vec4>(k_cells, uniform));
    }

    TEST_F(VolumeFlowDeviceTest, DivergenceBindsVelocityInAndDivergenceOut)
    {
        Buffers::DivergenceProcessor stage("velocity", "divergence", k_divergence);
        const auto& config = stage.get_config();

        ASSERT_EQ(config.bindings.size(), 2U);
        expect_binding(config, "velocity_in", 0U);
        expect_binding(config, "divergence_out", 1U);
    }

    TEST_F(VolumeFlowDeviceTest, DivergenceUsesCentralDifferencesWithClampedEdges)
    {
        auto volume = grid({ vector_decl("velocity"), scalar_decl("divergence", false) });
        std::vector<glm::vec4> ramp(k_cells);
        for (size_t i = 0; i < ramp.size(); ++i) {
            ramp[i] = glm::vec4(static_cast<float>(axis_x(i)), 0.0F, 0.0F, 0.0F);
        }
        seed_raw(volume, "velocity", ramp);
        auto stage = std::make_shared<Buffers::DivergenceProcessor>("velocity", "divergence", k_divergence);
        const auto initial_read = volume->read_handle("velocity");

        run(volume, stage);

        EXPECT_EQ(volume->read_handle("velocity"), initial_read);
        std::vector<float> expected(k_cells);
        for (size_t i = 0; i < expected.size(); ++i) {
            expected[i] = (axis_x(i) == 0U || axis_x(i) == 3U) ? 0.5F : 1.0F;
        }
        expect_scalars(read<float>(volume, "divergence"), expected);
    }

    TEST_F(VolumeFlowDeviceTest, SolenoidalBindsPressureAndReadsWritesVelocity)
    {
        Buffers::SolenoidalProcessor stage("pressure", "velocity", k_solenoidal);
        const auto& config = stage.get_config();

        ASSERT_EQ(config.bindings.size(), 3U);
        expect_binding(config, "pressure_in", 0U);
        expect_binding(config, "velocity_in", 1U);
        expect_binding(config, "velocity_out", 2U);
    }

    TEST_F(VolumeFlowDeviceTest, SolenoidalSubtractsThePressureGradient)
    {
        auto volume = grid({ scalar_decl("pressure"), vector_decl("velocity") });
        std::vector<float> pressure(k_cells);
        for (size_t i = 0; i < pressure.size(); ++i) {
            pressure[i] = static_cast<float>(axis_x(i));
        }
        seed_raw(volume, "pressure", pressure);
        fill(volume, "velocity", glm::vec4(10.0F, 10.0F, 10.0F, 5.0F));
        auto stage = std::make_shared<Buffers::SolenoidalProcessor>("pressure", "velocity", k_solenoidal);
        const auto initial_read = volume->read_handle("velocity");

        run(volume, stage);

        EXPECT_NE(volume->read_handle("velocity"), initial_read);
        std::vector<glm::vec4> expected(k_cells);
        for (size_t i = 0; i < expected.size(); ++i) {
            const float gradient = (axis_x(i) == 0U || axis_x(i) == 3U) ? 0.5F : 1.0F;
            expected[i] = glm::vec4(10.0F - gradient, 10.0F, 10.0F, 5.0F);
        }
        expect_vectors(read<glm::vec4>(volume, "velocity"), expected);
    }

    TEST_F(VolumeFlowDeviceTest, WallBindsVelocityAsReadAndWrite)
    {
        Buffers::WallProcessor stage("velocity", k_wall);
        const auto& config = stage.get_config();

        ASSERT_EQ(config.bindings.size(), 2U);
        expect_binding(config, "velocity_in", 0U);
        expect_binding(config, "velocity_out", 1U);
    }

    TEST_F(VolumeFlowDeviceTest, WallZeroesOnlyTheNormalComponentOnEachFace)
    {
        auto volume = grid({ vector_decl("velocity") });
        fill(volume, "velocity", glm::vec4(1.0F, 2.0F, 3.0F, 4.0F));
        auto stage = std::make_shared<Buffers::WallProcessor>("velocity", k_wall);
        const auto initial_read = volume->read_handle("velocity");

        run(volume, stage);

        EXPECT_NE(volume->read_handle("velocity"), initial_read);
        std::vector<glm::vec4> expected(k_cells);
        for (size_t i = 0; i < expected.size(); ++i) {
            const bool x_face = axis_x(i) == 0U || axis_x(i) == 3U;
            const bool y_face = axis_y(i) == 0U || axis_y(i) == 3U;
            const bool z_face = axis_z(i) == 0U || axis_z(i) == 3U;
            expected[i] = glm::vec4(x_face ? 0.0F : 1.0F, y_face ? 0.0F : 2.0F,
                z_face ? 0.0F : 3.0F, 4.0F);
        }
        expect_vectors(read<glm::vec4>(volume, "velocity"), expected);
    }

    TEST_F(VolumeFlowDeviceTest, PressureBindsDivergenceAndPingPongPair)
    {
        Buffers::PressureProcessor stage("divergence", "pressure", k_pressure);
        const auto& config = stage.get_config();

        ASSERT_EQ(config.bindings.size(), 3U);
        expect_binding(config, "divergence_in", 0U);
        expect_binding(config, "pressure_a", 1U);
        expect_binding(config, "pressure_b", 2U);
        EXPECT_EQ(stage.get_iteration_count(), 40U);
    }

    TEST_F(VolumeFlowDeviceTest, PressureOddIterationCountSwapsToTheWrittenSlot)
    {
        auto volume = grid({ scalar_decl("divergence", false), scalar_decl("pressure") });
        fill(volume, "divergence", 6.0F);
        fill(volume, "pressure", 0.0F);
        auto stage = std::make_shared<Buffers::PressureProcessor>("divergence", "pressure", k_pressure, 1U);
        const auto initial_read = volume->read_handle("pressure");

        run(volume, stage);

        EXPECT_NE(volume->read_handle("pressure"), initial_read);
        expect_scalars(read<float>(volume, "pressure"), std::vector<float>(k_cells, -1.0F));
    }

    TEST_F(VolumeFlowDeviceTest, PressureEvenIterationCountEndsOnTheOriginalSlot)
    {
        auto volume = grid({ scalar_decl("divergence", false), scalar_decl("pressure") });
        fill(volume, "divergence", 6.0F);
        fill(volume, "pressure", 0.0F);
        auto stage = std::make_shared<Buffers::PressureProcessor>("divergence", "pressure", k_pressure, 2U);
        const auto initial_read = volume->read_handle("pressure");

        run(volume, stage);

        EXPECT_EQ(volume->read_handle("pressure"), initial_read);
        expect_scalars(read<float>(volume, "pressure"), std::vector<float>(k_cells, -2.0F));
    }

    TEST_F(VolumeFlowDeviceTest, SetupFlowAppendsStagesInProjectionOrder)
    {
        auto volume = flow_volume();
        auto config = basic_flow(volume);
        config.scratch = volume->declare_vector("scratch");
        config.viscosity = 0.1F;
        config.jacobi_iterations = 8U;
        config.time_step = 0.25F;
        const auto smoke = volume->declare_scalar("smoke");
        config.carried.push_back({ .field = smoke, .dissipation = 0.9F });
        config.buoyancy = VolumeGridBuffer::FlowConfig::Buoyancy {
            .temperature = volume->declare_scalar("temperature"),
            .density = volume->declare_scalar("density"),
            .direction = glm::vec3(0.0F, 1.0F, 0.0F),
            .temperature_gain = 2.0F,
            .density_gain = 1.0F,
            .ambient = 0.5F,
        };

        const auto stages = volume->setup_flow(config);

        ASSERT_NE(stages.self_advect, nullptr);
        ASSERT_NE(stages.buoyancy, nullptr);
        ASSERT_NE(stages.diffuse, nullptr);
        ASSERT_NE(stages.wall_advected, nullptr);
        ASSERT_NE(stages.divergence, nullptr);
        ASSERT_NE(stages.pressure, nullptr);
        ASSERT_NE(stages.solenoidal, nullptr);
        ASSERT_NE(stages.wall_projected, nullptr);
        ASSERT_EQ(stages.carriers.size(), 1U);
        EXPECT_NE(stages.wall_advected, stages.wall_projected);
        EXPECT_EQ(stages.pressure->get_iteration_count(), 8U);
        EXPECT_EQ(stages.buoyancy->get_time_step(), 0.25F);

        const std::vector<std::shared_ptr<Buffers::BufferProcessor>> expected {
            stages.self_advect, stages.buoyancy, stages.diffuse, stages.wall_advected,
            stages.divergence, stages.pressure, stages.solenoidal, stages.wall_projected,
            stages.carriers.front()
        };
        const auto processors = volume->get_processing_chain()->get_processors(volume);
        ASSERT_EQ(processors.size(), expected.size());
        for (size_t i = 0; i < expected.size(); ++i) {
            EXPECT_EQ(processors[i], expected[i]) << "stage " << i;
        }
    }

    TEST_F(VolumeFlowDeviceTest, SetupFlowOmitsOptionalStagesWhenConfigured)
    {
        auto volume = flow_volume();
        auto config = basic_flow(volume);
        config.walls = false;

        const auto stages = volume->setup_flow(config);

        EXPECT_EQ(stages.buoyancy, nullptr);
        EXPECT_EQ(stages.diffuse, nullptr);
        EXPECT_EQ(stages.wall_advected, nullptr);
        EXPECT_EQ(stages.wall_projected, nullptr);
        EXPECT_TRUE(stages.carriers.empty());
        ASSERT_NE(stages.self_advect, nullptr);
        ASSERT_NE(stages.divergence, nullptr);
        ASSERT_NE(stages.pressure, nullptr);
        ASSERT_NE(stages.solenoidal, nullptr);
        EXPECT_EQ(volume->get_processing_chain()->get_processors(volume).size(), 4U);
    }

    TEST_F(VolumeFlowDeviceTest, SetupFlowRejectsMissingRequiredFieldsWithoutAddingStages)
    {
        auto volume = flow_volume();
        const auto declared = basic_flow(volume);

        auto config = declared;
        config.pressure = {};
        auto stages = volume->setup_flow(config);
        EXPECT_EQ(stages.self_advect, nullptr);
        EXPECT_TRUE(volume->get_processing_chain()->get_processors(volume).empty());

        config = declared;
        config.viscosity = 0.5F;
        stages = volume->setup_flow(config);
        EXPECT_EQ(stages.self_advect, nullptr);
        EXPECT_TRUE(volume->get_processing_chain()->get_processors(volume).empty());
    }

    TEST_F(VolumeFlowDeviceTest, SetupFlowRejectsNamesTheVolumeDoesNotDeclare)
    {
        auto volume = flow_volume();
        auto config = basic_flow(volume);
        config.velocity.name = "undeclared";

        const auto stages = volume->setup_flow(config);

        EXPECT_EQ(stages.self_advect, nullptr);
        EXPECT_TRUE(volume->get_processing_chain()->get_processors(volume).empty());
    }

    TEST_F(VolumeFlowDeviceTest, QuiescentFlowCycleOnlyDissipatesCarriedScalars)
    {
        auto volume = flow_volume();
        auto config = basic_flow(volume);
        const auto smoke = volume->declare_scalar("smoke");
        config.carried.push_back({ .field = smoke, .dissipation = 0.5F });
        volume->setup_flow(config);
        fill(volume, "velocity", glm::vec4(0.0F));
        fill(volume, "pressure", 0.0F);
        fill(volume, "divergence", 0.0F);
        seed_raw(volume, "smoke", indices());

        volume->get_processing_chain()->process(volume);

        std::vector<float> expected = indices();
        for (auto& value : expected) {
            value *= 0.5F;
        }
        expect_scalars(read<float>(volume, "smoke"), expected);
        expect_vectors(read<glm::vec4>(volume, "velocity"),
            std::vector<glm::vec4>(k_cells, glm::vec4(0.0F)));
    }

} // namespace
} // namespace MayaFlux::Test
