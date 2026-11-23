#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kakshya/Semantic/MeshAssemblyContainer.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::MeshAssemblyContainer;
    using Kakshya::MeshSkin;

    MeshSkin two_joint_skin(size_t vertices)
    {
        MeshSkin skin;
        skin.joints = { 1, 2 };
        skin.inverse_bind = { glm::mat4(1.0F), glm::mat4(1.0F) };
        skin.influence_joints.assign(vertices, glm::uvec4(0, 1, 0, 0));
        skin.influence_weights.assign(vertices, glm::vec4(0.5F, 0.5F, 0.0F, 0.0F));
        return skin;
    }

    TEST(MeshSkinTest, ASkinWhoseArraysAgreeIsValidForItsVertices)
    {
        const auto skin = two_joint_skin(3);

        EXPECT_TRUE(skin.valid());
        EXPECT_TRUE(skin.valid(3));
        EXPECT_EQ(skin.vertex_count(), 3U);
    }

    TEST(MeshSkinTest, ASkinForAnotherNumberOfVerticesIsNotValidForThem)
    {
        EXPECT_FALSE(two_joint_skin(3).valid(4));
    }

    TEST(MeshSkinTest, ArraysThatDisagreeMakeASkinInvalid)
    {
        auto fewer_matrices = two_joint_skin(3);
        fewer_matrices.inverse_bind.pop_back();
        auto fewer_weights = two_joint_skin(3);
        fewer_weights.influence_weights.pop_back();

        EXPECT_FALSE(fewer_matrices.valid());
        EXPECT_FALSE(fewer_weights.valid());
        EXPECT_FALSE(MeshSkin {}.valid());
    }

    TEST(MeshSkinTest, AWeightedComponentMustNameAJointButAnUnusedOneNeedNot)
    {
        auto names_nothing = two_joint_skin(2);
        names_nothing.influence_joints[1] = glm::uvec4(0, 5, 0, 0);
        auto unused = two_joint_skin(2);
        unused.influence_joints[1] = glm::uvec4(0, 1, 9, 9);

        EXPECT_FALSE(names_nothing.valid());
        EXPECT_TRUE(unused.valid());
    }

    TEST(MeshSkinTest, ASourceKeepsItsSkinInItsFacetsAndTheContainerFindsIt)
    {
        MeshAssemblyContainer assembly;
        Facets facets;
        facets.set(std::make_shared<const MeshSkin>(two_joint_skin(3)));
        const auto skinned = assembly.add_source({ .geometry = Kakshya::MeshData::empty(), .name = "skinned", .facets = std::move(facets) });
        const auto plain = assembly.add_source({ .geometry = Kakshya::MeshData::empty(), .name = "plain" });

        const auto found = assembly.skin(skinned);

        ASSERT_NE(found, nullptr);
        EXPECT_EQ(found->joints, (std::vector<uint64_t> { 1, 2 }));
        EXPECT_EQ(assembly.skin(plain), nullptr);
        EXPECT_EQ(assembly.skin(99), nullptr);
    }

    TEST(MeshSkinTest, FacetsMaySetAfterTheRowsExist)
    {
        MeshAssemblyContainer assembly;
        const auto source = assembly.add_source({ .geometry = Kakshya::MeshData::empty(), .name = "late" });
        Facets facets;
        facets.set(std::make_shared<const MeshSkin>(two_joint_skin(1)));

        EXPECT_TRUE(assembly.set_source_facets(source, std::move(facets)));
        EXPECT_FALSE(assembly.set_source_facets(7, Facets {}));
        EXPECT_NE(assembly.skin(source), nullptr);
    }

} // namespace
} // namespace MayaFlux::Test
