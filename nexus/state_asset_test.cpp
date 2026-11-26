#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/frame_scheduler.hpp"
#include "../support/nexus_support.hpp"
#include "../support/scoped_working_dir.hpp"

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

}
}
