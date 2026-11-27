#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/io_roundtrip_support.hpp"
#include "../support/scoped_working_dir.hpp"

#include "MayaFlux/IO/Spatial/CameraSequence.hpp"
#include "MayaFlux/IO/Spatial/SpatialAssembly.hpp"
#include "MayaFlux/Kakshya/Semantic/MeshAssemblyAccess.hpp"

#ifdef MAYAFLUX_PLATFORM_WINDOWS
#include <windows.h>
#endif

#include <Alembic/Abc/All.h>
#include <Alembic/AbcCoreOgawa/All.h>
#include <Alembic/AbcGeom/All.h>

#ifdef MAYAFLUX_PLATFORM_WINDOWS
#ifdef ERROR
#undef ERROR
#endif
#ifdef PASSTHROUGH
#undef PASSTHROUGH
#endif
#ifdef STRICT
#undef STRICT
#endif
#ifdef IGNORE
#undef IGNORE
#endif
#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif
#endif

#include <glm/gtc/matrix_transform.hpp>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {
namespace {

    using RoundTrip::path_in_cwd;

    class AlembicCameraTest : public ::testing::Test {
    protected:
        ScopedWorkingDir m_files;

        /**
         * A rig at (1, 2, 3) holding a camera whose focal length goes 35, 50, 70 a second apart, and a
         * second camera that never changes. The first has an overscan on its left and a film offset, so
         * its screen window is not the default one.
         */
        static void write_cameras(const std::string& path, bool moving_rig = false)
        {
            using namespace Alembic::AbcGeom;
            OArchive archive(Alembic::AbcCoreOgawa::WriteArchive(), path);
            const uint32_t each_second = archive.addTimeSampling(Alembic::AbcCoreAbstract::TimeSampling(1.0, 0.0));

            OXform rig(archive.getTop(), "rig");
            if (moving_rig) {
                rig.getSchema().setTimeSampling(each_second);
                for (int s = 0; s < 3; ++s) {
                    XformSample sample;
                    sample.setTranslation(Imath::V3d(2.0 * s, 0.0, 0.0));
                    rig.getSchema().set(sample);
                }
            } else {
                XformSample sample;
                sample.setTranslation(Imath::V3d(1.0, 2.0, 3.0));
                rig.getSchema().set(sample);
            }

            OCamera lens(rig, "lens");
            lens.getSchema().setTimeSampling(each_second);
            for (const double focal : { 35.0, 50.0, 70.0 }) {
                CameraSample sample;
                sample.setFocalLength(focal);
                sample.setNearClippingPlane(0.5);
                sample.setFarClippingPlane(500.0);
                sample.setFStop(2.8);
                sample.setFocusDistance(7.0);
                sample.setHorizontalFilmOffset(0.18);
                sample.setOverScanLeft(0.1);
                lens.getSchema().set(sample);
            }

            OCamera still(archive.getTop(), "still");
            CameraSample sample;
            sample.setFocalLength(24.0);
            still.getSchema().set(sample);
        }

        std::string cameras_path(bool moving_rig = false)
        {
            const auto path = path_in_cwd("cameras.abc");
            write_cameras(path, moving_rig);
            return path;
        }
    };

    TEST_F(AlembicCameraTest, CamerasAreObjectsOfTheirOwnKindAndAreListedInFileOrder)
    {
        IO::SpatialReader reader;
        ASSERT_TRUE(reader.open(cameras_path())) << reader.get_last_error();

        ASSERT_EQ(reader.get_camera_names().size(), 2U);
        EXPECT_EQ(reader.get_camera_names()[0], "rig/lens");
        EXPECT_EQ(reader.get_camera_names()[1], "still");

        const auto& objects = reader.get_objects();
        const auto camera = std::ranges::find(objects, "rig/lens", &IO::SpatialObject::path);
        ASSERT_NE(camera, objects.end());
        EXPECT_EQ(camera->kind, IO::SpatialObjectKind::Camera);
        EXPECT_EQ(camera->sample_count, 3U);
    }

    TEST_F(AlembicCameraTest, ACameraHasSamplesAndTheArchivesTimeForEach)
    {
        IO::SpatialReader reader;
        ASSERT_TRUE(reader.open(cameras_path())) << reader.get_last_error();

        EXPECT_EQ(reader.get_camera_sample_count("rig/lens"), 3U);
        EXPECT_EQ(reader.get_camera_sample_count("still"), 1U);
        EXPECT_EQ(reader.get_camera_sample_count("nope"), 0U);
        EXPECT_EQ(reader.camera_time("rig/lens", 2), std::optional<double> { 2.0 });
        EXPECT_EQ(reader.camera_time("rig/lens", 3), std::nullopt);
        EXPECT_EQ(reader.camera_time("nope", 0), std::nullopt);
    }

    TEST_F(AlembicCameraTest, ALensIsReadWithItsOpticsAndTheScreenWindowAlembicWorksOut)
    {
        IO::SpatialReader reader;
        ASSERT_TRUE(reader.open(cameras_path())) << reader.get_last_error();

        const auto lens = reader.read_lens("rig/lens", 1);

        ASSERT_TRUE(lens.has_value());
        EXPECT_DOUBLE_EQ(lens->focal_length, 50.0);
        EXPECT_DOUBLE_EQ(lens->horizontal_aperture, 3.6);
        EXPECT_DOUBLE_EQ(lens->vertical_aperture, 2.4);
        EXPECT_DOUBLE_EQ(lens->near, 0.5);
        EXPECT_DOUBLE_EQ(lens->far, 500.0);
        EXPECT_DOUBLE_EQ(lens->f_stop, 2.8);
        EXPECT_DOUBLE_EQ(lens->focus_distance, 7.0);
        EXPECT_NEAR(lens->window.min.x, -1.0, 1e-6);
        EXPECT_NEAR(lens->window.max.x, 1.1, 1e-6);
        EXPECT_NEAR(lens->window.max.y, 2.0 / 3.0, 1e-6);
        EXPECT_NEAR(lens->window.min.y, -2.0 / 3.0, 1e-6);
    }

    TEST_F(AlembicCameraTest, ASampleThatIsNotThereIsRefusedWithAReason)
    {
        IO::SpatialReader reader;
        ASSERT_TRUE(reader.open(cameras_path())) << reader.get_last_error();

        EXPECT_FALSE(reader.read_lens("rig/lens", 3).has_value());
        EXPECT_FALSE(reader.get_last_error().empty());
        EXPECT_FALSE(reader.read_lens("nope", 0).has_value());
    }

    TEST_F(AlembicCameraTest, ACameraIsAFrameSourceOfLensesOverTheArchivesTimes)
    {
        const auto frames = IO::CameraFileFrames::create(cameras_path(), "rig/lens");

        ASSERT_NE(frames, nullptr);
        EXPECT_EQ(frames->count(), 3U);
        EXPECT_EQ(frames->camera(), "rig/lens");
        EXPECT_EQ(frames->time_sampling().time(2), std::optional<double> { 2.0 });
        ASSERT_NE(frames->read(2), nullptr);
        EXPECT_DOUBLE_EQ(frames->read(2)->focal_length, 70.0);
        EXPECT_EQ(frames->read(3), nullptr);
    }

    TEST_F(AlembicCameraTest, AFileWithSeveralCamerasNeedsOneNamed)
    {
        const auto path = cameras_path();

        EXPECT_EQ(IO::CameraFileFrames::create(path), nullptr);
        EXPECT_NE(IO::CameraFileFrames::create(path, "still"), nullptr);
        EXPECT_EQ(IO::CameraFileFrames::create(path, "missing"), nullptr);
        EXPECT_EQ(IO::CameraFileFrames::create(std::shared_ptr<IO::SpatialArchive> {}, "still"), nullptr);
    }

    TEST_F(AlembicCameraTest, SeveralCamerasMayShareOneArchive)
    {
        const auto archive = IO::SpatialArchive::open(cameras_path());
        ASSERT_NE(archive, nullptr);

        const auto moving = IO::CameraFileFrames::create(archive, "rig/lens");
        const auto fixed = IO::CameraFileFrames::create(archive, "still");

        ASSERT_NE(moving, nullptr);
        ASSERT_NE(fixed, nullptr);
        EXPECT_DOUBLE_EQ(moving->read(1)->focal_length, 50.0);
        EXPECT_DOUBLE_EQ(fixed->read(0)->focal_length, 24.0);
    }

    TEST_F(AlembicCameraTest, ACameraIsASourceThatARowShowsAndPlacesThroughItsTransforms)
    {
        const auto assembly = IO::open_spatial_assembly(cameras_path());

        ASSERT_NE(assembly, nullptr);
        const Kakshya::MeshAssemblyAccess access(*assembly);
        const auto cameras = access.rows_of_kind(Kakshya::MeshAssemblyRowKind::Camera);
        ASSERT_EQ(cameras.size(), 2U);

        const auto lens_row = access.find("lens");
        ASSERT_TRUE(lens_row.has_value());
        EXPECT_EQ(access.row(*lens_row)->kind, Kakshya::MeshAssemblyRowKind::Camera);
        const auto source = access.source_of(*lens_row);
        ASSERT_TRUE(source.has_value());
        ASSERT_NE(access.lens(*source), nullptr);
        EXPECT_DOUBLE_EQ(access.lens(*source)->focal_length, 35.0);
        EXPECT_EQ(access.source(*source)->origin, "rig/lens");
        EXPECT_EQ(assembly->mesh(*source), nullptr);

        const auto world = access.world(*lens_row);
        ASSERT_TRUE(world.has_value());
        EXPECT_EQ(glm::vec3((*world)[3]), glm::vec3(1.0F, 2.0F, 3.0F));
    }

    TEST_F(AlembicCameraTest, OnlyACameraThatChangesGetsFrames)
    {
        const auto assembly = IO::open_spatial_assembly(cameras_path());
        ASSERT_NE(assembly, nullptr);
        const Kakshya::MeshAssemblyAccess access(*assembly);

        const auto moving = access.source_of(*access.find("lens"));
        const auto fixed = access.source_of(*access.find("still"));

        ASSERT_TRUE(moving.has_value());
        ASSERT_TRUE(fixed.has_value());
        ASSERT_NE(assembly->lens_frames(*moving), nullptr);
        EXPECT_EQ(assembly->lens_frames(*moving)->count(), 3U);
        EXPECT_EQ(assembly->lens_frames(*fixed), nullptr);
    }

} // namespace
} // namespace MayaFlux::Test
