#include "gtest/gtest.h"

#include <stdexcept>

#include "volume_grid_fixture.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::VolumeGridBuffer;

    class VolumeGridBufferDeviceTest : public VolumeGridDeviceFixture { };

    TEST_F(VolumeGridBufferDeviceTest, ConstructionKeepsLatticeFieldOrderAndSemantics)
    {
        const Kinesis::LatticeSemantics density_semantics {
            .value_class = Kinesis::LatticeValueClass::FogVolume,
        };
        const Kinesis::LatticeSemantics velocity_semantics {
            .variance = Kinesis::VectorVariance::ContravariantRelative,
        };
        auto volume = attach({
            { "density", sizeof(float), true, density_semantics },
            { "velocity", sizeof(glm::vec4), true, velocity_semantics },
            { "divergence", sizeof(float), false },
        });

        EXPECT_EQ(volume->get_width(), 2U);
        EXPECT_EQ(volume->get_height(), 2U);
        EXPECT_EQ(volume->get_depth(), 2U);
        EXPECT_EQ(volume->get_cell_count(), 8U);
        EXPECT_EQ(volume->get_cell_size(), glm::vec3(2.0F));
        EXPECT_EQ(volume->get_bounds().min, glm::vec3(-2.0F));
        EXPECT_EQ(volume->get_bounds().max, glm::vec3(2.0F));
        EXPECT_EQ(volume->get_field_names(),
            (std::vector<std::string> { "density", "velocity", "divergence" }));
        EXPECT_EQ(volume->get_field_stride("density"), sizeof(float));
        EXPECT_EQ(volume->get_field_bytes("density"), 8U * sizeof(float));
        EXPECT_EQ(volume->get_field_bytes("velocity"), 8U * sizeof(glm::vec4));
        EXPECT_EQ(volume->get_buffer_resources().back_buffers.size(), 5U);
        EXPECT_EQ(volume->get_field_semantics("density").value_class,
            Kinesis::LatticeValueClass::FogVolume);
        EXPECT_EQ(volume->get_field_semantics("velocity").variance,
            Kinesis::VectorVariance::ContravariantRelative);
        EXPECT_FALSE(volume->has_field("missing"));
        EXPECT_EQ(volume->get_field_stride("missing"), 0U);
        EXPECT_EQ(volume->get_field_bytes("missing"), 0U);
    }

    TEST_F(VolumeGridBufferDeviceTest, InitializerListConstructorRejectsNoValidFields)
    {
        EXPECT_THROW(static_cast<void>(VolumeGridBuffer(small_volume_lattice(),
                         { { "", sizeof(float) }, { "zero", 0U } })),
            std::runtime_error);
    }

    TEST_F(VolumeGridBufferDeviceTest, InvalidAndDuplicateDeclarationsLeaveExistingFieldsAlone)
    {
        auto volume = attach({
            { "density", sizeof(float) },
            { "density", sizeof(glm::vec4) },
            { "", sizeof(float) },
            { "zero", 0U },
        });

        EXPECT_EQ(volume->get_field_names(), (std::vector<std::string> { "density" }));
        EXPECT_EQ(volume->get_field_stride("density"), sizeof(float));
        EXPECT_EQ(volume->get_buffer_resources().back_buffers.size(), 2U);
        EXPECT_FALSE(volume->has_field("zero"));
    }

    TEST_F(VolumeGridBufferDeviceTest, LateDeclarationsIssueTypedOwnerBoundReferences)
    {
        auto volume = attach_empty();
        auto other = attach_empty();
        const auto density = volume->declare_scalar("density",
            { .value_class = Kinesis::LatticeValueClass::FogVolume });
        const auto velocity = volume->declare_vector("velocity",
            { .variance = Kinesis::VectorVariance::ContravariantRelative });
        const auto divergence = volume->declare_scratch("divergence");

        EXPECT_EQ(density.name, "density");
        EXPECT_EQ(velocity.name, "velocity");
        EXPECT_EQ(divergence.name, "divergence");
        EXPECT_TRUE(density.issued_by(volume.get()));
        EXPECT_TRUE(velocity.issued_by(volume.get()));
        EXPECT_TRUE(divergence.issued_by(volume.get()));
        EXPECT_FALSE(density.issued_by(other.get()));
        EXPECT_EQ(volume->get_field_names(),
            (std::vector<std::string> { "density", "velocity", "divergence" }));
        EXPECT_EQ(volume->get_buffer_resources().back_buffers.size(), 5U);
        EXPECT_EQ(volume->get_field_semantics("density").value_class,
            Kinesis::LatticeValueClass::FogVolume);
        EXPECT_EQ(volume->get_field_semantics("velocity").variance,
            Kinesis::VectorVariance::ContravariantRelative);
    }

    TEST_F(VolumeGridBufferDeviceTest, RejectedLateDeclarationsReturnEmptyReferences)
    {
        auto volume = attach_empty();
        const auto density = volume->declare_scalar("density");
        const auto duplicate = volume->declare_vector("density");
        const auto unnamed = volume->declare_scratch("");

        EXPECT_FALSE(density.name.empty());
        EXPECT_TRUE(duplicate.name.empty());
        EXPECT_TRUE(unnamed.name.empty());
        EXPECT_EQ(volume->get_buffer_resources().back_buffers.size(), 2U);
    }

    TEST_F(VolumeGridBufferDeviceTest, EnsureFieldsAddsOnlyValidMissingFields)
    {
        auto volume = attach({
            { "density", sizeof(float), true,
                { .value_class = Kinesis::LatticeValueClass::FogVolume } },
        });
        const auto added = volume->ensure_fields({
            { "density", sizeof(glm::vec4), false },
            { "pressure", sizeof(float) },
            { "divergence", sizeof(float), false },
            { "pressure", sizeof(float) },
            { "zero", 0U },
        });

        EXPECT_EQ(added, (std::vector<std::string> { "pressure", "divergence" }));
        EXPECT_EQ(volume->get_field_names(),
            (std::vector<std::string> { "density", "pressure", "divergence" }));
        EXPECT_EQ(volume->get_field_stride("density"), sizeof(float));
        EXPECT_EQ(volume->get_field_semantics("density").value_class,
            Kinesis::LatticeValueClass::FogVolume);
        EXPECT_EQ(volume->get_buffer_resources().back_buffers.size(), 5U);
    }

    TEST_F(VolumeGridBufferDeviceTest, SwappingOneFieldOnlyChangesItsOwnReadAndWriteHandles)
    {
        auto volume = attach({
            { "density", sizeof(float) },
            { "pressure", sizeof(float) },
            { "divergence", sizeof(float), false },
        });
        const auto density_read = volume->read_handle("density");
        const auto density_write = volume->write_handle("density");
        const auto pressure_read = volume->read_handle("pressure");
        const auto pressure_write = volume->write_handle("pressure");
        const auto scratch = volume->read_handle("divergence");

        ASSERT_TRUE(density_read);
        ASSERT_TRUE(density_write);
        EXPECT_NE(density_read, density_write);
        ASSERT_TRUE(scratch);
        EXPECT_EQ(scratch, volume->write_handle("divergence"));

        volume->swap_field("density");
        volume->swap_field("divergence");
        EXPECT_EQ(volume->read_handle("density"), density_write);
        EXPECT_EQ(volume->write_handle("density"), density_read);
        EXPECT_EQ(volume->read_handle("pressure"), pressure_read);
        EXPECT_EQ(volume->write_handle("pressure"), pressure_write);
        EXPECT_EQ(volume->read_handle("divergence"), scratch);

        volume->swap_field("density");
        EXPECT_EQ(volume->read_handle("density"), density_read);
    }

    TEST_F(VolumeGridBufferDeviceTest, RawSeedAndReadFollowTheCurrentGeneration)
    {
        auto volume = attach({ { "density", sizeof(float) } });
        const std::vector<float> first { 1, 2, 3, 4, 5, 6, 7, 8 };
        const std::vector<float> second { 8, 7, 6, 5, 4, 3, 2, 1 };

        seed_raw(volume, "density", first);
        EXPECT_EQ(read<float>(volume, "density"), first);
        volume->swap_field("density");
        seed_raw(volume, "density", second);
        EXPECT_EQ(read<float>(volume, "density"), second);
        volume->swap_field("density");
        EXPECT_EQ(read<float>(volume, "density"), first);
    }

    TEST_F(VolumeGridBufferDeviceTest, WrongRawSeedSizeDoesNotReplaceTheField)
    {
        auto volume = attach({ { "density", sizeof(float) } });
        const std::vector<float> original(8U, 3.0F);
        const std::vector<float> short_values(7U, 9.0F);
        seed_raw(volume, "density", original);
        seed_raw(volume, "density", short_values);

        EXPECT_EQ(read<float>(volume, "density"), original);
    }

    TEST_F(VolumeGridBufferDeviceTest, RawFieldsPreserveCustomCellStrides)
    {
        using Pair = std::array<float, 2>;
        auto volume = attach({ { "paired", sizeof(Pair), false } });
        const std::vector<Pair> values {
            { 1, 11 }, { 2, 12 }, { 3, 13 }, { 4, 14 },
            { 5, 15 }, { 6, 16 }, { 7, 17 }, { 8, 18 },
        };
        seed_raw(volume, "paired", values);

        EXPECT_EQ(volume->get_field_stride("paired"), sizeof(Pair));
        EXPECT_EQ(volume->get_field_bytes("paired"), values.size() * sizeof(values[0]));
        EXPECT_EQ(read<Pair>(volume, "paired"), values);
        EXPECT_EQ(volume->read_handle("paired"), volume->write_handle("paired"));
    }

    TEST_F(VolumeGridBufferDeviceTest, ScalarSeedSamplesCellCentresInXMajorOrder)
    {
        auto volume = attach({ { "density", sizeof(float) } });
        volume->seed("density", Kinesis::SpatialField { [](const glm::vec3& p) {
            return p.x + 10.0F * p.y + 100.0F * p.z;
        } });

        EXPECT_EQ(read<float>(volume, "density"),
            (std::vector<float> { -111.0F, -109.0F, -91.0F, -89.0F,
                89.0F, 91.0F, 109.0F, 111.0F }));
    }

    TEST_F(VolumeGridBufferDeviceTest, VectorSeedZerosFourthComponent)
    {
        auto volume = attach({ { "velocity", sizeof(glm::vec4) } });
        volume->seed("velocity", Kinesis::VectorField { [](const glm::vec3& p) {
            return glm::vec3(p.x, p.y, p.z);
        } });

        const auto values = read<glm::vec4>(volume, "velocity");
        for (uint32_t z = 0; z < 2U; ++z) {
            for (uint32_t y = 0; y < 2U; ++y) {
                for (uint32_t x = 0; x < 2U; ++x) {
                    const auto i = volume->get_lattice().index({ x, y, z });
                    EXPECT_EQ(values[i], glm::vec4(volume->get_lattice().cell_center({ x, y, z }), 0.0F));
                }
            }
        }
    }

    TEST_F(VolumeGridBufferDeviceTest, ScalarAccumulateAddsWithoutAffectingOtherFields)
    {
        auto volume = attach({ { "density", sizeof(float) }, { "pressure", sizeof(float) } });
        seed_raw(volume, "density", std::vector<float>(8U, 2.0F));
        seed_raw(volume, "pressure", std::vector<float>(8U, 7.0F));
        volume->accumulate("density", Kinesis::SpatialField { [](const glm::vec3& p) {
            return p.x;
        } });

        EXPECT_EQ(read<float>(volume, "density"),
            (std::vector<float> { 1, 3, 1, 3, 1, 3, 1, 3 }));
        EXPECT_EQ(read<float>(volume, "pressure"), std::vector<float>(8U, 7.0F));
    }

    TEST_F(VolumeGridBufferDeviceTest, VectorAccumulatePreservesFourthComponent)
    {
        auto volume = attach({ { "velocity", sizeof(glm::vec4) } });
        seed_raw(volume, "velocity", std::vector<glm::vec4>(8U, glm::vec4(1, 2, 3, 9)));
        volume->accumulate("velocity", Kinesis::VectorField { [](const glm::vec3& p) {
            return glm::vec3(p.x, p.y, p.z);
        } });

        const auto values = read<glm::vec4>(volume, "velocity");
        for (uint32_t z = 0; z < 2U; ++z) {
            for (uint32_t y = 0; y < 2U; ++y) {
                for (uint32_t x = 0; x < 2U; ++x) {
                    const auto i = volume->get_lattice().index({ x, y, z });
                    const auto p = volume->get_lattice().cell_center({ x, y, z });
                    EXPECT_EQ(values[i], glm::vec4(1.0F + p.x, 2.0F + p.y,
                                             3.0F + p.z, 9.0F));
                }
            }
        }
    }

    TEST_F(VolumeGridBufferDeviceTest, TypedWritesRejectFieldsWithTheWrongStride)
    {
        auto volume = attach({
            { "density", sizeof(float) },
            { "velocity", sizeof(glm::vec4) },
        });
        const std::vector<float> density(8U, 3.0F);
        const std::vector<glm::vec4> velocity(8U, glm::vec4(1, 2, 3, 4));
        seed_raw(volume, "density", density);
        seed_raw(volume, "velocity", velocity);

        volume->seed("density", Kinesis::VectorField { [](const glm::vec3&) {
            return glm::vec3(9.0F);
        } });
        volume->seed("velocity", Kinesis::SpatialField { [](const glm::vec3&) {
            return 9.0F;
        } });
        volume->accumulate("density", Kinesis::VectorField { [](const glm::vec3&) {
            return glm::vec3(9.0F);
        } });
        volume->accumulate("velocity", Kinesis::SpatialField { [](const glm::vec3&) {
            return 9.0F;
        } });

        EXPECT_EQ(read<float>(volume, "density"), density);
        EXPECT_EQ(read<glm::vec4>(volume, "velocity"), velocity);
    }

    TEST_F(VolumeGridBufferDeviceTest, BatchReadKeepsMixedFieldDestinationsSeparate)
    {
        auto volume = attach({
            { "density", sizeof(float) },
            { "velocity", sizeof(glm::vec4) },
            { "pressure", sizeof(float) },
        });
        const std::vector<float> density { 1, 2, 3, 4, 5, 6, 7, 8 };
        const std::vector<glm::vec4> velocity(8U, glm::vec4(2, 3, 4, 5));
        const std::vector<float> pressure(8U, 11.0F);
        seed_raw(volume, "density", density);
        seed_raw(volume, "velocity", velocity);
        seed_raw(volume, "pressure", pressure);

        std::vector<float> density_out(8U);
        std::vector<glm::vec4> velocity_out(8U);
        std::vector<float> pressure_out(8U);
        volume->read_fields(
            { "density", "velocity", "pressure" },
            { { density_out.data(), density_out.size() * sizeof(float) },
                { velocity_out.data(), velocity_out.size() * sizeof(glm::vec4) },
                { pressure_out.data(), pressure_out.size() * sizeof(float) } });

        EXPECT_EQ(density_out, density);
        EXPECT_EQ(velocity_out, velocity);
        EXPECT_EQ(pressure_out, pressure);
    }

    TEST_F(VolumeGridBufferDeviceTest, InvalidReadRequestsLeaveDestinationsUntouched)
    {
        auto volume = attach({ { "density", sizeof(float) } });
        seed_raw(volume, "density", std::vector<float>(8U, 4.0F));
        std::vector<float> untouched(8U, -1.0F);

        volume->read_field("missing", untouched.data(), untouched.size() * sizeof(float));
        volume->read_field("density", untouched.data(), 7U * sizeof(float));
        volume->read_fields({ "density" }, {});

        EXPECT_EQ(untouched, std::vector<float>(8U, -1.0F));
        EXPECT_EQ(read<float>(volume, "density"), std::vector<float>(8U, 4.0F));
    }

} // namespace
} // namespace MayaFlux::Test
