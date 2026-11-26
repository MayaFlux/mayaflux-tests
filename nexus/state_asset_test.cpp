#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/frame_scheduler.hpp"
#include "../support/nexus_support.hpp"
#include "../support/scoped_working_dir.hpp"

#include "../buffers/compute/volume_grid_fixture.hpp"
#include "MayaFlux/Kakshya/NDData/MeshSkin.hpp"
#include "MayaFlux/Buffers/Geometry/ComputeMeshBuffer.hpp"
#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Nodes/Network/ParticleNetwork.hpp"
#include "MayaFlux/Nodes/Network/Operators/PhysicsOperator.hpp"
#include "MayaFlux/Nodes/Graphics/PointCollectionNode.hpp"
#include "MayaFlux/IO/Volume/VDBWriter.hpp"
#include "MayaFlux/Buffers/BufferManager.hpp"
#include "MayaFlux/Buffers/Network/NetworkGeometryBuffer.hpp"
#include "MayaFlux/Buffers/Geometry/GeometryBuffer.hpp"
#include "MayaFlux/Nodes/Network/InstanceNetwork.hpp"
#include "MayaFlux/Buffers/Network/RecordedGeometryBuffer.hpp"
#include "MayaFlux/Buffers/Network/InstanceNetworkBuffer.hpp"
#include "MayaFlux/Nodes/Network/MeshNetwork.hpp"
#include "MayaFlux/Nodes/Graphics/MeshWriterNode.hpp"
#include "MayaFlux/Kakshya/Semantic/MeshAssemblyContainer.hpp"
#include "MayaFlux/IO/Model/ModelReader.hpp"
#include "MayaFlux/IO/Model/ModelExport.hpp"
#include "MayaFlux/Buffers/Network/MeshNetworkBuffer.hpp"
#include "MayaFlux/Buffers/Geometry/MeshBuffer.hpp"
#include "MayaFlux/IO/Image/EXRWriter.hpp"
#include "MayaFlux/IO/Model/AssimpModelWriter.hpp"
#include "MayaFlux/Nexus/State/Decoder.hpp"
#include "MayaFlux/Nexus/State/Encoder.hpp"
#include "MayaFlux/Vruta/EventManager.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    class NexusStateAssetTest : public ::testing::Test {
    protected:
        void SetUp() override
        {
            IO::EXRWriter::register_with_registry();
            IO::AssimpModelWriter::register_with_registry();
        }

        ScopedWorkingDir cwd;
        std::string base = (std::filesystem::current_path() / "snap").string();
        FrameScheduler frames;
        std::shared_ptr<Vruta::EventManager> events = std::make_shared<Vruta::EventManager>();
        std::shared_ptr<Buffers::BufferManager> buffers = std::make_shared<Buffers::BufferManager>();
        Nexus::Fabric source { frames.scheduler, events, buffers, 1.0F };
        Nexus::Fabric target { frames.scheduler, events, buffers, 1.0F };
    };

    TEST_F(NexusStateAssetTest, AMeshBufferWithoutADeclaredFileIsSnapshottedAndLoadedBack)
    {
        auto bindings = std::make_shared<Nexus::State::Bindings>();
        bindings->buffers["wedge"] = std::make_shared<Buffers::MeshBuffer>(triangle_mesh());
        source.wire(std::make_shared<Nexus::Emitter>()).finalise();

        Nexus::StateEncoder encoder(bindings);
        ASSERT_TRUE(encoder.encode(source, base)) << encoder.last_error();
        ASSERT_TRUE(std::filesystem::exists(std::filesystem::current_path() / "assets" / "wedge.glb"));
        ASSERT_TRUE(encoder.encode(source, base)) << encoder.last_error();
        EXPECT_TRUE(std::filesystem::exists(std::filesystem::current_path() / "assets_1" / "wedge.glb"));
        EXPECT_FALSE(std::filesystem::exists(std::filesystem::current_path() / "assets"));

        Nexus::StateDecoder decoder;
        const auto result = decoder.reconstruct(target, base);

        ASSERT_TRUE(result.buffers.contains("wedge"))
            << (result.warnings.empty() ? std::string {} : result.warnings.front());
        const auto loaded = std::dynamic_pointer_cast<Buffers::MeshBuffer>(result.buffers.at("wedge"));
        ASSERT_NE(loaded, nullptr);
        EXPECT_EQ(loaded->get_index_count(), 3U);
    }

    TEST_F(NexusStateAssetTest, AMeshNetworkSnapshotLoadsBackWithItsSlots)
    {
        auto network = std::make_shared<Nodes::Network::MeshNetwork>();
        for (const char* name : { "near", "far" }) {
            auto node = std::make_shared<Nodes::GpuSync::MeshWriterNode>(3U);
            std::vector<Nodes::MeshVertex> vertices(3);
            vertices[1].position = { 1.0F, 0.0F, 0.0F };
            vertices[2].position = { 0.0F, 1.0F, 0.0F };
            node->set_mesh(vertices, std::vector<uint32_t> { 0, 1, 2 });
            node->compute_frame();
            network->add_slot(name, node);
        }
        network->process_batch(1);
        auto bindings = std::make_shared<Nexus::State::Bindings>();
        bindings->buffers["pair"] = std::make_shared<Buffers::MeshNetworkBuffer>(network);
        source.wire(std::make_shared<Nexus::Emitter>()).finalise();
        Nexus::StateEncoder encoder(bindings);
        ASSERT_TRUE(encoder.encode(source, base)) << encoder.last_error();

        Nexus::StateDecoder decoder;
        const auto result = decoder.reconstruct(target, base);

        ASSERT_TRUE(result.buffers.contains("pair"))
            << (result.warnings.empty() ? std::string {} : result.warnings.front());
        const auto loaded = std::dynamic_pointer_cast<Buffers::MeshNetworkBuffer>(result.buffers.at("pair"));
        ASSERT_NE(loaded, nullptr);
        EXPECT_EQ(loaded->get_network()->slots().size(), 2U);
    }

    TEST_F(NexusStateAssetTest, APartReferenceLoadsTheBufferThatShowsItsRow)
    {
        const auto path = (std::filesystem::current_path() / "wedge.glb").string();
        ASSERT_TRUE(IO::save_mesh(std::make_shared<Buffers::MeshBuffer>(triangle_mesh()), path));
        IO::ModelReader reader;
        ASSERT_TRUE(reader.open(path)) << reader.get_last_error();
        const auto assembly = reader.create_mesh_assembly();
        ASSERT_NE(assembly, nullptr);
        std::optional<uint64_t> row;
        for (uint64_t i = 0; i < assembly->get_total_elements(); ++i) {
            const auto placement = assembly->placement(i);
            if (placement->source && assembly->mesh(*placement->source)) {
                row = i;
            }
        }
        ASSERT_TRUE(row);

        auto saved = std::make_shared<Nexus::State::Bindings>();
        saved->buffers["part"] = nullptr;
        saved->assets["part"] = { .file = path, .row = row };
        source.wire(std::make_shared<Nexus::Emitter>()).finalise();
        Nexus::StateEncoder encoder(saved);
        ASSERT_TRUE(encoder.encode(source, base)) << encoder.last_error();

        Nexus::StateDecoder decoder;
        const auto result = decoder.reconstruct(target, base);

        ASSERT_TRUE(result.buffers.contains("part"))
            << (result.warnings.empty() ? std::string {} : result.warnings.front());
        EXPECT_NE(result.buffers.at("part"), nullptr);
    }

    TEST_F(NexusStateAssetTest, ARecordedGeometrySnapshotLoadsBackWithItsPoints)
    {
        auto buffer = std::make_shared<Buffers::RecordedGeometryBuffer>(
            4, Portal::Graphics::PrimitiveTopology::POINT_LIST, Kinesis::SamplerBounds {});
        auto frame = std::make_shared<Kakshya::SpatialData>();
        frame->topology = Portal::Graphics::PrimitiveTopology::POINT_LIST;
        frame->positions = { { 0.0F, 0.0F, 0.0F }, { 1.0F, 2.0F, 3.0F } };
        buffer->write_frame(frame);
        auto bindings = std::make_shared<Nexus::State::Bindings>();
        bindings->buffers["dust"] = buffer;
        source.wire(std::make_shared<Nexus::Emitter>()).finalise();
        Nexus::StateEncoder encoder(bindings);
        ASSERT_TRUE(encoder.encode(source, base)) << encoder.last_error();

        Nexus::StateDecoder decoder;
        const auto result = decoder.reconstruct(target, base);

        ASSERT_TRUE(result.buffers.contains("dust"))
            << (result.warnings.empty() ? std::string {} : result.warnings.front());
        const auto loaded = std::dynamic_pointer_cast<Buffers::RecordedGeometryBuffer>(result.buffers.at("dust"));
        ASSERT_NE(loaded, nullptr);
        const auto shown = loaded->current_frame();
        ASSERT_NE(shown, nullptr);
        EXPECT_EQ(shown->positions, frame->positions);
    }

    TEST_F(NexusStateAssetTest, AnInstanceSnapshotLoadsBackWithEverySlot)
    {
        auto node = std::make_shared<Nodes::GpuSync::MeshWriterNode>(3U);
        std::vector<Nodes::MeshVertex> vertices(3);
        vertices[1].position = { 1.0F, 0.0F, 0.0F };
        vertices[2].position = { 0.0F, 1.0F, 0.0F };
        node->set_mesh(vertices, std::vector<uint32_t> { 0, 1, 2 });
        node->compute_frame();
        auto network = std::make_shared<Nodes::Network::InstanceNetwork>();
        network->add_slot("near", node);
        const uint32_t far = network->add_slot("far", node);
        network->get_slot(far).transform[3] = glm::vec4(5.0F, 0.0F, 0.0F, 1.0F);
        network->get_slot(far).tint = { 0.25F, 0.5F, 0.75F };
        auto bindings = std::make_shared<Nexus::State::Bindings>();
        bindings->buffers["wall"] = std::make_shared<Buffers::InstanceNetworkBuffer>(network);
        source.wire(std::make_shared<Nexus::Emitter>()).finalise();
        Nexus::StateEncoder encoder(bindings);
        ASSERT_TRUE(encoder.encode(source, base)) << encoder.last_error();

        Nexus::StateDecoder decoder;
        const auto result = decoder.reconstruct(target, base);

        ASSERT_TRUE(result.buffers.contains("wall"))
            << (result.warnings.empty() ? std::string {} : result.warnings.front());
        const auto loaded = std::dynamic_pointer_cast<Buffers::InstanceNetworkBuffer>(result.buffers.at("wall"));
        ASSERT_NE(loaded, nullptr);
        ASSERT_EQ(loaded->get_network()->slot_count(), 2U);
        EXPECT_EQ(loaded->get_network()->get_slot(1).transform, network->get_slot(far).transform);
        EXPECT_EQ(loaded->get_network()->get_slot(1).tint, glm::vec3(0.25F, 0.5F, 0.75F));
    }

    TEST_F(NexusStateAssetTest, AnInstanceTemplateKeepsItsSkinMorphsAndEachPose)
    {
        auto node = std::make_shared<Nodes::GpuSync::MeshWriterNode>(3U);
        std::vector<Nodes::MeshVertex> vertices(3);
        vertices[1].position = { 1.0F, 0.0F, 0.0F };
        vertices[2].position = { 0.0F, 1.0F, 0.0F };
        for (auto& vertex : vertices) {
            vertex.normal = { 0.0F, 0.0F, 1.0F };
        }
        node->set_mesh(vertices, std::vector<uint32_t> { 0, 1, 2 });
        node->compute_frame();

        auto skin = std::make_shared<Kakshya::MeshSkin>();
        skin->inverse_bind = { glm::mat4(1.0F) };
        skin->influence_joints.assign(3, glm::uvec4(0U));
        skin->influence_weights.assign(3, glm::vec4(1.0F, 0.0F, 0.0F, 0.0F));
        auto shape = node->get_mesh_vertices();
        for (auto& vertex : shape) {
            vertex.position.z += 1.0F;
        }

        auto network = std::make_shared<Nodes::Network::InstanceNetwork>();
        network->add_slot("one", node);
        network->get_slot(0).skin = skin;
        network->get_slot(0).morphs = std::make_shared<const Kakshya::MeshMorphTargets>(
            Kakshya::MeshMorphTargets { { "lift", shape } });
        network->get_slot(0).morph_weights = { 0.5F };
        glm::mat4 posed(1.0F);
        posed[3] = glm::vec4(0.0F, 2.0F, 0.0F, 1.0F);
        network->get_slot(0).palette = { posed };

        auto bindings = std::make_shared<Nexus::State::Bindings>();
        bindings->buffers["crowd"] = std::make_shared<Buffers::InstanceNetworkBuffer>(network);
        source.wire(std::make_shared<Nexus::Emitter>()).finalise();
        Nexus::StateEncoder encoder(bindings);
        ASSERT_TRUE(encoder.encode(source, base)) << encoder.last_error();

        Nexus::StateDecoder decoder;
        const auto result = decoder.reconstruct(target, base);

        ASSERT_TRUE(result.buffers.contains("crowd"))
            << (result.warnings.empty() ? std::string {} : result.warnings.front());
        const auto loaded = std::dynamic_pointer_cast<Buffers::InstanceNetworkBuffer>(result.buffers.at("crowd"));
        ASSERT_NE(loaded, nullptr);
        const auto& slot = loaded->get_network()->get_slot(0);
        ASSERT_NE(slot.skin, nullptr);
        EXPECT_EQ(slot.skin->joint_count(), 1U);
        ASSERT_NE(slot.morphs, nullptr);
        EXPECT_EQ(slot.morphs->size(), 1U);
        EXPECT_EQ(slot.morph_weights, (std::vector<float> { 0.5F }));
        ASSERT_EQ(slot.palette.size(), 1U);
        EXPECT_EQ(slot.palette[0], posed);
    }

    TEST_F(NexusStateAssetTest, AnInstanceSnapshotKeepsEveryJointInItsPlace)
    {
        const std::array<glm::vec3, 3> corners { glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(1.0F, 0.0F, 0.0F),
            glm::vec3(0.0F, 1.0F, 0.0F) };
        auto node = std::make_shared<Nodes::GpuSync::MeshWriterNode>(3U);
        std::vector<Nodes::MeshVertex> vertices(3);
        for (size_t v = 0; v < vertices.size(); ++v) {
            vertices[v].position = corners[v];
            vertices[v].normal = { 0.0F, 0.0F, 1.0F };
        }
        node->set_mesh(vertices, std::vector<uint32_t> { 0, 1, 2 });
        node->compute_frame();

        const auto at = [](float x) {
            glm::mat4 matrix(1.0F);
            matrix[3] = glm::vec4(x, 0.0F, 0.0F, 1.0F);
            return matrix;
        };
        const std::array<uint32_t, 3> joint_of { 3U, 2U, 0U };

        auto skin = std::make_shared<Kakshya::MeshSkin>();
        skin->inverse_bind = { at(1.0F), at(2.0F), at(3.0F), at(4.0F) };
        for (const uint32_t joint : joint_of) {
            skin->influence_joints.emplace_back(joint, 0U, 0U, 0U);
            skin->influence_weights.emplace_back(1.0F, 0.0F, 0.0F, 0.0F);
        }

        auto network = std::make_shared<Nodes::Network::InstanceNetwork>();
        network->add_slot("one", node);
        network->get_slot(0).skin = skin;
        network->get_slot(0).palette = { at(10.0F), at(20.0F), at(30.0F), at(40.0F) };

        auto bindings = std::make_shared<Nexus::State::Bindings>();
        bindings->buffers["crowd"] = std::make_shared<Buffers::InstanceNetworkBuffer>(network);
        source.wire(std::make_shared<Nexus::Emitter>()).finalise();
        Nexus::StateEncoder encoder(bindings);
        ASSERT_TRUE(encoder.encode(source, base)) << encoder.last_error();

        Nexus::StateDecoder decoder;
        const auto result = decoder.reconstruct(target, base);

        ASSERT_TRUE(result.buffers.contains("crowd"))
            << (result.warnings.empty() ? std::string {} : result.warnings.front());
        const auto loaded = std::dynamic_pointer_cast<Buffers::InstanceNetworkBuffer>(result.buffers.at("crowd"));
        ASSERT_NE(loaded, nullptr);
        const auto& slot = loaded->get_network()->get_slot(0);
        ASSERT_NE(slot.skin, nullptr);
        ASSERT_EQ(slot.skin->joint_count(), 4U);
        ASSERT_EQ(slot.palette.size(), 4U);

        const auto mesh = std::dynamic_pointer_cast<Nodes::GpuSync::MeshWriterNode>(slot.node);
        ASSERT_NE(mesh, nullptr);
        const auto& read = mesh->get_mesh_vertices();
        ASSERT_EQ(read.size(), 3U);
        ASSERT_EQ(slot.skin->influence_joints.size(), 3U);

        for (size_t v = 0; v < read.size(); ++v) {
            const auto corner = std::ranges::find_if(corners, [&](const glm::vec3& position) {
                return glm::distance(position, read[v].position) < 1e-4F;
            });
            ASSERT_NE(corner, corners.end());
            const uint32_t expected = joint_of[static_cast<size_t>(corner - corners.begin())];
            const uint32_t joint = slot.skin->influence_joints[v].x;

            ASSERT_LT(joint, 4U);
            EXPECT_FLOAT_EQ(slot.skin->inverse_bind[joint][3].x, static_cast<float>(expected + 1U));
            EXPECT_FLOAT_EQ(slot.palette[joint][3].x, static_cast<float>((expected + 1U) * 10U));
        }
    }

    TEST_F(NexusStateAssetTest, ANetworkGeometrySnapshotLoadsBackAsItsSavedPoints)
    {
        auto particles = std::make_shared<Nodes::Network::ParticleNetwork>(
            1, glm::vec3(0.0F), glm::vec3(0.0F), Kinesis::SpatialDistribution::RANDOM_VOLUME);
        auto* physics = particles->create_operator<Nodes::Network::PhysicsOperator>();
        physics->initialize_collections({
            { { .position = { 0.0F, 0.0F, 0.0F } }, { .position = { 2.0F, 0.0F, 0.0F } } },
        });
        auto bindings = std::make_shared<Nexus::State::Bindings>();
        bindings->buffers["swarm"] = std::make_shared<Buffers::NetworkGeometryBuffer>(particles);
        source.wire(std::make_shared<Nexus::Emitter>()).finalise();
        Nexus::StateEncoder encoder(bindings);
        ASSERT_TRUE(encoder.encode(source, base)) << encoder.last_error();

        Nexus::StateDecoder decoder;
        const auto result = decoder.reconstruct(target, base);

        ASSERT_TRUE(result.buffers.contains("swarm"))
            << (result.warnings.empty() ? std::string {} : result.warnings.front());
        const auto loaded = std::dynamic_pointer_cast<Buffers::RecordedGeometryBuffer>(result.buffers.at("swarm"));
        ASSERT_NE(loaded, nullptr);
        ASSERT_NE(loaded->current_frame(), nullptr);
        EXPECT_EQ(loaded->current_frame()->positions.size(), 3U);
        EXPECT_TRUE(std::ranges::any_of(loaded->current_frame()->positions,
            [](const glm::vec3& point) { return point == glm::vec3(2.0F, 0.0F, 0.0F); }));
    }

    TEST_F(NexusStateAssetTest, APointNodeSnapshotLoadsBackAsItsPoints)
    {
        auto node = std::make_shared<Nodes::GpuSync::PointCollectionNode>(std::vector<Kakshya::PointVertex> {
            { .position = { 0.0F, 0.0F, 0.0F } }, { .position = { 1.0F, 2.0F, 3.0F } } });
        node->compute_frame();
        auto bindings = std::make_shared<Nexus::State::Bindings>();
        bindings->buffers["marks"] = std::make_shared<Buffers::GeometryBuffer>(node);
        source.wire(std::make_shared<Nexus::Emitter>()).finalise();
        Nexus::StateEncoder encoder(bindings);
        ASSERT_TRUE(encoder.encode(source, base)) << encoder.last_error();

        Nexus::StateDecoder decoder;
        const auto result = decoder.reconstruct(target, base);

        ASSERT_TRUE(result.buffers.contains("marks"))
            << (result.warnings.empty() ? std::string {} : result.warnings.front());
        const auto loaded = std::dynamic_pointer_cast<Buffers::RecordedGeometryBuffer>(result.buffers.at("marks"));
        ASSERT_NE(loaded, nullptr);
        ASSERT_NE(loaded->current_frame(), nullptr);
        ASSERT_EQ(loaded->current_frame()->positions.size(), 2U);
        EXPECT_EQ(loaded->current_frame()->positions[1], glm::vec3(1.0F, 2.0F, 3.0F));
    }

    class NexusStateVolumeTest : public VolumeGridDeviceFixture {
    protected:
        void SetUp() override
        {
            VolumeGridDeviceFixture::SetUp();
            if (IsSkipped())
                return;
            IO::EXRWriter::register_with_registry();
            IO::VDBWriter::register_with_registry();
        }

        ScopedWorkingDir cwd;
        FrameScheduler frames;
        std::shared_ptr<Vruta::EventManager> events = std::make_shared<Vruta::EventManager>();
        std::shared_ptr<Buffers::BufferManager> manager = std::make_shared<Buffers::BufferManager>();
    };

    MAYAFLUX_TEST_WITH(TEST_F, NexusStateVolumeTest, AVolumeSnapshotLoadsBackWithItsLattice, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        const auto volume = attach({ scalar_decl("density") });
        fill(volume, "density", 0.5F);
        auto bindings = std::make_shared<Nexus::State::Bindings>();
        bindings->buffers["smoke"] = volume;
        Nexus::Fabric source { frames.scheduler, events, manager, 1.0F };
        Nexus::Fabric target { frames.scheduler, events, manager, 1.0F };
        source.wire(std::make_shared<Nexus::Emitter>()).finalise();
        const auto base = (std::filesystem::current_path() / "grid").string();
        Nexus::StateEncoder encoder(bindings);
        ASSERT_TRUE(encoder.encode(source, base)) << encoder.last_error();

        Nexus::StateDecoder decoder;
        const auto result = decoder.reconstruct(target, base);

        ASSERT_TRUE(result.buffers.contains("smoke"))
            << (result.warnings.empty() ? std::string {} : result.warnings.front());
        const auto loaded = std::dynamic_pointer_cast<Buffers::VolumeGridBuffer>(result.buffers.at("smoke"));
        ASSERT_NE(loaded, nullptr);
        EXPECT_EQ(loaded->get_width(), volume->get_width());
        EXPECT_EQ(loaded->get_height(), volume->get_height());
        EXPECT_EQ(loaded->get_depth(), volume->get_depth());
        EXPECT_EQ(loaded->get_bounds().min, volume->get_bounds().min);
        EXPECT_EQ(loaded->get_bounds().max, volume->get_bounds().max);
        manager->remove_buffer(loaded, Buffers::ProcessingToken::GRAPHICS_BACKEND);
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusStateVolumeTest, AComputeMeshSnapshotLoadsBackAsItsSurface, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        IO::AssimpModelWriter::register_with_registry();
        auto computed = std::make_shared<Buffers::ComputeMeshBuffer>(Buffers::ComputeMeshBuffer::Config {
            .source = Kinesis::SpatialField { .fn = [](const glm::vec3& p) { return p.x - 0.25F; } },
            .resolution_x = 4,
            .resolution_y = 4,
            .resolution_z = 4,
        });
        service->initialize_buffer(computed);
        ASSERT_TRUE(computed->is_initialized());
        computed->setup_processors(Buffers::ProcessingToken::GRAPHICS_BACKEND);
        computed->process_default();
        computed->get_processing_chain()->process(computed);
        ASSERT_TRUE(computed->get_live_vertex_count().value_or(0U) > 0U);

        auto bindings = std::make_shared<Nexus::State::Bindings>();
        bindings->buffers["surface"] = computed;
        Nexus::Fabric source { frames.scheduler, events, manager, 1.0F };
        Nexus::Fabric target { frames.scheduler, events, manager, 1.0F };
        source.wire(std::make_shared<Nexus::Emitter>()).finalise();
        const auto base = (std::filesystem::current_path() / "surface_state").string();
        Nexus::StateEncoder encoder(bindings);
        ASSERT_TRUE(encoder.encode(source, base)) << encoder.last_error();

        Nexus::StateDecoder decoder;
        const auto result = decoder.reconstruct(target, base);

        ASSERT_TRUE(result.buffers.contains("surface"))
            << (result.warnings.empty() ? std::string {} : result.warnings.front());
        const auto loaded = std::dynamic_pointer_cast<Buffers::MeshBuffer>(result.buffers.at("surface"));
        ASSERT_NE(loaded, nullptr);
        EXPECT_GT(loaded->get_index_count(), 0U);

        computed->set_default_processor(nullptr);
        service->destroy_buffer(computed);
    }
}
}
