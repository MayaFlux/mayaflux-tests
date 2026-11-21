#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/io_roundtrip_support.hpp"
#include "../support/log_capture.hpp"
#include "../support/scoped_working_dir.hpp"

#include "MayaFlux/IO/Spatial/SpatialExport.hpp"
#include "MayaFlux/IO/Spatial/SpatialReader.hpp"
#include "MayaFlux/IO/Spatial/SpatialTransfer.hpp"
#include "MayaFlux/Kakshya/NDData/VertexFormats.hpp"
#include "MayaFlux/Nodes/Network/Operators/PathOperator.hpp"
#include "MayaFlux/Nodes/Network/Operators/PhysicsOperator.hpp"
#include "MayaFlux/Nodes/Network/Operators/TopologyOperator.hpp"
#include "MayaFlux/Portal/Graphics/GraphicsUtils.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {
namespace {

    using Portal::Graphics::PrimitiveTopology;
    using RoundTrip::path_in_cwd;

    struct Owned {
        PrimitiveTopology topology { PrimitiveTopology::POINT_LIST };
        std::vector<glm::vec3> positions;
        std::vector<int32_t> counts;
        std::vector<uint64_t> ids;
        std::vector<glm::vec3> velocities;
        std::vector<IO::SpatialAttribute> attributes;

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

    glm::vec3 point_at(size_t i, float offset = 0.0F)
    {
        const auto f = static_cast<float>(i);
        return { f + offset, 0.5F * f, 0.25F - f };
    }

    std::vector<glm::vec3> points(size_t count, float offset = 0.0F)
    {
        std::vector<glm::vec3> out;
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            out.push_back(point_at(i, offset));
        }
        return out;
    }

    template <class T>
    IO::SpatialAttribute attribute(const std::string& name, IO::SpatialScope scope, std::vector<T> values)
    {
        return { .name = name, .scope = scope, .values = Kakshya::DataVariant { std::move(values) } };
    }

    std::vector<float> floats(size_t count, float scale = 1.0F)
    {
        std::vector<float> out(count);
        for (size_t i = 0; i < count; ++i) {
            out[i] = scale * static_cast<float>(i + 1);
        }
        return out;
    }

    const IO::SpatialAttribute* find_attribute(const IO::SpatialSample& sample, const std::string& name)
    {
        for (const auto& attribute : sample.attributes) {
            if (attribute.name == name) {
                return &attribute;
            }
        }
        return nullptr;
    }

    template <class T>
    void expect_attribute(
        const IO::SpatialSample& sample,
        const std::string& name,
        IO::SpatialScope scope,
        const std::vector<T>& expected)
    {
        const auto* found = find_attribute(sample, name);
        ASSERT_NE(found, nullptr) << name;
        EXPECT_EQ(found->scope, scope) << name;
        const auto* values = std::get_if<std::vector<T>>(&found->values);
        ASSERT_NE(values, nullptr) << name << " has another value type";
        EXPECT_EQ(*values, expected) << name;
    }

    template <class T>
    std::vector<T> to_vector(std::span<const T> view)
    {
        return std::vector<T>(view.begin(), view.end());
    }

    Owned point_cloud(size_t count)
    {
        Owned owned;
        owned.positions = points(count);
        owned.ids.resize(count);
        std::iota(owned.ids.begin(), owned.ids.end(), uint64_t { 100 });
        owned.velocities = points(count, 5.0F);
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

    class SpatialFileTest : public ::testing::Test {
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

        ScopedWorkingDir m_files;
    };

    TEST_F(SpatialFileTest, PointsRoundTripEveryAttributeKindAndScope)
    {
        auto source = point_cloud(6);
        using Scope = IO::SpatialScope;
        source.attributes = {
            attribute<float>("mass", Scope::Varying, floats(6)),
            attribute<glm::vec2>("uv", Scope::Varying, std::vector<glm::vec2>(6, glm::vec2(0.25F, 0.75F))),
            attribute<glm::vec3>("color", Scope::Varying, points(6, 1.0F)),
            attribute<glm::vec3>("normal", Scope::Varying, points(6, 2.0F)),
            attribute<glm::vec3>("dir", Scope::Varying, points(6, 3.0F)),
            attribute<glm::vec4>("tangent", Scope::Varying, std::vector<glm::vec4>(6, glm::vec4(1.0F, 2.0F, 3.0F, -1.0F))),
            attribute<uint32_t>("cluster", Scope::Varying, std::vector<uint32_t> { 0, 0, 1, 1, 2, 2 }),
            attribute<float>("gravity", Scope::Constant, { 9.5F }),
        };
        const auto path = path_in_cwd("points.abc");
        ASSERT_TRUE(write_all(path, [&](IO::SpatialCache& cache) { return cache.write("swarm", source.view()); }));

        IO::SpatialReader reader;
        ASSERT_TRUE(reader.open(path)) << reader.get_last_error();
        EXPECT_EQ(reader.get_stream_names(), std::vector<std::string> { "swarm" });
        EXPECT_EQ(reader.get_topology("swarm"), PrimitiveTopology::POINT_LIST);
        ASSERT_EQ(reader.get_sample_count("swarm"), 1U);

        const auto sample = reader.read_sample("swarm", 0);
        ASSERT_TRUE(sample.has_value()) << reader.get_last_error();
        EXPECT_EQ(sample->topology, PrimitiveTopology::POINT_LIST);
        EXPECT_EQ(to_vector(sample->positions), source.positions);
        EXPECT_EQ(to_vector(sample->ids), source.ids);
        EXPECT_EQ(to_vector(sample->velocities), source.velocities);
        EXPECT_TRUE(sample->vertex_counts_per_curve.empty());

        expect_attribute<float>(*sample, "mass", Scope::Varying, floats(6));
        expect_attribute<glm::vec2>(*sample, "uv", Scope::Varying, std::vector<glm::vec2>(6, glm::vec2(0.25F, 0.75F)));
        expect_attribute<glm::vec3>(*sample, "color", Scope::Varying, points(6, 1.0F));
        expect_attribute<glm::vec3>(*sample, "normal", Scope::Varying, points(6, 2.0F));
        expect_attribute<glm::vec3>(*sample, "dir", Scope::Varying, points(6, 3.0F));
        expect_attribute<glm::vec4>(*sample, "tangent", Scope::Varying, std::vector<glm::vec4>(6, glm::vec4(1.0F, 2.0F, 3.0F, -1.0F)));
        expect_attribute<uint32_t>(*sample, "cluster", Scope::Varying, std::vector<uint32_t> { 0, 0, 1, 1, 2, 2 });
        expect_attribute<float>(*sample, "gravity", Scope::Constant, { 9.5F });
    }

    TEST_F(SpatialFileTest, AVec4NamedColorIsAnRgbaColorAttribute)
    {
        Owned source;
        source.positions = points(4);
        source.attributes = { attribute<glm::vec4>("color", IO::SpatialScope::Varying,
            std::vector<glm::vec4> { { 1, 0, 0, 1 }, { 0, 1, 0, 0.5F }, { 0, 0, 1, 0.25F }, { 1, 1, 1, 0 } }) };
        const auto path = path_in_cwd("rgba.abc");
        ASSERT_TRUE(write_all(path, [&](IO::SpatialCache& cache) { return cache.write("tinted", source.view()); }));

        IO::SpatialReader reader;
        ASSERT_TRUE(reader.open(path));
        const auto sample = reader.read_sample("tinted", 0);

        ASSERT_TRUE(sample.has_value()) << reader.get_last_error();
        expect_attribute<glm::vec4>(*sample, "color", IO::SpatialScope::Varying,
            std::vector<glm::vec4> { { 1, 0, 0, 1 }, { 0, 1, 0, 0.5F }, { 0, 0, 1, 0.25F }, { 1, 1, 1, 0 } });
    }

    TEST_F(SpatialFileTest, PointsWrittenWithoutIdsOrVelocitiesReadBackWithoutThem)
    {
        Owned source;
        source.positions = points(5);
        const auto path = path_in_cwd("bare.abc");
        ASSERT_TRUE(write_all(path, [&](IO::SpatialCache& cache) { return cache.write("bare", source.view()); }));

        IO::SpatialReader reader;
        ASSERT_TRUE(reader.open(path));
        const auto sample = reader.read_sample("bare", 0);

        ASSERT_TRUE(sample.has_value());
        EXPECT_EQ(to_vector(sample->positions), source.positions);
        EXPECT_TRUE(sample->ids.empty());
        EXPECT_TRUE(sample->velocities.empty());
        EXPECT_TRUE(sample->attributes.empty());
    }

    TEST_F(SpatialFileTest, AUniformAttributeOnPointsReadsBackAsUniform)
    {
        auto source = point_cloud(4);
        source.attributes = { attribute<float>("group", IO::SpatialScope::Uniform, { 3.0F }) };
        const auto path = path_in_cwd("uniform_points.abc");
        ASSERT_TRUE(write_all(path, [&](IO::SpatialCache& cache) { return cache.write("p", source.view()); }));

        IO::SpatialReader reader;
        ASSERT_TRUE(reader.open(path));
        const auto sample = reader.read_sample("p", 0);

        ASSERT_TRUE(sample.has_value()) << reader.get_last_error();
        const auto* found = find_attribute(*sample, "group");
        ASSERT_NE(found, nullptr);
        EXPECT_EQ(found->scope, IO::SpatialScope::Uniform);
    }

    TEST_F(SpatialFileTest, CurvesRoundTripCountsAndPerCurveAttributes)
    {
        auto source = strips({ 3, 4 });
        using Scope = IO::SpatialScope;
        source.attributes = {
            attribute<float>("width", Scope::Uniform, { 0.5F, 1.5F }),
            attribute<glm::vec3>("color", Scope::Varying, points(7, 1.0F)),
            attribute<float>("tag", Scope::Constant, { 4.0F }),
        };
        const auto path = path_in_cwd("curves.abc");
        ASSERT_TRUE(write_all(path, [&](IO::SpatialCache& cache) { return cache.write("roots", source.view()); }));

        IO::SpatialReader reader;
        ASSERT_TRUE(reader.open(path)) << reader.get_last_error();
        EXPECT_EQ(reader.get_topology("roots"), PrimitiveTopology::LINE_STRIP);

        const auto sample = reader.read_sample("roots", 0);
        ASSERT_TRUE(sample.has_value()) << reader.get_last_error();
        EXPECT_EQ(sample->topology, PrimitiveTopology::LINE_STRIP);
        EXPECT_EQ(to_vector(sample->positions), source.positions);
        EXPECT_EQ(to_vector(sample->vertex_counts_per_curve), source.counts);
        EXPECT_TRUE(sample->ids.empty());
        EXPECT_TRUE(sample->velocities.empty());
        expect_attribute<float>(*sample, "width", Scope::Uniform, { 0.5F, 1.5F });
        expect_attribute<glm::vec3>(*sample, "color", Scope::Varying, points(7, 1.0F));
        expect_attribute<float>(*sample, "tag", Scope::Constant, { 4.0F });
    }

    TEST_F(SpatialFileTest, ALineListIsReadBackAsLineStrips)
    {
        auto source = strips({ 2, 2, 2 });
        source.topology = PrimitiveTopology::LINE_LIST;
        const auto path = path_in_cwd("list.abc");
        ASSERT_TRUE(write_all(path, [&](IO::SpatialCache& cache) { return cache.write("edges", source.view()); }));

        IO::SpatialReader reader;
        ASSERT_TRUE(reader.open(path));

        EXPECT_EQ(reader.get_topology("edges"), PrimitiveTopology::LINE_STRIP);
        const auto sample = reader.read_sample("edges", 0);
        ASSERT_TRUE(sample.has_value());
        EXPECT_EQ(to_vector(sample->vertex_counts_per_curve), source.counts);
        EXPECT_EQ(to_vector(sample->positions), source.positions);
    }

    TEST_F(SpatialFileTest, SamplesAppendInOrderAndMayChangeSize)
    {
        const auto path = path_in_cwd("grow.abc");
        const std::array<size_t, 3> sizes { 4, 6, 5 };
        ASSERT_TRUE(write_all(path, [&](IO::SpatialCache& cache) {
            for (size_t n = 0; n < sizes.size(); ++n) {
                Owned sample;
                sample.positions = points(sizes[n], static_cast<float>(n) * 10.0F);
                sample.attributes = { attribute<float>("mass", IO::SpatialScope::Varying, floats(sizes[n], static_cast<float>(n + 1))) };
                if (!cache.write("grow", sample.view())) {
                    return false;
                }
            }
            return true;
        }));

        IO::SpatialReader reader;
        ASSERT_TRUE(reader.open(path));
        ASSERT_EQ(reader.get_sample_count("grow"), 3U);

        double previous_time = -1.0;
        for (size_t n = 0; n < sizes.size(); ++n) {
            const auto sample = reader.read_sample("grow", n);
            ASSERT_TRUE(sample.has_value()) << n;
            EXPECT_EQ(to_vector(sample->positions), points(sizes[n], static_cast<float>(n) * 10.0F)) << n;
            expect_attribute<float>(*sample, "mass", IO::SpatialScope::Varying, floats(sizes[n], static_cast<float>(n + 1)));

            const auto time = reader.sample_time("grow", n);
            ASSERT_TRUE(time.has_value()) << n;
            EXPECT_GT(*time, previous_time) << n;
            previous_time = *time;
        }
    }

    TEST_F(SpatialFileTest, RereadingASampleGivesTheSameValues)
    {
        auto source = point_cloud(5);
        const auto path = path_in_cwd("again.abc");
        ASSERT_TRUE(write_all(path, [&](IO::SpatialCache& cache) { return cache.write("p", source.view()); }));

        IO::SpatialReader reader;
        ASSERT_TRUE(reader.open(path));
        const auto first = reader.read_sample("p", 0);
        ASSERT_TRUE(first.has_value());
        const auto first_positions = to_vector(first->positions);
        const auto second = reader.read_sample("p", 0);

        ASSERT_TRUE(second.has_value());
        EXPECT_EQ(to_vector(second->positions), first_positions);
    }

    TEST_F(SpatialFileTest, StreamsKeepTheirOwnSampleCountsAndNamesInCreationOrder)
    {
        const auto path = path_in_cwd("streams.abc");
        const auto cloud = point_cloud(4);
        const auto bonds = strips({ 2, 2 });
        ASSERT_TRUE(write_all(path, [&](IO::SpatialCache& cache) {
            return cache.write("particles", cloud.view())
                && cache.write("bonds", bonds.view())
                && cache.write("particles", cloud.view())
                && cache.write("particles", cloud.view());
        }));

        IO::SpatialReader reader;
        ASSERT_TRUE(reader.open(path));

        EXPECT_EQ(reader.get_stream_names(), (std::vector<std::string> { "particles", "bonds" }));
        EXPECT_EQ(reader.get_sample_count("particles"), 3U);
        EXPECT_EQ(reader.get_sample_count("bonds"), 1U);
        EXPECT_EQ(reader.get_topology("particles"), PrimitiveTopology::POINT_LIST);
        EXPECT_EQ(reader.get_topology("bonds"), PrimitiveTopology::LINE_STRIP);
    }

    TEST_F(SpatialFileTest, ReadingAnotherStreamDoesNotInvalidateASampleOfThisOne)
    {
        const auto path = path_in_cwd("borrow.abc");
        const auto cloud = point_cloud(4);
        const auto bonds = strips({ 2, 2 });
        ASSERT_TRUE(write_all(path, [&](IO::SpatialCache& cache) {
            return cache.write("particles", cloud.view()) && cache.write("bonds", bonds.view());
        }));

        IO::SpatialReader reader;
        ASSERT_TRUE(reader.open(path));
        const auto particles = reader.read_sample("particles", 0);
        ASSERT_TRUE(particles.has_value());
        const auto bond_sample = reader.read_sample("bonds", 0);
        ASSERT_TRUE(bond_sample.has_value());

        EXPECT_EQ(to_vector(particles->positions), cloud.positions);
        EXPECT_EQ(to_vector(bond_sample->positions), bonds.positions);
    }

    TEST_F(SpatialFileTest, MetadataQueuedBeforeTheFirstWriteReachesTheStream)
    {
        const auto path = path_in_cwd("meta.abc");
        const auto cloud = point_cloud(3);
        const auto bonds = strips({ 3 });
        ASSERT_TRUE(write_all(path, [&](IO::SpatialCache& cache) {
            return cache.write_metadata("particles", { { "operator", "PhysicsOperator" }, { "gravity", "9.8" } })
                && cache.write_metadata("bonds", { { "rule", "knn" } })
                && cache.write("particles", cloud.view())
                && cache.write("bonds", bonds.view());
        }));

        IO::SpatialReader reader;
        ASSERT_TRUE(reader.open(path));

        const auto particles = reader.get_metadata("particles");
        EXPECT_EQ(particles.at("operator"), "PhysicsOperator");
        EXPECT_EQ(particles.at("gravity"), "9.8");
        EXPECT_FALSE(particles.contains("schema"));
        EXPECT_EQ(reader.get_metadata("bonds").at("rule"), "knn");
        EXPECT_TRUE(reader.get_metadata("unknown").empty());
    }

    TEST_F(SpatialFileTest, MetadataAfterTheFirstWriteIsRejectedAndNotStored)
    {
        const auto path = path_in_cwd("late_meta.abc");
        const auto cloud = point_cloud(3);
        ASSERT_TRUE(write_all(path, [&](IO::SpatialCache& cache) {
            if (!cache.write("p", cloud.view())) {
                return false;
            }
            EXPECT_FALSE(cache.write_metadata("p", { { "late", "1" } }));
            EXPECT_NE(cache.get_last_error().find("already created"), std::string::npos);
            return true;
        }));

        IO::SpatialReader reader;
        ASSERT_TRUE(reader.open(path));
        EXPECT_FALSE(reader.get_metadata("p").contains("late"));
    }

    TEST_F(SpatialFileTest, AnArchiveWithNoStreamsOpensEmpty)
    {
        const auto path = path_in_cwd("empty.abc");
        ASSERT_TRUE(write_all(path, [](IO::SpatialCache&) { return true; }));

        IO::SpatialReader reader;
        ASSERT_TRUE(reader.open(path)) << reader.get_last_error();
        EXPECT_TRUE(reader.is_open());
        EXPECT_TRUE(reader.get_stream_names().empty());
    }

    TEST_F(SpatialFileTest, ReaderQueriesAndErrorsForUnknownStreamsAndIndices)
    {
        const auto path = path_in_cwd("queries.abc");
        const auto cloud = point_cloud(3);
        ASSERT_TRUE(write_all(path, [&](IO::SpatialCache& cache) { return cache.write("p", cloud.view()); }));

        IO::SpatialReader reader;
        ASSERT_TRUE(reader.open(path));

        EXPECT_FALSE(reader.get_topology("nope").has_value());
        EXPECT_EQ(reader.get_sample_count("nope"), 0U);
        EXPECT_FALSE(reader.sample_time("nope", 0).has_value());
        EXPECT_FALSE(reader.sample_time("p", 1).has_value());

        EXPECT_FALSE(reader.read_sample("nope", 0).has_value());
        EXPECT_NE(reader.get_last_error().find("no stream named"), std::string::npos);
        EXPECT_FALSE(reader.read_sample("p", 1).has_value());
        EXPECT_NE(reader.get_last_error().find("out of range"), std::string::npos);
    }

    TEST_F(SpatialFileTest, ClosingTheReaderEndsAccess)
    {
        const auto path = path_in_cwd("closing.abc");
        const auto cloud = point_cloud(3);
        ASSERT_TRUE(write_all(path, [&](IO::SpatialCache& cache) { return cache.write("p", cloud.view()); }));

        IO::SpatialReader reader;
        reader.close();
        EXPECT_FALSE(reader.is_open());
        ASSERT_TRUE(reader.open(path));
        reader.close();

        EXPECT_FALSE(reader.is_open());
        EXPECT_TRUE(reader.get_stream_names().empty());
        EXPECT_FALSE(reader.read_sample("p", 0).has_value());
        EXPECT_NE(reader.get_last_error().find("no archive open"), std::string::npos);
    }

    TEST_F(SpatialFileTest, OpeningASecondArchiveReplacesTheFirst)
    {
        const auto cloud = point_cloud(3);
        ASSERT_TRUE(write_all(path_in_cwd("one.abc"), [&](IO::SpatialCache& cache) { return cache.write("first", cloud.view()); }));
        ASSERT_TRUE(write_all(path_in_cwd("two.abc"), [&](IO::SpatialCache& cache) { return cache.write("second", cloud.view()); }));

        IO::SpatialReader reader;
        ASSERT_TRUE(reader.open(path_in_cwd("one.abc")));
        ASSERT_TRUE(reader.open(path_in_cwd("two.abc")));

        EXPECT_EQ(reader.get_stream_names(), std::vector<std::string> { "second" });
    }

    TEST_F(SpatialFileTest, AMovedReaderKeepsTheArchiveOpen)
    {
        const auto path = path_in_cwd("moved.abc");
        const auto cloud = point_cloud(3);
        ASSERT_TRUE(write_all(path, [&](IO::SpatialCache& cache) { return cache.write("p", cloud.view()); }));

        IO::SpatialReader reader;
        ASSERT_TRUE(reader.open(path));
        IO::SpatialReader moved(std::move(reader));

        EXPECT_TRUE(moved.is_open());
        EXPECT_TRUE(moved.read_sample("p", 0).has_value());
    }

    TEST_F(SpatialFileTest, WritingBeforeOpenOrAfterCloseFails)
    {
        const auto cloud = point_cloud(3);
        IO::SpatialCache cache;

        EXPECT_FALSE(cache.write("p", cloud.view()));
        EXPECT_NE(cache.get_last_error().find("archive not open"), std::string::npos);
        cache.close();

        ASSERT_TRUE(cache.open(path_in_cwd("closed.abc")));
        EXPECT_TRUE(cache.write("p", cloud.view()));
        cache.close();
        cache.close();
        EXPECT_FALSE(cache.write("p", cloud.view()));
    }

    TEST_F(SpatialFileTest, OpeningInAMissingDirectoryFails)
    {
        IO::SpatialCache cache;

        EXPECT_FALSE(cache.open(path_in_cwd("no/such/dir/cache.abc")));
        EXPECT_NE(cache.get_last_error().find("open:"), std::string::npos);
    }

    TEST_F(SpatialFileTest, SamplesThatBreakTheTopologyRulesAreRejected)
    {
        IO::SpatialCache cache;
        ASSERT_TRUE(cache.open(path_in_cwd("rules.abc")));

        Owned empty;
        EXPECT_FALSE(cache.write("empty", empty.view()));
        EXPECT_NE(cache.get_last_error().find("no positions"), std::string::npos);

        auto curve_with_ids = strips({ 3 });
        curve_with_ids.ids = { 1, 2, 3 };
        EXPECT_FALSE(cache.write("curve_ids", curve_with_ids.view()));
        EXPECT_NE(cache.get_last_error().find("ids/velocities"), std::string::npos);

        auto curve_without_counts = strips({ 3 });
        curve_without_counts.counts.clear();
        EXPECT_FALSE(cache.write("curve_counts", curve_without_counts.view()));
        EXPECT_NE(cache.get_last_error().find("vertex_counts_per_curve"), std::string::npos);

        auto curve_wrong_sum = strips({ 3 });
        curve_wrong_sum.counts = { 2 };
        EXPECT_FALSE(cache.write("curve_sum", curve_wrong_sum.view()));
        EXPECT_NE(cache.get_last_error().find("does not match"), std::string::npos);

        auto points_with_counts = point_cloud(3);
        points_with_counts.counts = { 3 };
        EXPECT_FALSE(cache.write("points_counts", points_with_counts.view()));
        EXPECT_NE(cache.get_last_error().find("POINT_LIST"), std::string::npos);

        auto triangles = point_cloud(3);
        triangles.topology = PrimitiveTopology::TRIANGLE_LIST;
        EXPECT_FALSE(cache.write("triangles", triangles.view()));
        EXPECT_NE(cache.get_last_error().find("unsupported topology"), std::string::npos);
    }

    TEST_F(SpatialFileTest, AttributesWithTheWrongCountOrTypeAreRejected)
    {
        IO::SpatialCache cache;
        ASSERT_TRUE(cache.open(path_in_cwd("attributes.abc")));

        auto short_attribute = point_cloud(4);
        short_attribute.attributes = { attribute<float>("mass", IO::SpatialScope::Varying, floats(3)) };
        EXPECT_FALSE(cache.write("short", short_attribute.view()));
        EXPECT_NE(cache.get_last_error().find("element count"), std::string::npos);

        auto wide_constant = point_cloud(4);
        wide_constant.attributes = { attribute<float>("g", IO::SpatialScope::Constant, floats(2)) };
        EXPECT_FALSE(cache.write("wide", wide_constant.view()));

        auto doubles = point_cloud(4);
        doubles.attributes = { attribute<double>("precise", IO::SpatialScope::Varying, std::vector<double>(4, 1.0)) };
        EXPECT_FALSE(cache.write("doubles", doubles.view()));

        auto first = point_cloud(4);
        first.attributes = { attribute<float>("m", IO::SpatialScope::Varying, floats(4)) };
        ASSERT_TRUE(cache.write("redeclared", first.view()));
        auto second = point_cloud(4);
        second.attributes = { attribute<glm::vec3>("m", IO::SpatialScope::Varying, points(4)) };
        EXPECT_FALSE(cache.write("redeclared", second.view()));
    }

    TEST_F(SpatialFileTest, ChangingATopologyOnAnExistingStreamNameFailsOrThrows)
    {
        IO::SpatialCache cache;
        ASSERT_TRUE(cache.open(path_in_cwd("switch.abc")));
        ASSERT_TRUE(cache.write("s", point_cloud(3).view()));

        bool threw = false;
        bool accepted = true;
        try {
            accepted = cache.write("s", strips({ 3 }).view());
        } catch (const std::exception&) {
            threw = true;
        }

        EXPECT_TRUE(threw || !accepted);
    }

    TEST_F(SpatialFileTest, MissingFileFailsToOpen)
    {
        IO::SpatialReader reader;

        EXPECT_FALSE(reader.open(path_in_cwd("absent.abc")));
        EXPECT_FALSE(reader.is_open());
        EXPECT_NE(reader.get_last_error().find("no such file"), std::string::npos);
    }

    TEST_F(SpatialFileTest, FilesThatAreNotArchivesFailToOpen)
    {
        IO::SpatialReader reader;

        RoundTrip::write_bytes(path_in_cwd("text.abc"), "not an archive\n");
        EXPECT_FALSE(reader.open(path_in_cwd("text.abc")));
        EXPECT_NE(reader.get_last_error().find("not an Alembic archive"), std::string::npos);

        RoundTrip::write_bytes(path_in_cwd("empty.abc"), "");
        EXPECT_FALSE(reader.open(path_in_cwd("empty.abc")));

        RoundTrip::write_bytes(path_in_cwd("garbage.abc"), RoundTrip::garbage(4096));
        EXPECT_FALSE(reader.open(path_in_cwd("garbage.abc")));
    }

    TEST_F(SpatialFileTest, FilesWithAnArchiveSignatureButNoArchiveFailToOpen)
    {
        IO::SpatialReader reader;

        RoundTrip::write_bytes(path_in_cwd("ogawa.abc"), RoundTrip::with_prefix("Ogawa", 512, '\0'));
        EXPECT_FALSE(reader.open(path_in_cwd("ogawa.abc")));
        EXPECT_FALSE(reader.is_open());

        RoundTrip::write_bytes(path_in_cwd("hdf5.abc"), RoundTrip::with_prefix(std::string("\x89HDF\r\n\x1a\n", 8), 512, '\0'));
        EXPECT_FALSE(reader.open(path_in_cwd("hdf5.abc")));
        EXPECT_FALSE(reader.is_open());
    }

    TEST_F(SpatialFileTest, ATruncatedArchiveFailsToOpen)
    {
        const auto good = path_in_cwd("good.abc");
        const auto cloud = point_cloud(500);
        ASSERT_TRUE(write_all(good, [&](IO::SpatialCache& cache) { return cache.write("p", cloud.view()); }));
        const auto bytes = RoundTrip::read_bytes(good);
        ASSERT_GT(bytes.size(), 64U);

        const auto cut = path_in_cwd("cut.abc");
        RoundTrip::write_bytes(cut, RoundTrip::truncated(bytes, bytes.size() / 2));

        IO::SpatialReader reader;
        EXPECT_FALSE(reader.open(cut));
    }

    std::vector<std::string> files_matching(const std::string& prefix, const std::string& suffix)
    {
        std::vector<std::string> found;
        for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::current_path())) {
            const auto name = entry.path().filename().string();
            if (name.starts_with(prefix) && name.ends_with(suffix)) {
                found.push_back(entry.path().string());
            }
        }
        return found;
    }

    TEST_F(SpatialFileTest, SnapshotWritesEverySourceIntoOneNumberedArchive)
    {
        const auto cloud = std::make_shared<Owned>(point_cloud(4));
        const auto bonds = std::make_shared<Owned>(strips({ 2, 2 }));
        const std::vector<IO::SpatialCaptureSource> sources {
            { .stream_name = "particles", .capture_frame = [cloud](IO::SpatialCache& cache, const std::string& name) { return cache.write(name, cloud->view()); } },
            { .stream_name = "bonds", .capture_frame = [bonds](IO::SpatialCache& cache, const std::string& name) { return cache.write(name, bonds->view()); } },
        };

        ASSERT_TRUE(IO::save_spatial_snapshot(path_in_cwd("snap.{}.abc"), sources));

        const auto written = files_matching("snap.", ".abc");
        ASSERT_EQ(written.size(), 1U);
        IO::SpatialReader reader;
        ASSERT_TRUE(reader.open(written.front()));
        EXPECT_EQ(reader.get_stream_names(), (std::vector<std::string> { "particles", "bonds" }));
        EXPECT_EQ(reader.get_sample_count("particles"), 1U);
        EXPECT_EQ(reader.get_sample_count("bonds"), 1U);
    }

    TEST_F(SpatialFileTest, SnapshotWithoutSourcesWritesNothing)
    {
        EXPECT_FALSE(IO::save_spatial_snapshot(path_in_cwd("none.{}.abc"), {}));
        EXPECT_TRUE(files_matching("none.", ".abc").empty());
    }

    TEST_F(SpatialFileTest, SnapshotFailsWhenASourceFails)
    {
        const std::vector<IO::SpatialCaptureSource> sources {
            { .stream_name = "broken", .capture_frame = [](IO::SpatialCache&, const std::string&) { return false; } },
        };

        EXPECT_FALSE(IO::save_spatial_snapshot(path_in_cwd("broken.{}.abc"), sources));
    }

    TEST_F(SpatialFileTest, SnapshotPatternWithTwoFieldsThrows)
    {
        const std::vector<IO::SpatialCaptureSource> sources {
            { .stream_name = "p", .capture_frame = [](IO::SpatialCache&, const std::string&) { return true; } },
        };

        EXPECT_THROW(static_cast<void>(IO::save_spatial_snapshot(path_in_cwd("two.{}.{}.abc"), sources)), std::format_error);
    }

    using Nodes::LineVertex;
    using Nodes::PointVertex;

    std::vector<LineVertex> zigzag(uint32_t count, float offset)
    {
        std::vector<LineVertex> pts;
        pts.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            LineVertex v;
            v.position = { static_cast<float>(i) + offset, (i % 2 == 0) ? 0.0F : 1.0F, offset };
            pts.push_back(v);
        }
        return pts;
    }

    std::vector<PointVertex> scatter(uint32_t count, float offset)
    {
        std::vector<PointVertex> pts;
        pts.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            PointVertex v;
            v.position = { static_cast<float>(i) + offset, offset, 0.0F };
            pts.push_back(v);
        }
        return pts;
    }

    std::vector<glm::vec3> operator_positions(const Nodes::Network::GraphicsOperator& op)
    {
        const auto bytes = op.get_vertex_data();
        const auto stride = op.get_vertex_layout().stride_bytes;
        std::vector<glm::vec3> out(op.get_vertex_count());
        for (size_t i = 0; i < out.size(); ++i) {
            std::memcpy(&out[i], bytes.data() + i * stride, sizeof(glm::vec3));
        }
        return out;
    }

    void expect_vertex_columns(const IO::SpatialSample& sample)
    {
        for (const auto* name : { "color", "scalar", "uv", "normal", "tangent", "cluster" }) {
            const auto* found = find_attribute(sample, name);
            ASSERT_NE(found, nullptr) << name;
            EXPECT_EQ(found->scope, IO::SpatialScope::Varying) << name;
        }
    }

    TEST_F(SpatialFileTest, PathOperatorIsWrittenAsOneStripPerPath)
    {
        Nodes::Network::PathOperator op;
        op.initialize_paths({ zigzag(6, 0.0F), zigzag(8, 10.0F) }, Kinesis::InterpolationMode::CATMULL_ROM);
        const auto path = path_in_cwd("path.abc");
        ASSERT_TRUE(write_all(path, [&](IO::SpatialCache& cache) { return IO::write_operator_sample(cache, "paths", &op); }));

        IO::SpatialReader reader;
        ASSERT_TRUE(reader.open(path));
        const auto sample = reader.read_sample("paths", 0);

        ASSERT_TRUE(sample.has_value()) << reader.get_last_error();
        EXPECT_EQ(sample->topology, PrimitiveTopology::LINE_STRIP);
        EXPECT_EQ(sample->vertex_counts_per_curve.size(), 2U);
        EXPECT_EQ(to_vector(sample->positions), operator_positions(op));
        expect_vertex_columns(*sample);
    }

    TEST_F(SpatialFileTest, TopologyOperatorIsWrittenAsTwoVertexCurves)
    {
        Nodes::Network::TopologyOperator op;
        op.initialize_topologies({ zigzag(6, 0.0F), zigzag(8, 10.0F) }, Kinesis::ProximityMode::K_NEAREST);
        const auto path = path_in_cwd("topology.abc");
        ASSERT_TRUE(write_all(path, [&](IO::SpatialCache& cache) { return IO::write_operator_sample(cache, "graph", &op); }));

        IO::SpatialReader reader;
        ASSERT_TRUE(reader.open(path));
        const auto sample = reader.read_sample("graph", 0);

        ASSERT_TRUE(sample.has_value()) << reader.get_last_error();
        const auto counts = to_vector(sample->vertex_counts_per_curve);
        EXPECT_FALSE(counts.empty());
        EXPECT_TRUE(std::ranges::all_of(counts, [](int32_t n) { return n == 2; }));
        EXPECT_EQ(counts.size() * 2, sample->positions.size());
        EXPECT_EQ(to_vector(sample->positions), operator_positions(op));
    }

    TEST_F(SpatialFileTest, PhysicsOperatorIsWrittenAsPointsWithIndexIdsAndClusters)
    {
        Nodes::Network::PhysicsOperator op;
        op.initialize_collections({ scatter(5, 0.0F), scatter(7, 10.0F) });
        const auto path = path_in_cwd("physics.abc");
        ASSERT_TRUE(write_all(path, [&](IO::SpatialCache& cache) { return IO::write_operator_sample(cache, "bodies", &op); }));

        IO::SpatialReader reader;
        ASSERT_TRUE(reader.open(path));
        const auto sample = reader.read_sample("bodies", 0);

        ASSERT_TRUE(sample.has_value()) << reader.get_last_error();
        EXPECT_EQ(sample->topology, PrimitiveTopology::POINT_LIST);
        EXPECT_EQ(to_vector(sample->positions), operator_positions(op));
        std::vector<uint64_t> expected_ids(12);
        std::iota(expected_ids.begin(), expected_ids.end(), uint64_t { 0 });
        EXPECT_EQ(to_vector(sample->ids), expected_ids);
        expect_vertex_columns(*sample);

        const auto* cluster = std::get_if<std::vector<uint32_t>>(&find_attribute(*sample, "cluster")->values);
        ASSERT_NE(cluster, nullptr);
        EXPECT_EQ(std::set<uint32_t>(cluster->begin(), cluster->end()).size(), 2U);
    }

    TEST_F(SpatialFileTest, OperatorSamplesAppendToTheSameStream)
    {
        Nodes::Network::PhysicsOperator op;
        op.initialize_collections({ scatter(5, 0.0F) });
        const auto path = path_in_cwd("physics_frames.abc");
        ASSERT_TRUE(write_all(path, [&](IO::SpatialCache& cache) {
            return IO::write_operator_sample(cache, "bodies", &op)
                && IO::write_operator_sample(cache, "bodies", &op);
        }));

        IO::SpatialReader reader;
        ASSERT_TRUE(reader.open(path));
        EXPECT_EQ(reader.get_sample_count("bodies"), 2U);
    }

    TEST_F(SpatialFileTest, NullAndEmptyOperatorsAreRejected)
    {
        IO::SpatialCache cache;
        ASSERT_TRUE(cache.open(path_in_cwd("rejected.abc")));
        Nodes::Network::PathOperator empty;

        EXPECT_FALSE(IO::write_operator_sample(cache, "null", nullptr));
        LogCapture::clear();
        EXPECT_FALSE(IO::write_operator_sample(cache, "empty", &empty));
        EXPECT_TRUE(LogCapture::contains("zero vertices"));
    }

    TEST_F(SpatialFileTest, SnapshotOfAnOperatorSourceRoundTripsThroughTheCache)
    {
        auto op = std::make_shared<Nodes::Network::PhysicsOperator>();
        op->initialize_collections({ scatter(5, 0.0F) });
        const std::vector<IO::SpatialCaptureSource> sources {
            { .stream_name = "bodies", .capture_frame = [op](IO::SpatialCache& cache, const std::string& name) { return IO::write_operator_sample(cache, name, op.get()); } },
        };

        ASSERT_TRUE(IO::save_spatial_snapshot(path_in_cwd("bodies.{}.abc"), sources));

        const auto written = files_matching("bodies.", ".abc");
        ASSERT_EQ(written.size(), 1U);
        IO::SpatialReader reader;
        ASSERT_TRUE(reader.open(written.front()));
        const auto sample = reader.read_sample("bodies", 0);
        ASSERT_TRUE(sample.has_value());
        EXPECT_EQ(to_vector(sample->positions), operator_positions(*op));
    }

    MAYAFLUX_TEST_WITH(TEST, SpatialAssetTest, RealHdf5ArchiveOpensAndEveryStreamReadsItsFirstSamples,
        (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::repo_assets>() }))
    {
        const auto asset = RoundTrip::repo_asset("res/spatial_hdf5_sample.abc");
        if (!asset) {
            GTEST_SKIP() << "res/spatial_hdf5_sample.abc is not present";
        }

        IO::SpatialReader reader;
        ASSERT_TRUE(reader.open(*asset)) << reader.get_last_error();
        ASSERT_FALSE(reader.get_stream_names().empty());

        for (const auto& name : reader.get_stream_names()) {
            const auto count = reader.get_sample_count(name);
            ASSERT_GT(count, 0U) << name;
            ASSERT_TRUE(reader.get_topology(name).has_value()) << name;
            for (size_t i = 0; i < std::min<size_t>(count, 5); ++i) {
                const auto sample = reader.read_sample(name, i);
                ASSERT_TRUE(sample.has_value()) << name << ' ' << i << ' ' << reader.get_last_error();
                EXPECT_FALSE(sample->positions.empty()) << name << ' ' << i;
                EXPECT_TRUE(reader.sample_time(name, i).has_value()) << name << ' ' << i;
                for (const auto& position : sample->positions) {
                    ASSERT_TRUE(std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z));
                }
            }
        }
    }

    MAYAFLUX_TEST_WITH(TEST, SpatialAssetTest, RealHdf5ArchiveRewritesToOgawaWithTheSameFirstSample,
        (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::repo_assets, ::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        const auto asset = RoundTrip::repo_asset("res/spatial_hdf5_sample.abc");
        if (!asset) {
            GTEST_SKIP() << "res/spatial_hdf5_sample.abc is not present";
        }

        IO::SpatialReader source;
        ASSERT_TRUE(source.open(*asset)) << source.get_last_error();
        const auto name = source.get_stream_names().front();
        const auto original = source.read_sample(name, 0);
        ASSERT_TRUE(original.has_value());

        ScopedWorkingDir files;
        const auto path = path_in_cwd("copy.abc");
        {
            IO::SpatialCache cache;
            ASSERT_TRUE(cache.open(path));
            ASSERT_TRUE(cache.write(name, *original)) << cache.get_last_error();
        }

        IO::SpatialReader copy;
        ASSERT_TRUE(copy.open(path)) << copy.get_last_error();
        const auto reread = copy.read_sample(name, 0);
        ASSERT_TRUE(reread.has_value());
        EXPECT_EQ(to_vector(reread->positions), to_vector(original->positions));
        EXPECT_EQ(to_vector(reread->vertex_counts_per_curve), to_vector(original->vertex_counts_per_curve));
        EXPECT_EQ(to_vector(reread->ids), to_vector(original->ids));
    }

}
}
