#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "io_manager_fixture.hpp"

#include "MayaFlux/IO/Composite/CompositeReader.hpp"
#include "MayaFlux/IO/Composite/CompositeWriter.hpp"
#include "MayaFlux/Kakshya/NDData/TensorInsertion.hpp"
#include "MayaFlux/Kakshya/Semantic/CompositeContainer.hpp"
#include "MayaFlux/Nodes/Network/MeshNetwork.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {

namespace {

    class IOManagerCompositeTest : public IOManagerFixture { };
    class IOManagerTensorTest : public IOManagerFixture { };
    class IOManagerModelTest : public IOManagerFixture { };
    class IOManagerVolumeTest : public IOManagerFixture { };
    class IOManagerSpatialTest : public IOManagerFixture { };

    constexpr const char* k_csv = "name,value\nalpha,1\nbeta,2\ngamma,3\n";
    constexpr const char* k_obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";

    std::optional<Kakshya::TensorData> floats(std::vector<uint64_t> shape, std::string name)
    {
        uint64_t count = 1;
        for (const auto extent : shape) {
            count *= extent;
        }
        std::vector<float> values(count);
        std::iota(values.begin(), values.end(), 0.0F);
        return Kakshya::TensorData::from_values<float>(values, std::move(shape), std::move(name));
    }

    std::vector<float> floats_of(const Kakshya::TensorData& tensor)
    {
        return tensor.to_vector<float>().value_or(std::vector<float> {});
    }

    std::string raw_floats(std::initializer_list<float> values)
    {
        std::string out(values.size() * sizeof(float), '\0');
        size_t offset = 0;
        for (const float value : values) {
            std::memcpy(out.data() + offset, &value, sizeof(float));
            offset += sizeof(float);
        }
        return out;
    }

}

// =========================================================================
// Composite files
// =========================================================================

TEST_F(IOManagerCompositeTest, MissingFileGivesNullReaderAndNullContainer)
{
    EXPECT_EQ(manager->open_composite_reader(path_of("missing.csv")), nullptr);
    EXPECT_EQ(manager->load_composite(path_of("missing.csv")), nullptr);
}

TEST_F(IOManagerCompositeTest, UnsupportedExtensionGivesNull)
{
    FFmpegFixture::write_bytes(root() / "table.bin", k_csv);

    EXPECT_EQ(manager->open_composite_reader(path_of("table.bin")), nullptr);
    EXPECT_EQ(manager->load_composite(path_of("table.bin")), nullptr);
}

TEST_F(IOManagerCompositeTest, EmptyPathGivesNull)
{
    EXPECT_EQ(manager->open_composite_reader(""), nullptr);
    EXPECT_EQ(manager->load_composite(""), nullptr);
}

TEST_F(IOManagerCompositeTest, ValidCsvOpensAReader)
{
    FFmpegFixture::write_bytes(root() / "table.csv", k_csv);

    EXPECT_NE(manager->open_composite_reader(path_of("table.csv")), nullptr);
}

TEST_F(IOManagerCompositeTest, ValidTsvOpensAReader)
{
    FFmpegFixture::write_bytes(root() / "table.tsv", "name\tvalue\nalpha\t1\nbeta\t2\n");

    EXPECT_NE(manager->open_composite_reader(path_of("table.tsv")), nullptr);
}

TEST_F(IOManagerCompositeTest, ValidCsvLoadsIntoAContainerWithRows)
{
    FFmpegFixture::write_bytes(root() / "table.csv", k_csv);

    const auto container = manager->load_composite(path_of("table.csv"));

    ASSERT_NE(container, nullptr);
    EXPECT_GT(container->get_data().size(), 0U);
}

TEST_F(IOManagerCompositeTest, BatchSizeIsCarriedIntoTheContainer)
{
    FFmpegFixture::write_bytes(root() / "table.csv", k_csv);

    const auto one = manager->load_composite(path_of("table.csv"));
    const auto five = manager->load_composite(path_of("table.csv"), std::nullopt, 5);

    ASSERT_NE(one, nullptr);
    ASSERT_NE(five, nullptr);
    EXPECT_EQ(one->batch_size(), 1U);
    EXPECT_EQ(five->batch_size(), 5U);
}

TEST_F(IOManagerCompositeTest, TwoLoadsOfTheSameFileAreIndependent)
{
    FFmpegFixture::write_bytes(root() / "table.csv", k_csv);

    const auto first = manager->load_composite(path_of("table.csv"));
    const auto second = manager->load_composite(path_of("table.csv"));

    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first, second);
    EXPECT_EQ(first->get_data().size(), second->get_data().size());
}

TEST_F(IOManagerCompositeTest, AReaderIsNotRetainedByTheManager)
{
    FFmpegFixture::write_bytes(root() / "table.csv", k_csv);
    const auto reader = manager->open_composite_reader(path_of("table.csv"));
    ASSERT_NE(reader, nullptr);
    const std::weak_ptr<IO::CompositeReader> weak = reader;

    manager->shutdown();

    EXPECT_FALSE(weak.expired());
}

TEST_F(IOManagerCompositeTest, SavingALoadedContainerRoundTripsItsRowCount)
{
    FFmpegFixture::write_bytes(root() / "table.csv", k_csv);
    const auto container = manager->load_composite(path_of("table.csv"));
    ASSERT_NE(container, nullptr);

    ASSERT_TRUE(manager->save_composite(container, path_of("copy.csv")));

    ASSERT_TRUE(exists("copy.csv"));
    const auto back = manager->load_composite(path_of("copy.csv"));
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(back->get_data().size(), container->get_data().size());
}

TEST_F(IOManagerCompositeTest, SavingToTsvRoundTripsToo)
{
    FFmpegFixture::write_bytes(root() / "table.csv", k_csv);
    const auto container = manager->load_composite(path_of("table.csv"));
    ASSERT_NE(container, nullptr);

    ASSERT_TRUE(manager->save_composite(container, path_of("copy.tsv")));

    const auto back = manager->load_composite(path_of("copy.tsv"));
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(back->get_data().size(), container->get_data().size());
}

TEST_F(IOManagerCompositeTest, SavingReplacesAnExistingFile)
{
    FFmpegFixture::write_bytes(root() / "table.csv", k_csv);
    FFmpegFixture::write_bytes(root() / "copy.csv", std::string(4096, 'x'));
    const auto container = manager->load_composite(path_of("table.csv"));
    ASSERT_NE(container, nullptr);

    ASSERT_TRUE(manager->save_composite(container, path_of("copy.csv")));

    const auto back = manager->load_composite(path_of("copy.csv"));
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(back->get_data().size(), container->get_data().size());
}

TEST_F(IOManagerCompositeTest, SavingToAnUnsupportedExtensionFailsAndWritesNothing)
{
    FFmpegFixture::write_bytes(root() / "table.csv", k_csv);
    const auto container = manager->load_composite(path_of("table.csv"));
    ASSERT_NE(container, nullptr);

    EXPECT_FALSE(manager->save_composite(container, path_of("copy.bin")));

    EXPECT_FALSE(exists("copy.bin"));
}

TEST_F(IOManagerCompositeTest, SavingToAMissingDirectoryFails)
{
    FFmpegFixture::write_bytes(root() / "table.csv", k_csv);
    const auto container = manager->load_composite(path_of("table.csv"));
    ASSERT_NE(container, nullptr);

    EXPECT_FALSE(manager->save_composite(container, path_of("no_such_dir/copy.csv")));
}

TEST_F(IOManagerCompositeTest, WriterOfAnUnsupportedExtensionIsNull)
{
    FFmpegFixture::write_bytes(root() / "table.csv", k_csv);
    const auto container = manager->load_composite(path_of("table.csv"));
    ASSERT_NE(container, nullptr);

    EXPECT_EQ(manager->open_composite_writer(path_of("out.bin"), container->get_data().layout()), nullptr);
}

TEST_F(IOManagerCompositeTest, WriterOfAMissingDirectoryIsNull)
{
    FFmpegFixture::write_bytes(root() / "table.csv", k_csv);
    const auto container = manager->load_composite(path_of("table.csv"));
    ASSERT_NE(container, nullptr);

    EXPECT_EQ(manager->open_composite_writer(path_of("no_such_dir/out.csv"), container->get_data().layout()), nullptr);
}

TEST_F(IOManagerCompositeTest, AWriterOpenedForTheContainersLayoutClosesCleanly)
{
    FFmpegFixture::write_bytes(root() / "table.csv", k_csv);
    const auto container = manager->load_composite(path_of("table.csv"));
    ASSERT_NE(container, nullptr);

    const auto writer = manager->open_composite_writer(path_of("out.csv"), container->get_data().layout());

    ASSERT_NE(writer, nullptr);
    EXPECT_TRUE(writer->close());
    EXPECT_TRUE(exists("out.csv"));
}

// =========================================================================
// Tensors
// =========================================================================

TEST_F(IOManagerTensorTest, SafetensorsRoundTripKeepsNamesValuesAndMetadata)
{
    Kakshya::TensorSet set;
    set.tensors.push_back(*floats({ 2, 3 }, "w"));
    set.tensors.push_back(*floats({ 4 }, "b"));
    set.metadata["author"] = "test";

    ASSERT_TRUE(manager->save_tensors(set, path_of("model.safetensors")));
    const auto back = manager->load_tensors(path_of("model.safetensors"));

    ASSERT_TRUE(back.has_value());
    ASSERT_EQ(back->tensors.size(), 2U);
    const auto* w = back->find("w");
    const auto* b = back->find("b");
    ASSERT_NE(w, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(w->spec().shape, (std::vector<uint64_t> { 2, 3 }));
    EXPECT_EQ(floats_of(*w), floats_of(*set.find("w")));
    EXPECT_EQ(floats_of(*b), floats_of(*set.find("b")));
    EXPECT_EQ(back->metadata.at("author"), "test");
}

TEST_F(IOManagerTensorTest, SelectingNamesReadsOnlyThose)
{
    Kakshya::TensorSet set;
    set.tensors.push_back(*floats({ 2 }, "w"));
    set.tensors.push_back(*floats({ 3 }, "b"));
    ASSERT_TRUE(manager->save_tensors(set, path_of("model.safetensors")));

    IO::TensorReadOptions options;
    options.names = { "b" };
    const auto back = manager->load_tensors(path_of("model.safetensors"), options);

    ASSERT_TRUE(back.has_value());
    ASSERT_EQ(back->tensors.size(), 1U);
    EXPECT_EQ(back->tensors[0].spec().name, "b");
}

TEST_F(IOManagerTensorTest, SelectedNamesComeBackInTheRequestedOrder)
{
    Kakshya::TensorSet set;
    set.tensors.push_back(*floats({ 2 }, "w"));
    set.tensors.push_back(*floats({ 3 }, "b"));
    ASSERT_TRUE(manager->save_tensors(set, path_of("model.safetensors")));

    IO::TensorReadOptions options;
    options.names = { "b", "w" };
    const auto back = manager->load_tensors(path_of("model.safetensors"), options);

    ASSERT_TRUE(back.has_value());
    ASSERT_EQ(back->tensors.size(), 2U);
    EXPECT_EQ(back->tensors[0].spec().name, "b");
    EXPECT_EQ(back->tensors[1].spec().name, "w");
}

TEST_F(IOManagerTensorTest, SelectingAnAbsentNameFailsTheWholeRead)
{
    Kakshya::TensorSet set;
    set.tensors.push_back(*floats({ 2 }, "w"));
    ASSERT_TRUE(manager->save_tensors(set, path_of("model.safetensors")));

    IO::TensorReadOptions options;
    options.names = { "w", "absent" };

    EXPECT_FALSE(manager->load_tensors(path_of("model.safetensors"), options).has_value());
}

TEST_F(IOManagerTensorTest, ReaderListsTensorsWithoutReadingThem)
{
    Kakshya::TensorSet set;
    set.tensors.push_back(*floats({ 2, 2 }, "w"));
    ASSERT_TRUE(manager->save_tensors(set, path_of("model.safetensors")));

    const auto reader = manager->open_tensor_reader(path_of("model.safetensors"));

    ASSERT_NE(reader, nullptr);
    EXPECT_FALSE(reader->describe().empty());
}

TEST_F(IOManagerTensorTest, NpyRoundTripsASingleTensor)
{
    Kakshya::TensorSet set;
    set.tensors.push_back(*floats({ 3, 2 }, "x"));

    ASSERT_TRUE(manager->save_tensors(set, path_of("one.npy")));
    const auto back = manager->load_tensors(path_of("one.npy"));

    ASSERT_TRUE(back.has_value());
    ASSERT_EQ(back->tensors.size(), 1U);
    EXPECT_EQ(back->tensors[0].spec().shape, (std::vector<uint64_t> { 3, 2 }));
    EXPECT_EQ(floats_of(back->tensors[0]), floats_of(set.tensors[0]));
}

TEST_F(IOManagerTensorTest, NpyRefusesAMultiTensorSetAndLeavesNoFile)
{
    Kakshya::TensorSet set;
    set.tensors.push_back(*floats({ 2 }, "a"));
    set.tensors.push_back(*floats({ 2 }, "b"));

    EXPECT_FALSE(manager->save_tensors(set, path_of("two.npy")));

    EXPECT_FALSE(exists("two.npy"));
}

TEST_F(IOManagerTensorTest, NpzRoundTripsSeveralTensors)
{
    Kakshya::TensorSet set;
    set.tensors.push_back(*floats({ 2 }, "a"));
    set.tensors.push_back(*floats({ 3 }, "b"));

    ASSERT_TRUE(manager->save_tensors(set, path_of("many.npz")));
    const auto back = manager->load_tensors(path_of("many.npz"));

    ASSERT_TRUE(back.has_value());
    ASSERT_EQ(back->tensors.size(), 2U);
    ASSERT_NE(back->find("a"), nullptr);
    ASSERT_NE(back->find("b"), nullptr);
    EXPECT_EQ(floats_of(*back->find("b")), floats_of(*set.find("b")));
}

TEST_F(IOManagerTensorTest, ASetWrittenAsOneFormatIsReadableByNameOverride)
{
    Kakshya::TensorSet set;
    set.tensors.push_back(*floats({ 2 }, "w"));
    ASSERT_TRUE(manager->save_tensors(set, path_of("weights.dat"), "safetensors"));

    const auto back = manager->load_tensors(path_of("weights.dat"), {}, "safetensors");

    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->tensors.size(), 1U);
}

TEST_F(IOManagerTensorTest, UnknownFormatNamesAreRefusedOnBothSides)
{
    Kakshya::TensorSet set;
    set.tensors.push_back(*floats({ 2 }, "w"));

    EXPECT_FALSE(manager->save_tensors(set, path_of("x.safetensors"), "no_such_format"));
    EXPECT_FALSE(exists("x.safetensors"));
    EXPECT_FALSE(manager->load_tensors(path_of("x.safetensors"), {}, "no_such_format").has_value());
    EXPECT_EQ(manager->open_tensor_reader(path_of("x.safetensors"), "no_such_format"), nullptr);
}

TEST_F(IOManagerTensorTest, UnknownExtensionIsRefusedOnBothSides)
{
    Kakshya::TensorSet set;
    set.tensors.push_back(*floats({ 2 }, "w"));
    FFmpegFixture::write_bytes(root() / "weights.unknownext", "irrelevant");

    EXPECT_FALSE(manager->save_tensors(set, path_of("out.unknownext")));
    EXPECT_FALSE(exists("out.unknownext"));
    EXPECT_FALSE(manager->load_tensors(path_of("weights.unknownext")).has_value());
    EXPECT_EQ(manager->open_tensor_reader(path_of("weights.unknownext")), nullptr);
}

TEST_F(IOManagerTensorTest, MissingFileIsRefused)
{
    EXPECT_FALSE(manager->load_tensors(path_of("missing.safetensors")).has_value());
    EXPECT_EQ(manager->open_tensor_reader(path_of("missing.safetensors")), nullptr);
}

TEST_F(IOManagerTensorTest, EmptyAndGarbageFilesAreRefused)
{
    FFmpegFixture::write_bytes(root() / "empty.safetensors", "");
    FFmpegFixture::write_bytes(root() / "garbage.safetensors", FFmpegFixture::noise_bytes(512));
    FFmpegFixture::write_bytes(root() / "garbage.npy", FFmpegFixture::noise_bytes(512));

    EXPECT_FALSE(manager->load_tensors(path_of("empty.safetensors")).has_value());
    EXPECT_FALSE(manager->load_tensors(path_of("garbage.safetensors")).has_value());
    EXPECT_FALSE(manager->load_tensors(path_of("garbage.npy")).has_value());
}

TEST_F(IOManagerTensorTest, ATruncatedFileIsRefusedNotMisread)
{
    Kakshya::TensorSet set;
    set.tensors.push_back(*floats({ 64 }, "w"));
    ASSERT_TRUE(manager->save_tensors(set, path_of("full.safetensors")));
    std::ifstream input(root() / "full.safetensors", std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    ASSERT_GT(bytes.size(), 32U);
    FFmpegFixture::write_bytes(root() / "cut.safetensors", bytes.substr(0, bytes.size() - 16));

    EXPECT_FALSE(manager->load_tensors(path_of("cut.safetensors")).has_value());
}

TEST_F(IOManagerTensorTest, SavingToAMissingDirectoryFails)
{
    Kakshya::TensorSet set;
    set.tensors.push_back(*floats({ 2 }, "w"));

    EXPECT_FALSE(manager->save_tensors(set, path_of("no_such_dir/out.safetensors")));
}

TEST_F(IOManagerTensorTest, SavingOverAnExistingFileReplacesIt)
{
    FFmpegFixture::write_bytes(root() / "model.safetensors", std::string(4096, 'x'));
    Kakshya::TensorSet set;
    set.tensors.push_back(*floats({ 2 }, "w"));

    ASSERT_TRUE(manager->save_tensors(set, path_of("model.safetensors")));

    const auto back = manager->load_tensors(path_of("model.safetensors"));
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->tensors.size(), 1U);
}

TEST_F(IOManagerTensorTest, RawReaderOpensAFileThatHoldsItsLayout)
{
    FFmpegFixture::write_bytes(root() / "raw.bin", raw_floats({ 1.0F, 2.0F, 3.0F, 4.0F }));
    IO::RawTensorEntry entry;
    entry.spec = Kakshya::TensorSpec::contiguous(Kakshya::TensorDType::of<float>(), { 4 }, "x");

    const auto reader = manager->open_raw_tensor_reader(path_of("raw.bin"), { entry });

    ASSERT_NE(reader, nullptr);
    const auto tensors = reader->read_tensors({});
    ASSERT_TRUE(tensors.has_value());
    ASSERT_EQ(tensors->tensors.size(), 1U);
    EXPECT_EQ(floats_of(tensors->tensors[0]), (std::vector<float> { 1.0F, 2.0F, 3.0F, 4.0F }));
}

TEST_F(IOManagerTensorTest, RawReaderRefusesAFileShorterThanItsLayout)
{
    FFmpegFixture::write_bytes(root() / "raw.bin", raw_floats({ 1.0F, 2.0F }));
    IO::RawTensorEntry entry;
    entry.spec = Kakshya::TensorSpec::contiguous(Kakshya::TensorDType::of<float>(), { 4 }, "x");

    EXPECT_EQ(manager->open_raw_tensor_reader(path_of("raw.bin"), { entry }), nullptr);
}

TEST_F(IOManagerTensorTest, RawReaderRefusesAMissingFile)
{
    IO::RawTensorEntry entry;
    entry.spec = Kakshya::TensorSpec::contiguous(Kakshya::TensorDType::of<float>(), { 4 }, "x");

    EXPECT_EQ(manager->open_raw_tensor_reader(path_of("missing.bin"), { entry }), nullptr);
}

TEST_F(IOManagerTensorTest, ReadersAreNotRetainedByTheManager)
{
    Kakshya::TensorSet set;
    set.tensors.push_back(*floats({ 2 }, "w"));
    ASSERT_TRUE(manager->save_tensors(set, path_of("model.safetensors")));
    const auto reader = manager->open_tensor_reader(path_of("model.safetensors"));
    ASSERT_NE(reader, nullptr);
    const std::weak_ptr<IO::TensorReader> weak = reader;

    manager->shutdown();

    EXPECT_FALSE(weak.expired());
}

// =========================================================================
// Models
// =========================================================================

TEST_F(IOManagerModelTest, MissingFileGivesNothing)
{
    EXPECT_EQ(manager->load_mesh_network(path_of("missing.obj")), nullptr);
    EXPECT_TRUE(manager->load_mesh(path_of("missing.obj")).empty());
}

TEST_F(IOManagerModelTest, UnsupportedExtensionGivesNothing)
{
    FFmpegFixture::write_bytes(root() / "mesh.notaformat", k_obj);

    EXPECT_EQ(manager->load_mesh_network(path_of("mesh.notaformat")), nullptr);
    EXPECT_TRUE(manager->load_mesh(path_of("mesh.notaformat")).empty());
}

TEST_F(IOManagerModelTest, GarbageUnderAModelExtensionGivesNothing)
{
    FFmpegFixture::write_bytes(root() / "mesh.glb", FFmpegFixture::noise_bytes(4096));

    EXPECT_EQ(manager->load_mesh_network(path_of("mesh.glb")), nullptr);
    EXPECT_TRUE(manager->load_mesh(path_of("mesh.glb")).empty());
}

TEST_F(IOManagerModelTest, EmptyPathGivesNothing)
{
    EXPECT_EQ(manager->load_mesh_network(""), nullptr);
    EXPECT_TRUE(manager->load_mesh("").empty());
}

TEST_F(IOManagerModelTest, ValidObjLoadsAsANetworkWithSlots)
{
    FFmpegFixture::write_bytes(root() / "tri.obj", k_obj);

    const auto network = manager->load_mesh_network(path_of("tri.obj"));

    ASSERT_NE(network, nullptr);
    EXPECT_GE(network->slot_count(), 1U);
}

TEST_F(IOManagerModelTest, NetworkLoadDoesNotRetainItsReader)
{
    FFmpegFixture::write_bytes(root() / "tri.obj", k_obj);

    ASSERT_NE(manager->load_mesh_network(path_of("tri.obj")), nullptr);

    EXPECT_TRUE(manager->get_model_readers().empty());
}

TEST_F(IOManagerModelTest, ACustomResolverIsCalledOnlyForTexturedMaterials)
{
    FFmpegFixture::write_bytes(root() / "tri.obj", k_obj);
    std::atomic<int> calls { 0 };

    const auto network = manager->load_mesh_network(path_of("tri.obj"),
        [&calls](const std::string&) -> std::shared_ptr<Core::VKImage> {
            calls.fetch_add(1);
            return nullptr;
        });

    ASSERT_NE(network, nullptr);
    EXPECT_EQ(calls.load(), 0);
}

TEST_F(IOManagerModelTest, SavedNetworkLoadsBackWithTheSameSlotCount)
{
    FFmpegFixture::write_bytes(root() / "tri.obj", k_obj);
    const auto network = manager->load_mesh_network(path_of("tri.obj"));
    ASSERT_NE(network, nullptr);

    ASSERT_TRUE(manager->save_mesh(network, path_of("out.obj")));
    manager->wait_for_pending_saves();

    ASSERT_TRUE(exists("out.obj"));
    const auto back = manager->load_mesh_network(path_of("out.obj"));
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(back->slot_count(), network->slot_count());
}

TEST_F(IOManagerModelTest, SavingAnEmptyMeshListReportsQueuedAndLeavesNoFile)
{
    EXPECT_TRUE(manager->save_mesh(std::vector<Kakshya::MeshData> {}, path_of("empty.obj")));
    EXPECT_NO_THROW(manager->wait_for_pending_saves());
}

TEST_F(IOManagerModelTest, SavingToAnUnknownExtensionReportsQueuedAndWritesNothing)
{
    FFmpegFixture::write_bytes(root() / "tri.obj", k_obj);
    const auto network = manager->load_mesh_network(path_of("tri.obj"));
    ASSERT_NE(network, nullptr);

    EXPECT_TRUE(manager->save_mesh(network, path_of("out.notaformat")));
    EXPECT_NO_THROW(manager->wait_for_pending_saves());

    EXPECT_FALSE(exists("out.notaformat"));
}

TEST_F(IOManagerModelTest, SavingToAMissingDirectoryReportsQueuedAndWritesNothing)
{
    FFmpegFixture::write_bytes(root() / "tri.obj", k_obj);
    const auto network = manager->load_mesh_network(path_of("tri.obj"));
    ASSERT_NE(network, nullptr);

    EXPECT_TRUE(manager->save_mesh(network, path_of("no_such_dir/out.obj")));
    EXPECT_NO_THROW(manager->wait_for_pending_saves());

    EXPECT_FALSE(std::filesystem::exists(root() / "no_such_dir"));
}

TEST_F(IOManagerModelTest, SavesToSeveralFormatsInOneBurstAllLand)
{
    FFmpegFixture::write_bytes(root() / "tri.obj", k_obj);
    const auto network = manager->load_mesh_network(path_of("tri.obj"));
    ASSERT_NE(network, nullptr);

    ASSERT_TRUE(manager->save_mesh(network, path_of("a.obj")));
    ASSERT_TRUE(manager->save_mesh(network, path_of("b.ply")));
    ASSERT_TRUE(manager->save_mesh(network, path_of("c.stl")));
    manager->wait_for_pending_saves();

    EXPECT_GT(size_of("a.obj"), 0U);
    EXPECT_GT(size_of("b.ply"), 0U);
    EXPECT_GT(size_of("c.stl"), 0U);
}

// =========================================================================
// Volumes
// =========================================================================

TEST_F(IOManagerVolumeTest, MissingFileGivesNullAndRetainsNothing)
{
    EXPECT_EQ(manager->load_volume(path_of("missing.vdb")), nullptr);
    EXPECT_TRUE(manager->get_loaded_volumes().empty());
}

TEST_F(IOManagerVolumeTest, UnsupportedExtensionGivesNull)
{
    FFmpegFixture::write_bytes(root() / "grid.notaformat", "data");

    EXPECT_EQ(manager->load_volume(path_of("grid.notaformat")), nullptr);
    EXPECT_TRUE(manager->get_loaded_volumes().empty());
}

TEST_F(IOManagerVolumeTest, GarbageUnderAVolumeExtensionGivesNull)
{
    FFmpegFixture::write_bytes(root() / "grid.vdb", FFmpegFixture::noise_bytes(2048));

    EXPECT_EQ(manager->load_volume(path_of("grid.vdb")), nullptr);
    EXPECT_TRUE(manager->get_loaded_volumes().empty());
}

TEST_F(IOManagerVolumeTest, SavingAnEmptyVolumeDoesNotThrow)
{
    EXPECT_NO_THROW({
        static_cast<void>(manager->save_volume(Kakshya::VolumeData {}, path_of("empty.vdb")));
        manager->wait_for_pending_saves();
    });
}

TEST_F(IOManagerVolumeTest, SavingToAnUnknownExtensionReportsQueuedAndWritesNothing)
{
    EXPECT_TRUE(manager->save_volume(Kakshya::VolumeData {}, path_of("grid.notaformat")));
    EXPECT_NO_THROW(manager->wait_for_pending_saves());

    EXPECT_FALSE(exists("grid.notaformat"));
}

TEST_F(IOManagerVolumeTest, ACaptureNeedsAVolume)
{
    EXPECT_EQ(manager->capture_volume(nullptr, path_of("seq.{:04}.vdb")), 0U);
    EXPECT_TRUE(manager->get_volume_capture_ids().empty());
}

// =========================================================================
// Spatial captures
// =========================================================================

TEST_F(IOManagerSpatialTest, ACaptureNeedsSources)
{
    EXPECT_EQ(manager->capture_spatial(path_of("empty.abc"), {}), 0U);
    EXPECT_TRUE(manager->get_spatial_capture_ids().empty());
}

TEST_F(IOManagerSpatialTest, ACaptureIntoAMissingDirectoryGivesZero)
{
    std::vector<IO::SpatialCaptureSource> sources(1);
    sources[0].stream_name = "stream";
    sources[0].capture_frame = [](IO::SpatialCache&, const std::string&) { return true; };

    EXPECT_EQ(manager->capture_spatial(path_of("no_such_dir/out.abc"), std::move(sources)), 0U);
    EXPECT_TRUE(manager->get_spatial_capture_ids().empty());
}

TEST_F(IOManagerSpatialTest, AValidCaptureGetsAnIdAndStopRemovesIt)
{
    std::vector<IO::SpatialCaptureSource> sources(1);
    sources[0].stream_name = "stream";
    sources[0].capture_frame = [](IO::SpatialCache&, const std::string&) { return true; };

    const auto id = manager->capture_spatial(path_of("out.abc"), std::move(sources));

    ASSERT_NE(id, 0U);
    EXPECT_EQ(manager->get_spatial_capture_ids(), std::vector<uint32_t> { id });
    manager->stop_spatial_capture(id);
    EXPECT_TRUE(manager->get_spatial_capture_ids().empty());
    EXPECT_TRUE(exists("out.abc"));
}

TEST_F(IOManagerSpatialTest, TwoCapturesOfOnePathShareTheArchiveAndGetDistinctIds)
{
    auto make_sources = [] {
        std::vector<IO::SpatialCaptureSource> sources(1);
        sources[0].stream_name = "stream";
        sources[0].capture_frame = [](IO::SpatialCache&, const std::string&) { return true; };
        return sources;
    };

    const auto first = manager->capture_spatial(path_of("out.abc"), make_sources());
    const auto second = manager->capture_spatial(path_of("out.abc"), make_sources());

    ASSERT_NE(first, 0U);
    ASSERT_NE(second, 0U);
    EXPECT_NE(first, second);
    EXPECT_EQ(manager->get_spatial_capture_ids().size(), 2U);

    manager->stop_spatial_capture(first);
    EXPECT_EQ(manager->get_spatial_capture_ids(), std::vector<uint32_t> { second });
    manager->stop_spatial_capture(second);
    EXPECT_TRUE(manager->get_spatial_capture_ids().empty());
    EXPECT_TRUE(exists("out.abc"));
}

TEST_F(IOManagerSpatialTest, AReopenedPathAfterTheLastStopOpensAFreshArchive)
{
    auto make_sources = [] {
        std::vector<IO::SpatialCaptureSource> sources(1);
        sources[0].stream_name = "stream";
        sources[0].capture_frame = [](IO::SpatialCache&, const std::string&) { return true; };
        return sources;
    };

    const auto first = manager->capture_spatial(path_of("out.abc"), make_sources());
    ASSERT_NE(first, 0U);
    manager->stop_spatial_capture(first);

    const auto second = manager->capture_spatial(path_of("out.abc"), make_sources());

    EXPECT_NE(second, 0U);
    EXPECT_NE(second, first);
}

TEST_F(IOManagerSpatialTest, StoppingTwiceIsANoOp)
{
    std::vector<IO::SpatialCaptureSource> sources(1);
    sources[0].stream_name = "stream";
    sources[0].capture_frame = [](IO::SpatialCache&, const std::string&) { return true; };
    const auto id = manager->capture_spatial(path_of("out.abc"), std::move(sources));
    ASSERT_NE(id, 0U);

    manager->stop_spatial_capture(id);

    EXPECT_NO_THROW(manager->stop_spatial_capture(id));
}

TEST_F(IOManagerSpatialTest, DifferentPathsAreDifferentArchives)
{
    auto make_sources = [] {
        std::vector<IO::SpatialCaptureSource> sources(1);
        sources[0].stream_name = "stream";
        sources[0].capture_frame = [](IO::SpatialCache&, const std::string&) { return true; };
        return sources;
    };

    const auto a = manager->capture_spatial(path_of("a.abc"), make_sources());
    const auto b = manager->capture_spatial(path_of("b.abc"), make_sources());
    ASSERT_NE(a, 0U);
    ASSERT_NE(b, 0U);

    manager->stop_spatial_capture(a);
    manager->stop_spatial_capture(b);

    EXPECT_TRUE(exists("a.abc"));
    EXPECT_TRUE(exists("b.abc"));
}

TEST_F(IOManagerSpatialTest, StopFinalisesAnOutstandingCapture)
{
    std::vector<IO::SpatialCaptureSource> sources(1);
    sources[0].stream_name = "stream";
    sources[0].capture_frame = [](IO::SpatialCache&, const std::string&) { return true; };
    ASSERT_NE(manager->capture_spatial(path_of("out.abc"), std::move(sources)), 0U);

    manager->stop();

    EXPECT_TRUE(manager->get_spatial_capture_ids().empty());
    EXPECT_TRUE(exists("out.abc"));
}

TEST_F(IOManagerSpatialTest, ShutdownFinalisesAnOutstandingCapture)
{
    std::vector<IO::SpatialCaptureSource> sources(1);
    sources[0].stream_name = "stream";
    sources[0].capture_frame = [](IO::SpatialCache&, const std::string&) { return true; };
    ASSERT_NE(manager->capture_spatial(path_of("out.abc"), std::move(sources)), 0U);

    manager->shutdown();

    EXPECT_TRUE(manager->get_spatial_capture_ids().empty());
}

TEST_F(IOManagerSpatialTest, DestructionFinalisesAnOutstandingCapture)
{
    std::vector<IO::SpatialCaptureSource> sources(1);
    sources[0].stream_name = "stream";
    sources[0].capture_frame = [](IO::SpatialCache&, const std::string&) { return true; };
    ASSERT_NE(manager->capture_spatial(path_of("out.abc"), std::move(sources)), 0U);

    EXPECT_NO_THROW(manager.reset());
    EXPECT_TRUE(exists("out.abc"));
}

TEST_F(IOManagerSpatialTest, SnapshotWithNoSourcesReportsQueuedAndWritesNothing)
{
    EXPECT_TRUE(manager->save_spatial_snapshot({}, path_of("snap_{}.abc")));
    EXPECT_NO_THROW(manager->wait_for_pending_saves());
}

}
