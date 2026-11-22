#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/io_roundtrip_support.hpp"
#include "../support/log_capture.hpp"
#include "../support/scoped_working_dir.hpp"

#include "MayaFlux/IO/Spatial/SpatialSequence.hpp"
#include "MayaFlux/Portal/Graphics/GraphicsUtils.hpp"

#ifdef MAYAFLUX_PLATFORM_WINDOWS
#include <windows.h>
#endif

#include <Alembic/Abc/All.h>
#include <Alembic/AbcGeom/All.h>
#if __has_include(<Alembic/AbcCoreHDF5/All.h>)
#include <Alembic/AbcCoreHDF5/All.h>
#define MAYAFLUX_TEST_HAS_ALEMBIC_HDF5 1
#endif

#ifdef MAYAFLUX_PLATFORM_WINDOWS
#ifdef ERROR
#undef ERROR
#endif
#endif

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {
namespace {

    using Portal::Graphics::PrimitiveTopology;
    using RoundTrip::path_in_cwd;
    using Scope = Kakshya::SpatialScope;

    struct Owned {
        PrimitiveTopology topology { PrimitiveTopology::POINT_LIST };
        std::vector<glm::vec3> positions;
        std::vector<int32_t> counts;
        std::vector<uint64_t> ids;
        std::vector<glm::vec3> velocities;
        std::vector<Kakshya::SpatialAttribute> attributes;

        [[nodiscard]] IO::SpatialSample view() const
        {
            return {
                .topology = topology,
                .positions = positions,
                .vertex_counts_per_curve = counts,
                .ids = ids,
                .velocities = velocities,
                .attributes = attributes
            };
        }
    };

    std::vector<glm::vec3> points(size_t count, float offset = 0.0F)
    {
        std::vector<glm::vec3> out;
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const auto f = static_cast<float>(i);
            out.emplace_back(f + offset, 0.5F * f, 0.25F - f);
        }
        return out;
    }

    std::vector<float> floats(size_t count, float scale = 1.0F)
    {
        std::vector<float> out(count);
        for (size_t i = 0; i < count; ++i) {
            out[i] = scale * static_cast<float>(i + 1);
        }
        return out;
    }

    template <class T>
    Kakshya::SpatialAttribute attribute(const std::string& name, Scope scope, std::vector<T> values)
    {
        return { .name = name, .scope = scope, .values = Kakshya::DataVariant { std::move(values) } };
    }

    const Kakshya::SpatialAttribute* find_attribute(const Kakshya::SpatialData& frame, const std::string& name)
    {
        for (const auto& candidate : frame.attributes) {
            if (candidate.name == name) {
                return &candidate;
            }
        }
        return nullptr;
    }

    Owned cloud(size_t count, float offset = 0.0F)
    {
        Owned owned;
        owned.positions = points(count, offset);
        owned.ids.resize(count);
        std::iota(owned.ids.begin(), owned.ids.end(), uint64_t { 100 });
        owned.velocities = points(count, offset + 5.0F);
        owned.attributes = { attribute<float>("mass", Scope::Varying, floats(count)) };
        return owned;
    }

    Owned strips(const std::vector<int32_t>& counts)
    {
        Owned owned;
        owned.topology = PrimitiveTopology::LINE_STRIP;
        owned.counts = counts;
        owned.positions = points(static_cast<size_t>(std::accumulate(counts.begin(), counts.end(), 0)));
        return owned;
    }

    constexpr std::array<size_t, 3> k_sizes { 4, 5, 6 };

    class SpatialSequenceTest : public ::testing::Test {
    protected:
        void SetUp() override
        {
            LogCapture::install();
            LogCapture::clear();
        }

        static bool write_all(const std::string& path, const std::function<bool(IO::SpatialCache&)>& body)
        {
            IO::SpatialCache cache;
            if (!cache.open(path)) {
                ADD_FAILURE() << cache.get_last_error();
                return false;
            }
            const bool ok = body(cache);
            cache.close();
            return ok;
        }

        std::string write_growing_cloud(const std::string& name)
        {
            const auto path = path_in_cwd(name);
            const bool written = write_all(path, [](IO::SpatialCache& cache) {
                for (size_t s = 0; s < k_sizes.size(); ++s) {
                    if (!cache.write("swarm", cloud(k_sizes[s], static_cast<float>(s)).view())) {
                        return false;
                    }
                }
                return true;
            });
            EXPECT_TRUE(written);
            return path;
        }

        ScopedWorkingDir m_files;
    };

    TEST_F(SpatialSequenceTest, ASampleBecomesDataThatOutlivesWhatItBorrowed)
    {
        auto source = cloud(4);
        const auto data = source.view().to_data();

        source.positions.assign(4, glm::vec3(9.0F));
        source.ids.clear();
        source.velocities.clear();
        source.attributes.clear();

        EXPECT_EQ(data.topology, PrimitiveTopology::POINT_LIST);
        EXPECT_EQ(data.positions, points(4));
        EXPECT_EQ(data.ids, (std::vector<uint64_t> { 100, 101, 102, 103 }));
        EXPECT_EQ(data.velocities, points(4, 5.0F));
        ASSERT_EQ(data.attributes.size(), 1U);
        EXPECT_EQ(data.attributes.front().name, "mass");
        EXPECT_EQ(std::get<std::vector<float>>(data.attributes.front().values), floats(4));
    }

    TEST_F(SpatialSequenceTest, AStreamIsServedFrameByFrameAndTimedByTheArchive)
    {
        const auto path = write_growing_cloud("swarm.abc");

        const auto frames = IO::SpatialFileFrames::create(path);

        ASSERT_NE(frames, nullptr);
        EXPECT_EQ(frames->count(), 3U);
        EXPECT_EQ(frames->stream(), "swarm");
        EXPECT_EQ(frames->topology(), PrimitiveTopology::POINT_LIST);
        for (size_t i = 0; i < k_sizes.size(); ++i) {
            const auto frame = frames->read(i);
            ASSERT_NE(frame, nullptr) << i;
            EXPECT_EQ(frame->positions, points(k_sizes[i], static_cast<float>(i))) << i;
            EXPECT_EQ(frame->ids.size(), k_sizes[i]) << i;
            EXPECT_EQ(frame->velocities.size(), k_sizes[i]) << i;
            EXPECT_TRUE(frame->vertex_counts_per_curve.empty()) << i;
            ASSERT_TRUE(frames->time_sampling().time(i).has_value()) << i;
            EXPECT_DOUBLE_EQ(*frames->time_sampling().time(i), static_cast<double>(i)) << i;
        }
        EXPECT_EQ(frames->time_sampling().floor_index(1.5, frames->count()), std::optional<size_t> { 1 });
    }

    TEST_F(SpatialSequenceTest, ACurvesStreamKeepsItsCountsAndHasNoIds)
    {
        const auto path = path_in_cwd("curves.abc");
        const auto source = strips({ 3, 4, 2 });
        ASSERT_TRUE(write_all(path, [&](IO::SpatialCache& cache) { return cache.write("web", source.view()); }));

        const auto frames = IO::SpatialFileFrames::create(path);

        ASSERT_NE(frames, nullptr);
        EXPECT_EQ(frames->topology(), PrimitiveTopology::LINE_STRIP);
        const auto frame = frames->read(0);
        ASSERT_NE(frame, nullptr);
        EXPECT_EQ(frame->topology, PrimitiveTopology::LINE_STRIP);
        EXPECT_EQ(frame->vertex_counts_per_curve, (std::vector<int32_t> { 3, 4, 2 }));
        EXPECT_EQ(frame->positions, source.positions);
        EXPECT_TRUE(frame->ids.empty());
        EXPECT_TRUE(frame->velocities.empty());
    }

    TEST_F(SpatialSequenceTest, TheLargestFrameOfAStreamIsFoundWithoutReadingAny)
    {
        const auto frames = IO::SpatialFileFrames::create(write_growing_cloud("swarm.abc"));
        ASSERT_NE(frames, nullptr);

        EXPECT_EQ(frames->max_vertex_count(), std::optional<uint32_t> { 6 });
        EXPECT_EQ(frames->max_vertex_count(), std::optional<uint32_t> { 6 });
    }

    TEST_F(SpatialSequenceTest, TheLargestFrameOfACurvesStreamCountsEveryCurve)
    {
        const auto path = path_in_cwd("curves_max.abc");
        ASSERT_TRUE(write_all(path, [&](IO::SpatialCache& cache) {
            return cache.write("web", strips({ 3, 4 }).view()) && cache.write("web", strips({ 5, 5, 5 }).view())
                && cache.write("web", strips({ 2 }).view());
        }));

        const auto frames = IO::SpatialFileFrames::create(path);

        ASSERT_NE(frames, nullptr);
        EXPECT_EQ(frames->max_vertex_count(), std::optional<uint32_t> { 15 });
    }

    TEST_F(SpatialSequenceTest, AStreamWithNoAttributesGivesFramesWithNone)
    {
        const auto path = path_in_cwd("bare.abc");
        Owned bare;
        bare.positions = points(3);
        ASSERT_TRUE(write_all(path, [&](IO::SpatialCache& cache) { return cache.write("bare", bare.view()); }));

        const auto frames = IO::SpatialFileFrames::create(path);

        ASSERT_NE(frames, nullptr);
        const auto frame = frames->read(0);
        ASSERT_NE(frame, nullptr);
        EXPECT_EQ(frame->positions, points(3));
        EXPECT_TRUE(frame->attributes.empty());
        EXPECT_TRUE(frame->ids.empty());
    }

    TEST_F(SpatialSequenceTest, ConstantAndUniformAttributesKeepTheirScope)
    {
        const auto path = path_in_cwd("scopes.abc");
        auto source = cloud(4);
        source.attributes = {
            attribute<float>("mass", Scope::Varying, floats(4)),
            attribute<float>("gravity", Scope::Constant, { 9.5F }),
            attribute<float>("group", Scope::Uniform, { 3.0F }),
        };
        ASSERT_TRUE(write_all(path, [&](IO::SpatialCache& cache) { return cache.write("scoped", source.view()); }));

        const auto frames = IO::SpatialFileFrames::create(path);

        ASSERT_NE(frames, nullptr);
        const auto frame = frames->read(0);
        ASSERT_NE(frame, nullptr);
        const auto* mass = find_attribute(*frame, "mass");
        const auto* gravity = find_attribute(*frame, "gravity");
        const auto* group = find_attribute(*frame, "group");
        ASSERT_NE(mass, nullptr);
        ASSERT_NE(gravity, nullptr);
        ASSERT_NE(group, nullptr);
        EXPECT_EQ(mass->scope, Scope::Varying);
        EXPECT_EQ(std::get<std::vector<float>>(mass->values), floats(4));
        EXPECT_EQ(gravity->scope, Scope::Constant);
        EXPECT_EQ(std::get<std::vector<float>>(gravity->values), std::vector<float> { 9.5F });
        EXPECT_EQ(group->scope, Scope::Uniform);
        EXPECT_EQ(std::get<std::vector<float>>(group->values), std::vector<float> { 3.0F });
    }

    TEST_F(SpatialSequenceTest, AContainerServesTheSameFramesAsItsSource)
    {
        const auto path = write_growing_cloud("container.abc");

        const auto container = IO::open_spatial_sequence(path);

        ASSERT_NE(container, nullptr);
        EXPECT_EQ(container->count(), 3U);
        EXPECT_EQ(container->get_num_frames(), 3U);
        for (size_t i = 0; i < k_sizes.size(); ++i) {
            const auto frame = container->read(i);
            ASSERT_NE(frame, nullptr) << i;
            EXPECT_EQ(frame->positions.size(), k_sizes[i]) << i;
        }
        EXPECT_EQ(container->read(3), nullptr);
    }

    TEST_F(SpatialSequenceTest, AFileWithSeveralStreamsNeedsOneNamed)
    {
        const auto path = path_in_cwd("streams.abc");
        const auto first = cloud(4);
        const auto second = cloud(7);
        ASSERT_TRUE(write_all(path, [&](IO::SpatialCache& cache) {
            return cache.write("first", first.view()) && cache.write("second", second.view());
        }));

        EXPECT_EQ(IO::SpatialFileFrames::create(path), nullptr);
        EXPECT_EQ(IO::SpatialFileFrames::create(path, "missing"), nullptr);

        const auto chosen = IO::SpatialFileFrames::create(path, "second");
        ASSERT_NE(chosen, nullptr);
        EXPECT_EQ(chosen->stream(), "second");
        ASSERT_NE(chosen->read(0), nullptr);
        EXPECT_EQ(chosen->read(0)->positions.size(), 7U);
    }

    TEST_F(SpatialSequenceTest, AFileThatCannotBeOpenedGivesNothingAndSaysWhy)
    {
        EXPECT_EQ(IO::SpatialFileFrames::create(path_in_cwd("absent.abc")), nullptr);

        const auto plain = path_in_cwd("plain.abc");
        std::ofstream(plain) << "not an archive";
        EXPECT_EQ(IO::SpatialFileFrames::create(plain), nullptr);
        EXPECT_EQ(IO::open_spatial_sequence(plain), nullptr);

        EXPECT_FALSE(LogCapture::messages().empty());
    }

    TEST_F(SpatialSequenceTest, ReadingAnIndexAgainSharesTheFrameWhileSomeoneHoldsIt)
    {
        const auto path = write_growing_cloud("shared.abc");
        const auto frames = IO::SpatialFileFrames::create(path);
        ASSERT_NE(frames, nullptr);

        auto held = frames->read(1);
        const auto again = frames->read(1);

        ASSERT_NE(held, nullptr);
        EXPECT_EQ(held, again);
        EXPECT_EQ(frames->read(3), nullptr);

        const auto expected = held->positions;
        held.reset();
        const auto reread = frames->read(1);
        ASSERT_NE(reread, nullptr);
        EXPECT_EQ(reread->positions, expected);
    }

    TEST_F(SpatialSequenceTest, SeveralThreadsReadDifferentFramesAndEachGetsItsOwn)
    {
        const auto path = write_growing_cloud("threads.abc");
        const auto frames = IO::SpatialFileFrames::create(path);
        ASSERT_NE(frames, nullptr);

        std::atomic<int> wrong { 0 };
        std::vector<std::thread> readers;
        readers.reserve(4);
        for (size_t t = 0; t < 4; ++t) {
            readers.emplace_back([&frames, &wrong, t] {
                for (size_t pass = 0; pass < 50; ++pass) {
                    const size_t index = (t + pass) % k_sizes.size();
                    const auto frame = frames->read(index);
                    if (!frame || frame->positions != points(k_sizes[index], static_cast<float>(index))) {
                        ++wrong;
                    }
                }
            });
        }
        for (auto& reader : readers) {
            reader.join();
        }

        EXPECT_EQ(wrong.load(), 0);
    }

#ifdef MAYAFLUX_TEST_HAS_ALEMBIC_HDF5
    TEST_F(SpatialSequenceTest, AnHdf5ArchiveIsServedLikeAnOgawaOne)
    {
        const auto path = path_in_cwd("cloud.abc");
        {
            using namespace Alembic::AbcGeom;
            OArchive archive(Alembic::AbcCoreHDF5::WriteArchive(), path);
            OPoints cloud_object(archive.getTop(), "cloud");
            for (size_t s = 0; s < 2; ++s) {
                const std::vector<V3f> positions(3 + s, V3f(static_cast<float>(s), 1.0F, 2.0F));
                const std::vector<uint64_t> ids(3 + s, 7);
                cloud_object.getSchema().set(OPointsSchema::Sample(
                    P3fArraySample(positions.data(), positions.size()),
                    UInt64ArraySample(ids.data(), ids.size())));
            }
        }

        const auto frames = IO::SpatialFileFrames::create(path, "cloud");

        ASSERT_NE(frames, nullptr);
        EXPECT_EQ(frames->count(), 2U);
        ASSERT_NE(frames->read(1), nullptr);
        EXPECT_EQ(frames->read(1)->positions.size(), 4U);
        EXPECT_EQ(frames->read(1)->ids, std::vector<uint64_t>(4, 7));
    }
#endif

} // namespace
} // namespace MayaFlux::Test
