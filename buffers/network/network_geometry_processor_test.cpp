#include "network_buffer_fixture.hpp"

#include "MayaFlux/Nodes/Network/AssemblyNetwork.hpp"
#include "MayaFlux/Nodes/Network/Operators/AssemblyOperator.hpp"

namespace MayaFlux::Test {
namespace {

    using Nodes::Network::AssemblyNetwork;
    using Nodes::Network::AssemblyOperator;
    using Portal::Graphics::PolygonMode;
    using Portal::Graphics::PrimitiveTopology;
    using Portal::Graphics::RenderConfig;

    constexpr uint8_t k_poison = 0xAB;

    std::vector<glm::vec3> positions(uint32_t count, float offset = 0.0F)
    {
        std::vector<glm::vec3> out;
        out.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
            out.emplace_back(static_cast<float>(i) + offset, offset, 0.0F);
        return out;
    }

    std::vector<uint8_t> copy_of(std::span<const uint8_t> bytes)
    {
        return { bytes.begin(), bytes.end() };
    }

    std::vector<uint8_t> slice(const std::vector<uint8_t>& bytes, size_t offset, size_t length)
    {
        return { bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + length) };
    }

    class NetworkUploadTest : public NetworkBufferDeviceTest {
    protected:
        void attach_assembly(std::shared_ptr<AssemblyNetwork> value, float over_allocate_factor = 2.0F)
        {
            assembly = std::move(value);
            attach(assembly, over_allocate_factor);
        }

        [[nodiscard]] AssemblyOperator* primary() const { return assembly->assembly(); }

        [[nodiscard]] size_t stride() const { return primary()->get_vertex_layout().stride_bytes; }

        [[nodiscard]] size_t uploaded_bytes() const
        {
            return buffer->get_processor()->get_binding(buffer->get_binding_name())->last_total_bytes;
        }

        void poison(size_t byte_count)
        {
            const std::vector<uint8_t> bytes(byte_count, k_poison);
            auto binding = buffer->get_processor()->get_binding(buffer->get_binding_name());
            Buffers::upload_to_gpu(bytes.data(), bytes.size(), buffer, binding->staging_buffer);
        }

        void settle()
        {
            assembly->process_batch(1);
            buffer->process_default();
            buffer->process_default();
            ASSERT_FALSE(primary()->is_vertex_data_dirty());
        }

        std::shared_ptr<AssemblyNetwork> assembly;
    };

    // Bindings

    TEST_F(NetworkUploadTest, BindingsAreKeyedByNameAndCanBeRemoved)
    {
        attach_assembly(std::make_shared<AssemblyNetwork>());
        assembly->add_geometry(positions(4));
        const auto processor = buffer->get_processor();

        processor->bind_network("second", assembly, buffer);

        EXPECT_EQ(processor->get_binding_count(), 2U);
        EXPECT_TRUE(processor->has_binding("second"));
        auto names = processor->get_binding_names();
        std::ranges::sort(names);
        EXPECT_EQ(names, (std::vector<std::string> { "network_geometry", "second" }));

        processor->unbind_network("second");
        EXPECT_FALSE(processor->has_binding("second"));
        EXPECT_EQ(processor->get_binding_count(), 1U);
        EXPECT_NO_THROW(processor->unbind_network("never_bound"));
        EXPECT_FALSE(processor->get_binding("never_bound").has_value());
    }

    TEST_F(NetworkUploadTest, BindingNullNetworkOrNullBufferIsRejected)
    {
        attach_assembly(std::make_shared<AssemblyNetwork>());
        const auto processor = buffer->get_processor();

        EXPECT_ANY_THROW(processor->bind_network("bad_network", nullptr, buffer));
        EXPECT_ANY_THROW(processor->bind_network("bad_buffer", assembly, nullptr));
        EXPECT_FALSE(processor->has_binding("bad_network"));
        EXPECT_FALSE(processor->has_binding("bad_buffer"));
    }

    TEST_F(NetworkUploadTest, StagingExistsExactlyWhenTheVertexBufferIsDeviceLocal)
    {
        attach_assembly(std::make_shared<AssemblyNetwork>());
        assembly->add_geometry(positions(4));

        const auto binding = buffer->get_processor()->get_binding(buffer->get_binding_name());
        ASSERT_TRUE(binding.has_value());
        EXPECT_EQ(binding->staging_buffer != nullptr, !buffer->is_host_visible());
    }

    TEST_F(NetworkUploadTest, ProcessingWithNoBindingsIsInert)
    {
        attach_assembly(std::make_shared<AssemblyNetwork>());
        assembly->add_geometry(positions(4));
        buffer->get_processor()->unbind_network(buffer->get_binding_name());

        EXPECT_NO_THROW(buffer->process_default());
    }

    // Full upload

    TEST_F(NetworkUploadTest, FullUploadPutsTheOperatorBytesOnTheGpuWithLayoutAndCounts)
    {
        attach_assembly(std::make_shared<AssemblyNetwork>());
        assembly->add_geometry(positions(4));
        assembly->add_geometry(positions(3, 10.0F), PrimitiveTopology::LINE_LIST);
        assembly->process_batch(1);

        buffer->process_default();

        const auto expected = copy_of(primary()->get_vertex_data());
        ASSERT_EQ(expected.size(), 7 * stride());
        EXPECT_EQ(read_vertices(expected.size()), expected);
        EXPECT_EQ(uploaded_bytes(), expected.size());

        const auto layout = buffer->get_vertex_layout();
        ASSERT_TRUE(layout.has_value());
        EXPECT_EQ(layout->stride_bytes, stride());
        EXPECT_EQ(layout->vertex_count, 7U);
    }

    TEST_F(NetworkUploadTest, DisabledNetworksAreNotUploaded)
    {
        attach_assembly(std::make_shared<AssemblyNetwork>());
        assembly->add_geometry(positions(4));
        assembly->process_batch(1);
        assembly->set_enabled(false);

        buffer->process_default();

        EXPECT_EQ(uploaded_bytes(), 0U);
        EXPECT_FALSE(buffer->get_vertex_layout().has_value());
    }

    TEST_F(NetworkUploadTest, EmptyNetworkUploadsNothing)
    {
        attach_assembly(std::make_shared<AssemblyNetwork>());

        EXPECT_NO_THROW(buffer->process_default());
        EXPECT_EQ(uploaded_bytes(), 0U);
    }

    TEST_F(NetworkUploadTest, GrowthBeyondCapacityResizesWithHeadroomAndKeepsTheData)
    {
        attach_assembly(std::make_shared<AssemblyNetwork>(), 1.0F);
        assembly->add_geometry(positions(2));
        assembly->process_batch(1);
        buffer->process_default();
        const auto old_capacity = buffer->get_size_bytes();

        assembly->add_geometry(positions(64, 5.0F));
        assembly->process_batch(1);
        buffer->process_default();

        const auto expected = copy_of(primary()->get_vertex_data());
        EXPECT_GT(buffer->get_size_bytes(), old_capacity);
        EXPECT_GT(buffer->get_size_bytes(), expected.size());
        EXPECT_EQ(read_vertices(expected.size()), expected);
        EXPECT_EQ(uploaded_bytes(), expected.size());
    }

    // Incremental upload

    TEST_F(NetworkUploadTest, SecondPassOnUnchangedDataIsIncrementalAndLeavesTheOperatorClean)
    {
        attach_assembly(std::make_shared<AssemblyNetwork>());
        assembly->add_geometry(positions(3));
        assembly->add_geometry(positions(4, 10.0F));

        settle();

        const auto expected = copy_of(primary()->get_vertex_data());
        EXPECT_EQ(read_vertices(expected.size()), expected);
        EXPECT_EQ(uploaded_bytes(), expected.size());
    }

    TEST_F(NetworkUploadTest, IncrementalUploadWritesOnlyTheDirtyItemsRange)
    {
        attach_assembly(std::make_shared<AssemblyNetwork>());
        assembly->add_geometry(positions(3));
        auto edited = assembly->add_geometry(positions(4, 10.0F));
        settle();
        const auto total = primary()->get_vertex_data().size();
        poison(total);

        edited->set_positions(positions(4, 20.0F));
        assembly->process_batch(1);
        buffer->process_default();

        const auto expected = copy_of(primary()->get_vertex_data());
        const auto gpu = read_vertices(total);
        const size_t first_bytes = 3 * stride();
        EXPECT_EQ(slice(gpu, 0, first_bytes), std::vector<uint8_t>(first_bytes, k_poison));
        EXPECT_EQ(slice(gpu, first_bytes, total - first_bytes), slice(expected, first_bytes, total - first_bytes));
        EXPECT_FALSE(primary()->is_vertex_data_dirty());
    }

    TEST_F(NetworkUploadTest, NothingDirtyUploadsNothing)
    {
        attach_assembly(std::make_shared<AssemblyNetwork>());
        assembly->add_geometry(positions(3));
        assembly->add_geometry(positions(4, 10.0F));
        settle();
        const auto total = primary()->get_vertex_data().size();
        poison(total);

        buffer->process_default();

        EXPECT_EQ(read_vertices(total), std::vector<uint8_t>(total, k_poison));
    }

    TEST_F(NetworkUploadTest, ByteTotalChangeFallsBackToAFullUpload)
    {
        attach_assembly(std::make_shared<AssemblyNetwork>());
        assembly->add_geometry(positions(3));
        settle();
        poison(primary()->get_vertex_data().size());

        assembly->add_geometry(positions(2, 7.0F));
        assembly->process_batch(1);
        buffer->process_default();

        const auto expected = copy_of(primary()->get_vertex_data());
        EXPECT_EQ(read_vertices(expected.size()), expected);
        EXPECT_EQ(uploaded_bytes(), expected.size());
        EXPECT_EQ(buffer->get_vertex_layout()->vertex_count, 5U);
    }

    // Operator chain slices

    class NetworkChainUploadTest : public NetworkUploadTest {
    protected:
        void SetUp() override
        {
            NetworkUploadTest::SetUp();
            if (IsSkipped())
                return;
            attach_assembly(std::make_shared<AssemblyNetwork>());
            first_item = assembly->add_geometry(positions(3));
            chain_op = assembly->get_operator_chain()->emplace<AssemblyOperator>();
            chain_item = chain_op->add_geometry(positions(4, 10.0F), PrimitiveTopology::LINE_LIST);
        }

        [[nodiscard]] std::vector<uint8_t> expected_concatenation() const
        {
            auto bytes = copy_of(primary()->get_vertex_data());
            const auto chain_bytes = chain_op->get_vertex_data();
            bytes.insert(bytes.end(), chain_bytes.begin(), chain_bytes.end());
            return bytes;
        }

        std::shared_ptr<AssemblyOperator> chain_op;
        std::shared_ptr<Nodes::GpuSync::GeometryLeafNode> first_item;
        std::shared_ptr<Nodes::GpuSync::GeometryLeafNode> chain_item;
    };

    TEST_F(NetworkChainUploadTest, SlicesAreConcatenatedPrimaryFirst)
    {
        assembly->process_batch(1);
        buffer->process_default();

        const auto expected = expected_concatenation();
        EXPECT_EQ(read_vertices(expected.size()), expected);
        EXPECT_EQ(uploaded_bytes(), expected.size());
        EXPECT_EQ(buffer->get_vertex_layout()->vertex_count, 7U);
    }

    TEST_F(NetworkChainUploadTest, NonRenderingChainOperatorsContributeNoSlice)
    {
        chain_op->set_participates_in_rendering(false);
        assembly->process_batch(1);

        buffer->process_default();

        const auto expected = copy_of(primary()->get_vertex_data());
        EXPECT_EQ(read_vertices(expected.size()), expected);
        EXPECT_EQ(uploaded_bytes(), expected.size());
        EXPECT_EQ(buffer->get_vertex_layout()->vertex_count, 3U);
    }

    TEST_F(NetworkChainUploadTest, EachSliceReceivesItsRunsRebasedPastThePrecedingSlices)
    {
        render(RenderConfig { .polygon_mode = PolygonMode::POINT, .triangulate = true });
        buffer->add_chain_operator_rendering(RenderConfig { .target_window = window, .triangulate = true });
        assembly->process_batch(1);

        buffer->process_default();

        const auto primary_runs = primary()->topology_runs();
        const auto chain_runs = chain_op->topology_runs();
        ASSERT_FALSE(primary_runs.empty());
        ASSERT_FALSE(chain_runs.empty());

        const auto& primary_applied = buffer->get_render_processor()->get_runs();
        ASSERT_EQ(primary_applied.size(), primary_runs.size());
        EXPECT_EQ(primary_applied[0].vertex_offset, primary_runs[0].vertex_offset);
        EXPECT_EQ(primary_applied[0].vertex_count, primary_runs[0].vertex_count);

        const auto& chain_applied = buffer->get_chain_render_processor(0)->get_runs();
        ASSERT_EQ(chain_applied.size(), chain_runs.size());
        EXPECT_EQ(chain_applied[0].vertex_offset, chain_runs[0].vertex_offset + 3U);
        EXPECT_EQ(chain_applied[0].vertex_count, chain_runs[0].vertex_count);
        EXPECT_EQ(chain_applied[0].topology, PrimitiveTopology::LINE_LIST);
    }

    TEST_F(NetworkChainUploadTest, EditingOnlyThePrimaryStaysIncrementalAndSparesTheChainSlice)
    {
        assembly->process_batch(1);
        buffer->process_default();
        buffer->process_default();
        chain_op->mark_vertex_data_clean();
        const auto total = expected_concatenation().size();
        poison(total);

        first_item->set_positions(positions(3, 30.0F));
        assembly->process_batch(1);
        buffer->process_default();

        const auto gpu = read_vertices(total);
        const size_t primary_bytes = 3 * stride();
        EXPECT_EQ(slice(gpu, 0, primary_bytes), copy_of(primary()->get_vertex_data()));
        EXPECT_EQ(slice(gpu, primary_bytes, total - primary_bytes),
            std::vector<uint8_t>(total - primary_bytes, k_poison));
    }

    TEST_F(NetworkChainUploadTest, EditingAChainOperatorForcesAFullUpload)
    {
        assembly->process_batch(1);
        buffer->process_default();
        buffer->process_default();
        chain_op->mark_vertex_data_clean();
        const auto total = expected_concatenation().size();
        poison(total);

        chain_item->set_positions(positions(4, 40.0F));
        assembly->process_batch(1);
        buffer->process_default();

        const auto expected = expected_concatenation();
        EXPECT_EQ(read_vertices(total), expected);
    }

} // namespace
} // namespace MayaFlux::Test
