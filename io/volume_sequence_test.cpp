#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/io_roundtrip_support.hpp"
#include "../support/log_capture.hpp"
#include "../support/scoped_working_dir.hpp"

#include "MayaFlux/IO/Volume/VDBWriter.hpp"
#include "MayaFlux/IO/Volume/VolumeSequence.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {
namespace {

    using IO::LatticeSource;
    using IO::VolumeFileSequence;
    using IO::VolumeSequenceOptions;
    using Kakshya::DataModality;
    using Kakshya::Region;
    using Kakshya::VolumeData;
    using Kakshya::VolumeField;
    using Kinesis::Lattice3D;
    using Kinesis::TimeSampling;
    using RoundTrip::path_in_cwd;

    constexpr uint32_t k_side = 24;

    Lattice3D cube(uint32_t side = k_side, float cell = 1.0F)
    {
        Lattice3D lattice;
        lattice.resolution = glm::uvec3(side);
        lattice.bounds.min = glm::vec3(0.0F);
        lattice.bounds.max = glm::vec3(static_cast<float>(side) * cell);
        return lattice;
    }

    VolumeField spark(const std::string& name, const Lattice3D& lattice, const glm::uvec3& cell, float value)
    {
        VolumeField field;
        field.name = name;
        std::vector<float> values(lattice.cell_count(), 0.0F);
        values[lattice.index(cell)] = value;
        field.values = std::move(values);
        return field;
    }

    VolumeData frame_of(const Lattice3D& lattice, std::vector<VolumeField> fields)
    {
        VolumeData data;
        data.lattice = lattice;
        data.fields = std::move(fields);
        return data;
    }

    TimeSampling uniform(double step)
    {
        auto sampling = TimeSampling::uniform(step);
        if (!sampling) {
            throw std::runtime_error("sampling was rejected");
        }
        return *sampling;
    }

    float at(const VolumeData& frame, const glm::uvec3& cell)
    {
        return (*frame.fields.front().as_scalar())[frame.lattice.index(cell)];
    }

    float sum(const VolumeData& frame)
    {
        const auto& values = *frame.fields.front().as_scalar();
        return std::accumulate(values.begin(), values.end(), 0.0F);
    }

    class VolumeSequenceTest : public ::testing::Test {
    protected:
        void SetUp() override
        {
            IO::VDBWriter::register_with_registry();
            LogCapture::install();
            LogCapture::clear();
        }

        void write(const std::string& name, const VolumeData& data)
        {
            IO::VDBWriter writer;
            ASSERT_TRUE(writer.write(path_in_cwd(name), data)) << writer.get_last_error();
        }

        /** Three frames whose one lit cell crosses three leaf blocks. */
        void write_travelling_spark(const std::string& stem = "spark", uint64_t first = 0, uint64_t stride = 1)
        {
            const auto lattice = cube();
            const std::array<glm::uvec3, 3> cells { { { 1, 1, 1 }, { 12, 1, 1 }, { 20, 1, 1 } } };
            for (size_t i = 0; i < cells.size(); ++i) {
                write(std::format("{}.{:04}.vdb", stem, first + i * stride),
                    frame_of(lattice, { spark("density", lattice, cells[i], static_cast<float>(i + 1)) }));
            }
        }

        [[nodiscard]] static VolumeSequenceOptions options(const std::string& stem = "spark", size_t count = 3)
        {
            VolumeSequenceOptions out;
            out.pattern = path_in_cwd(stem + ".{:04}.vdb");
            out.count = count;
            return out;
        }

        ScopedWorkingDir m_files;
    };

    TEST_F(VolumeSequenceTest, UnusableOptionsMakeNoSequence)
    {
        write_travelling_spark();

        auto none = options();
        none.count = 0;
        EXPECT_EQ(VolumeFileSequence::create(none, uniform(1.0), LatticeSource::Union), nullptr);

        auto still = options();
        still.stride = 0;
        EXPECT_EQ(VolumeFileSequence::create(still, uniform(1.0), LatticeSource::Union), nullptr);

        auto two_fields = options();
        two_fields.pattern = path_in_cwd("spark.{}.{}.vdb");
        EXPECT_EQ(VolumeFileSequence::create(two_fields, uniform(1.0), LatticeSource::Union), nullptr);

        auto bad_field = options();
        bad_field.pattern = path_in_cwd("spark.{:q}.vdb");
        EXPECT_EQ(VolumeFileSequence::create(bad_field, uniform(1.0), LatticeSource::Union), nullptr);

        auto flat = Lattice3D {};
        flat.resolution = glm::uvec3(4, 0, 4);
        EXPECT_EQ(VolumeFileSequence::create(options(), uniform(1.0), flat), nullptr);

        auto wrapping = options();
        wrapping.first_number = std::numeric_limits<uint64_t>::max() - 1;
        EXPECT_EQ(VolumeFileSequence::create(wrapping, uniform(1.0), LatticeSource::Union), nullptr);

        EXPECT_NE(VolumeFileSequence::create(options(), uniform(1.0), LatticeSource::Union), nullptr);
    }

    TEST_F(VolumeSequenceTest, TheTimeSamplingMustTimeEveryFrame)
    {
        write_travelling_spark();

        const auto two = TimeSampling::acyclic({ 0.0, 1.0 });
        const auto three = TimeSampling::acyclic({ 0.0, 1.0, 5.0 });
        const auto four = TimeSampling::acyclic({ 0.0, 1.0, 5.0, 6.0 });
        ASSERT_TRUE(two && three && four);

        EXPECT_EQ(VolumeFileSequence::create(options(), *two, LatticeSource::Union), nullptr);
        EXPECT_EQ(VolumeFileSequence::create(options(), *four, LatticeSource::Union), nullptr);

        const auto sequence = VolumeFileSequence::create(options(), *three, LatticeSource::Union);
        ASSERT_NE(sequence, nullptr);
        EXPECT_EQ(*sequence->time_sampling().time(2), 5.0);
        EXPECT_EQ(sequence->locate(4.0)->lower, 1U);
    }

    TEST_F(VolumeSequenceTest, AFirstFileThatCannotBeReadMakesNoSequence)
    {
        EXPECT_EQ(VolumeFileSequence::create(options(), uniform(1.0), LatticeSource::Union), nullptr);
        EXPECT_TRUE(LogCapture::contains("frame 0"));
    }

    TEST_F(VolumeSequenceTest, TheUnionLatticeCoversEveryFramesContent)
    {
        write_travelling_spark();

        const auto sequence = VolumeFileSequence::create(options(), uniform(0.5), LatticeSource::Union);

        ASSERT_NE(sequence, nullptr);
        EXPECT_EQ(sequence->count(), 3U);
        EXPECT_EQ(sequence->lattice().resolution, glm::uvec3(24, 8, 8));
        EXPECT_TRUE(RoundTrip::near(sequence->lattice().bounds.min, glm::vec3(0.0F)));
        EXPECT_TRUE(RoundTrip::near(sequence->lattice().bounds.max, glm::vec3(24.0F, 8.0F, 8.0F)));

        const std::array<glm::uvec3, 3> cells { { { 1, 1, 1 }, { 12, 1, 1 }, { 20, 1, 1 } } };
        for (size_t i = 0; i < 3; ++i) {
            const auto frame = sequence->read(i);
            ASSERT_NE(frame, nullptr) << i;
            EXPECT_TRUE(frame->is_consistent());
            EXPECT_EQ(frame->lattice.resolution, sequence->lattice().resolution);
            EXPECT_EQ(at(*frame, cells[i]), static_cast<float>(i + 1)) << i;
            EXPECT_EQ(sum(*frame), static_cast<float>(i + 1)) << i;
        }
    }

    TEST_F(VolumeSequenceTest, TheFirstFrameLatticeDropsContentOutsideIt)
    {
        write_travelling_spark();

        const auto sequence = VolumeFileSequence::create(options(), uniform(1.0), LatticeSource::FirstFrame);

        ASSERT_NE(sequence, nullptr);
        EXPECT_EQ(sequence->lattice().resolution, glm::uvec3(8));
        EXPECT_EQ(at(*sequence->read(0), { 1, 1, 1 }), 1.0F);
        EXPECT_EQ(sum(*sequence->read(1)), 0.0F);
        EXPECT_EQ(sum(*sequence->read(2)), 0.0F);
    }

    TEST_F(VolumeSequenceTest, AGivenLatticeIsUsedAsGivenForEveryFrame)
    {
        write_travelling_spark();
        auto given = cube();
        given.resolution = glm::uvec3(16, 4, 4);
        given.bounds.max = glm::vec3(16.0F, 4.0F, 4.0F);

        const auto sequence = VolumeFileSequence::create(options(), uniform(1.0), given);

        ASSERT_NE(sequence, nullptr);
        EXPECT_EQ(sequence->lattice().resolution, given.resolution);
        EXPECT_EQ(at(*sequence->read(0), { 1, 1, 1 }), 1.0F);
        EXPECT_EQ(at(*sequence->read(1), { 12, 1, 1 }), 2.0F);
        EXPECT_EQ(sum(*sequence->read(2)), 0.0F);
        EXPECT_EQ(sequence->read(1)->lattice.resolution, given.resolution);
    }

    TEST_F(VolumeSequenceTest, FilesThatDisagreeOnVoxelSizeRefuseAUnion)
    {
        const auto lattice = cube();
        write("coarse.0000.vdb", frame_of(lattice, { spark("density", lattice, { 1, 1, 1 }, 1.0F) }));
        const auto big = cube(k_side, 2.0F);
        write("coarse.0001.vdb", frame_of(big, { spark("density", big, { 1, 1, 1 }, 1.0F) }));

        EXPECT_EQ(VolumeFileSequence::create(options("coarse", 2), uniform(1.0), LatticeSource::Union), nullptr);
        EXPECT_TRUE(LogCapture::contains("voxel size"));
    }

    TEST_F(VolumeSequenceTest, FilesWithDifferentFieldsRefuseAUnionAndFailTheirOwnRead)
    {
        const auto lattice = cube();
        write("mixed.0000.vdb", frame_of(lattice, { spark("density", lattice, { 1, 1, 1 }, 1.0F) }));
        write("mixed.0001.vdb", frame_of(lattice, { spark("temperature", lattice, { 1, 1, 1 }, 1.0F) }));

        EXPECT_EQ(VolumeFileSequence::create(options("mixed", 2), uniform(1.0), LatticeSource::Union), nullptr);

        LogCapture::clear();
        const auto sequence = VolumeFileSequence::create(options("mixed", 2), uniform(1.0), LatticeSource::FirstFrame);
        ASSERT_NE(sequence, nullptr);
        EXPECT_NE(sequence->read(0), nullptr);
        EXPECT_EQ(sequence->read(1), nullptr);
        EXPECT_TRUE(LogCapture::contains("different fields"));
    }

    TEST_F(VolumeSequenceTest, AMissingFileLosesOnlyItsOwnFrame)
    {
        write_travelling_spark();
        const auto sequence = VolumeFileSequence::create(options(), uniform(1.0), LatticeSource::Union);
        ASSERT_NE(sequence, nullptr);

        std::filesystem::remove(sequence->path(1));
        LogCapture::clear();

        EXPECT_EQ(sequence->read(1), nullptr);
        EXPECT_TRUE(LogCapture::contains("frame 1"));
        EXPECT_NE(sequence->read(0), nullptr);
        EXPECT_NE(sequence->read(2), nullptr);
    }

    TEST_F(VolumeSequenceTest, ACorruptFileLosesOnlyItsOwnFrame)
    {
        write_travelling_spark();
        const auto sequence = VolumeFileSequence::create(options(), uniform(1.0), LatticeSource::Union);
        ASSERT_NE(sequence, nullptr);

        {
            std::ofstream damaged(sequence->path(2), std::ios::binary | std::ios::trunc);
            damaged << "not a volume";
        }

        EXPECT_EQ(sequence->read(2), nullptr);
        EXPECT_NE(sequence->read(1), nullptr);
    }

    TEST_F(VolumeSequenceTest, IndicesPastTheEndReadNothingAndNameNoPath)
    {
        write_travelling_spark();
        const auto sequence = VolumeFileSequence::create(options(), uniform(1.0), LatticeSource::Union);
        ASSERT_NE(sequence, nullptr);

        EXPECT_EQ(sequence->read(3), nullptr);
        EXPECT_EQ(sequence->read(std::numeric_limits<size_t>::max()), nullptr);
        EXPECT_TRUE(sequence->path(3).empty());
    }

    TEST_F(VolumeSequenceTest, FirstNumberAndStrideSelectTheFiles)
    {
        write_travelling_spark("skip", 10, 5);
        auto spec = options("skip", 3);
        spec.first_number = 10;
        spec.stride = 5;

        const auto sequence = VolumeFileSequence::create(spec, uniform(1.0), LatticeSource::Union);

        ASSERT_NE(sequence, nullptr);
        EXPECT_TRUE(sequence->path(2).ends_with("skip.0020.vdb"));
        EXPECT_EQ(at(*sequence->read(2), { 20, 1, 1 }), 3.0F);
    }

    TEST_F(VolumeSequenceTest, AHeldFrameIsSharedAndAReleasedOneIsReadAgainEqual)
    {
        write_travelling_spark();
        const auto sequence = VolumeFileSequence::create(options(), uniform(1.0), LatticeSource::Union);
        ASSERT_NE(sequence, nullptr);

        auto first = sequence->read(1);
        const auto second = sequence->read(1);
        ASSERT_NE(first, nullptr);
        EXPECT_EQ(first.get(), second.get());

        const auto values = *first->fields.front().as_scalar();
        first.reset();
        const auto third = sequence->read(1);
        ASSERT_NE(third, nullptr);
        EXPECT_NE(second, nullptr);
        EXPECT_EQ(*third->fields.front().as_scalar(), values);
    }

    TEST_F(VolumeSequenceTest, FieldSelectionNarrowsEveryFrame)
    {
        const auto lattice = cube();
        for (uint64_t i = 0; i < 2; ++i) {
            write(std::format("pair.{:04}.vdb", i),
                frame_of(lattice, { spark("density", lattice, { 1, 1, 1 }, 1.0F), spark("heat", lattice, { 2, 2, 2 }, 2.0F) }));
        }
        auto spec = options("pair", 2);
        spec.read.field_names = { "heat" };

        const auto sequence = VolumeFileSequence::create(spec, uniform(1.0), LatticeSource::Union);

        ASSERT_NE(sequence, nullptr);
        const auto frame = sequence->read(1);
        ASSERT_NE(frame, nullptr);
        ASSERT_EQ(frame->fields.size(), 1U);
        EXPECT_EQ(frame->fields.front().name, "heat");
    }

    TEST_F(VolumeSequenceTest, ManyThreadsReadEveryFrameAndAgree)
    {
        write_travelling_spark();
        const auto sequence = VolumeFileSequence::create(options(), uniform(1.0), LatticeSource::Union);
        ASSERT_NE(sequence, nullptr);

        std::atomic<int> wrong { 0 };
        std::vector<std::thread> pool;
        pool.reserve(6);
        for (int t = 0; t < 6; ++t) {
            pool.emplace_back([&sequence, &wrong, t] {
                for (size_t i = 0; i < 30; ++i) {
                    const size_t index = (i + static_cast<size_t>(t)) % 3;
                    const auto frame = sequence->read(index);
                    if (!frame || sum(*frame) != static_cast<float>(index + 1)) {
                        ++wrong;
                    }
                }
            });
        }
        for (auto& thread : pool) {
            thread.join();
        }

        EXPECT_EQ(wrong.load(), 0);
    }

    TEST_F(VolumeSequenceTest, TheContainerServesFrameRangesAndSharesFrames)
    {
        write_travelling_spark();

        const auto container = IO::open_volume_sequence(options(), uniform(1.0), LatticeSource::Union);

        ASSERT_NE(container, nullptr);
        EXPECT_EQ(container->get_structure().modality, DataModality::VOLUMETRIC_3D);
        EXPECT_EQ(container->get_num_frames(), 3U);

        const auto frames = container->get_region_data(Region({ 1 }, { 2 }));
        ASSERT_EQ(frames.size(), 2U);
        EXPECT_EQ(sum(frames[0]), 2.0F);
        EXPECT_EQ(sum(frames[1]), 3.0F);

        const auto held = container->read(0);
        EXPECT_EQ(held.get(), container->read(0).get());
        EXPECT_TRUE(container->get_region_data(Region({ 1 }, { 3 })).empty());
    }

    TEST_F(VolumeSequenceTest, ARegionOverAMissingFrameCopiesNothing)
    {
        write_travelling_spark();
        const auto container = IO::open_volume_sequence(options(), uniform(1.0), LatticeSource::Union);
        ASSERT_NE(container, nullptr);

        std::filesystem::remove(path_in_cwd("spark.0001.vdb"));

        EXPECT_TRUE(container->get_region_data(Region({ 0 }, { 2 })).empty());
        EXPECT_EQ(container->get_region_data(Region({ 2 }, { 2 })).size(), 1U);
    }

}
}
