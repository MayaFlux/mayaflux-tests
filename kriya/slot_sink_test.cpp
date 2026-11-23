#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kriya/MotionReflow.hpp"
#include "MayaFlux/Nodes/Graphics/MeshWriterNode.hpp"
#include "MayaFlux/Nodes/Network/MeshNetwork.hpp"

#include <glm/gtc/matrix_transform.hpp>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::MeshAssemblyContainer;
    using Kakshya::MeshAssemblyLocals;
    using Kakshya::MeshAssemblyMotionFrames;
    using Kinesis::TimeSampling;
    using Kriya::FrameContext;
    using Kriya::MotionReflowBuilder;
    using Kriya::slot_sink;
    using Nodes::Network::MeshNetwork;
    using Nodes::Network::make_mesh_network;

    constexpr double k_sink_rate = 8.0;

    /** A base joint and an arm showing a triangle, the arm sliding from x = 0 to x = 2 over two seconds. */
    struct SlidingArm : ::testing::Test {
        void SetUp() override
        {
            assembly = std::make_shared<MeshAssemblyContainer>();
            const auto source = assembly->add_source({ .geometry = Kakshya::MeshData::from_positions(
                                                                      std::vector<glm::vec3> { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 1.0F, 0.0F } },
                                                                      std::vector<uint32_t> { 0, 1, 2 })
                                                                      .value(),
                .name = "arm" });
            const auto base = assembly->add_placement({ .name = "base" }).value();
            arm_row = assembly->add_placement({ .name = "arm", .parent = base, .source = source }).value();

            const auto times = *TimeSampling::acyclic({ 0.0, 2.0 });
            auto slide = Kinesis::TimedValues<glm::vec3>::create({ .times = times,
                .values = { glm::vec3(0.0F), glm::vec3(2.0F, 0.0F, 0.0F) } });
            std::ignore = assembly->add_motion({ .name = "slide", .channels = { { .row = arm_row, .translation = slide } } });

            network = make_mesh_network(assembly);
            frames = MeshAssemblyMotionFrames::create(assembly, 0);
        }

        std::shared_ptr<MeshAssemblyContainer> assembly;
        std::shared_ptr<MeshNetwork> network;
        std::shared_ptr<const MeshAssemblyMotionFrames> frames;
        size_t arm_row {};
    };

    TEST_F(SlidingArm, AReflowOfTheMotionMovesTheSlotOfTheRowItTargets)
    {
        ASSERT_NE(network, nullptr);
        ASSERT_NE(frames, nullptr);
        auto reflow = MotionReflowBuilder::sampling(frames, slot_sink(network), k_sink_rate).build();
        ASSERT_NE(reflow, nullptr);

        for (int i = 0; i < 5; ++i) {
            reflow->advance();
        }

        EXPECT_NEAR(network->get_slot(static_cast<uint32_t>(arm_row)).local_transform[3].x, 0.5F, 1e-5F);
        EXPECT_NEAR(network->get_slot(0).local_transform[3].x, 0.0F, 1e-5F);
    }

    TEST_F(SlidingArm, EachWriteMarksTheSlotDirtySoTheNetworkUploadsIt)
    {
        auto reflow = MotionReflowBuilder::sampling(frames, slot_sink(network), k_sink_rate).build();
        ASSERT_NE(reflow, nullptr);
        for (auto& slot : network->slots()) {
            slot.dirty = false;
        }

        reflow->advance();

        EXPECT_TRUE(network->get_slot(static_cast<uint32_t>(arm_row)).dirty);
    }

    TEST_F(SlidingArm, ANetworkThatIsGoneEndsTheWritesQuietly)
    {
        auto reflow = MotionReflowBuilder::sampling(frames, slot_sink(network), k_sink_rate).build();
        ASSERT_NE(reflow, nullptr);
        network.reset();

        for (int i = 0; i < 3; ++i) {
            reflow->advance();
        }

        EXPECT_EQ(reflow->failures(), 0U);
    }

    TEST_F(SlidingArm, ANetworkWithFewerSlotsThanThePlacementsGetsTheOnesItHas)
    {
        auto smaller = std::make_shared<MeshNetwork>();
        smaller->add_slot("only", nullptr);
        const auto sink = slot_sink(smaller);
        MeshAssemblyLocals locals;
        locals.matrices = { glm::translate(glm::mat4(1.0F), glm::vec3(4.0F, 0.0F, 0.0F)), glm::mat4(1.0F) };

        sink(std::make_shared<const MeshAssemblyLocals>(std::move(locals)), FrameContext {});

        EXPECT_FLOAT_EQ(smaller->get_slot(0).local_transform[3].x, 4.0F);
        EXPECT_TRUE(smaller->get_slot(0).dirty);
    }

    TEST_F(SlidingArm, ANullFrameChangesNothing)
    {
        const auto sink = slot_sink(network);
        network->get_slot(1).local_transform = glm::translate(glm::mat4(1.0F), glm::vec3(7.0F, 0.0F, 0.0F));

        sink(nullptr, FrameContext {});

        EXPECT_FLOAT_EQ(network->get_slot(1).local_transform[3].x, 7.0F);
    }

} // namespace

}
