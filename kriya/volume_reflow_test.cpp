#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../buffers/compute/volume_grid_fixture.hpp"

#include "MayaFlux/IO/Volume/VolumeTransfer.hpp"
#include "MayaFlux/Kriya/VolumeReflow.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::ResidentFrames;
    using Kakshya::VolumeData;
    using Kakshya::VolumeField;
    using Kriya::FrameContext;
    using Kriya::VolumeReflowBuilder;

    VolumeField scalar_of(const std::string& name, size_t cells, float value)
    {
        VolumeField field;
        field.name = name;
        field.values = std::vector<float>(cells, value);
        return field;
    }

    VolumeField vector_of(const std::string& name, size_t cells, const glm::vec3& value)
    {
        VolumeField field;
        field.name = name;
        field.values = std::vector<glm::vec3>(cells, value);
        return field;
    }

    /** A frame over the small lattice with a density of the given value and a velocity along x. */
    VolumeData frame(float value, const Kinesis::Lattice3D& lattice = small_volume_lattice())
    {
        VolumeData data;
        data.lattice = lattice;
        data.fields = {
            scalar_of("density", lattice.cell_count(), value),
            vector_of("velocity", lattice.cell_count(), { value, -value, 0.5F * value }),
        };
        return data;
    }

    std::shared_ptr<const VolumeData> shared(VolumeData data)
    {
        return std::make_shared<const VolumeData>(std::move(data));
    }

    class VolumeReflowDeviceTest : public VolumeGridDeviceFixture {
    protected:
        void expect_frame(const std::shared_ptr<Buffers::VolumeGridBuffer>& volume, float value)
        {
            const auto cells = volume->get_cell_count();
            expect_scalars(read<float>(volume, "density"), std::vector<float>(cells, value));
            expect_vectors(read<glm::vec4>(volume, "velocity"),
                std::vector<glm::vec4>(cells, glm::vec4(value, -value, 0.5F * value, 0.0F)));
        }
    };

    TEST(VolumeFieldDeclsTest, OneDeclarationPerFieldWithTheGpuStrideAndTheDataSemantics)
    {
        auto data = frame(1.0F);
        data.fields[0].semantics.value_class = Kinesis::LatticeValueClass::FogVolume;

        const auto decls = IO::volume_field_decls(data);

        ASSERT_EQ(decls.size(), 2U);
        EXPECT_EQ(decls[0].name, "density");
        EXPECT_EQ(decls[0].stride_bytes, sizeof(float));
        EXPECT_TRUE(decls[0].double_buffered);
        EXPECT_EQ(decls[0].semantics.value_class, Kinesis::LatticeValueClass::FogVolume);
        EXPECT_EQ(decls[1].name, "velocity");
        EXPECT_EQ(decls[1].stride_bytes, sizeof(glm::vec4));
        EXPECT_TRUE(IO::volume_field_decls(VolumeData {}).empty());
    }

    TEST_F(VolumeReflowDeviceTest, AFrameReplacesEveryFieldAndPadsTheVectors)
    {
        const auto volume = attach(IO::volume_field_decls(frame(1.0F)));
        const auto sink = Kriya::volume_sink(volume);

        sink(shared(frame(3.0F)), FrameContext {});
        settle(volume, "density");
        settle(volume, "velocity");

        expect_frame(volume, 3.0F);

        sink(shared(frame(7.0F)), FrameContext {});
        settle(volume, "density");
        settle(volume, "velocity");

        expect_frame(volume, 7.0F);
    }

    TEST_F(VolumeReflowDeviceTest, AFrameOnAnotherLatticeWritesNothing)
    {
        const auto volume = attach(IO::volume_field_decls(frame(1.0F)));
        const auto sink = Kriya::volume_sink(volume);
        sink(shared(frame(2.0F)), FrameContext {});
        settle(volume, "density");

        auto other = small_volume_lattice();
        other.resolution = { 4U, 2U, 2U };
        sink(shared(frame(9.0F, other)), FrameContext {});

        auto moved = small_volume_lattice();
        moved.bounds.max = { 3.0F, 2.0F, 2.0F };
        sink(shared(frame(9.0F, moved)), FrameContext {});
        settle(volume, "density");

        EXPECT_EQ(read<float>(volume, "density"), std::vector<float>(volume->get_cell_count(), 2.0F));
    }

    TEST_F(VolumeReflowDeviceTest, AFieldTheBufferLacksOrDeclaresDifferentlyIsLeftOut)
    {
        const auto volume = attach({ scalar_decl("density"), scalar_decl("velocity") });
        fill(volume, "velocity", 0.0F);
        const auto sink = Kriya::volume_sink(volume);

        auto data = frame(4.0F);
        data.fields.push_back(scalar_of("extra", data.lattice.cell_count(), 1.0F));
        sink(shared(std::move(data)), FrameContext {});
        settle(volume, "density");

        EXPECT_EQ(read<float>(volume, "density"), std::vector<float>(volume->get_cell_count(), 4.0F));
        EXPECT_EQ(read<float>(volume, "velocity"), std::vector<float>(volume->get_cell_count(), 0.0F));
    }

    TEST_F(VolumeReflowDeviceTest, AnInconsistentOrFieldlessFrameWritesNothing)
    {
        const auto volume = attach(IO::volume_field_decls(frame(1.0F)));
        const auto sink = Kriya::volume_sink(volume);
        sink(shared(frame(2.0F)), FrameContext {});
        settle(volume, "density");

        auto short_field = frame(8.0F);
        std::get<std::vector<float>>(short_field.fields[0].values).pop_back();
        sink(shared(std::move(short_field)), FrameContext {});
        sink(shared(VolumeData { .lattice = small_volume_lattice(), .fields = {} }), FrameContext {});
        sink(nullptr, FrameContext {});
        settle(volume, "density");

        EXPECT_EQ(read<float>(volume, "density"), std::vector<float>(volume->get_cell_count(), 2.0F));
    }

    TEST_F(VolumeReflowDeviceTest, ASinkOutlivingItsBufferWritesNothingAndDoesNotKeepItAlive)
    {
        std::weak_ptr<Buffers::VolumeGridBuffer> weak;
        Kriya::VolumeReflow::Sink sink;
        {
            auto volume = std::make_shared<Buffers::VolumeGridBuffer>(
                small_volume_lattice(), IO::volume_field_decls(frame(1.0F)));
            weak = volume;
            sink = Kriya::volume_sink(volume);
        }

        EXPECT_TRUE(weak.expired());
        EXPECT_NO_THROW(sink(shared(frame(5.0F)), FrameContext {}));
    }

    TEST_F(VolumeReflowDeviceTest, AReflowPlaysASequenceIntoTheBuffer)
    {
        auto times = Kinesis::TimeSampling::uniform(0.125);
        ASSERT_TRUE(times.has_value());
        std::vector<VolumeData> frames;
        frames.push_back(frame(1.0F));
        frames.push_back(frame(2.0F));
        frames.push_back(frame(3.0F));
        const auto source = ResidentFrames<VolumeData>::create(*times, std::move(frames));
        ASSERT_NE(source, nullptr);

        const auto volume = attach(IO::volume_field_decls(*source->read(0)));
        auto reflow = VolumeReflowBuilder(source, Kriya::volume_sink(volume), 8.0).build();
        ASSERT_NE(reflow, nullptr);

        reflow->advance();
        settle(volume, "density");
        settle(volume, "velocity");
        expect_frame(volume, 1.0F);

        reflow->advance();
        reflow->advance();
        settle(volume, "density");
        settle(volume, "velocity");
        expect_frame(volume, 3.0F);
        EXPECT_EQ(reflow->writes(), 3U);

        reflow->advance();
        EXPECT_EQ(reflow->writes(), 3U);
        EXPECT_EQ(reflow->current().get(), source->read(2).get());
    }

}
}
