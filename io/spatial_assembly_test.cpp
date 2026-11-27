#include "../support/test_metadata.hpp"
#include "gtest/gtest.h"

#include "../support/io_roundtrip_support.hpp"
#include "../support/scoped_working_dir.hpp"

#include "MayaFlux/Buffers/Network/RecordedGeometryBuffer.hpp"
#include "MayaFlux/IO/Spatial/MeshSequence.hpp"
#include "MayaFlux/IO/Spatial/SpatialArchive.hpp"
#include "MayaFlux/IO/Spatial/SpatialAssembly.hpp"
#include "MayaFlux/IO/Spatial/SpatialSequence.hpp"
#include "MayaFlux/Kakshya/NDData/VertexFormats.hpp"
#include "MayaFlux/Kakshya/Semantic/MeshAssemblyContainer.hpp"
#include "MayaFlux/Kriya/Realization/MeshAssemblyRealization.hpp"
#include "MayaFlux/Nodes/Graphics/MeshWriterNode.hpp"

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

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {
namespace {

    using RoundTrip::path_in_cwd;

    float first_x(const Kakshya::MeshData& mesh)
    {
        const auto& bytes = std::get<std::vector<uint8_t>>(mesh.vertex_variant);
        Kakshya::MeshVertex vertex {};
        std::memcpy(&vertex, bytes.data(), sizeof(vertex));
        return vertex.position.x;
    }

    size_t source_named(const Kakshya::MeshAssemblyContainer& assembly, const std::string& name)
    {
        const auto& sources = assembly.get_data().sources;
        const auto it = std::ranges::find(sources, name, &Kakshya::MeshAssemblySource::name);
        return static_cast<size_t>(it - sources.begin());
    }

    class SpatialAssemblyTest : public ::testing::Test {
    protected:
        ScopedWorkingDir m_files;

        /**
         * A level at x = 5 holding a wall that never changes, a walker that moves one unit
         * along x each second for three samples, and a cloud of dust of two, three and four points.
         */
        static void write_level(const std::string& path)
        {
            using namespace Alembic::AbcGeom;
            OArchive archive(Alembic::AbcCoreOgawa::WriteArchive(), path);
            const uint32_t each_second = archive.addTimeSampling(Alembic::AbcCoreAbstract::TimeSampling(1.0, 0.0));

            OXform level(archive.getTop(), "level");
            {
                XformSample sample;
                sample.setTranslation(Imath::V3d(5.0, 0.0, 0.0));
                level.getSchema().set(sample);
            }

            const std::vector<int32_t> indices { 0, 1, 2 };
            const std::vector<int32_t> counts { 3 };

            OPolyMesh wall(level, "wall");
            {
                const std::vector<V3f> positions { V3f(0, 0, 0), V3f(1, 0, 0), V3f(0, 1, 0) };
                wall.getSchema().set(OPolyMeshSchema::Sample(
                    P3fArraySample(positions.data(), positions.size()),
                    Int32ArraySample(indices.data(), indices.size()),
                    Int32ArraySample(counts.data(), counts.size())));
            }

            OPolyMesh walker(level, "walker");
            walker.getSchema().setTimeSampling(each_second);
            for (int s = 0; s < 3; ++s) {
                const auto x = static_cast<float>(s);
                const std::vector<V3f> positions { V3f(x, 0, 0), V3f(x + 1, 0, 0), V3f(x, 1, 0) };
                walker.getSchema().set(OPolyMeshSchema::Sample(
                    P3fArraySample(positions.data(), positions.size()),
                    Int32ArraySample(indices.data(), indices.size()),
                    Int32ArraySample(counts.data(), counts.size())));
            }

            OPoints dust(level, "dust");
            dust.getSchema().setTimeSampling(each_second);
            for (size_t count : { 2U, 3U, 4U }) {
                std::vector<V3f> positions(count, V3f(0, 0, 0));
                std::vector<uint64_t> ids(count, 1);
                dust.getSchema().set(OPointsSchema::Sample(
                    P3fArraySample(positions.data(), positions.size()), UInt64ArraySample(ids.data(), ids.size())));
            }
        }

        /** An arm that slides two units along x each second for three samples, with a hand that never moves under it. */
        static void write_arm(const std::string& path)
        {
            using namespace Alembic::AbcGeom;
            OArchive archive(Alembic::AbcCoreOgawa::WriteArchive(), path);
            const uint32_t each_second = archive.addTimeSampling(Alembic::AbcCoreAbstract::TimeSampling(1.0, 0.0));

            OXform arm(archive.getTop(), "arm");
            arm.getSchema().setTimeSampling(each_second);
            for (int s = 0; s < 3; ++s) {
                XformSample sample;
                sample.setTranslation(Imath::V3d(2.0 * s, 0.0, 0.0));
                arm.getSchema().set(sample);
            }

            OPolyMesh hand(arm, "hand");
            const std::vector<V3f> positions { V3f(0, 0, 0), V3f(1, 0, 0), V3f(0, 1, 0) };
            const std::vector<int32_t> indices { 0, 1, 2 };
            const std::vector<int32_t> counts { 3 };
            hand.getSchema().set(OPolyMeshSchema::Sample(
                P3fArraySample(positions.data(), positions.size()),
                Int32ArraySample(indices.data(), indices.size()),
                Int32ArraySample(counts.data(), counts.size())));
        }

        std::string level_path()
        {
            const auto path = path_in_cwd("level.abc");
            write_level(path);
            return path;
        }
    };

    TEST_F(SpatialAssemblyTest, AnArchiveThatCannotBeReadMakesNoAssembly)
    {
        EXPECT_EQ(IO::open_spatial_assembly(path_in_cwd("missing.abc")), nullptr);
    }

    TEST_F(SpatialAssemblyTest, EverySourceSaysWhereInTheFileItCameFrom)
    {
        const auto assembly = IO::open_spatial_assembly(level_path());

        ASSERT_NE(assembly, nullptr);
        ASSERT_EQ(assembly->get_data().sources.size(), 3U);
        for (const auto& source : assembly->get_data().sources) {
            EXPECT_FALSE(source.origin.empty()) << source.name;
            EXPECT_NE(source.origin.find(source.name), std::string::npos);
        }
    }

    TEST_F(SpatialAssemblyTest, OnlyTheSourcesThatChangeGetFrames)
    {
        const auto assembly = IO::open_spatial_assembly(level_path());
        ASSERT_NE(assembly, nullptr);

        const size_t wall = source_named(*assembly, "wall");
        const size_t walker = source_named(*assembly, "walker");
        const size_t dust = source_named(*assembly, "dust");

        EXPECT_EQ(assembly->mesh_frames(wall), nullptr);
        ASSERT_NE(assembly->mesh_frames(walker), nullptr);
        EXPECT_EQ(assembly->mesh_frames(walker)->count(), 3U);
        ASSERT_NE(assembly->spatial_frames(dust), nullptr);
        EXPECT_EQ(assembly->spatial_frames(dust)->count(), 3U);
    }

    TEST_F(SpatialAssemblyTest, ASourceOfPointsIsToldTheMostVerticesItsFramesHold)
    {
        const auto assembly = IO::open_spatial_assembly(level_path());
        ASSERT_NE(assembly, nullptr);

        EXPECT_EQ(assembly->spatial_max_vertices(source_named(*assembly, "dust")), std::optional<uint32_t> { 4U });
    }

    TEST_F(SpatialAssemblyTest, TheFramesOfASourceAreTheSamplesOfItsObject)
    {
        const auto assembly = IO::open_spatial_assembly(level_path());
        ASSERT_NE(assembly, nullptr);
        const auto walker = assembly->mesh_frames(source_named(*assembly, "walker"));
        const auto dust = assembly->spatial_frames(source_named(*assembly, "dust"));
        ASSERT_NE(walker, nullptr);
        ASSERT_NE(dust, nullptr);

        for (size_t i = 0; i < 3; ++i) {
            ASSERT_NE(walker->read(i), nullptr);
            EXPECT_FLOAT_EQ(first_x(*walker->read(i)), static_cast<float>(i));
            ASSERT_NE(dust->read(i), nullptr);
            EXPECT_EQ(dust->read(i)->positions.size(), i + 2);
        }
    }

    TEST_F(SpatialAssemblyTest, SourcesOverOneArchiveShareItsReaderAndReadIndependently)
    {
        const auto archive = IO::SpatialArchive::open(level_path());
        ASSERT_NE(archive, nullptr);
        const auto meshes = archive->read([](IO::SpatialReader& reader) { return reader.get_mesh_names(); });
        const auto streams = archive->read([](IO::SpatialReader& reader) { return reader.get_stream_names(); });
        ASSERT_EQ(meshes.size(), 2U);
        ASSERT_EQ(streams.size(), 1U);

        const auto first = IO::MeshFileFrames::create(archive, meshes[0]);
        const auto second = IO::MeshFileFrames::create(archive, meshes[1]);
        const auto dust = IO::SpatialFileFrames::create(archive, streams[0]);

        ASSERT_NE(first, nullptr);
        ASSERT_NE(second, nullptr);
        ASSERT_NE(dust, nullptr);
        EXPECT_NE(first->read(0), nullptr);
        EXPECT_NE(second->read(0), nullptr);
        EXPECT_EQ(dust->read(2)->positions.size(), 4U);
        EXPECT_NE(first->read(0), second->read(0));
        EXPECT_EQ(archive->path(), path_in_cwd("level.abc"));
    }

    TEST_F(SpatialAssemblyTest, AnArchiveThatIsNotThereMakesNoArchiveAndNoSources)
    {
        EXPECT_EQ(IO::SpatialArchive::open(path_in_cwd("missing.abc")), nullptr);
        EXPECT_EQ(IO::MeshFileFrames::create(std::shared_ptr<IO::SpatialArchive> {}, "x"), nullptr);
        EXPECT_EQ(IO::SpatialFileFrames::create(std::shared_ptr<IO::SpatialArchive> {}, "x"), nullptr);
    }

    TEST_F(SpatialAssemblyTest, ASourceNamedByPathStillOpensItsOwnArchive)
    {
        const auto path = level_path();
        const auto archive = IO::SpatialArchive::open(path);
        ASSERT_NE(archive, nullptr);
        const auto streams = archive->read([](IO::SpatialReader& reader) { return reader.get_stream_names(); });

        const auto alone = IO::SpatialFileFrames::create(path, streams[0]);

        ASSERT_NE(alone, nullptr);
        EXPECT_EQ(alone->count(), 3U);
    }

    TEST_F(SpatialAssemblyTest, SeveralThreadsMayReadDifferentSourcesOfOneArchive)
    {
        const auto assembly = IO::open_spatial_assembly(level_path());
        ASSERT_NE(assembly, nullptr);
        const auto walker = assembly->mesh_frames(source_named(*assembly, "walker"));
        const auto dust = assembly->spatial_frames(source_named(*assembly, "dust"));
        ASSERT_NE(walker, nullptr);
        ASSERT_NE(dust, nullptr);
        std::atomic<int> wrong { 0 };

        std::thread meshes([&] {
            for (int i = 0; i < 100; ++i) {
                const auto frame = walker->read(static_cast<size_t>(i % 3));
                if (!frame || first_x(*frame) != static_cast<float>(i % 3)) {
                    wrong.fetch_add(1);
                }
            }
        });
        std::thread points([&] {
            for (int i = 0; i < 100; ++i) {
                const auto frame = dust->read(static_cast<size_t>(i % 3));
                if (!frame || frame->positions.size() != static_cast<size_t>(i % 3) + 2) {
                    wrong.fetch_add(1);
                }
            }
        });
        meshes.join();
        points.join();

        EXPECT_EQ(wrong.load(), 0);
    }

    TEST_F(SpatialAssemblyTest, TheTransformSamplesOfAnArchiveMoveTheMeshesUnderThemThroughAReflow)
    {
        const auto path = path_in_cwd("arm.abc");
        write_arm(path);
        const auto assembly = IO::open_spatial_assembly(path);
        ASSERT_NE(assembly, nullptr);
        ASSERT_EQ(assembly->get_data().motions.size(), 1U);
        const auto frames = Kakshya::MeshAssemblyMotionFrames::create(assembly, 0);
        ASSERT_NE(frames, nullptr);
        const auto realized = Kriya::MeshAssemblyRealization::create(assembly);
        ASSERT_NE(realized, nullptr);
        ASSERT_EQ(realized->parts().size(), 1U);
        auto reflow = Kriya::MotionReflowBuilder::sampling(frames, realized->placement_sink(), 1.0).build();
        ASSERT_NE(reflow, nullptr);
        const auto& network = realized->parts()[0].network;
        ASSERT_NE(network, nullptr);

        reflow->advance();
        EXPECT_NEAR(network->get_slot(0).local_transform[3].x, 0.0F, 1e-4F);
        reflow->advance();
        EXPECT_NEAR(network->get_slot(0).local_transform[3].x, 2.0F, 1e-4F);
        reflow->advance();
        EXPECT_NEAR(network->get_slot(0).local_transform[3].x, 4.0F, 1e-4F);
    }

    TEST_F(SpatialAssemblyTest, AnArchiveBecomesTargetsThatArePlacedAndPlayedFromItsOwnFrames)
    {
        const auto assembly = IO::open_spatial_assembly(level_path());
        ASSERT_NE(assembly, nullptr);
        Kriya::MeshAssemblyRealizeOptions options;
        options.partition = Kakshya::Partitions::by_geometry_change();

        const auto realized = Kriya::MeshAssemblyRealization::create(assembly, options);

        ASSERT_NE(realized, nullptr);
        ASSERT_EQ(realized->parts().size(), 3U);

        const size_t walker_source = source_named(*assembly, "walker");
        const Kriya::MeshAssemblyRealizedPart* still = nullptr;
        const Kriya::MeshAssemblyRealizedPart* moving = nullptr;
        const Kriya::MeshAssemblyRealizedPart* cloud = nullptr;
        for (const auto& part : realized->parts()) {
            if (part.kind == Kakshya::MeshAssemblyRowKind::Points) {
                cloud = &part;
            } else if (assembly->placement(part.rows.front())->source == walker_source) {
                moving = &part;
            } else {
                still = &part;
            }
        }
        ASSERT_NE(still, nullptr);
        ASSERT_NE(moving, nullptr);
        ASSERT_NE(cloud, nullptr);
        ASSERT_NE(still->network, nullptr);
        ASSERT_NE(moving->network, nullptr);
        EXPECT_FLOAT_EQ(still->network->get_slot(0).local_transform[3].x, 5.0F);

        auto mesh_routes = realized->mesh_routes();
        ASSERT_EQ(mesh_routes.size(), 1U);
        mesh_routes[0].course.speed = 8.0;
        auto mesh_reflow = Kriya::MeshReflowBuilder(nullptr, {}, 8.0).route(mesh_routes[0]).build();
        ASSERT_NE(mesh_reflow, nullptr);
        for (int i = 0; i < 3; ++i) {
            mesh_reflow->advance();
        }
        EXPECT_FLOAT_EQ(moving->network->get_slot(0).node->get_mesh_vertices()[0].position.x, 2.0F);
        EXPECT_FLOAT_EQ(still->network->get_slot(0).node->get_mesh_vertices()[0].position.x, 0.0F);

        auto spatial_routes = realized->spatial_routes();
        ASSERT_EQ(spatial_routes.size(), 1U);
        spatial_routes[0].course.speed = 8.0;
        auto spatial_reflow = Kriya::SpatialReflowBuilder(nullptr, {}, 8.0).route(spatial_routes[0]).build();
        ASSERT_NE(spatial_reflow, nullptr);
        for (int i = 0; i < 3; ++i) {
            spatial_reflow->advance();
        }
        const auto& buffer = cloud->spatial_buffers[0];
        ASSERT_NE(buffer->current_frame(), nullptr);
        EXPECT_EQ(buffer->current_frame()->positions.size(), 4U);
    }

} // namespace
} // namespace MayaFlux::Test
