#include "../test_config.h"

#include "MayaFlux/Buffers/BufferManager.hpp"
#include "MayaFlux/Nexus/Tapestry.hpp"

#include "MayaFlux/MayaFlux.hpp"

namespace MayaFlux::Test {
namespace {

    class ApiStagecraftTest : public ::testing::Test {
    protected:
        void SetUp() override
        {
            MayaFlux::End();
            MayaFlux::Init(TestConfig::SAMPLE_RATE, TestConfig::BUFFER_SIZE, TestConfig::NUM_CHANNELS, 0);
        }

        void TearDown() override { MayaFlux::End(); }

        [[nodiscard]] static size_t sinks_on(uint32_t channel)
        {
            return MayaFlux::get_buffer_manager()->get_buffers(Buffers::ProcessingToken::AUDIO_BACKEND, channel).size();
        }
    };

}

TEST_F(ApiStagecraftTest, CreateFabricReturnsAFabricWithItsName)
{
    const auto fabric = MayaFlux::create_fabric("main");

    ASSERT_NE(fabric, nullptr);
    EXPECT_EQ(fabric->name(), "main");
    EXPECT_TRUE(fabric->all_ids().empty());
}

TEST_F(ApiStagecraftTest, CreateFabricTakesACellSize)
{
    const auto fabric = MayaFlux::create_fabric("grid", 4.0F);
    auto emitter = std::make_shared<Nexus::Emitter>();
    emitter->set_position({ 1.0F, 0.0F, 0.0F });
    fabric->wire(emitter).finalise();
    fabric->commit();

    EXPECT_EQ(fabric->within_radius({ 0.0F, 0.0F, 0.0F }, 2.0F).size(), 1U);
}

TEST_F(ApiStagecraftTest, AFabricFromCreateFabricRunsOnTheEnginesManagers)
{
    const auto fabric = MayaFlux::create_fabric("main");
    int fired {};
    auto emitter = std::make_shared<Nexus::Emitter>([&fired](const Nexus::InfluenceContext&) { ++fired; });
    fabric->wire(emitter).finalise();

    fabric->commit();

    EXPECT_EQ(fired, 1);
}

TEST_F(ApiStagecraftTest, CreateTapestryReturnsATapestryWhoseFabricsWork)
{
    const auto tapestry = MayaFlux::create_tapestry();
    ASSERT_NE(tapestry, nullptr);

    const auto fabric = tapestry->create_fabric("alpha");
    int fired {};
    fabric->wire(std::make_shared<Nexus::Emitter>([&fired](const Nexus::InfluenceContext&) { ++fired; })).finalise();
    fabric->commit();

    EXPECT_EQ(tapestry->get_fabric("alpha"), fabric);
    EXPECT_EQ(fired, 1);
}

TEST_F(ApiStagecraftTest, SinkAudioUsesTheEnginesBufferManager)
{
    const auto agent = std::make_shared<Nexus::Agent>(1.0F);
    const size_t before = sinks_on(0);

    MayaFlux::sink_audio(agent, 0);

    EXPECT_EQ(sinks_on(0), before + 1);
    EXPECT_EQ(agent->audio_sinks().size(), 1U);
}

TEST_F(ApiStagecraftTest, SinkAudioForwardsAProducerAndName)
{
    const auto emitter = std::make_shared<Nexus::Emitter>();

    MayaFlux::sink_audio(emitter, 0, [](const Nexus::InfluenceContext&) {
        return Kakshya::DataVariant { std::vector<double> { 0.0 } };
    },
        "tone");

    ASSERT_EQ(emitter->audio_sinks().size(), 1U);
    EXPECT_EQ(emitter->audio_sinks().front().fn_name, "tone");
}

TEST_F(ApiStagecraftTest, RemoveAudioSinkUnregistersIt)
{
    const auto agent = std::make_shared<Nexus::Agent>(1.0F);
    const size_t before = sinks_on(0);
    MayaFlux::sink_audio(agent, 0);

    MayaFlux::remove_audio_sink(agent, 0);

    EXPECT_EQ(sinks_on(0), before);
    EXPECT_TRUE(agent->audio_sinks().empty());
}

TEST_F(ApiStagecraftTest, AFabricFromCreateFabricReleasesTheSinksOfARemovedEntity)
{
    const auto fabric = MayaFlux::create_fabric("main");
    const auto agent = std::make_shared<Nexus::Agent>(1.0F);
    const size_t before = sinks_on(0);
    MayaFlux::sink_audio(agent, 0);
    fabric->wire(agent).finalise();

    fabric->remove(fabric->all_ids().front());

    EXPECT_EQ(sinks_on(0), before);
}

TEST_F(ApiStagecraftTest, AFabricFromACreatedTapestryReleasesTheSinksOfARemovedEntity)
{
    const auto tapestry = MayaFlux::create_tapestry();
    const auto fabric = tapestry->create_fabric();
    const auto agent = std::make_shared<Nexus::Agent>(1.0F);
    const size_t before = sinks_on(0);
    MayaFlux::sink_audio(agent, 0);
    fabric->wire(agent).finalise();

    tapestry->remove_fabric(fabric);

    EXPECT_EQ(sinks_on(0), before);
}

}
