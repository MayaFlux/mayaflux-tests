#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/io_roundtrip_support.hpp"
#include "../support/scoped_working_dir.hpp"

#include "MayaFlux/IO/Model/ModelReader.hpp"
#include "MayaFlux/Kakshya/Semantic/MeshAssemblyContainer.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {
namespace {

    using RoundTrip::path_in_cwd;

    template <typename T>
    std::string skin_fixture_bytes(const std::vector<T>& values)
    {
        return { reinterpret_cast<const char*>(values.data()), values.size() * sizeof(T) };
    }

    /**
     * One triangle skinned by two joints, joint_b a child of joint_a one unit up. Vertex 0 follows
     * joint_a, vertex 2 follows joint_b and vertex 1 is shared between them equally.
     */
    std::string skinned_triangle_gltf()
    {
        std::string payload = skin_fixture_bytes<float>({ 0, 0, 0, 1, 0, 0, 0, 1, 0 });
        payload += skin_fixture_bytes<uint8_t>({ 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0 });
        payload += skin_fixture_bytes<float>({ 1, 0, 0, 0, 0.5F, 0.5F, 0, 0, 1, 0, 0, 0 });
        payload += skin_fixture_bytes<float>({ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
            1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1 });
        payload += skin_fixture_bytes<uint16_t>({ 0, 1, 2, 0 });

        return R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,1]}],)"
               R"("nodes":[{"name":"skin_mesh","mesh":0,"skin":0},)"
               R"({"name":"joint_a","children":[2]},{"name":"joint_b","translation":[0,1,0]}],)"
               R"("skins":[{"joints":[1,2],"inverseBindMatrices":3}],)"
               R"("meshes":[{"name":"tri","primitives":[{"attributes":{"POSITION":0,"JOINTS_0":1,"WEIGHTS_0":2},"indices":4,"mode":4}]}],)"
               R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
               R"({"bufferView":1,"componentType":5121,"count":3,"type":"VEC4"},)"
               R"({"bufferView":2,"componentType":5126,"count":3,"type":"VEC4"},)"
               R"({"bufferView":3,"componentType":5126,"count":2,"type":"MAT4"},)"
               R"({"bufferView":4,"componentType":5123,"count":3,"type":"SCALAR"}],)"
               R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":12},)"
               R"({"buffer":0,"byteOffset":48,"byteLength":48},{"buffer":0,"byteOffset":96,"byteLength":128},)"
               R"({"buffer":0,"byteOffset":224,"byteLength":6}],)"
               R"("buffers":[{"byteLength":232,"uri":"data:application/octet-stream;base64,)"
            + RoundTrip::base64(payload) + R"("}]})";
    }

    class ModelSkinTest : public ::testing::Test {
    protected:
        ScopedWorkingDir m_files;

        std::shared_ptr<Kakshya::MeshAssemblyContainer> open_assembly()
        {
            const auto path = path_in_cwd("skinned.gltf");
            RoundTrip::write_bytes(path, skinned_triangle_gltf());
            if (!m_reader.open(path)) {
                ADD_FAILURE() << m_reader.get_last_error();
                return nullptr;
            }
            return m_reader.create_mesh_assembly();
        }

        IO::ModelReader m_reader;
    };

    TEST_F(ModelSkinTest, ASkinnedMeshKeepsItsSkinInTheFacetsOfItsSource)
    {
        const auto assembly = open_assembly();
        ASSERT_NE(assembly, nullptr);

        const auto skin = assembly->skin(0);

        ASSERT_NE(skin, nullptr);
        EXPECT_EQ(skin->joint_count(), 2U);
        EXPECT_EQ(skin->inverse_bind.size(), 2U);
        EXPECT_TRUE(skin->valid(3));
    }

    TEST_F(ModelSkinTest, TheJointsAreTheRowsOfTheNodesTheSkinNames)
    {
        const auto assembly = open_assembly();
        ASSERT_NE(assembly, nullptr);
        ASSERT_EQ(assembly->get_data().skeletons.size(), 1U);
        const auto& joints = assembly->get_data().skeletons[0];
        ASSERT_EQ(joints.size(), 2U);

        EXPECT_EQ(assembly->placement(joints[0])->name, "joint_a");
        EXPECT_EQ(assembly->placement(joints[1])->name, "joint_b");
    }

    TEST_F(ModelSkinTest, TheInverseBindMatricesAreTheFilesOwn)
    {
        const auto assembly = open_assembly();
        ASSERT_NE(assembly, nullptr);
        const auto skin = assembly->skin(0);
        ASSERT_NE(skin, nullptr);

        EXPECT_EQ(skin->inverse_bind[0], glm::mat4(1.0F));
        EXPECT_FLOAT_EQ(skin->inverse_bind[1][3].y, -1.0F);
    }

    TEST_F(ModelSkinTest, EachVertexHasItsJointsAndWeightsThatSumToOne)
    {
        const auto assembly = open_assembly();
        ASSERT_NE(assembly, nullptr);
        const auto skin = assembly->skin(0);
        ASSERT_NE(skin, nullptr);

        EXPECT_EQ(skin->influence_joints[0][0], 0U);
        EXPECT_FLOAT_EQ(skin->influence_weights[0][0], 1.0F);
        EXPECT_FLOAT_EQ(skin->influence_weights[1][0] + skin->influence_weights[1][1], 1.0F);
        EXPECT_FLOAT_EQ(skin->influence_weights[1][0], 0.5F);
        EXPECT_EQ(skin->influence_joints[2][0], 1U);
        EXPECT_FLOAT_EQ(skin->influence_weights[2][0], 1.0F);
    }

    TEST_F(ModelSkinTest, AMeshWithNoBonesHasNoSkin)
    {
        const auto path = path_in_cwd("plain.gltf");
        RoundTrip::write_bytes(path,
            R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"name":"tri","mesh":0}],)"
            R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"mode":4}]}],)"
            R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],)"
            R"("bufferViews":[{"buffer":0,"byteLength":36}],"buffers":[{"byteLength":36,"uri":"data:application/octet-stream;base64,)"
                + RoundTrip::base64(skin_fixture_bytes<float>({ 0, 0, 0, 1, 0, 0, 0, 1, 0 })) + R"("}]})");
        ASSERT_TRUE(m_reader.open(path)) << m_reader.get_last_error();

        const auto assembly = m_reader.create_mesh_assembly();

        ASSERT_NE(assembly, nullptr);
        EXPECT_EQ(assembly->skin(0), nullptr);
    }

} // namespace
} // namespace MayaFlux::Test
