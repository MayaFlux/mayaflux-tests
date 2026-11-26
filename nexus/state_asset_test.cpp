#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/frame_scheduler.hpp"
#include "../support/nexus_support.hpp"
#include "../support/scoped_working_dir.hpp"

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


}
}
