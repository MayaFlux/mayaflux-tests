#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/frame_scheduler.hpp"
#include "../support/nexus_support.hpp"
#include "../support/scoped_working_dir.hpp"

#include "../buffers/compute/volume_grid_fixture.hpp"
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
}
}
