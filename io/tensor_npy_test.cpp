#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/IO/Tensor/NpyFormat.hpp"
#include "MayaFlux/IO/Tensor/NpzFormat.hpp"
#include "MayaFlux/IO/Tensor/SafetensorsFormat.hpp"
#include "MayaFlux/IO/Tensor/TensorFormat.hpp"
#include "MayaFlux/Kakshya/NDData/TensorInsertion.hpp"

#include "miniz.h"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {
namespace {

    namespace fs = std::filesystem;

    using IO::NpyReader;
    using IO::NpyWriter;
    using IO::NpzReader;
    using IO::NpzWriter;
    using IO::SafetensorsReader;
    using IO::SafetensorsWriter;
    using IO::TensorFormatRegistry;
    using IO::TensorReadOptions;
    using Kakshya::TensorData;
    using Kakshya::TensorDType;
    using Kakshya::TensorDTypeKind;
    using Kakshya::TensorInsertion;
    using Kakshya::TensorSet;
    using Kakshya::TensorSpec;

    class TensorNpyFixture : public ::testing::Test {
    protected:
        void TearDown() override
        {
            for (const auto& path : m_paths) {
                std::error_code error;
                fs::remove(path, error);
            }
        }

        std::string temp_path(const std::string& suffix)
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            auto path = fs::temp_directory_path()
                / ("mayaflux_tensor_npy_" + std::to_string(stamp) + "_" + std::to_string(m_counter++) + suffix);
            m_paths.push_back(path);
            return path.string();
        }

        std::string write_file(const std::string& suffix, const std::string& content)
        {
            const auto path = temp_path(suffix);
            std::ofstream file(path, std::ios::binary);
            file.write(content.data(), static_cast<std::streamsize>(content.size()));
            return path;
        }

        bool write_zip(const std::string& path,
            const std::vector<std::pair<std::string, std::string>>& entries,
            mz_uint level = MZ_NO_COMPRESSION)
        {
            mz_zip_archive zip {};
            if (!mz_zip_writer_init_file(&zip, path.c_str(), 0))
                return false;
            for (const auto& [name, content] : entries) {
                if (!mz_zip_writer_add_mem(&zip, name.c_str(), content.data(), content.size(), level)) {
                    mz_zip_writer_end(&zip);
                    return false;
                }
            }
            const bool finalized = mz_zip_writer_finalize_archive(&zip) != 0;
            mz_zip_writer_end(&zip);
            return finalized;
        }

    private:
        std::vector<fs::path> m_paths;
        int m_counter {};
    };

    std::string read_file(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    }

    std::string float_bytes(std::initializer_list<float> values)
    {
        std::string out(values.size() * sizeof(float), '\0');
        size_t offset = 0;
        for (const float value : values) {
            std::memcpy(out.data() + offset, &value, sizeof(float));
            offset += sizeof(float);
        }
        return out;
    }

    std::string npy_bytes(const std::string& dict, const std::string& data, uint8_t major = 1)
    {
        const std::string header = dict + "\n";
        std::string out(1, '\x93');
        out += "NUMPY";
        out.push_back(static_cast<char>(major));
        out.push_back('\0');
        const size_t length_bytes = major == 1 ? 2 : 4;
        for (size_t i = 0; i < length_bytes; ++i)
            out.push_back(static_cast<char>((header.size() >> (8 * i)) & 0xFF));
        return out + header + data;
    }

    std::string npy_dict(const std::string& descr, const std::string& shape, const std::string& order = "False")
    {
        return "{'descr': '" + descr + "', 'fortran_order': " + order + ", 'shape': " + shape + ", }";
    }

    std::optional<TensorData> floats(std::vector<uint64_t> shape, std::string name)
    {
        uint64_t count = 1;
        for (const auto extent : shape)
            count *= extent;
        std::vector<float> values(count);
        std::iota(values.begin(), values.end(), 0.0F);
        return TensorData::from_values<float>(values, std::move(shape), std::move(name));
    }

    std::vector<float> floats_of(const TensorData& tensor)
    {
        return tensor.to_vector<float>().value_or(std::vector<float> {});
    }

    bool same_bytes(const TensorData& a, const TensorData& b)
    {
        return a.byte_size() == b.byte_size()
            && (a.byte_size() == 0 || std::memcmp(a.storage().data(), b.storage().data(), a.byte_size()) == 0);
    }

    struct DTypeCase {
        const char* name;
        TensorDType dtype;
        std::vector<uint64_t> shape;
    };

    const std::vector<DTypeCase>& numpy_cases()
    {
        static const std::vector<DTypeCase> cases {
            { "F16", TensorDType::float16(), { 4 } },
            { "F32", TensorDType::of<float>(), { 2, 3 } },
            { "F64", TensorDType::of<double>(), { 3 } },
            { "I8", TensorDType::of<int8_t>(), { 5 } },
            { "I16", TensorDType::of<int16_t>(), { 3 } },
            { "I32", TensorDType::of<int32_t>(), { 3 } },
            { "I64", TensorDType::of<int64_t>(), { 2, 2 } },
            { "U8", TensorDType::of<uint8_t>(), { 7 } },
            { "U16", TensorDType::of<uint16_t>(), { 3 } },
            { "U32", TensorDType::of<uint32_t>(), { 3 } },
            { "U64", TensorDType::of<uint64_t>(), { 2 } },
            { "Bool", TensorDType::of<bool>(), { 6 } },
            { "Complex64", TensorDType::of<std::complex<float>>(), { 3 } },
            { "Complex128", TensorDType::of<std::complex<double>>(), { 2 } },
            { "Scalar", TensorDType::of<float>(), {} },
            { "Empty", TensorDType::of<float>(), { 0, 4 } },
            { "Rank4", TensorDType::of<int16_t>(), { 2, 1, 3, 2 } },
        };
        return cases;
    }

    TensorData patterned(const DTypeCase& param, const std::string& name)
    {
        const auto spec = TensorSpec::contiguous(param.dtype, param.shape, name);
        std::vector<std::byte> bytes(param.dtype.storage_bytes(spec.element_count().value_or(0)).value_or(0));
        for (size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<std::byte>((i * 7 + 3) % 251);
        return std::move(*TensorData::from_bytes(spec, bytes));
    }

    // ---------------------------------------------------------------------
    // .npy: writing and round trips
    // ---------------------------------------------------------------------

    class TensorNpyRoundTripTest : public TensorNpyFixture,
                                   public ::testing::WithParamInterface<DTypeCase> { };

    INSTANTIATE_TEST_SUITE_P(DTypes, TensorNpyRoundTripTest, ::testing::ValuesIn(numpy_cases()),
        [](const ::testing::TestParamInfo<DTypeCase>& info) { return std::string(info.param.name); });

    TEST_P(TensorNpyRoundTripTest, BytesShapeAndTypeSurviveAFileTrip)
    {
        const auto& param = GetParam();
        const auto original = patterned(param, "t");

        TensorSet set;
        set.tensors.push_back(original);
        const auto path = temp_path(".npy");
        NpyWriter writer;
        ASSERT_TRUE(writer.write(path, set)) << writer.get_last_error();

        NpyReader reader;
        ASSERT_TRUE(reader.open(path)) << reader.get_last_error();
        ASSERT_EQ(reader.describe().size(), 1U);
        const auto back = reader.read("");
        ASSERT_TRUE(back.has_value()) << reader.get_last_error();
        EXPECT_EQ(back->spec().dtype, param.dtype);
        EXPECT_EQ(back->spec().shape, param.shape);
        EXPECT_TRUE(same_bytes(original, *back));
    }

    class TensorNpyTest : public TensorNpyFixture { };

    TEST_F(TensorNpyTest, TheWrittenHeaderIsAlignedAndSelfDescribing)
    {
        TensorSet set;
        set.tensors.push_back(*floats({ 2, 3 }, "w"));
        const auto path = temp_path(".npy");
        ASSERT_TRUE(NpyWriter {}.write(path, set));

        const auto file = read_file(path);
        ASSERT_GT(file.size(), 10U);
        EXPECT_EQ(static_cast<uint8_t>(file[0]), 0x93U);
        EXPECT_EQ(file.substr(1, 5), "NUMPY");
        EXPECT_EQ(file[6], 1);
        EXPECT_EQ(file[7], 0);

        const size_t length = static_cast<uint8_t>(file[8]) | (static_cast<size_t>(static_cast<uint8_t>(file[9])) << 8);
        EXPECT_EQ((10 + length) % 64, 0U);
        EXPECT_EQ(file.size(), 10 + length + 24);

        const auto header = file.substr(10, length);
        EXPECT_NE(header.find("'descr': '<f4'"), std::string::npos);
        EXPECT_NE(header.find("'fortran_order': False"), std::string::npos);
        EXPECT_NE(header.find("'shape': (2, 3)"), std::string::npos);
        EXPECT_EQ(header.back(), '\n');
    }

    TEST_F(TensorNpyTest, OneDimensionalAndScalarShapesUseNumpysSpelling)
    {
        std::vector<float> three { 1, 2, 3 };
        TensorSet vector_set;
        vector_set.tensors.push_back(*TensorData::from_values<float>(three, {}, "v"));
        const auto vector_path = temp_path(".npy");
        ASSERT_TRUE(NpyWriter {}.write(vector_path, vector_set));
        EXPECT_NE(read_file(vector_path).find("'shape': (3,)"), std::string::npos);

        TensorSet scalar_set;
        scalar_set.tensors.push_back(*TensorData::allocate(TensorSpec::contiguous(TensorDType::of<float>(), {}, "s")));
        const auto scalar_path = temp_path(".npy");
        ASSERT_TRUE(NpyWriter {}.write(scalar_path, scalar_set));
        EXPECT_NE(read_file(scalar_path).find("'shape': ()"), std::string::npos);
    }

    TEST_F(TensorNpyTest, VeryLargeHeadersUseVersionTwo)
    {
        TensorSet set;
        set.tensors.push_back(*TensorData::allocate(
            TensorSpec::contiguous(TensorDType::of<float>(), std::vector<uint64_t>(25000, 1), "deep")));
        const auto path = temp_path(".npy");
        ASSERT_TRUE(NpyWriter {}.write(path, set));
        EXPECT_EQ(read_file(path)[6], 2);

        NpyReader reader;
        ASSERT_TRUE(reader.open(path)) << reader.get_last_error();
        EXPECT_EQ(reader.describe().front().shape.size(), 25000U);
    }

    TEST_F(TensorNpyTest, StridedTensorsAreWrittenInLogicalCOrder)
    {
        auto layout = TensorSpec::contiguous(TensorDType::of<float>(), { 2, 3 }, "w");
        layout.strides = { 1, 2 };
        auto strided = TensorData::allocate(layout);
        ASSERT_TRUE(strided.has_value());
        ASSERT_TRUE(TensorInsertion(*strided).assign(*floats({ 2, 3 }, "w")->access()));

        TensorSet set;
        set.tensors.push_back(*strided);
        const auto path = temp_path(".npy");
        ASSERT_TRUE(NpyWriter {}.write(path, set));

        NpyReader reader;
        ASSERT_TRUE(reader.open(path));
        const auto back = reader.read("");
        ASSERT_TRUE(back.has_value());
        EXPECT_TRUE(back->spec().is_contiguous());
        EXPECT_EQ(floats_of(*back), (std::vector<float> { 0, 1, 2, 3, 4, 5 }));
    }

    TEST_F(TensorNpyTest, TheWriterRefusesWhatNumpyHasNoTypeFor)
    {
        const std::vector<TensorDType> unsupported {
            TensorDType::bfloat16(),
            { TensorDTypeKind::FLOAT8_E5M2, 8, 1 },
            { TensorDTypeKind::FLOAT4_E2M1FN, 4, 1 },
            { TensorDTypeKind::FLOAT, 32, 4 },
        };
        for (const auto& dtype : unsupported) {
            TensorSet set;
            set.tensors.push_back(*TensorData::allocate(TensorSpec::contiguous(dtype, { 8 }, "x")));
            NpyWriter writer;
            EXPECT_FALSE(writer.write(temp_path(".npy"), set)) << dtype.name();
            EXPECT_NE(writer.get_last_error().find("numpy has no type"), std::string::npos) << dtype.name();
        }
    }

    TEST_F(TensorNpyTest, TheWriterNeedsExactlyOneTensor)
    {
        NpyWriter empty_writer;
        EXPECT_FALSE(empty_writer.write(temp_path(".npy"), TensorSet {}));
        EXPECT_NE(empty_writer.get_last_error().find("exactly one"), std::string::npos);

        TensorSet two;
        two.tensors.push_back(*floats({ 1 }, "a"));
        two.tensors.push_back(*floats({ 1 }, "b"));
        NpyWriter two_writer;
        EXPECT_FALSE(two_writer.write(temp_path(".npy"), two));
        EXPECT_NE(two_writer.get_last_error().find("exactly one"), std::string::npos);
    }

    // ---------------------------------------------------------------------
    // .npy: reading files written elsewhere
    // ---------------------------------------------------------------------

    TEST_F(TensorNpyTest, TheTensorIsNamedAfterTheFileStem)
    {
        TensorSet set;
        set.tensors.push_back(*floats({ 2 }, "ignored"));
        const auto path = temp_path(".npy");
        ASSERT_TRUE(NpyWriter {}.write(path, set));

        NpyReader reader;
        ASSERT_TRUE(reader.open(path));
        const auto stem = fs::path(path).stem().string();
        EXPECT_EQ(reader.describe().front().name, stem);
        EXPECT_TRUE(reader.read(stem).has_value());
        EXPECT_TRUE(reader.read("").has_value());
        EXPECT_FALSE(reader.read("other").has_value());
        EXPECT_NE(reader.get_last_error().find("'other'"), std::string::npos);
    }

    TEST_F(TensorNpyTest, BigEndianFilesAreConvertedToTheHostsOrder)
    {
        const std::string one_and_two("\x3F\x80\x00\x00\x40\x00\x00\x00", 8);
        NpyReader floats_reader;
        ASSERT_TRUE(floats_reader.open(write_file(".npy", npy_bytes(npy_dict(">f4", "(2,)"), one_and_two))));
        EXPECT_EQ(floats_of(*floats_reader.read("")), (std::vector<float> { 1.0F, 2.0F }));

        NpyReader shorts_reader;
        ASSERT_TRUE(shorts_reader.open(write_file(".npy", npy_bytes(npy_dict(">u2", "(1,)"), std::string("\x01\x02", 2)))));
        const auto shorts = shorts_reader.read("");
        ASSERT_TRUE(shorts.has_value());
        EXPECT_EQ(shorts->to_vector<uint16_t>(), std::optional { std::vector<uint16_t> { 0x0102 } });
    }

    TEST_F(TensorNpyTest, FortranOrderKeepsDataInPlaceAndDescribesItWithStrides)
    {
        NpyReader reader;
        ASSERT_TRUE(reader.open(write_file(".npy",
            npy_bytes(npy_dict("<f4", "(2, 3)", "True"), float_bytes({ 0, 1, 2, 3, 4, 5 })))));

        const auto spec = reader.describe().front();
        EXPECT_EQ(spec.shape, (std::vector<uint64_t> { 2, 3 }));
        EXPECT_EQ(spec.strides, (std::vector<int64_t> { 1, 2 }));
        EXPECT_FALSE(spec.is_contiguous());

        const auto tensor = reader.read("");
        ASSERT_TRUE(tensor.has_value());
        const auto dense = TensorData::copy_of(*tensor->access());
        ASSERT_TRUE(dense.has_value());
        EXPECT_EQ(floats_of(*dense), (std::vector<float> { 0, 2, 4, 1, 3, 5 }));
    }

    TEST_F(TensorNpyTest, AllThreeFormatVersionsAreAccepted)
    {
        for (const uint8_t major : { uint8_t { 1 }, uint8_t { 2 }, uint8_t { 3 } }) {
            NpyReader reader;
            ASSERT_TRUE(reader.open(write_file(".npy",
                npy_bytes(npy_dict("<f4", "(2,)"), float_bytes({ 5.0F, 6.0F }), major))))
                << "version " << static_cast<int>(major) << ": " << reader.get_last_error();
            EXPECT_EQ(floats_of(*reader.read("")), (std::vector<float> { 5.0F, 6.0F }));
        }
    }

    TEST_F(TensorNpyTest, HeaderSpellingVariantsAreAccepted)
    {
        NpyReader double_quotes;
        ASSERT_TRUE(double_quotes.open(write_file(".npy",
            npy_bytes(R"({"descr": "<f4", "fortran_order": False, "shape": (1,), })", float_bytes({ 3.0F })))));
        EXPECT_EQ(floats_of(*double_quotes.read("")), (std::vector<float> { 3.0F }));

        NpyReader long_suffix;
        ASSERT_TRUE(long_suffix.open(write_file(".npy",
            npy_bytes(npy_dict("<f4", "(2L, 1L)"), float_bytes({ 1.0F, 2.0F })))));
        EXPECT_EQ(long_suffix.describe().front().shape, (std::vector<uint64_t> { 2, 1 }));

        NpyReader scalar;
        ASSERT_TRUE(scalar.open(write_file(".npy", npy_bytes(npy_dict("<f4", "()"), float_bytes({ 7.0F })))));
        EXPECT_TRUE(scalar.describe().front().shape.empty());
        EXPECT_EQ(floats_of(*scalar.read("")), (std::vector<float> { 7.0F }));
    }

    TEST_F(TensorNpyTest, TrailingBytesAfterTheDataAreIgnored)
    {
        NpyReader reader;
        ASSERT_TRUE(reader.open(write_file(".npy",
            npy_bytes(npy_dict("<f4", "(1,)"), float_bytes({ 4.0F, 99.0F })))));
        EXPECT_EQ(floats_of(*reader.read("")), (std::vector<float> { 4.0F }));
    }

    struct MalformedCase {
        const char* name;
        std::string content;
    };

    class TensorNpyMalformedTest : public TensorNpyFixture,
                                   public ::testing::WithParamInterface<MalformedCase> { };

    std::string bad_version()
    {
        std::string out(1, '\x93');
        out += "NUMPY";
        out.push_back('\x09');
        out.push_back('\0');
        out += std::string(8, '\0');
        return out;
    }

    std::string lying_length()
    {
        std::string out = npy_bytes(npy_dict("<f4", "(1,)"), "");
        out[8] = static_cast<char>(0xC8);
        out[9] = 0;
        return out;
    }

    INSTANTIATE_TEST_SUITE_P(Files, TensorNpyMalformedTest,
        ::testing::Values(
            MalformedCase { "NotNumpy", "this is plain text and not an array" },
            MalformedCase { "TooShort", std::string(5, 'x') },
            MalformedCase { "UnknownVersion", bad_version() },
            MalformedCase { "HeaderLengthPastTheFile", lying_length() },
            MalformedCase { "StructuredDtype",
                npy_bytes("{'descr': [('a', '<i4'), ('b', '<f4')], 'fortran_order': False, 'shape': (1,), }", std::string(8, '\0')) },
            MalformedCase { "ObjectDtype", npy_bytes(npy_dict("|O", "(1,)"), std::string(8, '\0')) },
            MalformedCase { "StringDtype", npy_bytes(npy_dict("|S4", "(1,)"), std::string(4, '\0')) },
            MalformedCase { "LongDouble", npy_bytes(npy_dict("<f16", "(1,)"), std::string(16, '\0')) },
            MalformedCase { "VoidDtype", npy_bytes(npy_dict("|V2", "(1,)"), std::string(2, '\0')) },
            MalformedCase { "DatetimeDtype", npy_bytes(npy_dict("<M8[s]", "(1,)"), std::string(8, '\0')) },
            MalformedCase { "MissingShape", npy_bytes("{'descr': '<f4', 'fortran_order': False, }", std::string(4, '\0')) },
            MalformedCase { "NegativeShape", npy_bytes(npy_dict("<f4", "(-1,)"), std::string(4, '\0')) },
            MalformedCase { "MissingFortranOrder", npy_bytes("{'descr': '<f4', 'shape': (1,), }", std::string(4, '\0')) },
            MalformedCase { "DataShorterThanTheShape", npy_bytes(npy_dict("<f4", "(4,)"), std::string(8, '\0')) }),
        [](const ::testing::TestParamInfo<MalformedCase>& info) { return std::string(info.param.name); });

    TEST_P(TensorNpyMalformedTest, OpeningFailsWithAReasonAndLeavesNothingOpen)
    {
        NpyReader reader;
        EXPECT_FALSE(reader.open(write_file(".npy", GetParam().content)));
        EXPECT_FALSE(reader.is_open());
        EXPECT_FALSE(reader.get_last_error().empty());
        EXPECT_TRUE(reader.describe().empty());
    }

    TEST_F(TensorNpyTest, ProbeAndCanReadRecogniseTheMagic)
    {
        const auto good = npy_bytes(npy_dict("<f4", "(1,)"), float_bytes({ 1.0F }));
        const auto as_bytes = [](const std::string& text) {
            return std::vector<std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                reinterpret_cast<const std::byte*>(text.data()) + text.size());
        };
        EXPECT_TRUE(NpyReader::probe(as_bytes(good), good.size()));
        EXPECT_FALSE(NpyReader::probe(as_bytes(std::string(32, 'x')), 32));
        EXPECT_FALSE(NpyReader::probe(as_bytes(good.substr(0, 6)), good.size()));

        NpyReader reader;
        EXPECT_TRUE(reader.can_read(temp_path(".npy")));
        EXPECT_TRUE(reader.can_read(write_file(".bin", good)));
        EXPECT_FALSE(reader.can_read(write_file(".txt", "plain text, nothing to see")));
    }

    TEST_F(TensorNpyTest, ReadingBeforeOpeningAndMissingFilesReportWhy)
    {
        NpyReader reader;
        EXPECT_FALSE(reader.read("").has_value());
        EXPECT_EQ(reader.get_last_error(), "no file is open");
        EXPECT_FALSE(reader.open(temp_path(".npy")));
        EXPECT_FALSE(reader.is_open());
        EXPECT_TRUE(reader.describe().empty());
    }

    // ---------------------------------------------------------------------
    // .npz
    // ---------------------------------------------------------------------

    class TensorNpzRoundTripTest : public TensorNpyFixture,
                                   public ::testing::WithParamInterface<DTypeCase> { };

    INSTANTIATE_TEST_SUITE_P(DTypes, TensorNpzRoundTripTest, ::testing::ValuesIn(numpy_cases()),
        [](const ::testing::TestParamInfo<DTypeCase>& info) { return std::string(info.param.name); });

    TEST_P(TensorNpzRoundTripTest, BytesShapeAndTypeSurviveAnArchiveTrip)
    {
        const auto& param = GetParam();
        const auto original = patterned(param, "group/t");

        TensorSet set;
        set.tensors.push_back(original);
        const auto path = temp_path(".npz");
        NpzWriter writer;
        ASSERT_TRUE(writer.write(path, set)) << writer.get_last_error();

        NpzReader reader;
        ASSERT_TRUE(reader.open(path)) << reader.get_last_error();
        ASSERT_EQ(reader.describe().size(), 1U);
        EXPECT_EQ(reader.describe().front().name, "group/t");
        const auto back = reader.read("group/t");
        ASSERT_TRUE(back.has_value()) << reader.get_last_error();
        EXPECT_EQ(back->spec().dtype, param.dtype);
        EXPECT_EQ(back->spec().shape, param.shape);
        EXPECT_TRUE(same_bytes(original, *back));
    }

    class TensorNpzTest : public TensorNpyFixture { };

    TEST_F(TensorNpzTest, SeveralTensorsKeepTheirOrderAndAreSelectable)
    {
        std::vector<int64_t> wide { 10, 20 };
        TensorSet set;
        set.tensors.push_back(*floats({ 2, 2 }, "a"));
        set.tensors.push_back(*TensorData::from_values<int64_t>(wide, {}, "layer/w"));
        set.tensors.push_back(*floats({ 3 }, "b"));

        const auto path = temp_path(".npz");
        ASSERT_TRUE(NpzWriter {}.write(path, set));

        NpzReader reader;
        ASSERT_TRUE(reader.open(path));
        const auto specs = reader.describe();
        ASSERT_EQ(specs.size(), 3U);
        EXPECT_EQ(specs[0].name, "a");
        EXPECT_EQ(specs[1].name, "layer/w");
        EXPECT_EQ(specs[2].name, "b");
        EXPECT_TRUE(reader.tensor_metadata().empty());

        const auto picked = reader.read_tensors(TensorReadOptions { .names = { "b", "layer/w" } });
        ASSERT_TRUE(picked.has_value());
        ASSERT_EQ(picked->tensors.size(), 2U);
        EXPECT_EQ(floats_of(picked->tensors[0]), (std::vector<float> { 0, 1, 2 }));
        const auto wide_view = picked->tensors[1].access();
        ASSERT_TRUE(wide_view.has_value());
        const auto wide_values = wide_view->as<int64_t>();
        EXPECT_EQ(std::vector<int64_t>(wide_values.begin(), wide_values.end()), wide);

        EXPECT_FALSE(reader.read_tensors(TensorReadOptions { .names = { "missing" } }).has_value());
        EXPECT_NE(reader.get_last_error().find("'missing'"), std::string::npos);
    }

    TEST_F(TensorNpzTest, EntriesAreStoredAndNamedAfterTheirTensors)
    {
        TensorSet set;
        set.tensors.push_back(*floats({ 4 }, "alpha"));
        set.tensors.push_back(*floats({ 2 }, "beta"));
        const auto path = temp_path(".npz");
        ASSERT_TRUE(NpzWriter {}.write(path, set));

        mz_zip_archive zip {};
        ASSERT_TRUE(mz_zip_reader_init_file(&zip, path.c_str(), 0));
        ASSERT_EQ(mz_zip_reader_get_num_files(&zip), 2U);
        std::vector<std::string> names;
        for (mz_uint i = 0; i < 2; ++i) {
            mz_zip_archive_file_stat stat {};
            ASSERT_TRUE(mz_zip_reader_file_stat(&zip, i, &stat));
            names.emplace_back(stat.m_filename);
            EXPECT_EQ(stat.m_method, 0U);
        }
        mz_zip_reader_end(&zip);
        EXPECT_EQ(names, (std::vector<std::string> { "alpha.npy", "beta.npy" }));
    }

    TEST_F(TensorNpzTest, CompressedArchivesFromOtherWritersAreRead)
    {
        std::vector<float> zeros(4096, 0.0F);
        zeros[100] = 8.0F;
        TensorSet set;
        set.tensors.push_back(*TensorData::from_values<float>(zeros, {}, "z"));
        const auto npy_path = temp_path(".npy");
        ASSERT_TRUE(NpyWriter {}.write(npy_path, set));

        const auto path = temp_path(".npz");
        ASSERT_TRUE(write_zip(path, { { "z.npy", read_file(npy_path) } }, MZ_BEST_COMPRESSION));
        EXPECT_LT(fs::file_size(path), fs::file_size(npy_path));

        NpzReader reader;
        ASSERT_TRUE(reader.open(path)) << reader.get_last_error();
        ASSERT_EQ(reader.describe().size(), 1U);
        EXPECT_EQ(reader.describe().front().shape, (std::vector<uint64_t> { 4096 }));
        const auto back = reader.read("z");
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(floats_of(*back), zeros);
    }

    TEST_F(TensorNpzTest, FortranAndBigEndianEntriesAreHandledLikeStandaloneFiles)
    {
        const auto path = temp_path(".npz");
        ASSERT_TRUE(write_zip(path, {
                                        { "f.npy", npy_bytes(npy_dict("<f4", "(2, 3)", "True"), float_bytes({ 0, 1, 2, 3, 4, 5 })) },
                                        { "b.npy", npy_bytes(npy_dict(">f4", "(2,)"), std::string("\x3F\x80\x00\x00\x40\x00\x00\x00", 8)) },
                                    }));

        NpzReader reader;
        ASSERT_TRUE(reader.open(path));
        EXPECT_EQ(reader.find_spec("f")->strides, (std::vector<int64_t> { 1, 2 }));
        EXPECT_EQ(floats_of(*reader.read("b")), (std::vector<float> { 1.0F, 2.0F }));
    }

    TEST_F(TensorNpzTest, EntriesThatAreNotArraysOrNotReadableAreSkipped)
    {
        const auto good = npy_bytes(npy_dict("<f4", "(1,)"), float_bytes({ 2.0F }));
        const auto path = temp_path(".npz");
        ASSERT_TRUE(write_zip(path, {
                                        { "notes.txt", "not an array" },
                                        { "object.npy", npy_bytes(npy_dict("|O", "(1,)"), std::string(8, '\0')) },
                                        { "good.npy", good },
                                        { "short.npy", npy_bytes(npy_dict("<f4", "(9,)"), std::string(4, '\0')) },
                                    }));

        NpzReader reader;
        ASSERT_TRUE(reader.open(path)) << reader.get_last_error();
        const auto specs = reader.describe();
        ASSERT_EQ(specs.size(), 1U);
        EXPECT_EQ(specs[0].name, "good");
        EXPECT_EQ(floats_of(*reader.read("good")), (std::vector<float> { 2.0F }));
    }

    TEST_F(TensorNpzTest, AnArchiveWithoutArraysOpensEmpty)
    {
        const auto path = temp_path(".npz");
        ASSERT_TRUE(write_zip(path, { { "readme.txt", "hello" } }));

        NpzReader reader;
        ASSERT_TRUE(reader.open(path));
        EXPECT_TRUE(reader.describe().empty());
        const auto all = reader.read_tensors();
        ASSERT_TRUE(all.has_value());
        EXPECT_TRUE(all->tensors.empty());
    }

    TEST_F(TensorNpzTest, NonArchivesAndMissingFilesDoNotOpen)
    {
        NpzReader reader;
        EXPECT_FALSE(reader.open(write_file(".npz", "this is not a zip file")));
        EXPECT_NE(reader.get_last_error().find("cannot open archive"), std::string::npos);
        EXPECT_FALSE(reader.is_open());
        EXPECT_FALSE(reader.open(temp_path(".npz")));
        EXPECT_FALSE(reader.read("a").has_value());
    }

    TEST_F(TensorNpzTest, OpeningAnotherArchiveReplacesTheFirst)
    {
        TensorSet first;
        first.tensors.push_back(*floats({ 1 }, "one"));
        TensorSet second;
        second.tensors.push_back(*floats({ 1 }, "two"));
        const auto first_path = temp_path(".npz");
        const auto second_path = temp_path(".npz");
        ASSERT_TRUE(NpzWriter {}.write(first_path, first));
        ASSERT_TRUE(NpzWriter {}.write(second_path, second));

        NpzReader reader;
        ASSERT_TRUE(reader.open(first_path));
        ASSERT_TRUE(reader.open(second_path));
        ASSERT_EQ(reader.describe().size(), 1U);
        EXPECT_EQ(reader.describe().front().name, "two");
        reader.close();
        EXPECT_FALSE(reader.is_open());
        EXPECT_TRUE(reader.describe().empty());
    }

    TEST_F(TensorNpzTest, TheWriterRefusesTheWholeSetForOneBadTensor)
    {
        TensorSet unsupported;
        unsupported.tensors.push_back(*floats({ 2 }, "fine"));
        unsupported.tensors.push_back(*TensorData::allocate(
            TensorSpec::contiguous(TensorDType::bfloat16(), { 2 }, "brain")));
        NpzWriter first;
        EXPECT_FALSE(first.write(temp_path(".npz"), unsupported));
        EXPECT_NE(first.get_last_error().find("numpy has no type"), std::string::npos);

        TensorSet duplicate;
        duplicate.tensors.push_back(*floats({ 1 }, "a"));
        duplicate.tensors.push_back(*floats({ 1 }, "a"));
        NpzWriter second;
        EXPECT_FALSE(second.write(temp_path(".npz"), duplicate));
        EXPECT_FALSE(second.get_last_error().empty());
    }

    TEST_F(TensorNpzTest, FileReaderSurfaceReportsTheFormat)
    {
        TensorSet set;
        set.tensors.push_back(*floats({ 2 }, "a"));
        const auto path = temp_path(".npz");
        ASSERT_TRUE(NpzWriter {}.write(path, set));

        NpzReader reader;
        ASSERT_TRUE(reader.open(path));
        const auto metadata = reader.get_metadata();
        ASSERT_TRUE(metadata.has_value());
        EXPECT_EQ(metadata->format, "npz");
        EXPECT_EQ(metadata->file_size, fs::file_size(path));
        EXPECT_TRUE(reader.can_read(temp_path(".npz")));
        EXPECT_FALSE(reader.can_read(temp_path(".npy")));
    }

    // ---------------------------------------------------------------------
    // Registry with all three formats
    // ---------------------------------------------------------------------

    class TensorNumpyRegistryTest : public TensorNpyFixture {
    protected:
        void SetUp() override
        {
            SafetensorsReader::register_with_registry();
            NpyReader::register_with_registry();
            NpzReader::register_with_registry();
        }

        std::string written(const std::string& suffix)
        {
            TensorSet set;
            set.tensors.push_back(*floats({ 3 }, "a"));
            const auto path = temp_path(suffix);
            auto writer = TensorFormatRegistry::instance().create_writer(path);
            EXPECT_NE(writer, nullptr);
            EXPECT_TRUE(writer && writer->write(path, set));
            return path;
        }
    };

    TEST_F(TensorNumpyRegistryTest, EachFormatIsFoundByNameAndExtension)
    {
        const auto& registry = TensorFormatRegistry::instance();
        for (const auto* name : { "safetensors", "npy", "npz" }) {
            const auto* format = registry.find(name);
            ASSERT_NE(format, nullptr) << name;
            EXPECT_TRUE(static_cast<bool>(format->make_reader)) << name;
            EXPECT_TRUE(static_cast<bool>(format->make_writer)) << name;
            EXPECT_NE(registry.create_writer(std::string("out.") + name), nullptr) << name;
            EXPECT_NE(registry.create_writer(std::string("OUT.") + name), nullptr) << name;
        }
    }

    TEST_F(TensorNumpyRegistryTest, ContentSeparatesTheFormatsWhateverTheExtension)
    {
        const auto& registry = TensorFormatRegistry::instance();

        TensorSet set;
        set.tensors.push_back(*floats({ 3 }, "a"));
        const auto npy_path = temp_path(".npy");
        const auto safetensors_path = temp_path(".safetensors");
        ASSERT_TRUE(NpyWriter {}.write(npy_path, set));
        ASSERT_TRUE(SafetensorsWriter {}.write(safetensors_path, set));

        const auto npy_renamed = write_file(".dat", read_file(npy_path));
        const auto safetensors_renamed = write_file(".dat", read_file(safetensors_path));

        ASSERT_NE(registry.detect(npy_renamed), nullptr);
        EXPECT_EQ(registry.detect(npy_renamed)->name, "npy");
        ASSERT_NE(registry.detect(safetensors_renamed), nullptr);
        EXPECT_EQ(registry.detect(safetensors_renamed)->name, "safetensors");
    }

    TEST_F(TensorNumpyRegistryTest, ZipContainersAreChosenByExtensionNotContent)
    {
        const auto& registry = TensorFormatRegistry::instance();
        const auto path = written(".npz");
        ASSERT_NE(registry.detect(path), nullptr);
        EXPECT_EQ(registry.detect(path)->name, "npz");

        const auto renamed = write_file(".bin", read_file(path));
        EXPECT_EQ(registry.detect(renamed), nullptr);
        EXPECT_NE(registry.create_reader(renamed, "npz"), nullptr);
    }

    TEST_F(TensorNumpyRegistryTest, WritersAndReadersFromTheRegistryRoundTripEveryFormat)
    {
        for (const auto* suffix : { ".safetensors", ".npy", ".npz" }) {
            const auto path = written(suffix);
            auto reader = TensorFormatRegistry::instance().create_reader(path);
            ASSERT_NE(reader, nullptr) << suffix;
            ASSERT_TRUE(reader->open(path)) << suffix << ": " << reader->get_last_error();
            const auto all = reader->read_tensors();
            ASSERT_TRUE(all.has_value()) << suffix;
            ASSERT_EQ(all->tensors.size(), 1U) << suffix;
            EXPECT_EQ(floats_of(all->tensors.front()), (std::vector<float> { 0, 1, 2 })) << suffix;
        }
    }

} // namespace
} // namespace MayaFlux::Test
