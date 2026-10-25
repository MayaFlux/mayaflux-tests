#include "gtest/gtest.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "volume_grid_fixture.hpp"

#include "MayaFlux/Buffers/State/InfluxProcessor.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::InfluxProcessor;
    using Buffers::VolumeGridBuffer;

    constexpr const char* k_blob = "volume_influx_blob.comp.spv";
    constexpr uint32_t k_cells = 64U;

    InfluxProcessor::Spot blob_spot()
    {
        return {
            .center = glm::vec3(1.5F),
            .radius = 0.5F,
            .rate = 2.0F,
            .time_step = 0.5F,
            .falloff = 1.0F,
        };
    }

    std::vector<float> blob_field(float base)
    {
        std::vector<float> expected(k_cells);
        for (size_t i = 0; i < expected.size(); ++i) {
            const float dx = static_cast<float>(i % 4U) - 1.0F;
            const float dy = static_cast<float>((i / 4U) % 4U) - 1.0F;
            const float dz = static_cast<float>(i / 16U) - 1.0F;
            const float r2 = (dx * dx + dy * dy + dz * dz) / 0.25F;
            expected[i] = base + std::exp(-r2);
        }
        return expected;
    }

    class VolumeInfluxDeviceTest : public VolumeGridDeviceFixture {
    protected:
        std::shared_ptr<VolumeGridBuffer> density_volume(
            Kinesis::Lattice3D lattice = unit_cell_lattice())
        {
            return attach({ scalar_decl("density") }, lattice);
        }

        static void run(const std::shared_ptr<VolumeGridBuffer>& volume,
            const std::shared_ptr<InfluxProcessor>& stage)
        {
            volume->set_default_processor(stage);
            volume->process_default();
        }
    };

    TEST_F(VolumeInfluxDeviceTest, BindsOneFieldAsReadAndWriteInSetZero)
    {
        InfluxProcessor stage("density", k_blob);
        const auto& bindings = stage.get_config().bindings;

        ASSERT_EQ(bindings.size(), 2U);
        EXPECT_EQ(bindings.at("field_in").set, 0U);
        EXPECT_EQ(bindings.at("field_in").binding, 0U);
        EXPECT_EQ(bindings.at("field_out").set, 0U);
        EXPECT_EQ(bindings.at("field_out").binding, 1U);
    }

    TEST_F(VolumeInfluxDeviceTest, SpotConstructorAppliesThePlacementBeforeAttachment)
    {
        auto lattice = unit_cell_lattice();
        lattice.bounds = { .min = glm::vec3(-2.0F), .max = glm::vec3(2.0F) };
        auto volume = density_volume(lattice);
        auto stage = std::make_shared<InfluxProcessor>("density", k_blob,
            InfluxProcessor::Spot {
                .center = glm::vec3(1.0F, 2.0F, 3.0F),
                .radius = 0.5F,
                .rate = 2.0F,
                .time_step = 0.25F,
                .falloff = 3.0F,
            });
        volume->set_default_processor(stage);

        const auto& data = stage->get_push_constant_data();
        ASSERT_GE(data.size(), 80U);
        EXPECT_EQ(stage->get_config().push_constant_size, 80U);
        EXPECT_EQ(constant_at<uint32_t>(data, 0U), 4U);
        EXPECT_EQ(constant_at<float>(data, 16U), 1.0F);
        EXPECT_EQ(constant_at<float>(data, 28U), 0.0F);
        EXPECT_EQ(constant_at<float>(data, 32U), 1.0F);
        EXPECT_EQ(constant_at<float>(data, 36U), 2.0F);
        EXPECT_EQ(constant_at<float>(data, 40U), 3.0F);
        EXPECT_EQ(constant_at<float>(data, 44U), 0.5F);
        EXPECT_EQ(constant_at<float>(data, 48U), 2.0F);
        EXPECT_EQ(constant_at<float>(data, 52U), 0.25F);
        EXPECT_EQ(constant_at<float>(data, 56U), 3.0F);
        EXPECT_EQ(constant_at<float>(data, 64U), -2.0F);
        EXPECT_EQ(constant_at<float>(data, 68U), -2.0F);
        EXPECT_EQ(constant_at<float>(data, 72U), -2.0F);
    }

    TEST_F(VolumeInfluxDeviceTest, SettersRewriteTheTailOnceAttached)
    {
        auto volume = density_volume();
        auto stage = std::make_shared<InfluxProcessor>("density", k_blob);
        volume->set_default_processor(stage);
        const auto& data = stage->get_push_constant_data();

        stage->set_center(glm::vec3(0.5F, 1.5F, 2.5F));
        stage->set_radius(0.75F);
        stage->set_rate(4.0F);
        stage->set_time_step(0.125F);
        stage->set_falloff(2.0F);

        EXPECT_EQ(constant_at<float>(data, 32U), 0.5F);
        EXPECT_EQ(constant_at<float>(data, 36U), 1.5F);
        EXPECT_EQ(constant_at<float>(data, 40U), 2.5F);
        EXPECT_EQ(constant_at<float>(data, 44U), 0.75F);
        EXPECT_EQ(constant_at<float>(data, 48U), 4.0F);
        EXPECT_EQ(constant_at<float>(data, 52U), 0.125F);
        EXPECT_EQ(constant_at<float>(data, 56U), 2.0F);
    }

    TEST_F(VolumeInfluxDeviceTest, ZeroRadiusIsEightPercentOfTheSmallestSide)
    {
        auto lattice = unit_cell_lattice();
        lattice.bounds = { .min = glm::vec3(0.0F), .max = glm::vec3(4.0F, 8.0F, 6.0F) };
        auto volume = density_volume(lattice);
        auto stage = std::make_shared<InfluxProcessor>("density", k_blob);
        stage->configure({ .radius = 0.0F });
        volume->set_default_processor(stage);
        const auto& data = stage->get_push_constant_data();

        EXPECT_NEAR(constant_at<float>(data, 44U), 0.32F, 1e-6F);

        stage->set_radius(0.75F);
        EXPECT_EQ(constant_at<float>(data, 44U), 0.75F);

        stage->configure({ .radius = 0.0F });
        EXPECT_NEAR(constant_at<float>(data, 44U), 0.32F, 1e-6F);
    }

    TEST_F(VolumeInfluxDeviceTest, ElapsedAdvancesPerCycleAndRestarts)
    {
        auto volume = density_volume();
        fill(volume, "density", 0.0F);
        auto stage = std::make_shared<InfluxProcessor>("density", k_blob, blob_spot());
        run(volume, stage);

        EXPECT_EQ(stage->get_elapsed(), 0.5F);
        EXPECT_EQ(constant_at<float>(stage->get_push_constant_data(), 28U), 0.5F);

        volume->process_default();
        EXPECT_EQ(stage->get_elapsed(), 1.0F);
        EXPECT_EQ(constant_at<float>(stage->get_push_constant_data(), 28U), 1.0F);

        stage->restart();
        EXPECT_EQ(stage->get_elapsed(), 0.0F);
        EXPECT_EQ(constant_at<float>(stage->get_push_constant_data(), 28U), 0.0F);
    }

    TEST_F(VolumeInfluxDeviceTest, BlobAddsRateTimesStepWithGaussianFalloffIntoTheField)
    {
        auto volume = density_volume();
        fill(volume, "density", 1.0F);
        auto stage = std::make_shared<InfluxProcessor>("density", k_blob, blob_spot());
        const auto initial_read = volume->read_handle("density");

        run(volume, stage);

        EXPECT_NE(volume->read_handle("density"), initial_read);
        expect_scalars(read<float>(volume, "density"), blob_field(1.0F), 1e-4F);
    }

    TEST_F(VolumeInfluxDeviceTest, RepeatedCyclesAccumulateTheDeposit)
    {
        auto volume = density_volume();
        fill(volume, "density", 0.0F);
        auto stage = std::make_shared<InfluxProcessor>("density", k_blob, blob_spot());

        run(volume, stage);
        volume->process_default();

        auto expected = blob_field(0.0F);
        for (auto& value : expected) {
            value *= 2.0F;
        }
        expect_scalars(read<float>(volume, "density"), expected, 1e-4F);
    }

    TEST_F(VolumeInfluxDeviceTest, VolumeInfluxAppendsTheStageToItsChain)
    {
        auto volume = density_volume();
        volume->setup_processors(Buffers::ProcessingToken::GRAPHICS_BACKEND);
        fill(volume, "density", 0.0F);

        const auto stage = volume->influx("density", blob_spot());

        ASSERT_NE(stage, nullptr);
        const auto processors = volume->get_processing_chain()->get_processors(volume);
        ASSERT_EQ(processors.size(), 1U);
        EXPECT_EQ(processors.front(), stage);

        volume->get_processing_chain()->process(volume);
        expect_scalars(read<float>(volume, "density"), blob_field(0.0F), 1e-4F);
    }

    TEST_F(VolumeInfluxDeviceTest, VolumeInfluxStagesRunInTheOrderTheyWereAdded)
    {
        auto volume = attach({ scalar_decl("density"), scalar_decl("heat") }, unit_cell_lattice());
        volume->setup_processors(Buffers::ProcessingToken::GRAPHICS_BACKEND);

        const auto first = volume->influx("density", blob_spot());
        const auto second = volume->influx("heat", blob_spot());

        ASSERT_NE(first, nullptr);
        ASSERT_NE(second, nullptr);
        EXPECT_NE(first, second);
        const auto processors = volume->get_processing_chain()->get_processors(volume);
        ASSERT_EQ(processors.size(), 2U);
        EXPECT_EQ(processors[0], first);
        EXPECT_EQ(processors[1], second);
    }

} // namespace
} // namespace MayaFlux::Test
