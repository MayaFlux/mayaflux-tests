#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/IO/Tensor/RawTensorReader.hpp"
#include "MayaFlux/IO/Tensor/SafetensorsFormat.hpp"
#include "MayaFlux/IO/Tensor/TensorFormat.hpp"
#include "MayaFlux/Kakshya/NDData/TensorInsertion.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {
namespace {

    namespace fs = std::filesystem;

    using IO::RawTensorEntry;
    using IO::RawTensorReader;
    using IO::SafetensorsReader;
    using IO::SafetensorsWriter;
    using IO::TensorFormat;
    using IO::TensorFormatRegistry;
    using IO::TensorReadOptions;
    using IO::TensorReader;
    using Kakshya::TensorData;
    using Kakshya::TensorDType;
    using Kakshya::TensorDTypeKind;
    using Kakshya::TensorInsertion;
    using Kakshya::TensorSet;
    using Kakshya::TensorSpec;

    class TensorIoFixture : public ::testing::Test {
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
                / ("mayaflux_tensor_io_" + std::to_string(stamp) + "_" + std::to_string(m_counter++) + suffix);
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

    private:
        std::vector<fs::path> m_paths;
        int m_counter {};
    };

    std::string le64(uint64_t value)
    {
        std::string out;
        for (size_t i = 0; i < 8; ++i)
            out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
        return out;
    }

    std::string safetensors_bytes(const std::string& header, const std::string& data = {})
    {
        return le64(header.size()) + header + data;
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

    std::string read_file(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
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

    // ---------------------------------------------------------------------
    // Safetensors: round trips
    // ---------------------------------------------------------------------

    struct DTypeCase {
        const char* name;
        TensorDType dtype;
        std::vector<uint64_t> shape;
    };

    class TensorSafetensorsRoundTripTest : public TensorIoFixture,
                                           public ::testing::WithParamInterface<DTypeCase> { };

    INSTANTIATE_TEST_SUITE_P(DTypes, TensorSafetensorsRoundTripTest,
        ::testing::Values(
            DTypeCase { "F32", TensorDType::of<float>(), { 2, 3 } },
            DTypeCase { "F64", TensorDType::of<double>(), { 3 } },
            DTypeCase { "F16", TensorDType::float16(), { 4 } },
            DTypeCase { "BF16", TensorDType::bfloat16(), { 2, 2 } },
            DTypeCase { "I8", TensorDType::of<int8_t>(), { 5 } },
            DTypeCase { "I16", TensorDType::of<int16_t>(), { 3 } },
            DTypeCase { "I32", TensorDType::of<int32_t>(), { 3 } },
            DTypeCase { "I64", TensorDType::of<int64_t>(), { 2, 2 } },
            DTypeCase { "U8", TensorDType::of<uint8_t>(), { 7 } },
            DTypeCase { "U16", TensorDType::of<uint16_t>(), { 3 } },
            DTypeCase { "U32", TensorDType::of<uint32_t>(), { 3 } },
            DTypeCase { "U64", TensorDType::of<uint64_t>(), { 2 } },
            DTypeCase { "Bool", TensorDType::of<bool>(), { 6 } },
            DTypeCase { "Complex64", TensorDType::of<std::complex<float>>(), { 3 } },
            DTypeCase { "Float8E4M3FN", { TensorDTypeKind::FLOAT8_E4M3FN, 8, 1 }, { 4 } },
            DTypeCase { "Float8E5M2", { TensorDTypeKind::FLOAT8_E5M2, 8, 1 }, { 4 } },
            DTypeCase { "Float8E8M0", { TensorDTypeKind::FLOAT8_E8M0FNU, 8, 1 }, { 4 } },
            DTypeCase { "Float6", { TensorDTypeKind::FLOAT6_E2M3FN, 6, 1 }, { 4 } },
            DTypeCase { "Float4", { TensorDTypeKind::FLOAT4_E2M1FN, 4, 1 }, { 8 } },
            DTypeCase { "Scalar", TensorDType::of<float>(), {} },
            DTypeCase { "Empty", TensorDType::of<float>(), { 0, 4 } }),
        [](const ::testing::TestParamInfo<DTypeCase>& info) { return std::string(info.param.name); });

    TEST_P(TensorSafetensorsRoundTripTest, BytesShapeAndTypeSurviveAFileTrip)
    {
        const auto& param = GetParam();
        const auto spec = TensorSpec::contiguous(param.dtype, param.shape, "t");
        const auto count = spec.element_count().value_or(0);
        std::vector<std::byte> bytes(param.dtype.storage_bytes(count).value_or(0));
        for (size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<std::byte>((i * 7 + 3) % 251);

        auto original = TensorData::from_bytes(spec, bytes);
        ASSERT_TRUE(original.has_value());

        TensorSet set;
        set.tensors.push_back(*original);
        const auto path = temp_path(".safetensors");
        SafetensorsWriter writer;
        ASSERT_TRUE(writer.write(path, set)) << writer.get_last_error();

        SafetensorsReader reader;
        ASSERT_TRUE(reader.open(path)) << reader.get_last_error();
        const auto back = reader.read("t");
        ASSERT_TRUE(back.has_value()) << reader.get_last_error();
        EXPECT_EQ(back->spec().dtype, param.dtype);
        EXPECT_EQ(back->spec().shape, param.shape);
        EXPECT_TRUE(same_bytes(*original, *back));
    }

    class TensorSafetensorsTest : public TensorIoFixture { };

    TEST_F(TensorSafetensorsTest, MetadataTravelsWithTheSet)
    {
        TensorSet set;
        set.tensors.push_back(*floats({ 2 }, "w"));
        set.metadata["format"] = "pt";
        set.metadata["author"] = "test";

        const auto path = temp_path(".safetensors");
        ASSERT_TRUE(SafetensorsWriter {}.write(path, set));

        SafetensorsReader reader;
        ASSERT_TRUE(reader.open(path));
        EXPECT_EQ(reader.tensor_metadata(), set.metadata);
        const auto back = reader.read_tensors();
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(back->metadata, set.metadata);
    }

    TEST_F(TensorSafetensorsTest, WiderElementsComeFirstAndTheIndexIsEightByteAligned)
    {
        TensorSet set;
        std::vector<uint8_t> narrow { 1, 2, 3 };
        std::vector<double> wide { 1.0, 2.0 };
        set.tensors.push_back(*TensorData::from_values<uint8_t>(narrow, {}, "a"));
        set.tensors.push_back(*TensorData::from_values<double>(wide, {}, "b"));
        set.tensors.push_back(*floats({ 1 }, "c"));

        const auto path = temp_path(".safetensors");
        ASSERT_TRUE(SafetensorsWriter {}.write(path, set));

        SafetensorsReader reader;
        ASSERT_TRUE(reader.open(path));
        const auto specs = reader.describe();
        ASSERT_EQ(specs.size(), 3U);
        EXPECT_EQ(specs[0].name, "b");
        EXPECT_EQ(specs[1].name, "c");
        EXPECT_EQ(specs[2].name, "a");

        const auto file = read_file(path);
        uint64_t header_size = 0;
        for (size_t i = 0; i < 8; ++i)
            header_size |= static_cast<uint64_t>(static_cast<uint8_t>(file[i])) << (8 * i);
        EXPECT_EQ(header_size % 8, 0U);
    }

    TEST_F(TensorSafetensorsTest, StridedTensorsAreWrittenInLogicalCOrder)
    {
        auto layout = TensorSpec::contiguous(TensorDType::of<float>(), { 2, 3 }, "w");
        layout.strides = { 1, 2 };
        auto strided = TensorData::allocate(layout);
        ASSERT_TRUE(strided.has_value());
        const auto source = floats({ 2, 3 }, "w");
        ASSERT_TRUE(TensorInsertion(*strided).assign(*source->access()));

        TensorSet set;
        set.tensors.push_back(*strided);
        const auto path = temp_path(".safetensors");
        ASSERT_TRUE(SafetensorsWriter {}.write(path, set));

        SafetensorsReader reader;
        ASSERT_TRUE(reader.open(path));
        const auto back = reader.read("w");
        ASSERT_TRUE(back.has_value());
        EXPECT_TRUE(back->spec().strides.empty());
        EXPECT_EQ(floats_of(*back), (std::vector<float> { 0, 1, 2, 3, 4, 5 }));
    }

    TEST_F(TensorSafetensorsTest, TheWriterRefusesWhatTheFormatCannotName)
    {
        auto no_name = TensorData::allocate(
            TensorSpec::contiguous({ TensorDTypeKind::FLOAT8_E4M3, 8, 1 }, { 2 }, "x"));
        auto wide_lane = TensorData::allocate(
            TensorSpec::contiguous({ TensorDTypeKind::FLOAT, 32, 4 }, { 2 }, "y"));
        ASSERT_TRUE(no_name.has_value());
        ASSERT_TRUE(wide_lane.has_value());

        for (const auto* tensor : { &*no_name, &*wide_lane }) {
            TensorSet set;
            set.tensors.push_back(*tensor);
            SafetensorsWriter writer;
            EXPECT_FALSE(writer.write(temp_path(".safetensors"), set));
            EXPECT_NE(writer.get_last_error().find("cannot store"), std::string::npos);
        }
    }

    TEST_F(TensorSafetensorsTest, TheWriterRefusesDuplicateAndUnnamedTensors)
    {
        TensorSet duplicate;
        duplicate.tensors.push_back(*floats({ 1 }, "a"));
        duplicate.tensors.push_back(*floats({ 1 }, "a"));
        SafetensorsWriter first;
        EXPECT_FALSE(first.write(temp_path(".safetensors"), duplicate));
        EXPECT_FALSE(first.get_last_error().empty());

        TensorSet unnamed;
        unnamed.tensors.push_back(*floats({ 1 }, ""));
        SafetensorsWriter second;
        EXPECT_FALSE(second.write(temp_path(".safetensors"), unnamed));
    }

    // ---------------------------------------------------------------------
    // Safetensors: reading
    // ---------------------------------------------------------------------

    TEST_F(TensorSafetensorsTest, OpeningListsTensorsWithoutReadingTheirData)
    {
        TensorSet set;
        set.tensors.push_back(*floats({ 2, 3 }, "w"));
        set.tensors.push_back(*floats({ 4 }, "b"));
        const auto path = temp_path(".safetensors");
        ASSERT_TRUE(SafetensorsWriter {}.write(path, set));

        SafetensorsReader reader;
        ASSERT_TRUE(reader.open(path));
        EXPECT_TRUE(reader.is_open());
        ASSERT_EQ(reader.describe().size(), 2U);
        const auto spec = reader.find_spec("w");
        ASSERT_TRUE(spec.has_value());
        EXPECT_EQ(spec->shape, (std::vector<uint64_t> { 2, 3 }));
        EXPECT_TRUE(spec->dtype.is<float>());
        EXPECT_FALSE(reader.find_spec("nope").has_value());
    }

    TEST_F(TensorSafetensorsTest, ReadTensorsHonoursTheRequestedNamesAndOrder)
    {
        TensorSet set;
        set.tensors.push_back(*floats({ 1 }, "a"));
        set.tensors.push_back(*floats({ 2 }, "b"));
        set.tensors.push_back(*floats({ 3 }, "c"));
        const auto path = temp_path(".safetensors");
        ASSERT_TRUE(SafetensorsWriter {}.write(path, set));

        SafetensorsReader reader;
        ASSERT_TRUE(reader.open(path));
        const auto picked = reader.read_tensors(TensorReadOptions { .names = { "c", "a" } });
        ASSERT_TRUE(picked.has_value());
        ASSERT_EQ(picked->tensors.size(), 2U);
        EXPECT_EQ(picked->tensors[0].spec().name, "c");
        EXPECT_EQ(picked->tensors[1].spec().name, "a");

        EXPECT_FALSE(reader.read_tensors(TensorReadOptions { .names = { "a", "missing" } }).has_value());
        EXPECT_NE(reader.get_last_error().find("missing"), std::string::npos);
    }

    TEST_F(TensorSafetensorsTest, AnUnknownDtypeIsSkippedAndTheRestStayReadable)
    {
        const std::string header = R"({"x":{"dtype":"Q99","shape":[1],"data_offsets":[0,4]},)"
                                   R"("y":{"dtype":"F32","shape":[1],"data_offsets":[4,8]}})";
        const auto path = write_file(".safetensors", safetensors_bytes(header, float_bytes({ 9.0F, 1.5F })));

        SafetensorsReader reader;
        ASSERT_TRUE(reader.open(path)) << reader.get_last_error();
        const auto specs = reader.describe();
        ASSERT_EQ(specs.size(), 1U);
        EXPECT_EQ(specs[0].name, "y");
        const auto y = reader.read("y");
        ASSERT_TRUE(y.has_value());
        EXPECT_EQ(floats_of(*y), (std::vector<float> { 1.5F }));
    }

    TEST_F(TensorSafetensorsTest, ReadingBeforeOpeningOrMissingNamesReportsWhy)
    {
        SafetensorsReader reader;
        EXPECT_FALSE(reader.read("a").has_value());
        EXPECT_EQ(reader.get_last_error(), "no file is open");
        EXPECT_FALSE(reader.read_tensors().has_value());

        const auto path = write_file(".safetensors",
            safetensors_bytes(R"({"a":{"dtype":"F32","shape":[1],"data_offsets":[0,4]}})", float_bytes({ 1.0F })));
        ASSERT_TRUE(reader.open(path));
        EXPECT_FALSE(reader.read("b").has_value());
        EXPECT_NE(reader.get_last_error().find("'b'"), std::string::npos);
    }

    TEST_F(TensorSafetensorsTest, OpeningAnotherFileReplacesTheFirst)
    {
        const auto first = write_file(".safetensors",
            safetensors_bytes(R"({"a":{"dtype":"F32","shape":[1],"data_offsets":[0,4]}})", float_bytes({ 1.0F })));
        const auto second = write_file(".safetensors",
            safetensors_bytes(R"({"b":{"dtype":"F32","shape":[1],"data_offsets":[0,4]}})", float_bytes({ 2.0F })));

        SafetensorsReader reader;
        ASSERT_TRUE(reader.open(first));
        ASSERT_TRUE(reader.open(second));
        ASSERT_EQ(reader.describe().size(), 1U);
        EXPECT_EQ(reader.describe()[0].name, "b");
        reader.close();
        EXPECT_FALSE(reader.is_open());
        EXPECT_TRUE(reader.describe().empty());
    }

    TEST_F(TensorSafetensorsTest, FileReaderSurfaceReportsFormatAndRefusesContainers)
    {
        const auto path = write_file(".safetensors",
            safetensors_bytes(R"({"a":{"dtype":"F32","shape":[1],"data_offsets":[0,4]}})", float_bytes({ 1.0F })));
        SafetensorsReader reader;
        EXPECT_FALSE(reader.get_metadata().has_value());
        ASSERT_TRUE(reader.open(path));

        const auto metadata = reader.get_metadata();
        ASSERT_TRUE(metadata.has_value());
        EXPECT_EQ(metadata->format, "safetensors");
        EXPECT_EQ(metadata->file_size, fs::file_size(path));

        EXPECT_TRUE(reader.read_all().empty());
        EXPECT_FALSE(reader.get_last_error().empty());
        EXPECT_EQ(reader.create_container(), nullptr);
        EXPECT_FALSE(reader.load_into_container(nullptr));
        EXPECT_TRUE(reader.get_regions().empty());
        EXPECT_EQ(reader.get_data_type(), std::type_index(typeid(Kakshya::TensorSet)));
    }

    TEST_F(TensorSafetensorsTest, MissingFilesDoNotOpen)
    {
        SafetensorsReader reader;
        EXPECT_FALSE(reader.open(temp_path(".safetensors")));
        EXPECT_FALSE(reader.is_open());
    }

    struct MalformedCase {
        const char* name;
        std::string content;
    };

    class TensorSafetensorsMalformedTest : public TensorIoFixture,
                                           public ::testing::WithParamInterface<MalformedCase> { };

    INSTANTIATE_TEST_SUITE_P(Files, TensorSafetensorsMalformedTest,
        ::testing::Values(
            MalformedCase { "ShorterThanThePrefix", std::string(4, '\x01') },
            MalformedCase { "ZeroHeaderSize", std::string(16, '\0') },
            MalformedCase { "HeaderLargerThanTheFile", le64(1000) + "{}" },
            MalformedCase { "HeaderIsNotJson", safetensors_bytes("not json at all") },
            MalformedCase { "HeaderIsNotAnObject", safetensors_bytes("[1,2]") },
            MalformedCase { "TensorIsNotAnObject", safetensors_bytes(R"({"t":3})") },
            MalformedCase { "MissingFields", safetensors_bytes(R"({"t":{"dtype":"F32"}})") },
            MalformedCase { "NegativeExtent",
                safetensors_bytes(R"({"t":{"dtype":"F32","shape":[-1],"data_offsets":[0,4]}})", float_bytes({ 1.0F })) },
            MalformedCase { "OffsetsDisagreeWithShape",
                safetensors_bytes(R"({"t":{"dtype":"F32","shape":[2],"data_offsets":[0,4]}})", float_bytes({ 1.0F })) },
            MalformedCase { "OffsetsPastTheData",
                safetensors_bytes(R"({"t":{"dtype":"F32","shape":[2],"data_offsets":[0,8]}})", float_bytes({ 1.0F })) },
            MalformedCase { "ReversedOffsets",
                safetensors_bytes(R"({"t":{"dtype":"F32","shape":[1],"data_offsets":[4,0]}})", float_bytes({ 1.0F, 2.0F })) },
            MalformedCase { "MetadataValueIsNotAString",
                safetensors_bytes(R"({"__metadata__":{"k":1}})") }),
        [](const ::testing::TestParamInfo<MalformedCase>& info) { return std::string(info.param.name); });

    TEST_P(TensorSafetensorsMalformedTest, OpeningFailsWithAReasonAndLeavesNothingOpen)
    {
        const auto path = write_file(".safetensors", GetParam().content);
        SafetensorsReader reader;
        EXPECT_FALSE(reader.open(path));
        EXPECT_FALSE(reader.is_open());
        EXPECT_FALSE(reader.get_last_error().empty());
        EXPECT_TRUE(reader.describe().empty());
    }

    TEST_F(TensorSafetensorsTest, ProbeRecognisesTheHeaderShape)
    {
        const auto good = safetensors_bytes(R"({"a":{"dtype":"F32","shape":[1],"data_offsets":[0,4]}})", float_bytes({ 1.0F }));
        const auto as_bytes = [](const std::string& text) {
            return std::vector<std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                reinterpret_cast<const std::byte*>(text.data()) + text.size());
        };

        EXPECT_TRUE(SafetensorsReader::probe(as_bytes(good), good.size()));
        EXPECT_FALSE(SafetensorsReader::probe(as_bytes(good.substr(0, 5)), good.size()));
        EXPECT_FALSE(SafetensorsReader::probe(as_bytes(good), 20));
        EXPECT_FALSE(SafetensorsReader::probe(as_bytes(std::string(32, 'x')), 32));
        EXPECT_FALSE(SafetensorsReader::probe(as_bytes(le64(0) + "{}"), 10));

        std::string no_brace = good;
        no_brace[8] = '[';
        EXPECT_FALSE(SafetensorsReader::probe(as_bytes(no_brace), no_brace.size()));
    }

    TEST_F(TensorSafetensorsTest, CanReadByExtensionOrByContent)
    {
        const auto good = safetensors_bytes(R"({"a":{"dtype":"F32","shape":[1],"data_offsets":[0,4]}})", float_bytes({ 1.0F }));
        SafetensorsReader reader;

        EXPECT_TRUE(reader.can_read(temp_path(".safetensors")));
        EXPECT_TRUE(reader.can_read(write_file(".bin", good)));
        EXPECT_FALSE(reader.can_read(write_file(".txt", "plain text, nothing to see")));
    }

    // ---------------------------------------------------------------------
    // Registry
    // ---------------------------------------------------------------------

    class TensorRegistryTest : public TensorIoFixture {
    protected:
        void SetUp() override { SafetensorsReader::register_with_registry(); }

        std::string good_file(const std::string& suffix)
        {
            return write_file(suffix,
                safetensors_bytes(R"({"a":{"dtype":"F32","shape":[1],"data_offsets":[0,4]}})", float_bytes({ 1.0F })));
        }
    };

    TEST_F(TensorRegistryTest, RegisteredFormatsAreFoundByName)
    {
        const auto& registry = TensorFormatRegistry::instance();
        const auto* format = registry.find("safetensors");
        ASSERT_NE(format, nullptr);
        EXPECT_EQ(format->extensions, (std::vector<std::string> { "safetensors" }));
        EXPECT_TRUE(static_cast<bool>(format->make_reader));
        EXPECT_TRUE(static_cast<bool>(format->make_writer));
        EXPECT_EQ(registry.find("no-such-format"), nullptr);

        const auto names = registry.format_names();
        EXPECT_NE(std::ranges::find(names, "safetensors"), names.end());
    }

    TEST_F(TensorRegistryTest, ContentIdentifiesAFileWithoutAnExtension)
    {
        const auto path = good_file(".bin");
        const auto* format = TensorFormatRegistry::instance().detect(path);
        ASSERT_NE(format, nullptr);
        EXPECT_EQ(format->name, "safetensors");

        auto reader = TensorFormatRegistry::instance().create_reader(path);
        ASSERT_NE(reader, nullptr);
        EXPECT_TRUE(reader->open(path));
        EXPECT_EQ(reader->format_name(), "safetensors");
    }

    TEST_F(TensorRegistryTest, TheExtensionDecidesWhenContentIsUnrecognised)
    {
        const auto path = write_file(".safetensors", "this is not a tensor file at all");
        const auto* format = TensorFormatRegistry::instance().detect(path);
        ASSERT_NE(format, nullptr);
        EXPECT_EQ(format->name, "safetensors");

        auto reader = TensorFormatRegistry::instance().create_reader(path);
        ASSERT_NE(reader, nullptr);
        EXPECT_FALSE(reader->open(path));
    }

    TEST_F(TensorRegistryTest, UnrecognisedFilesHaveNoFormat)
    {
        const auto path = write_file(".txt", "plain text, nothing to see");
        EXPECT_EQ(TensorFormatRegistry::instance().detect(path), nullptr);
        EXPECT_EQ(TensorFormatRegistry::instance().create_reader(path), nullptr);
    }

    TEST_F(TensorRegistryTest, AnExplicitNameOverridesDetection)
    {
        const auto path = write_file(".txt", "plain text, nothing to see");
        EXPECT_NE(TensorFormatRegistry::instance().create_reader(path, "safetensors"), nullptr);
        EXPECT_EQ(TensorFormatRegistry::instance().create_reader(path, "no-such-format"), nullptr);
    }

    TEST_F(TensorRegistryTest, WritersAreChosenByNameOrExtension)
    {
        const auto& registry = TensorFormatRegistry::instance();
        EXPECT_NE(registry.create_writer("out.safetensors"), nullptr);
        EXPECT_NE(registry.create_writer("OUT.SAFETENSORS"), nullptr);
        EXPECT_EQ(registry.create_writer("out.unknown"), nullptr);
        EXPECT_NE(registry.create_writer("out.unknown", "safetensors"), nullptr);
        EXPECT_EQ(registry.create_writer("out.safetensors", "no-such-format"), nullptr);
    }

    TEST_F(TensorRegistryTest, AWriterAndReaderFromTheRegistryRoundTrip)
    {
        TensorSet set;
        set.tensors.push_back(*floats({ 2, 2 }, "w"));
        const auto path = temp_path(".safetensors");

        auto writer = TensorFormatRegistry::instance().create_writer(path);
        ASSERT_NE(writer, nullptr);
        ASSERT_TRUE(writer->write(path, set));

        auto reader = TensorFormatRegistry::instance().create_reader(path);
        ASSERT_NE(reader, nullptr);
        ASSERT_TRUE(reader->open(path));
        const auto back = reader->read_tensors();
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(floats_of(back->tensors.at(0)), (std::vector<float> { 0, 1, 2, 3 }));
    }

    TEST_F(TensorRegistryTest, ANewFormatPlugsInWithoutChangingAnythingElse)
    {
        const std::string magic = "MFTENSORTEST";
        auto& registry = TensorFormatRegistry::instance();

        TensorFormat custom;
        custom.name = "tensor_test_format";
        custom.extensions = { "mftt" };
        custom.probe = [magic](std::span<const std::byte> head, uint64_t) {
            return head.size() >= magic.size() && std::memcmp(head.data(), magic.data(), magic.size()) == 0;
        };
        custom.make_reader = []() -> std::unique_ptr<TensorReader> { return std::make_unique<RawTensorReader>(); };
        registry.register_format(custom);

        const auto path = write_file(".dat", magic + " payload");
        const auto* detected = registry.detect(path);
        ASSERT_NE(detected, nullptr);
        EXPECT_EQ(detected->name, "tensor_test_format");
        EXPECT_EQ(registry.create_writer("x.mftt"), nullptr);

        custom.probe = {};
        custom.extensions = { "mftt2" };
        registry.register_format(custom);

        const auto names = registry.format_names();
        EXPECT_EQ(std::ranges::count(names, "tensor_test_format"), 1);
        EXPECT_EQ(registry.find("tensor_test_format")->extensions, (std::vector<std::string> { "mftt2" }));
        EXPECT_EQ(registry.detect(path), nullptr);
    }

    // ---------------------------------------------------------------------
    // Raw
    // ---------------------------------------------------------------------

    class TensorRawReaderTest : public TensorIoFixture { };

    RawTensorEntry raw_floats(const std::string& name, std::vector<uint64_t> shape, std::optional<uint64_t> offset = std::nullopt)
    {
        return { .spec = TensorSpec::contiguous(TensorDType::of<float>(), std::move(shape), name), .file_offset = offset };
    }

    TEST_F(TensorRawReaderTest, EntriesDefaultToFollowingTheirPredecessor)
    {
        const auto path = write_file(".bin", float_bytes({ 0, 1, 2, 3, 4, 5 }));
        RawTensorReader reader;
        ASSERT_TRUE(reader.set_layout({ raw_floats("a", { 2 }), raw_floats("b", { 2, 2 }) }));
        ASSERT_TRUE(reader.open(path)) << reader.get_last_error();

        EXPECT_EQ(reader.format_name(), "raw");
        ASSERT_EQ(reader.describe().size(), 2U);
        EXPECT_EQ(floats_of(*reader.read("a")), (std::vector<float> { 0, 1 }));
        EXPECT_EQ(floats_of(*reader.read("b")), (std::vector<float> { 2, 3, 4, 5 }));

        const auto all = reader.read_tensors();
        ASSERT_TRUE(all.has_value());
        EXPECT_EQ(all->tensors.size(), 2U);
    }

    TEST_F(TensorRawReaderTest, ExplicitOffsetsPlaceTensorsAnywhere)
    {
        const auto path = write_file(".bin", float_bytes({ 0, 1, 2, 3, 4, 5 }));
        RawTensorReader reader;
        ASSERT_TRUE(reader.set_layout({ raw_floats("tail", { 2 }, 16), raw_floats("head", { 2 }, 0) }));
        ASSERT_TRUE(reader.open(path));
        EXPECT_EQ(floats_of(*reader.read("tail")), (std::vector<float> { 4, 5 }));
        EXPECT_EQ(floats_of(*reader.read("head")), (std::vector<float> { 0, 1 }));
    }

    TEST_F(TensorRawReaderTest, ALayoutLargerThanTheFileIsRefused)
    {
        const auto path = write_file(".bin", float_bytes({ 0, 1 }));
        RawTensorReader reader;
        ASSERT_TRUE(reader.set_layout({ raw_floats("a", { 3 }) }));
        EXPECT_FALSE(reader.open(path));
        EXPECT_NE(reader.get_last_error().find("past the end"), std::string::npos);
        EXPECT_FALSE(reader.is_open());
    }

    TEST_F(TensorRawReaderTest, OpeningNeedsALayoutFirst)
    {
        RawTensorReader reader;
        EXPECT_FALSE(reader.open(write_file(".bin", float_bytes({ 0 }))));
        EXPECT_NE(reader.get_last_error().find("set_layout"), std::string::npos);
    }

    TEST_F(TensorRawReaderTest, InvalidLayoutsAreRefused)
    {
        RawTensorReader reader;
        EXPECT_FALSE(reader.set_layout({ raw_floats("", { 1 }) }));
        EXPECT_FALSE(reader.set_layout({ raw_floats("a", { 1 }), raw_floats("a", { 1 }) }));

        auto device = raw_floats("d", { 1 });
        device.spec.device = { Kakshya::TensorDeviceType::CUDA, 0 };
        EXPECT_FALSE(reader.set_layout({ device }));

        auto bad = raw_floats("b", { 2 });
        bad.spec.strides = { 1, 2 };
        EXPECT_FALSE(reader.set_layout({ bad }));
    }

    TEST_F(TensorRawReaderTest, ByteOrderIsConvertedToTheHosts)
    {
        const auto path = write_file(".bin", std::string("\x01\x02\x03\x04", 4));
        RawTensorEntry entry;
        entry.spec = TensorSpec::contiguous(TensorDType::of<uint32_t>(), { 1 }, "v");
        entry.big_endian = true;

        RawTensorReader reader;
        ASSERT_TRUE(reader.set_layout({ entry }));
        ASSERT_TRUE(reader.open(path));
        const auto value = reader.read("v");
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(value->to_vector<uint32_t>(), std::optional { std::vector<uint32_t> { 0x01020304U } });
    }

    TEST_F(TensorRawReaderTest, StridedLayoutsKeepTheirStrides)
    {
        const auto path = write_file(".bin", float_bytes({ 0, 1, 2, 3, 4, 5 }));
        RawTensorEntry entry;
        entry.spec = TensorSpec::contiguous(TensorDType::of<float>(), { 2, 3 }, "f");
        entry.spec.strides = { 1, 2 };

        RawTensorReader reader;
        ASSERT_TRUE(reader.set_layout({ entry }));
        ASSERT_TRUE(reader.open(path));
        const auto tensor = reader.read("f");
        ASSERT_TRUE(tensor.has_value());
        EXPECT_EQ(tensor->spec().strides, (std::vector<int64_t> { 1, 2 }));

        const auto dense = TensorData::copy_of(*tensor->access());
        ASSERT_TRUE(dense.has_value());
        EXPECT_EQ(floats_of(*dense), (std::vector<float> { 0, 2, 4, 1, 3, 5 }));
    }

    TEST_F(TensorRawReaderTest, UnknownNamesAndUnopenedReadersReportWhy)
    {
        RawTensorReader reader;
        ASSERT_TRUE(reader.set_layout({ raw_floats("a", { 1 }) }));
        EXPECT_FALSE(reader.read("a").has_value());
        EXPECT_EQ(reader.get_last_error(), "no file is open");

        ASSERT_TRUE(reader.open(write_file(".bin", float_bytes({ 1.0F }))));
        EXPECT_FALSE(reader.read("b").has_value());
        EXPECT_NE(reader.get_last_error().find("'b'"), std::string::npos);
    }

    TEST_F(TensorRawReaderTest, AnyExistingFileCanBeRead)
    {
        RawTensorReader reader;
        EXPECT_TRUE(reader.can_read(write_file(".bin", "x")));
        EXPECT_FALSE(reader.can_read(temp_path(".bin")));
        EXPECT_TRUE(reader.get_supported_extensions().empty());
    }

} // namespace
} // namespace MayaFlux::Test
