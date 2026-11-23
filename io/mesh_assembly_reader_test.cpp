#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/io_roundtrip_support.hpp"
#include "../support/scoped_working_dir.hpp"

#include "MayaFlux/IO/Model/ModelReader.hpp"
#include "MayaFlux/IO/Spatial/SpatialReader.hpp"
#include "MayaFlux/Kakshya/Semantic/MeshAssemblyContainer.hpp"
#include "MayaFlux/Portal/Graphics/GraphicsUtils.hpp"

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
#endif

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::MeshAssemblyContainer;
    using RoundTrip::path_in_cwd;

    bool close(const glm::vec3& a, const glm::vec3& b, float tolerance = 1e-4F)
    {
        return glm::length(a - b) <= tolerance;
    }

    std::string float_bytes(const std::vector<float>& values)
    {
        return { reinterpret_cast<const char*>(values.data()), values.size() * sizeof(float) };
    }

    std::string shared_wheel_gltf()
    {
        const std::string payload = float_bytes({ 0, 0, 0, 1, 0, 0, 0, 1, 0 });
        return R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)"
               R"("nodes":[{"name":"parent","translation":[0,5,0],"children":[1,2]},)"
               R"({"name":"wheel_fl","mesh":0,"translation":[1,0,0]},)"
               R"({"name":"wheel_fr","mesh":0,"translation":[-1,0,0]}],)"
               R"("meshes":[{"name":"tri","primitives":[{"attributes":{"POSITION":0},"mode":4}]}],)"
               R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],)"
               R"("bufferViews":[{"buffer":0,"byteLength":36,"target":34962}],)"
               R"("buffers":[{"byteLength":36,"uri":"data:application/octet-stream;base64,)"
            + RoundTrip::base64(payload) + R"("}]})";
    }

    std::string sliding_box_gltf()
    {
        const std::string payload = float_bytes({ 0, 0, 0, 1, 0, 0, 0, 1, 0, 0.0F, 1.0F, 0, 0, 0, 2, 0, 0 });
        return R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)"
               R"("nodes":[{"name":"box","mesh":0}],)"
               R"("meshes":[{"name":"tri","primitives":[{"attributes":{"POSITION":0},"mode":4}]}],)"
               R"("animations":[{"name":"slide","channels":[{"sampler":0,"target":{"node":0,"path":"translation"}}],)"
               R"("samplers":[{"input":1,"output":2,"interpolation":"LINEAR"}]}],)"
               R"("accessors":[)"
               R"({"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
               R"({"bufferView":1,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},)"
               R"({"bufferView":2,"componentType":5126,"count":2,"type":"VEC3"}],)"
               R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36,"target":34962},)"
               R"({"buffer":0,"byteOffset":36,"byteLength":8},)"
               R"({"buffer":0,"byteOffset":44,"byteLength":24}],)"
               R"("buffers":[{"byteLength":68,"uri":"data:application/octet-stream;base64,)"
            + RoundTrip::base64(payload) + R"("}]})";
    }

    class MeshAssemblyModelTest : public ::testing::Test {
    protected:
        ScopedWorkingDir m_files;
    };

    class MeshAssemblyAlembicTest : public ::testing::Test {
    protected:
        ScopedWorkingDir m_files;

        static void write_rig(const std::string& path)
        {
            using namespace Alembic::AbcGeom;
            OArchive archive(Alembic::AbcCoreOgawa::WriteArchive(), path);
            const uint32_t each_second = archive.addTimeSampling(Alembic::AbcCoreAbstract::TimeSampling(1.0, 0.0));

            OXform base(archive.getTop(), "base");
            {
                XformSample sample;
                sample.setTranslation(Imath::V3d(0.0, 10.0, 0.0));
                base.getSchema().set(sample);
            }

            OXform arm(base, "arm");
            arm.getSchema().setTimeSampling(each_second);
            for (int s = 0; s < 3; ++s) {
                XformSample sample;
                sample.setTranslation(Imath::V3d(2.0 * s, 0.0, 0.0));
                arm.getSchema().set(sample);
            }

            OPoints cloud(arm, "cloud");
            {
                const std::vector<V3f> positions { V3f(0, 0, 0), V3f(1, 0, 0), V3f(2, 0, 0) };
                const std::vector<uint64_t> ids { 1, 2, 3 };
                cloud.getSchema().set(OPointsSchema::Sample(
                    P3fArraySample(positions.data(), positions.size()), UInt64ArraySample(ids.data(), ids.size())));
            }

            OXform detached(base, "detached");
            {
                XformSample sample;
                sample.setTranslation(Imath::V3d(7.0, 0.0, 0.0));
                sample.setInheritsXforms(false);
                detached.getSchema().set(sample);
            }

            OPoints swarm(archive.getTop(), "swarm");
            swarm.getSchema().setTimeSampling(each_second);
            for (int s = 0; s < 2; ++s) {
                const std::vector<V3f> positions { V3f(0, 0, static_cast<float>(s)), V3f(1, 0, static_cast<float>(s)) };
                const std::vector<uint64_t> ids { 1, 2 };
                swarm.getSchema().set(OPointsSchema::Sample(
                    P3fArraySample(positions.data(), positions.size()), UInt64ArraySample(ids.data(), ids.size())));
            }

            OPolyMesh surface(archive.getTop(), "surface");
            {
                const std::vector<V3f> positions { V3f(0, 0, 0), V3f(1, 0, 0), V3f(0, 1, 0) };
                const std::vector<int32_t> indices { 0, 1, 2 };
                const std::vector<int32_t> counts { 3 };
                surface.getSchema().set(OPolyMeshSchema::Sample(
                    P3fArraySample(positions.data(), positions.size()),
                    Int32ArraySample(indices.data(), indices.size()),
                    Int32ArraySample(counts.data(), counts.size())));
            }

            archive.getTop().addChildInstance(base, "base_copy");
        }

        std::string rig_path()
        {
            const auto path = path_in_cwd("rig.abc");
            write_rig(path);
            return path;
        }
    };

}

TEST_F(MeshAssemblyModelTest, ASceneNeedsToBeOpenFirst)
{
    IO::ModelReader reader;

    EXPECT_EQ(reader.create_mesh_assembly(), nullptr);
    EXPECT_NE(reader.get_last_error().find("No scene loaded"), std::string::npos);
}

TEST_F(MeshAssemblyModelTest, ANodeTreeBecomesRowsAndASharedMeshStaysOneSource)
{
    const auto path = path_in_cwd("wheels.gltf");
    RoundTrip::write_bytes(path, shared_wheel_gltf());
    IO::ModelReader reader;
    ASSERT_TRUE(reader.open(path)) << reader.get_last_error();

    const auto assembly = reader.create_mesh_assembly();

    ASSERT_NE(assembly, nullptr);
    ASSERT_EQ(assembly->get_total_elements(), 3U);
    EXPECT_EQ(assembly->get_data().sources.size(), 1U);
    EXPECT_NE(assembly->mesh(0), nullptr);

    const auto parent = assembly->placement(0).value();
    const auto left = assembly->placement(1).value();
    const auto right = assembly->placement(2).value();
    EXPECT_EQ(parent.name, "parent");
    EXPECT_FALSE(parent.parent.has_value());
    EXPECT_FALSE(parent.source.has_value());
    EXPECT_EQ(left.name, "wheel_fl");
    EXPECT_EQ(left.parent, std::optional<uint64_t> { 0 });
    EXPECT_EQ(left.source, std::optional<uint64_t> { 0 });
    EXPECT_EQ(right.source, std::optional<uint64_t> { 0 });
    EXPECT_TRUE(close(glm::vec3(left.local[3]), glm::vec3(1.0F, 0.0F, 0.0F)));

    const auto groups = assembly->groups();
    ASSERT_EQ(groups.size(), 1U);
    EXPECT_EQ(groups[0].rows, (std::vector<uint64_t> { 1, 2 }));

    const auto world = assembly->world_transforms();
    EXPECT_TRUE(close(glm::vec3(world[1][3]), glm::vec3(1.0F, 5.0F, 0.0F)));
    EXPECT_TRUE(close(glm::vec3(world[2][3]), glm::vec3(-1.0F, 5.0F, 0.0F)));
}

TEST_F(MeshAssemblyModelTest, TheSceneInfoIsKeptInTheFacets)
{
    const auto path = path_in_cwd("wheels.gltf");
    RoundTrip::write_bytes(path, shared_wheel_gltf());
    IO::ModelReader reader;
    ASSERT_TRUE(reader.open(path));

    const auto assembly = reader.create_mesh_assembly();

    ASSERT_NE(assembly, nullptr);
    const auto* info = assembly->get_data().facets.get<IO::ModelInfo>();
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->mesh_count, 1U);
}

TEST_F(MeshAssemblyModelTest, AnAnimationBecomesAClipInSeconds)
{
    const auto path = path_in_cwd("slide.gltf");
    RoundTrip::write_bytes(path, sliding_box_gltf());
    IO::ModelReader reader;
    ASSERT_TRUE(reader.open(path)) << reader.get_last_error();

    const auto assembly = reader.create_mesh_assembly();

    ASSERT_NE(assembly, nullptr);
    ASSERT_EQ(assembly->get_data().clips.size(), 1U);
    const auto& clip = assembly->get_data().clips[0];
    EXPECT_EQ(clip.name, "slide");
    ASSERT_EQ(clip.channels.size(), 1U);
    EXPECT_EQ(clip.channels[0].row, 0U);
    EXPECT_TRUE(clip.channels[0].translation.has_value());

    const auto halfway = assembly->sample(0, 0.5).value();
    const auto end = assembly->sample(0, 1.0).value();
    EXPECT_TRUE(close(glm::vec3(halfway[0][3]), glm::vec3(1.0F, 0.0F, 0.0F)));
    EXPECT_TRUE(close(glm::vec3(end[0][3]), glm::vec3(2.0F, 0.0F, 0.0F)));
}

TEST_F(MeshAssemblyAlembicTest, AnArchiveNeedsToBeOpenFirst)
{
    IO::SpatialReader reader;

    EXPECT_EQ(reader.create_mesh_assembly(), nullptr);
    EXPECT_FALSE(reader.get_last_error().empty());
}

TEST_F(MeshAssemblyAlembicTest, TransformsAndGroupsAreRowsParentedAsTheArchiveIs)
{
    IO::SpatialReader reader;
    ASSERT_TRUE(reader.open(rig_path())) << reader.get_last_error();

    const auto assembly = reader.create_mesh_assembly();

    ASSERT_NE(assembly, nullptr);
    const auto base = assembly->placement(0).value();
    const auto arm = assembly->placement(1).value();
    const auto cloud = assembly->placement(2).value();
    EXPECT_EQ(base.name, "base");
    EXPECT_FALSE(base.parent.has_value());
    EXPECT_TRUE(close(glm::vec3(base.local[3]), glm::vec3(0.0F, 10.0F, 0.0F)));
    EXPECT_EQ(arm.name, "arm");
    EXPECT_EQ(arm.parent, std::optional<uint64_t> { 0 });
    EXPECT_EQ(cloud.name, "cloud");
    EXPECT_EQ(cloud.parent, std::optional<uint64_t> { 1 });
    EXPECT_TRUE(cloud.source.has_value());
}

TEST_F(MeshAssemblyAlembicTest, ATransformThatDoesNotInheritIsARoot)
{
    IO::SpatialReader reader;
    ASSERT_TRUE(reader.open(rig_path()));

    const auto assembly = reader.create_mesh_assembly();

    ASSERT_NE(assembly, nullptr);
    const auto detached = assembly->placement(3).value();
    EXPECT_EQ(detached.name, "detached");
    EXPECT_FALSE(detached.parent.has_value());
    const auto world = assembly->world_transforms();
    EXPECT_TRUE(close(glm::vec3(world[3][3]), glm::vec3(7.0F, 0.0F, 0.0F)));
}

TEST_F(MeshAssemblyAlembicTest, AnInstanceSharesTheSourcesOfTheObjectItInstances)
{
    IO::SpatialReader reader;
    ASSERT_TRUE(reader.open(rig_path()));

    const auto& objects = reader.get_objects();
    const auto copy = std::ranges::find_if(objects, [](const IO::SpatialObject& object) { return object.path == "base_copy"; });
    ASSERT_NE(copy, objects.end());
    EXPECT_EQ(copy->instance_source, "base");

    const auto assembly = reader.create_mesh_assembly();

    ASSERT_NE(assembly, nullptr);
    EXPECT_EQ(assembly->get_data().sources.size(), 3U);
    const auto groups = assembly->groups();
    const auto shared = std::ranges::find_if(groups, [](const Kakshya::MeshAssemblyGroup& group) { return group.rows.size() == 2; });
    ASSERT_NE(shared, groups.end());
}

TEST_F(MeshAssemblyAlembicTest, ATransformThatChangesOverTimeBecomesAChannelOfOneClip)
{
    IO::SpatialReader reader;
    ASSERT_TRUE(reader.open(rig_path()));

    const auto assembly = reader.create_mesh_assembly();

    ASSERT_NE(assembly, nullptr);
    ASSERT_EQ(assembly->get_data().clips.size(), 1U);
    const auto& clip = assembly->get_data().clips[0];
    EXPECT_EQ(clip.name, "rig");
    EXPECT_EQ(clip.channels.size(), 2U);
    EXPECT_TRUE(clip.channels[0].translation.has_value());
    EXPECT_TRUE(clip.channels[0].rotation.has_value());
    EXPECT_TRUE(clip.channels[0].scale.has_value());

    const auto at_start = assembly->world_transforms(assembly->sample(0, 0.0).value());
    const auto at_end = assembly->world_transforms(assembly->sample(0, 2.0).value());
    EXPECT_TRUE(close(glm::vec3(at_start[1][3]), glm::vec3(0.0F, 10.0F, 0.0F)));
    EXPECT_TRUE(close(glm::vec3(at_end[1][3]), glm::vec3(4.0F, 10.0F, 0.0F)));
}

TEST_F(MeshAssemblyAlembicTest, ASourceKeepsItsSampleTimesAndTopologyVariance)
{
    IO::SpatialReader reader;
    ASSERT_TRUE(reader.open(rig_path()));

    const auto assembly = reader.create_mesh_assembly();

    ASSERT_NE(assembly, nullptr);
    const auto& sources = assembly->get_data().sources;
    const auto swarm = std::ranges::find_if(sources, [](const Kakshya::MeshAssemblySource& source) { return source.name == "swarm"; });
    ASSERT_NE(swarm, sources.end());
    EXPECT_EQ(swarm->variance, Kakshya::TopologyVariance::Heterogeneous);
    ASSERT_TRUE(swarm->time.has_value());
    EXPECT_EQ(swarm->time->time(1), std::optional<double> { 1.0 });
    ASSERT_NE(assembly->spatial(0), nullptr);

    const auto cloud = std::ranges::find_if(sources, [](const Kakshya::MeshAssemblySource& source) { return source.name == "cloud"; });
    ASSERT_NE(cloud, sources.end());
    EXPECT_EQ(cloud->variance, Kakshya::TopologyVariance::Constant);
}

TEST_F(MeshAssemblyAlembicTest, AMeshObjectIsASourceHoldingItsFirstSampleAndARowThatShowsIt)
{
    IO::SpatialReader reader;
    ASSERT_TRUE(reader.open(rig_path()));

    const auto assembly = reader.create_mesh_assembly();

    ASSERT_NE(assembly, nullptr);
    const auto& sources = assembly->get_data().sources;
    const auto surface = std::ranges::find_if(sources, [](const Kakshya::MeshAssemblySource& source) { return source.name == "surface"; });
    ASSERT_NE(surface, sources.end());
    EXPECT_EQ(surface->variance, Kakshya::TopologyVariance::Constant);
    const auto* geometry = std::get_if<Kakshya::MeshData>(&surface->geometry);
    ASSERT_NE(geometry, nullptr);
    EXPECT_EQ(geometry->vertex_count(), 3U);
    EXPECT_EQ(geometry->face_count(), 1U);

    bool shown = false;
    for (size_t row = 0; row < assembly->get_total_elements(); ++row) {
        const auto placement = assembly->placement(row);
        shown = shown || (placement->name == "surface" && placement->source.has_value());
    }
    EXPECT_TRUE(shown);
}

TEST_F(MeshAssemblyAlembicTest, ATransformMatrixPlacesTheChildrenAsItsTranslationSays)
{
    IO::SpatialReader reader;
    ASSERT_TRUE(reader.open(rig_path()));

    const auto matrix = reader.read_transform("base", 0);

    ASSERT_TRUE(matrix.has_value());
    EXPECT_TRUE(close(glm::vec3((*matrix)[3]), glm::vec3(0.0F, 10.0F, 0.0F)));
    EXPECT_FALSE(reader.read_transform("base", 5).has_value());
    EXPECT_FALSE(reader.read_transform("cloud", 0).has_value());
    EXPECT_EQ(reader.transform_time("base/arm", 2), std::optional<double> { 2.0 });
}

}
