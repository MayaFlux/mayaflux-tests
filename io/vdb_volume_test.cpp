#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/io_roundtrip_support.hpp"
#include "../support/log_capture.hpp"
#include "../support/scoped_working_dir.hpp"

#include "MayaFlux/IO/Volume/VDBWriter.hpp"
#include "MayaFlux/IO/Volume/VolumeReader.hpp"
#include "MayaFlux/IO/Volume/VolumeTransfer.hpp"

extern "C" {
#include "tinyvdb_ray.h"
#include "tinyvdb_sparse_tree.h"
}

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::VolumeData;
    using Kakshya::VolumeField;
    using Kinesis::Lattice3D;
    using Kinesis::LatticeValueClass;
    using Kinesis::VectorVariance;
    using RoundTrip::near;
    using RoundTrip::path_in_cwd;

    Lattice3D make_lattice(const glm::uvec3& resolution, const glm::vec3& lo, const glm::vec3& hi)
    {
        Lattice3D lattice;
        lattice.resolution = resolution;
        lattice.bounds.min = lo;
        lattice.bounds.max = hi;
        return lattice;
    }

    Lattice3D skewed_lattice()
    {
        return make_lattice({ 8, 6, 4 }, { -1.0F, -0.5F, 0.25F }, { 3.0F, 1.5F, 1.25F });
    }

    Lattice3D unit_cell_lattice(uint32_t side)
    {
        return make_lattice({ side, side, side }, glm::vec3(0.0F), glm::vec3(static_cast<float>(side)));
    }

    VolumeField scalar_field(
        const std::string& name,
        const Lattice3D& lattice,
        const std::function<float(size_t)>& value)
    {
        VolumeField field;
        field.name = name;
        std::vector<float> values(lattice.cell_count());
        for (size_t i = 0; i < values.size(); ++i) {
            values[i] = value(i);
        }
        field.values = std::move(values);
        return field;
    }

    VolumeField vector_field(
        const std::string& name,
        const Lattice3D& lattice,
        const std::function<glm::vec3(size_t)>& value)
    {
        VolumeField field;
        field.name = name;
        std::vector<glm::vec3> values(lattice.cell_count());
        for (size_t i = 0; i < values.size(); ++i) {
            values[i] = value(i);
        }
        field.values = std::move(values);
        return field;
    }

    VolumeField ramp(const std::string& name, const Lattice3D& lattice)
    {
        return scalar_field(name, lattice, [](size_t i) { return 1.0F + 0.25F * static_cast<float>(i); });
    }

    VolumeField flow(const std::string& name, const Lattice3D& lattice)
    {
        return vector_field(name, lattice, [](size_t i) {
            const auto f = static_cast<float>(i);
            return glm::vec3(1.0F + f, -1.0F - 0.5F * f, 0.25F + 0.125F * f);
        });
    }

    VolumeField single_cell(const std::string& name, const Lattice3D& lattice, const glm::uvec3& cell, float value)
    {
        auto field = scalar_field(name, lattice, [](size_t) { return 0.0F; });
        (*field.as_scalar())[lattice.index(cell)] = value;
        return field;
    }

    VolumeData volume_of(const Lattice3D& lattice, std::vector<VolumeField> fields)
    {
        VolumeData data;
        data.lattice = lattice;
        data.fields = std::move(fields);
        return data;
    }

    bool same_lattice(const Lattice3D& a, const Lattice3D& b)
    {
        return a.resolution == b.resolution && near(a.bounds.min, b.bounds.min) && near(a.bounds.max, b.bounds.max);
    }

    std::vector<std::string> names_of(const VolumeData& data)
    {
        std::vector<std::string> names;
        for (const auto& field : data.fields) {
            names.push_back(field.name);
        }
        return names;
    }

    void* test_malloc(size_t size, void*) { return std::malloc(size); }
    void* test_realloc(void* ptr, size_t, size_t size, void*) { return std::realloc(ptr, size); }
    void test_free(void* ptr, size_t, void*) { std::free(ptr); }

    struct ForeignGrid {
        std::string grid_type;
        tvdb_value_type_t value_type;
        std::string name;
        std::vector<glm::ivec3> coords;
        const void* values;
        const void* background;
        std::function<void(tvdb_transform_t&)> transform;
    };

    bool write_foreign_grid(const std::string& path, const ForeignGrid& spec)
    {
        tvdb_grid_t tmpl;
        std::memset(&tmpl, 0, sizeof(tmpl));

        std::string grid_type = spec.grid_type;
        tmpl.descriptor.grid_type = grid_type.data();

        tmpl.tree.layout.num_levels = 4;
        tmpl.tree.layout.levels[0].node_type = TVDB_NODE_ROOT;
        tmpl.tree.layout.levels[1].node_type = TVDB_NODE_INTERNAL;
        tmpl.tree.layout.levels[2].node_type = TVDB_NODE_INTERNAL;
        tmpl.tree.layout.levels[3].node_type = TVDB_NODE_LEAF;
        tmpl.tree.layout.levels[0].log2dim = 0;
        tmpl.tree.layout.levels[1].log2dim = 5;
        tmpl.tree.layout.levels[2].log2dim = 4;
        tmpl.tree.layout.levels[3].log2dim = 3;
        for (int level = 0; level < 4; ++level) {
            tmpl.tree.layout.levels[level].value_type = spec.value_type;
        }
        spec.transform(tmpl.transform);

        tvdb_grid_t grid;
        const bool built = tvdb_grid_from_sparse_typed_using_template(
            &tmpl,
            reinterpret_cast<const tvdb_vec3i*>(spec.coords.data()),
            spec.values,
            spec.coords.size(),
            spec.value_type,
            spec.background,
            spec.name.c_str(),
            &grid);
        if (!built) {
            return false;
        }

        tvdb_file_t out;
        std::memset(&out, 0, sizeof(out));
        out.alloc.malloc_fn = &test_malloc;
        out.alloc.realloc_fn = &test_realloc;
        out.alloc.free_fn = &test_free;
        out.num_grids = 1;
        out.grids = &grid;

        tvdb_error_t err;
        std::memset(&err, 0, sizeof(err));
        const bool saved = tvdb_file_save(&out, path.c_str(), TVDB_COMPRESS_NONE, 0, 0, &err) == TVDB_OK;
        tvdb_grid_destroy_owned(&grid);
        return saved;
    }

    void unit_scale_transform(tvdb_transform_t& transform)
    {
        transform.type = TVDB_TRANSFORM_SCALE_TRANSLATE;
        for (int axis = 0; axis < 3; ++axis) {
            transform.scale_values[axis] = 1.0;
            transform.voxel_size[axis] = 1.0;
            transform.translation[axis] = 0.0;
        }
    }

    class VolumeFileTest : public ::testing::Test {
    protected:
        void SetUp() override
        {
            IO::VDBWriter::register_with_registry();
            LogCapture::install();
            LogCapture::clear();
        }

        std::optional<VolumeData> round_trip(
            const VolumeData& data,
            const IO::VolumeWriteOptions& options = {},
            const std::string& name = "volume.vdb")
        {
            IO::VDBWriter writer;
            const auto path = path_in_cwd(name);
            if (!writer.write(path, data, options)) {
                ADD_FAILURE() << writer.get_last_error();
                return std::nullopt;
            }
            IO::VolumeReader reader;
            auto loaded = reader.load(path, data.lattice);
            if (!loaded) {
                ADD_FAILURE() << reader.get_last_error();
            }
            return loaded;
        }

        ScopedWorkingDir m_files;
    };

    TEST_F(VolumeFileTest, ScalarAndVectorFieldsRoundTripExactlyThroughTheWritersLattice)
    {
        const auto lattice = skewed_lattice();
        const auto source = volume_of(lattice, { ramp("density", lattice), flow("velocity", lattice) });

        const auto loaded = round_trip(source);

        ASSERT_TRUE(loaded.has_value());
        EXPECT_TRUE(same_lattice(loaded->lattice, lattice));
        ASSERT_EQ(loaded->fields.size(), 2U);
        EXPECT_EQ(names_of(*loaded), (std::vector<std::string> { "density", "velocity" }));
        EXPECT_EQ(*loaded->fields[0].as_scalar(), *source.fields[0].as_scalar());
        EXPECT_EQ(*loaded->fields[1].as_vector(), *source.fields[1].as_vector());
        EXPECT_TRUE(loaded->is_consistent());
    }

    TEST_F(VolumeFileTest, ASecondWriteToTheSamePathReplacesTheFirst)
    {
        const auto lattice = skewed_lattice();
        ASSERT_TRUE(round_trip(volume_of(lattice, { ramp("first", lattice) })).has_value());

        const auto loaded = round_trip(volume_of(lattice, { flow("second", lattice) }));

        ASSERT_TRUE(loaded.has_value());
        EXPECT_EQ(names_of(*loaded), (std::vector<std::string> { "second" }));
    }

    TEST_F(VolumeFileTest, ReadingWithoutALatticeWidensTheExtentToWholeLeafBlocks)
    {
        const auto lattice = skewed_lattice();
        const auto source = volume_of(lattice, { ramp("density", lattice) });
        IO::VDBWriter writer;
        const auto path = path_in_cwd("leaf.vdb");
        ASSERT_TRUE(writer.write(path, source)) << writer.get_last_error();

        IO::VolumeReader reader;
        const auto loaded = reader.load(path);

        ASSERT_TRUE(loaded.has_value()) << reader.get_last_error();
        EXPECT_EQ(loaded->lattice.resolution, glm::uvec3(8, 8, 8));
        EXPECT_TRUE(near(loaded->lattice.bounds.min, lattice.bounds.min));
        const auto cell = lattice.cell_size();
        EXPECT_TRUE(near(loaded->lattice.bounds.max, lattice.bounds.min + 8.0F * cell));

        const auto& values = *loaded->fields.front().as_scalar();
        const auto& original = *source.fields.front().as_scalar();
        for (uint32_t z = 0; z < 8; ++z) {
            for (uint32_t y = 0; y < 8; ++y) {
                for (uint32_t x = 0; x < 8; ++x) {
                    const float expected = lattice.in_bounds({ x, y, z }) ? original[lattice.index({ x, y, z })] : 0.0F;
                    EXPECT_EQ(values[loaded->lattice.index({ x, y, z })], expected) << x << ' ' << y << ' ' << z;
                }
            }
        }
    }

    TEST_F(VolumeFileTest, ASingleActiveCellComesBackInsideItsEightCellBlock)
    {
        const auto lattice = unit_cell_lattice(20);
        const auto source = volume_of(lattice, { single_cell("spark", lattice, { 10, 11, 12 }, 5.0F) });
        IO::VDBWriter writer;
        const auto path = path_in_cwd("spark.vdb");
        ASSERT_TRUE(writer.write(path, source)) << writer.get_last_error();

        IO::VolumeReader reader;
        const auto loaded = reader.load(path);

        ASSERT_TRUE(loaded.has_value()) << reader.get_last_error();
        EXPECT_EQ(loaded->lattice.resolution, glm::uvec3(8, 8, 8));
        EXPECT_TRUE(near(loaded->lattice.bounds.min, glm::vec3(8.0F)));
        EXPECT_TRUE(near(loaded->lattice.bounds.max, glm::vec3(16.0F)));

        const auto& values = *loaded->fields.front().as_scalar();
        EXPECT_EQ(values[loaded->lattice.index({ 2, 3, 4 })], 5.0F);
        EXPECT_FLOAT_EQ(std::accumulate(values.begin(), values.end(), 0.0F), 5.0F);
    }

    TEST_F(VolumeFileTest, TheUnionOfSeveralFieldsCanReachPastTheOriginalLattice)
    {
        const auto lattice = unit_cell_lattice(20);
        const auto source = volume_of(lattice,
            { single_cell("near", lattice, { 1, 1, 1 }, 2.0F), single_cell("far", lattice, { 17, 17, 17 }, 3.0F) });
        IO::VDBWriter writer;
        const auto path = path_in_cwd("union.vdb");
        ASSERT_TRUE(writer.write(path, source)) << writer.get_last_error();

        IO::VolumeReader reader;
        const auto loaded = reader.load(path);

        ASSERT_TRUE(loaded.has_value()) << reader.get_last_error();
        EXPECT_EQ(loaded->lattice.resolution, glm::uvec3(24, 24, 24));
        EXPECT_EQ(loaded->fields.size(), 2U);
        EXPECT_EQ((*loaded->find("near")->as_scalar())[loaded->lattice.index({ 1, 1, 1 })], 2.0F);
        EXPECT_EQ((*loaded->find("far")->as_scalar())[loaded->lattice.index({ 17, 17, 17 })], 3.0F);
    }

    TEST_F(VolumeFileTest, ASubRegionLatticeReadsTheMatchingCells)
    {
        const auto lattice = skewed_lattice();
        const auto source = volume_of(lattice, { ramp("density", lattice) });
        IO::VDBWriter writer;
        const auto path = path_in_cwd("region.vdb");
        ASSERT_TRUE(writer.write(path, source)) << writer.get_last_error();

        const auto cell = lattice.cell_size();
        const auto region = make_lattice({ 4, 3, 2 },
            lattice.bounds.min + cell * glm::vec3(2, 1, 1),
            lattice.bounds.min + cell * glm::vec3(6, 4, 3));

        IO::VolumeReader reader;
        const auto loaded = reader.load(path, region);

        ASSERT_TRUE(loaded.has_value()) << reader.get_last_error();
        const auto& values = *loaded->fields.front().as_scalar();
        const auto& original = *source.fields.front().as_scalar();
        for (uint32_t z = 0; z < 2; ++z) {
            for (uint32_t y = 0; y < 3; ++y) {
                for (uint32_t x = 0; x < 4; ++x) {
                    EXPECT_EQ(values[region.index({ x, y, z })], original[lattice.index({ x + 2, y + 1, z + 1 })]);
                }
            }
        }
    }

    TEST_F(VolumeFileTest, ARegionOutsideTheFilesContentReadsAsBackground)
    {
        const auto lattice = skewed_lattice();
        IO::VolumeWriteOptions options;
        options.background = -3.0F;
        IO::VDBWriter writer;
        const auto path = path_in_cwd("outside.vdb");
        ASSERT_TRUE(writer.write(path, volume_of(lattice, { ramp("density", lattice) }), options));

        const auto cell = lattice.cell_size();
        const auto far_region = make_lattice({ 2, 2, 2 },
            lattice.bounds.min + cell * glm::vec3(100.0F),
            lattice.bounds.min + cell * glm::vec3(102.0F));

        IO::VolumeReader reader;
        const auto loaded = reader.load(path, far_region);

        ASSERT_TRUE(loaded.has_value()) << reader.get_last_error();
        for (const float v : *loaded->fields.front().as_scalar()) {
            EXPECT_EQ(v, -3.0F);
        }
    }

    TEST_F(VolumeFileTest, CellsBelowTheThresholdTakeTheBackgroundValue)
    {
        const auto lattice = make_lattice({ 8, 2, 2 }, glm::vec3(0.0F), glm::vec3(8.0F, 2.0F, 2.0F));
        const auto source = volume_of(lattice, { scalar_field("mix", lattice, [](size_t i) {
            constexpr std::array<float, 3> pattern { 0.5F, 1e-9F, -2.0F };
            return pattern[i % 3];
        }) });
        IO::VolumeWriteOptions options;
        options.background = -3.0F;

        const auto loaded = round_trip(source, options);

        ASSERT_TRUE(loaded.has_value());
        const auto& values = *loaded->fields.front().as_scalar();
        for (size_t i = 0; i < values.size(); ++i) {
            constexpr std::array<float, 3> expected { 0.5F, -3.0F, -2.0F };
            EXPECT_EQ(values[i], expected[i % 3]) << i;
        }
    }

    TEST_F(VolumeFileTest, APerFieldThresholdOverridesTheGlobalOne)
    {
        const auto lattice = make_lattice({ 8, 2, 2 }, glm::vec3(0.0F), glm::vec3(8.0F, 2.0F, 2.0F));
        auto pattern = [](size_t i) {
            constexpr std::array<float, 4> values { 0.05F, 0.1F, 0.15F, 0.2F };
            return values[i % 4];
        };
        const auto source = volume_of(lattice,
            { scalar_field("strict", lattice, pattern), scalar_field("loose", lattice, pattern) });
        IO::VolumeWriteOptions options;
        options.field_thresholds["strict"] = 0.12F;
        options.field_thresholds["not_a_field"] = 100.0F;

        const auto loaded = round_trip(source, options);

        ASSERT_TRUE(loaded.has_value());
        const auto& strict = *loaded->find("strict")->as_scalar();
        const auto& loose = *loaded->find("loose")->as_scalar();
        for (size_t i = 0; i < strict.size(); ++i) {
            const float original = pattern(i);
            EXPECT_EQ(loose[i], original);
            EXPECT_EQ(strict[i], original < 0.12F ? 0.0F : original);
        }
    }

    TEST_F(VolumeFileTest, VectorActivityIsDecidedByLengthNotByComponent)
    {
        const auto lattice = make_lattice({ 8, 2, 2 }, glm::vec3(0.0F), glm::vec3(8.0F, 2.0F, 2.0F));
        const auto source = volume_of(lattice, { vector_field("flow", lattice, [](size_t i) {
            return i % 2 == 0 ? glm::vec3(0.5F, 0.0F, 0.0F) : glm::vec3(0.0005F);
        }) });
        IO::VolumeWriteOptions options;
        options.activity_threshold = 1e-3F;
        options.background = 2.0F;

        const auto loaded = round_trip(source, options);

        ASSERT_TRUE(loaded.has_value());
        const auto& values = *loaded->fields.front().as_vector();
        for (size_t i = 0; i < values.size(); ++i) {
            EXPECT_EQ(values[i], i % 2 == 0 ? glm::vec3(0.5F, 0.0F, 0.0F) : glm::vec3(2.0F)) << i;
        }
    }

    TEST_F(VolumeFileTest, TheHalfFloatOptionIsCurrentlyIgnored)
    {
        const auto lattice = make_lattice({ 4, 4, 4 }, glm::vec3(0.0F), glm::vec3(4.0F));
        const auto source = volume_of(lattice, { scalar_field("fine", lattice, [](size_t i) {
            return 0.123456789F + static_cast<float>(i) * 1e-4F;
        }) });
        IO::VolumeWriteOptions options;
        options.half_float = true;

        const auto loaded = round_trip(source, options);

        ASSERT_TRUE(loaded.has_value());
        EXPECT_EQ(*loaded->fields.front().as_scalar(), *source.fields.front().as_scalar());
    }

    class VolumeCompressionTest
        : public VolumeFileTest,
          public ::testing::WithParamInterface<int> { };

    TEST_P(VolumeCompressionTest, EveryCodecRoundTripsTheSameValues)
    {
        const auto lattice = skewed_lattice();
        const auto source = volume_of(lattice, { ramp("density", lattice), flow("velocity", lattice) });
        IO::VolumeWriteOptions options;
        options.compression = GetParam();

        const auto loaded = round_trip(source, options);

        ASSERT_TRUE(loaded.has_value());
        EXPECT_EQ(*loaded->fields[0].as_scalar(), *source.fields[0].as_scalar());
        EXPECT_EQ(*loaded->fields[1].as_vector(), *source.fields[1].as_vector());
    }

    INSTANTIATE_TEST_SUITE_P(
        Codecs,
        VolumeCompressionTest,
        ::testing::Values(-1, 0, 1, 2, 3),
        [](const ::testing::TestParamInfo<int>& info) {
            return info.param < 0 ? std::string("Default") : "Flags" + std::to_string(info.param);
        });

    TEST_F(VolumeFileTest, ValueClassAndVarianceSurviveTheFile)
    {
        const auto lattice = make_lattice({ 4, 4, 4 }, glm::vec3(0.0F), glm::vec3(4.0F));
        auto scalar = [&](const std::string& name, LatticeValueClass value_class) {
            auto field = scalar_field(name, lattice, [](size_t) { return 1.0F; });
            field.semantics.value_class = value_class;
            return field;
        };
        auto vec = [&](const std::string& name, VectorVariance variance) {
            auto field = vector_field(name, lattice, [](size_t) { return glm::vec3(1.0F, 2.0F, 3.0F); });
            field.semantics.variance = variance;
            return field;
        };
        const auto source = volume_of(lattice,
            { scalar("fog", LatticeValueClass::FogVolume),
                scalar("level", LatticeValueClass::LevelSet),
                scalar("stag", LatticeValueClass::Staggered),
                scalar("none", LatticeValueClass::Unknown),
                vec("cov", VectorVariance::Covariant),
                vec("covn", VectorVariance::CovariantNormalize),
                vec("rel", VectorVariance::ContravariantRelative),
                vec("abs", VectorVariance::ContravariantAbsolute),
                vec("inv", VectorVariance::Invariant) });

        const auto loaded = round_trip(source);

        ASSERT_TRUE(loaded.has_value());
        ASSERT_EQ(loaded->fields.size(), source.fields.size());
        for (size_t i = 0; i < source.fields.size(); ++i) {
            EXPECT_EQ(loaded->fields[i].semantics.value_class, source.fields[i].semantics.value_class) << source.fields[i].name;
            EXPECT_EQ(loaded->fields[i].semantics.variance, source.fields[i].semantics.variance) << source.fields[i].name;
        }
    }

    TEST_F(VolumeFileTest, FieldNamesSelectGridsInRequestedOrder)
    {
        const auto lattice = skewed_lattice();
        IO::VDBWriter writer;
        const auto path = path_in_cwd("select.vdb");
        ASSERT_TRUE(writer.write(path, volume_of(lattice, { ramp("a", lattice), ramp("b", lattice), flow("c", lattice) })));

        IO::VolumeReader reader;
        IO::VolumeReadOptions options;
        options.field_names = { "c", "a" };
        const auto loaded = reader.load(path, lattice, options);

        ASSERT_TRUE(loaded.has_value()) << reader.get_last_error();
        EXPECT_EQ(names_of(*loaded), (std::vector<std::string> { "c", "a" }));
    }

    TEST_F(VolumeFileTest, AnUnknownFieldNameIsSkippedAndTheRestProceed)
    {
        const auto lattice = skewed_lattice();
        IO::VDBWriter writer;
        const auto path = path_in_cwd("skip.vdb");
        ASSERT_TRUE(writer.write(path, volume_of(lattice, { ramp("a", lattice) })));

        IO::VolumeReader reader;
        IO::VolumeReadOptions options;
        options.field_names = { "missing", "a" };
        const auto loaded = reader.load(path, lattice, options);

        ASSERT_TRUE(loaded.has_value());
        EXPECT_EQ(names_of(*loaded), (std::vector<std::string> { "a" }));
    }

    TEST_F(VolumeFileTest, OnlyUnknownFieldNamesFailTheRead)
    {
        const auto lattice = skewed_lattice();
        IO::VDBWriter writer;
        const auto path = path_in_cwd("none.vdb");
        ASSERT_TRUE(writer.write(path, volume_of(lattice, { ramp("a", lattice) })));

        IO::VolumeReader reader;
        IO::VolumeReadOptions options;
        options.field_names = { "missing" };

        EXPECT_FALSE(reader.load(path, lattice, options).has_value());
        EXPECT_NE(reader.get_last_error().find("No matching grids"), std::string::npos);
        EXPECT_FALSE(reader.load(path, options).has_value());
    }

    TEST_F(VolumeFileTest, OpenReportsTheGridCountAndCloseDropsIt)
    {
        const auto lattice = skewed_lattice();
        IO::VDBWriter writer;
        const auto path = path_in_cwd("meta.vdb");
        ASSERT_TRUE(writer.write(path, volume_of(lattice, { ramp("a", lattice), flow("b", lattice) })));

        IO::VolumeReader reader;
        EXPECT_FALSE(reader.get_metadata().has_value());
        ASSERT_TRUE(reader.open(path)) << reader.get_last_error();
        EXPECT_TRUE(reader.is_open());

        const auto metadata = reader.get_metadata();
        ASSERT_TRUE(metadata.has_value());
        EXPECT_EQ(metadata->format, "vdb");
        const auto* info = metadata->get<IO::VolumeInfo>();
        ASSERT_NE(info, nullptr);
        EXPECT_EQ(info->grid_count, 2U);

        reader.close();
        EXPECT_FALSE(reader.is_open());
        EXPECT_FALSE(reader.extract().has_value());
        EXPECT_NE(reader.get_last_error().find("No file open"), std::string::npos);
    }

    TEST_F(VolumeFileTest, ExtractAfterOpenMatchesLoad)
    {
        const auto lattice = skewed_lattice();
        IO::VDBWriter writer;
        const auto path = path_in_cwd("extract.vdb");
        ASSERT_TRUE(writer.write(path, volume_of(lattice, { ramp("a", lattice) })));

        IO::VolumeReader opened;
        ASSERT_TRUE(opened.open(path));
        const auto via_extract = opened.extract(lattice);

        IO::VolumeReader loader;
        const auto via_load = loader.load(path, lattice);

        ASSERT_TRUE(via_extract.has_value());
        ASSERT_TRUE(via_load.has_value());
        EXPECT_EQ(*via_extract->fields.front().as_scalar(), *via_load->fields.front().as_scalar());
    }

    TEST_F(VolumeFileTest, ContainerStyleInterfaceIsInertForVolumes)
    {
        IO::VolumeReader reader;

        EXPECT_EQ(reader.create_container(), nullptr);
        EXPECT_NE(reader.get_last_error().find("SignalSourceContainer"), std::string::npos);
        EXPECT_FALSE(reader.load_into_container(nullptr));
    }

    TEST_F(VolumeFileTest, ReaderAndWriterClaimOnlyTheVdbExtension)
    {
        IO::VolumeReader reader;
        IO::VDBWriter writer;

        EXPECT_TRUE(reader.can_read("a.vdb"));
        EXPECT_TRUE(reader.can_read("A.VDB"));
        EXPECT_FALSE(reader.can_read("a.abc"));
        EXPECT_TRUE(writer.can_write("A.Vdb"));
        EXPECT_FALSE(writer.can_write("a.exr"));
        EXPECT_EQ(writer.get_supported_extensions(), std::vector<std::string> { "vdb" });
    }

    TEST_F(VolumeFileTest, InconsistentDataIsRejectedBeforeAnythingIsWritten)
    {
        const auto lattice = skewed_lattice();
        IO::VDBWriter writer;

        auto short_field = volume_of(lattice, { ramp("a", lattice) });
        short_field.fields.front().as_scalar()->pop_back();
        auto duplicated = volume_of(lattice, { ramp("a", lattice), ramp("a", lattice) });
        auto unnamed = volume_of(lattice, { ramp("", lattice) });
        auto no_fields = volume_of(lattice, {});
        auto no_cells = volume_of(make_lattice({ 0, 4, 4 }, glm::vec3(0.0F), glm::vec3(1.0F)), {});

        int index = 0;
        for (const auto* data : { &short_field, &duplicated, &unnamed, &no_fields, &no_cells }) {
            const auto path = path_in_cwd("bad" + std::to_string(index++) + ".vdb");
            EXPECT_FALSE(writer.write(path, *data)) << path;
            EXPECT_NE(writer.get_last_error().find("is_consistent"), std::string::npos) << path;
            EXPECT_FALSE(RoundTrip::exists(path)) << path;
        }
    }

    TEST_F(VolumeFileTest, WriterErrorIsClearedByTheNextSuccessfulWrite)
    {
        const auto lattice = skewed_lattice();
        IO::VDBWriter writer;
        ASSERT_FALSE(writer.write(path_in_cwd("bad.vdb"), volume_of(lattice, {})));
        ASSERT_FALSE(writer.get_last_error().empty());

        ASSERT_TRUE(writer.write(path_in_cwd("good.vdb"), volume_of(lattice, { ramp("a", lattice) })));
        EXPECT_TRUE(writer.get_last_error().empty());
    }

    TEST_F(VolumeFileTest, SaveVolumeDispatchesByExtensionThroughTheRegistry)
    {
        const auto lattice = skewed_lattice();
        const auto source = volume_of(lattice, { ramp("a", lattice) });

        EXPECT_TRUE(IO::save_volume(source, path_in_cwd("registry.vdb"), {}));
        EXPECT_TRUE(RoundTrip::exists(path_in_cwd("registry.vdb")));
        EXPECT_FALSE(IO::save_volume(source, path_in_cwd("registry.unknown"), {}));
        EXPECT_FALSE(RoundTrip::exists(path_in_cwd("registry.unknown")));
        EXPECT_FALSE(IO::save_volume(volume_of(lattice, {}), path_in_cwd("empty.vdb"), {}));
    }

    TEST_F(VolumeFileTest, AFieldWithNoActiveCellsEitherFailsToWriteOrCannotBeReadWithoutALattice)
    {
        const auto lattice = skewed_lattice();
        IO::VolumeWriteOptions options;
        options.activity_threshold = 1000.0F;
        IO::VDBWriter writer;
        const auto path = path_in_cwd("inactive.vdb");

        if (writer.write(path, volume_of(lattice, { ramp("a", lattice) }), options)) {
            IO::VolumeReader reader;
            EXPECT_FALSE(reader.load(path).has_value());
        } else {
            EXPECT_FALSE(writer.get_last_error().empty());
        }
    }

    TEST_F(VolumeFileTest, MissingFileFailsToOpen)
    {
        IO::VolumeReader reader;

        EXPECT_FALSE(reader.open(path_in_cwd("absent.vdb")));
        EXPECT_FALSE(reader.is_open());
        EXPECT_NE(reader.get_last_error().find("open failed"), std::string::npos);
        EXPECT_FALSE(reader.load(path_in_cwd("absent.vdb")).has_value());
    }

    TEST_F(VolumeFileTest, WrongExtensionIsRejectedBeforeTheFileIsTouched)
    {
        const auto path = path_in_cwd("notes.txt");
        RoundTrip::write_bytes(path, "plain text");

        IO::VolumeReader reader;
        EXPECT_FALSE(reader.open(path));
        EXPECT_NE(reader.get_last_error().find("Unsupported volume format"), std::string::npos);
    }

    TEST_F(VolumeFileTest, GarbageAndEmptyFilesFailToOpen)
    {
        IO::VolumeReader reader;

        RoundTrip::write_bytes(path_in_cwd("garbage.vdb"), RoundTrip::garbage(4096));
        EXPECT_FALSE(reader.open(path_in_cwd("garbage.vdb")));

        RoundTrip::write_bytes(path_in_cwd("empty.vdb"), "");
        EXPECT_FALSE(reader.open(path_in_cwd("empty.vdb")));
        EXPECT_FALSE(reader.get_last_error().empty());
    }

    TEST_F(VolumeFileTest, ATruncatedFileFailsToOpen)
    {
        const auto lattice = skewed_lattice();
        IO::VDBWriter writer;
        const auto good = path_in_cwd("good.vdb");
        ASSERT_TRUE(writer.write(good, volume_of(lattice, { ramp("a", lattice) })));
        const auto bytes = RoundTrip::read_bytes(good);
        ASSERT_GT(bytes.size(), 128U);

        const auto cut = path_in_cwd("cut.vdb");
        RoundTrip::write_bytes(cut, RoundTrip::truncated(bytes, bytes.size() / 2));

        IO::VolumeReader reader;
        EXPECT_FALSE(reader.open(cut));
    }

    TEST_F(VolumeFileTest, AFileWithOnlyItsHeaderFailsToOpen)
    {
        const auto lattice = skewed_lattice();
        IO::VDBWriter writer;
        const auto good = path_in_cwd("good.vdb");
        ASSERT_TRUE(writer.write(good, volume_of(lattice, { ramp("a", lattice) })));
        const auto bytes = RoundTrip::read_bytes(good);

        const auto cut = path_in_cwd("header_only.vdb");
        RoundTrip::write_bytes(cut, RoundTrip::truncated(bytes, 16));

        IO::VolumeReader reader;
        EXPECT_FALSE(reader.open(cut));
    }

    TEST_F(VolumeFileTest, ExtractionOnAnUnopenedReaderFails)
    {
        IO::VolumeReader reader;

        EXPECT_FALSE(reader.extract().has_value());
        EXPECT_FALSE(reader.extract(skewed_lattice()).has_value());
        EXPECT_NE(reader.get_last_error().find("No file open"), std::string::npos);
    }

    TEST_F(VolumeFileTest, Int32GridIsNarrowedToFloatWithAPrecisionWarning)
    {
        const std::vector<glm::ivec3> coords { { 1, 2, 3 }, { 4, 5, 6 } };
        const std::array<int32_t, 2> values { 7, 16777217 };
        const int32_t background = -2;
        const auto path = path_in_cwd("int32.vdb");
        ASSERT_TRUE(write_foreign_grid(path,
            { .grid_type = "Tree_int32_5_4_3",
                .value_type = TVDB_VALUE_INT32,
                .name = "counts",
                .coords = coords,
                .values = values.data(),
                .background = &background,
                .transform = unit_scale_transform }));
        LogCapture::clear();

        IO::VolumeReader reader;
        const auto loaded = reader.load(path);

        ASSERT_TRUE(loaded.has_value()) << reader.get_last_error();
        EXPECT_EQ(loaded->lattice.resolution, glm::uvec3(8, 8, 8));
        EXPECT_TRUE(near(loaded->lattice.bounds.min, glm::vec3(-0.5F)));
        EXPECT_TRUE(near(loaded->lattice.bounds.max, glm::vec3(7.5F)));
        const auto& cells = *loaded->fields.front().as_scalar();
        EXPECT_EQ(cells[loaded->lattice.index({ 1, 2, 3 })], 7.0F);
        EXPECT_NEAR(cells[loaded->lattice.index({ 4, 5, 6 })], 16777217.0F, 1.0F);
        EXPECT_EQ(cells[loaded->lattice.index({ 0, 0, 0 })], -2.0F);
        EXPECT_EQ(cells[loaded->lattice.index({ 7, 7, 7 })], -2.0F);
        EXPECT_TRUE(LogCapture::contains("lost precision"));
    }

    TEST_F(VolumeFileTest, Int32GridThatFitsFloatIsNarrowedLosslessly)
    {
        const std::vector<glm::ivec3> coords { { 0, 0, 0 }, { 1, 0, 0 } };
        const std::array<int32_t, 2> values { 7, 9 };
        const int32_t background = 0;
        const auto path = path_in_cwd("int32_exact.vdb");
        ASSERT_TRUE(write_foreign_grid(path,
            { .grid_type = "Tree_int32_5_4_3",
                .value_type = TVDB_VALUE_INT32,
                .name = "counts",
                .coords = coords,
                .values = values.data(),
                .background = &background,
                .transform = unit_scale_transform }));
        LogCapture::clear();

        IO::VolumeReader reader;
        const auto loaded = reader.load(path);

        ASSERT_TRUE(loaded.has_value());
        const auto& cells = *loaded->fields.front().as_scalar();
        EXPECT_EQ(cells[loaded->lattice.index({ 0, 0, 0 })], 7.0F);
        EXPECT_EQ(cells[loaded->lattice.index({ 1, 0, 0 })], 9.0F);
        EXPECT_TRUE(LogCapture::contains("losslessly"));
        EXPECT_FALSE(LogCapture::contains("lost precision"));
    }

    TEST_F(VolumeFileTest, AffineTransformGridUsesTheMatrixDiagonalAndLastColumn)
    {
        const std::vector<glm::ivec3> coords { { 0, 0, 0 } };
        const std::array<float, 1> values { 1.0F };
        const float background = 0.0F;
        const auto path = path_in_cwd("affine.vdb");
        ASSERT_TRUE(write_foreign_grid(path,
            { .grid_type = "Tree_float_5_4_3",
                .value_type = TVDB_VALUE_FLOAT,
                .name = "affine",
                .coords = coords,
                .values = values.data(),
                .background = &background,
                .transform = [](tvdb_transform_t& transform) {
                    transform.type = TVDB_TRANSFORM_AFFINE;
                    transform.matrix[0][0] = 0.5;
                    transform.matrix[1][1] = 0.25;
                    transform.matrix[2][2] = 2.0;
                    transform.matrix[0][3] = 3.0;
                    transform.matrix[1][3] = -4.0;
                    transform.matrix[2][3] = 5.0;
                    transform.matrix[3][3] = 1.0;
                } }));

        IO::VolumeReader reader;
        const auto loaded = reader.load(path);

        ASSERT_TRUE(loaded.has_value()) << reader.get_last_error();
        EXPECT_TRUE(near(loaded->lattice.bounds.min, glm::vec3(2.75F, -4.125F, 4.0F)));
        EXPECT_TRUE(near(loaded->lattice.bounds.max, glm::vec3(6.75F, -2.125F, 20.0F)));
    }

    MAYAFLUX_TEST_WITH(TEST, VolumeAssetTest, RealFireVdbLoadsAsAConsistentVolumeAndRewritesLosslessly,
        (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::repo_assets, ::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        const auto asset = RoundTrip::repo_asset("res/fire.vdb");
        if (!asset) {
            GTEST_SKIP() << "res/fire.vdb is not present";
        }
        IO::VDBWriter::register_with_registry();

        IO::VolumeReader reader;
        ASSERT_TRUE(reader.open(*asset)) << reader.get_last_error();
        const auto metadata = reader.get_metadata();
        ASSERT_TRUE(metadata.has_value());
        const auto* info = metadata->get<IO::VolumeInfo>();
        ASSERT_NE(info, nullptr);
        EXPECT_GT(info->grid_count, 0U);

        auto loaded = reader.extract();
        ASSERT_TRUE(loaded.has_value()) << reader.get_last_error();
        EXPECT_TRUE(loaded->is_consistent());
        EXPECT_EQ(loaded->fields.size(), info->grid_count);
        for (const auto& field : loaded->fields) {
            EXPECT_EQ(field.element_count(), loaded->cell_count()) << field.name;
        }

        loaded->fields.resize(1);
        const auto& first = loaded->fields.front();

        ScopedWorkingDir files;
        IO::VDBWriter writer;
        const auto path = path_in_cwd("fire_copy.vdb");
        ASSERT_TRUE(writer.write(path, *loaded)) << writer.get_last_error();

        IO::VolumeReader again;
        const auto reread = again.load(path, loaded->lattice);
        ASSERT_TRUE(reread.has_value()) << again.get_last_error();
        ASSERT_EQ(reread->fields.size(), 1U);

        if (const auto* scalars = first.as_scalar()) {
            const auto& copy = *reread->fields.front().as_scalar();
            ASSERT_EQ(copy.size(), scalars->size());
            for (size_t i = 0; i < copy.size(); ++i) {
                ASSERT_NEAR(copy[i], (*scalars)[i], 1e-6F) << i;
            }
        } else {
            const auto& vectors = *first.as_vector();
            const auto& copy = *reread->fields.front().as_vector();
            ASSERT_EQ(copy.size(), vectors.size());
            for (size_t i = 0; i < copy.size(); ++i) {
                ASSERT_TRUE(near(copy[i], vectors[i], 1e-6F)) << i;
            }
        }
    }

}
}
