#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Nodes/Graphics/GeometryLeafNode.hpp"
#include "MayaFlux/Nodes/Network/InstanceNetwork.hpp"
#include "MayaFlux/Nodes/Network/Operators/InstanceFieldOperator.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Nodes::GpuSync::GeometryLeafNode;
    using Nodes::Network::InstanceFieldOperator;
    using Nodes::Network::InstanceNetwork;

    struct PayloadNetwork {
        InstanceNetwork network;
        std::shared_ptr<InstanceFieldOperator> op;

        PayloadNetwork()
        {
            const auto node = std::make_shared<GeometryLeafNode>(std::vector<glm::vec3> { glm::vec3(0.0F), glm::vec3(1.0F), glm::vec3(2.0F) });
            network.add_slot("a", node);
            network.add_slot("b", node);
            op = network.create_operator<InstanceFieldOperator>();
        }

        void settle()
        {
            for (auto& slot : network.slots()) {
                slot.dirty = false;
            }
        }
    };

    TEST(InstancePayloadFieldTest, ATintFieldRewritesTheTintOfItsSlotAndMarksOnlyThatSlotDirty)
    {
        PayloadNetwork level;
        level.op->bind_tint(0, [](const glm::vec3& tint) { return tint * 0.5F; });
        level.settle();

        level.network.process_batch(1);

        EXPECT_EQ(level.network.get_slot(0).tint, glm::vec3(0.5F));
        EXPECT_TRUE(level.network.get_slot(0).dirty);
        EXPECT_EQ(level.network.get_slot(1).tint, glm::vec3(1.0F));
        EXPECT_FALSE(level.network.get_slot(1).dirty);
    }

    TEST(InstancePayloadFieldTest, AFieldIsAFunctionOfTheCurrentValueSoItCarriesOnEachCycle)
    {
        PayloadNetwork level;
        level.op->bind_tint(0, [](const glm::vec3& tint) { return tint * 0.5F; });

        level.network.process_batch(1);
        level.network.process_batch(1);

        EXPECT_EQ(level.network.get_slot(0).tint, glm::vec3(0.25F));
    }

    TEST(InstancePayloadFieldTest, TheScalarAndTheTextureIndexAreDrivenTheSameWay)
    {
        PayloadNetwork level;
        level.op->bind_scalar(1, [](float scalar) { return scalar + 2.0F; });
        level.op->bind_texture(1, [](uint32_t texture) { return texture + 3U; });
        level.settle();

        level.network.process_batch(1);

        EXPECT_FLOAT_EQ(level.network.get_slot(1).scalar, 3.0F);
        EXPECT_EQ(level.network.get_slot(1).texture, 3U);
        EXPECT_TRUE(level.network.get_slot(1).dirty);
        EXPECT_FLOAT_EQ(level.network.get_slot(0).scalar, 1.0F);
        EXPECT_EQ(level.network.get_slot(0).texture, 0U);
    }

    TEST(InstancePayloadFieldTest, AFieldThatChangesNothingLeavesTheSlotClean)
    {
        PayloadNetwork level;
        level.op->bind_tint(0, [](const glm::vec3& tint) { return tint; });
        level.op->bind_scalar(0, [](float scalar) { return scalar; });
        level.op->bind_texture(0, [](uint32_t texture) { return texture; });
        level.settle();

        level.network.process_batch(1);

        EXPECT_FALSE(level.network.get_slot(0).dirty);
    }

    TEST(InstancePayloadFieldTest, APayloadFieldComposesWithAPositionFieldOnTheSameSlot)
    {
        PayloadNetwork level;
        level.op->bind_position(0, Kinesis::VectorField { .fn = [](const glm::vec3& p) { return p + glm::vec3(0.0F, 4.0F, 0.0F); } });
        level.op->bind_tint(0, [](const glm::vec3&) { return glm::vec3(1.0F, 0.0F, 0.0F); });

        level.network.process_batch(1);

        EXPECT_FLOAT_EQ(level.network.get_slot(0).transform[3].y, 4.0F);
        EXPECT_EQ(level.network.get_slot(0).tint, glm::vec3(1.0F, 0.0F, 0.0F));
    }

    TEST(InstancePayloadFieldTest, UnbindingASlotDropsItsPayloadFieldsToo)
    {
        PayloadNetwork level;
        level.op->bind_tint(0, [](const glm::vec3&) { return glm::vec3(0.0F); });
        level.op->bind_scalar(0, [](float) { return 9.0F; });
        level.op->bind_texture(0, [](uint32_t) { return 5U; });

        level.op->unbind(0);
        level.network.process_batch(1);

        EXPECT_EQ(level.network.get_slot(0).tint, glm::vec3(1.0F));
        EXPECT_FLOAT_EQ(level.network.get_slot(0).scalar, 1.0F);
        EXPECT_EQ(level.network.get_slot(0).texture, 0U);
    }

    TEST(InstancePayloadFieldTest, UnbindingEverythingDropsEveryPayloadField)
    {
        PayloadNetwork level;
        level.op->bind_tint(0, [](const glm::vec3&) { return glm::vec3(0.0F); });
        level.op->bind_scalar(1, [](float) { return 9.0F; });

        level.op->unbind_all();
        level.network.process_batch(1);

        EXPECT_EQ(level.network.get_slot(0).tint, glm::vec3(1.0F));
        EXPECT_FLOAT_EQ(level.network.get_slot(1).scalar, 1.0F);
    }

} // namespace
} // namespace MayaFlux::Test
