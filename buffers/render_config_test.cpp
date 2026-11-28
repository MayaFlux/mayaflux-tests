#include "../support/forma_rig.hpp"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Buffers/Geometry/MeshBuffer.hpp"
#include "MayaFlux/Buffers/Textures/TextureBuffer.hpp"
#include "MayaFlux/Kakshya/NDData/MeshData.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::system_font, ::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test {

class RenderConfigCarryTest : public FormaRigTest {
protected:
    Portal::Graphics::RenderConfig declared(Portal::Graphics::PrimitiveTopology topology) const
    {
        return { .target_window = window,
            .topology = topology,
            .depth_test = Portal::Graphics::CompareOp::LESS_OR_EQUAL,
            .depth_write = false,
            .view_pinned = true,
            .draw_band = Portal::Graphics::DrawBand::FOREMOST };
    }

    static void expect_carried(const std::shared_ptr<Buffers::VKBuffer>& buffer)
    {
        const auto config = buffer->get_render_config();
        EXPECT_EQ(config.depth_test, Portal::Graphics::CompareOp::LESS_OR_EQUAL);
        EXPECT_FALSE(config.depth_write);
        EXPECT_TRUE(config.view_pinned);
        EXPECT_EQ(config.draw_band, Portal::Graphics::DrawBand::FOREMOST);
        ASSERT_NE(buffer->get_render_processor(), nullptr);
        EXPECT_TRUE(buffer->get_render_processor()->view_pinned());
    }

    static std::shared_ptr<Buffers::MeshBuffer> triangle()
    {
        const std::vector<glm::vec3> positions { { 0.F, 0.F, 0.F }, { 1.F, 0.F, 0.F }, { 0.F, 1.F, 0.F } };
        auto data = Kakshya::MeshData::from_positions(positions, std::vector<uint32_t> { 0, 1, 2 });
        return data ? std::make_shared<Buffers::MeshBuffer>(std::move(*data)) : nullptr;
    }
};

TEST_F(RenderConfigCarryTest, ATextureBufferKeepsTheDrawMembersItWasGiven)
{
    auto buffer = std::make_shared<Buffers::TextureBuffer>(4, 4, Portal::Graphics::ImageFormat::RGBA8);
    buffer_manager->add_buffer(buffer, Buffers::ProcessingToken::GRAPHICS_BACKEND);

    buffer->setup_rendering(declared(Portal::Graphics::PrimitiveTopology::TRIANGLE_STRIP));

    expect_carried(buffer);
}

TEST_F(RenderConfigCarryTest, AMeshBufferKeepsTheDrawMembersItWasGiven)
{
    auto buffer = triangle();
    ASSERT_NE(buffer, nullptr);
    buffer_manager->add_buffer(buffer, Buffers::ProcessingToken::GRAPHICS_BACKEND);

    buffer->setup_rendering(declared(Portal::Graphics::PrimitiveTopology::TRIANGLE_LIST));

    expect_carried(buffer);
}

TEST_F(RenderConfigCarryTest, LeftUnsaidTheMembersLeaveTheProcessorUnpinnedAndTheBandUnset)
{
    auto buffer = triangle();
    ASSERT_NE(buffer, nullptr);
    buffer_manager->add_buffer(buffer, Buffers::ProcessingToken::GRAPHICS_BACKEND);

    buffer->setup_rendering({ .target_window = window });

    const auto config = buffer->get_render_config();
    EXPECT_FALSE(config.depth_test.has_value());
    EXPECT_FALSE(config.draw_band.has_value());
    EXPECT_FALSE(buffer->get_render_processor()->view_pinned());
}

}
