#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/Staging/SliceSet.hpp"
#include "MayaFlux/Buffers/Staging/StagingUtils.hpp"
#include "MayaFlux/Buffers/VKBuffer.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::SliceSet;
    using Buffers::VKBuffer;
    using Portal::Graphics::DrawRun;
    using Portal::Graphics::PrimitiveTopology;

    constexpr uint32_t k_stride = 8;
    constexpr auto k_topology = PrimitiveTopology::TRIANGLE_LIST;
    constexpr uint8_t k_sentinel = 0xEE;

    std::vector<uint8_t> vertices(size_t count, uint8_t fill)
    {
        return std::vector<uint8_t>(count * k_stride, fill);
    }

    std::vector<uint8_t> concat(std::initializer_list<std::vector<uint8_t>> parts)
    {
        std::vector<uint8_t> out;
        for (const auto& part : parts) {
            out.insert(out.end(), part.begin(), part.end());
        }
        return out;
    }

    bool same_run(const DrawRun& run, uint32_t offset, uint32_t count)
    {
        return run.topology == k_topology && run.vertex_offset == offset && run.vertex_count == count;
    }

    TEST(SliceSetTest, ZeroStrideIsRejected)
    {
        EXPECT_THROW(SliceSet(0, k_topology), std::invalid_argument);
    }

    TEST(SliceSetTest, AddReturnsDistinctNonZeroIdsAndRejectsPartialVertices)
    {
        SliceSet set(k_stride, k_topology);

        const auto first = set.add(vertices(2, 1));
        const auto second = set.add(vertices(1, 2));
        const std::vector<uint8_t> partial(k_stride + 1, 3);
        const auto rejected = set.add(partial);

        EXPECT_NE(first, 0U);
        EXPECT_NE(second, 0U);
        EXPECT_NE(first, second);
        EXPECT_EQ(rejected, 0U);
        EXPECT_TRUE(set.contains(first));
        EXPECT_TRUE(set.contains(second));
        EXPECT_FALSE(set.contains(rejected));
        EXPECT_EQ(set.vertex_count(), 3U);
        EXPECT_EQ(set.size_bytes(), 3U * k_stride);
    }

    TEST(SliceSetTest, FreshSetReportsOneEmptyRunListThenNothing)
    {
        SliceSet set(k_stride, k_topology);

        const auto runs = set.take_runs();
        ASSERT_TRUE(runs.has_value());
        EXPECT_TRUE(runs->empty());
        EXPECT_FALSE(set.take_runs().has_value());
    }

    TEST(SliceSetTest, AdjacentVisibleSlicesCoalesceIntoOneRun)
    {
        SliceSet set(k_stride, k_topology);
        set.add(vertices(2, 1));
        set.add(vertices(3, 2));
        set.add(vertices(1, 3));

        const auto runs = set.take_runs();
        ASSERT_TRUE(runs.has_value());
        ASSERT_EQ(runs->size(), 1U);
        EXPECT_TRUE(same_run((*runs)[0], 0, 6));
        EXPECT_FALSE(set.take_runs().has_value());
    }

    TEST(SliceSetTest, StripAndFanTopologiesNeverCoalesceAdjacentSlices)
    {
        for (const auto topology : { PrimitiveTopology::LINE_STRIP, PrimitiveTopology::TRIANGLE_STRIP, PrimitiveTopology::TRIANGLE_FAN }) {
            SliceSet set(k_stride, topology);
            set.add(vertices(3, 1));
            set.add(vertices(4, 2));

            const auto runs = set.take_runs();

            ASSERT_TRUE(runs.has_value());
            ASSERT_EQ(runs->size(), 2U);
            EXPECT_EQ((*runs)[0].topology, topology);
            EXPECT_EQ((*runs)[0].vertex_offset, 0U);
            EXPECT_EQ((*runs)[0].vertex_count, 3U);
            EXPECT_EQ((*runs)[1].vertex_offset, 3U);
            EXPECT_EQ((*runs)[1].vertex_count, 4U);
        }
    }

    TEST(SliceSetTest, SliceTopologyDefaultsToTheSetsAndDifferingNeighboursStaySeparate)
    {
        SliceSet set(k_stride, PrimitiveTopology::TRIANGLE_LIST);
        set.add(vertices(3, 1));
        set.add(vertices(4, 2), PrimitiveTopology::TRIANGLE_STRIP);
        set.add(vertices(3, 3));
        set.add(vertices(3, 4), PrimitiveTopology::TRIANGLE_LIST);
        set.add(vertices(2, 5), PrimitiveTopology::LINE_LIST);

        const auto runs = set.take_runs();

        ASSERT_TRUE(runs.has_value());
        ASSERT_EQ(runs->size(), 4U);
        EXPECT_TRUE(same_run((*runs)[0], 0, 3));
        EXPECT_EQ((*runs)[1].topology, PrimitiveTopology::TRIANGLE_STRIP);
        EXPECT_EQ((*runs)[1].vertex_offset, 3U);
        EXPECT_EQ((*runs)[1].vertex_count, 4U);
        EXPECT_TRUE(same_run((*runs)[2], 7, 6));
        EXPECT_EQ((*runs)[3].topology, PrimitiveTopology::LINE_LIST);
        EXPECT_EQ((*runs)[3].vertex_offset, 13U);
        EXPECT_EQ((*runs)[3].vertex_count, 2U);
    }

    TEST(SliceSetTest, HidingTheMiddleSliceSplitsTheRun)
    {
        SliceSet set(k_stride, k_topology);
        const auto a = set.add(vertices(2, 1));
        const auto b = set.add(vertices(3, 2));
        const auto c = set.add(vertices(1, 3));
        static_cast<void>(set.take_runs());

        set.set_visible(b, false);
        const auto runs = set.take_runs();

        ASSERT_TRUE(runs.has_value());
        ASSERT_EQ(runs->size(), 2U);
        EXPECT_TRUE(same_run((*runs)[0], 0, 2));
        EXPECT_TRUE(same_run((*runs)[1], 5, 1));
        EXPECT_EQ(set.vertex_count(), 6U);

        set.set_visible(b, true);
        const auto restored = set.take_runs();
        ASSERT_TRUE(restored.has_value());
        ASSERT_EQ(restored->size(), 1U);
        EXPECT_TRUE(same_run((*restored)[0], 0, 6));
        static_cast<void>(a);
        static_cast<void>(c);
    }

    TEST(SliceSetTest, SettingTheCurrentVisibilityReportsNoChange)
    {
        SliceSet set(k_stride, k_topology);
        const auto a = set.add(vertices(2, 1));
        static_cast<void>(set.take_runs());

        set.set_visible(a, true);

        EXPECT_FALSE(set.take_runs().has_value());
    }

    TEST(SliceSetTest, BringToFrontReordersDrawsWithoutMovingMemory)
    {
        SliceSet set(k_stride, k_topology);
        const auto a = set.add(vertices(2, 1));
        set.add(vertices(3, 2));
        set.add(vertices(1, 3));
        static_cast<void>(set.take_runs());

        set.bring_to_front(a);
        const auto runs = set.take_runs();

        ASSERT_TRUE(runs.has_value());
        ASSERT_EQ(runs->size(), 2U);
        EXPECT_TRUE(same_run((*runs)[0], 2, 4));
        EXPECT_TRUE(same_run((*runs)[1], 0, 2));
        EXPECT_EQ(set.size_bytes(), 6U * k_stride);
    }

    TEST(SliceSetTest, SendToBackReordersDrawsWithoutMovingMemory)
    {
        SliceSet set(k_stride, k_topology);
        set.add(vertices(2, 1));
        set.add(vertices(3, 2));
        const auto c = set.add(vertices(1, 3));
        static_cast<void>(set.take_runs());

        set.send_to_back(c);
        const auto runs = set.take_runs();

        ASSERT_TRUE(runs.has_value());
        ASSERT_EQ(runs->size(), 2U);
        EXPECT_TRUE(same_run((*runs)[0], 5, 1));
        EXPECT_TRUE(same_run((*runs)[1], 0, 5));
    }

    TEST(SliceSetTest, ReorderingTheOnlyEdgeSliceIsANoOp)
    {
        SliceSet set(k_stride, k_topology);
        set.add(vertices(1, 1));
        const auto last = set.add(vertices(1, 2));
        static_cast<void>(set.take_runs());

        set.bring_to_front(last);

        EXPECT_FALSE(set.take_runs().has_value());
    }

    TEST(SliceSetTest, RemovingASliceShiftsLaterSlicesDown)
    {
        SliceSet set(k_stride, k_topology);
        set.add(vertices(2, 1));
        const auto b = set.add(vertices(3, 2));
        const auto c = set.add(vertices(1, 3));
        static_cast<void>(set.take_runs());

        set.remove(b);
        const auto runs = set.take_runs();

        EXPECT_FALSE(set.contains(b));
        EXPECT_TRUE(set.contains(c));
        EXPECT_EQ(set.vertex_count(), 3U);
        ASSERT_TRUE(runs.has_value());
        ASSERT_EQ(runs->size(), 1U);
        EXPECT_TRUE(same_run((*runs)[0], 0, 3));
    }

    TEST(SliceSetTest, SameSizeUpdateKeepsTheRunsUnchanged)
    {
        SliceSet set(k_stride, k_topology);
        const auto a = set.add(vertices(2, 1));
        static_cast<void>(set.take_runs());

        set.update(a, vertices(2, 9));

        EXPECT_FALSE(set.take_runs().has_value());
        EXPECT_EQ(set.vertex_count(), 2U);
    }

    TEST(SliceSetTest, ResizingASliceShiftsTheOnesAfterIt)
    {
        SliceSet set(k_stride, k_topology);
        const auto a = set.add(vertices(2, 1));
        const auto b = set.add(vertices(1, 2));
        const auto c = set.add(vertices(2, 3));
        static_cast<void>(set.take_runs());

        set.update(b, vertices(3, 9));
        EXPECT_EQ(set.vertex_count(), 7U);

        const auto grown = set.take_runs();
        ASSERT_TRUE(grown.has_value());
        ASSERT_EQ(grown->size(), 1U);
        EXPECT_TRUE(same_run((*grown)[0], 0, 7));

        set.set_visible(a, false);
        set.set_visible(b, false);
        const auto tail = set.take_runs();
        ASSERT_TRUE(tail.has_value());
        ASSERT_EQ(tail->size(), 1U);
        EXPECT_TRUE(same_run((*tail)[0], 5, 2));
        static_cast<void>(c);
    }

    TEST(SliceSetTest, EmptySliceProducesNoRunButStaysLive)
    {
        SliceSet set(k_stride, k_topology);
        const auto empty = set.add({});
        set.add(vertices(2, 1));

        const auto runs = set.take_runs();

        EXPECT_NE(empty, 0U);
        EXPECT_TRUE(set.contains(empty));
        ASSERT_TRUE(runs.has_value());
        ASSERT_EQ(runs->size(), 1U);
        EXPECT_TRUE(same_run((*runs)[0], 0, 2));
    }

    TEST(SliceSetTest, UnknownIdsAreIgnored)
    {
        SliceSet set(k_stride, k_topology);
        set.add(vertices(2, 1));
        static_cast<void>(set.take_runs());

        set.update(999, vertices(2, 2));
        set.remove(999);
        set.set_visible(999, false);
        set.bring_to_front(999);
        set.send_to_back(999);

        EXPECT_EQ(set.vertex_count(), 2U);
        EXPECT_FALSE(set.take_runs().has_value());
    }

    TEST(SliceSetTest, UpdateWithPartialVerticesIsRejected)
    {
        SliceSet set(k_stride, k_topology);
        const auto a = set.add(vertices(2, 1));
        static_cast<void>(set.take_runs());

        const std::vector<uint8_t> partial(k_stride + 3, 5);
        set.update(a, partial);

        EXPECT_EQ(set.vertex_count(), 2U);
        EXPECT_FALSE(set.take_runs().has_value());
    }

    TEST(StagingUtilsTest, UploadIntervalsRejectsANullTarget)
    {
        std::shared_ptr<VKBuffer> staging;
        const std::array<std::pair<size_t, size_t>, 1> intervals { { { 0, 4 } } };
        const std::array<uint8_t, 4> source {};

        EXPECT_THROW(Buffers::upload_intervals(source.data(), intervals, nullptr, staging), std::invalid_argument);
    }

    TEST(StagingUtilsTest, UploadToGpuRejectsANullTarget)
    {
        const uint8_t byte = 1;

        EXPECT_THROW(Buffers::upload_to_gpu(&byte, 1, nullptr), std::invalid_argument);
    }

    TEST(SliceSetTest, FlushRejectsANullTarget)
    {
        SliceSet set(k_stride, k_topology);
        std::shared_ptr<VKBuffer> staging;

        EXPECT_THROW(set.flush(nullptr, staging), std::invalid_argument);
    }

    class StagingUtilsDeviceTest : public VulkanDeviceTest, public ::testing::WithParamInterface<bool> {
    protected:
        void TearDown() override
        {
            auto* service = Registry::BackendRegistry::instance()
                                .get_service<Registry::Service::BufferService>();
            for (const auto& buffer : buffers) {
                if (service && buffer && buffer->is_initialized()) {
                    service->destroy_buffer(buffer);
                }
            }
            buffers.clear();
            VulkanDeviceTest::TearDown();
        }

        [[nodiscard]] bool device_local() const { return GetParam(); }

        std::shared_ptr<VKBuffer> make_buffer(size_t bytes)
        {
            auto buffer = std::make_shared<VKBuffer>(
                bytes,
                device_local() ? VKBuffer::Usage::VERTEX : VKBuffer::Usage::HOST_STORAGE,
                Kakshya::DataModality::UNKNOWN);
            auto* service = Registry::BackendRegistry::instance()
                                .get_service<Registry::Service::BufferService>();
            EXPECT_NE(service, nullptr);
            service->initialize_buffer(buffer);
            EXPECT_TRUE(buffer->is_initialized());
            buffers.push_back(buffer);
            return buffer;
        }

        void track(const std::shared_ptr<VKBuffer>& buffer)
        {
            if (buffer) {
                buffers.push_back(buffer);
            }
        }

        std::shared_ptr<VKBuffer> make_filled(size_t bytes, uint8_t value)
        {
            auto buffer = make_buffer(bytes);
            const std::vector<uint8_t> data(bytes, value);
            Buffers::upload_to_gpu(data.data(), bytes, buffer);
            return buffer;
        }

        std::vector<uint8_t> read_all(const std::shared_ptr<VKBuffer>& buffer)
        {
            std::vector<uint8_t> out(buffer->get_size_bytes());
            if (buffer->is_host_visible()) {
                std::memcpy(out.data(), buffer->get_buffer_resources().mapped_ptr, out.size());
            } else {
                std::shared_ptr<VKBuffer> staging;
                Buffers::download_from_gpu_async(buffer, out.data(), out.size(), staging);
                track(staging);
            }
            return out;
        }

        std::vector<std::shared_ptr<VKBuffer>> buffers;
    };

    TEST_P(StagingUtilsDeviceTest, UploadToGpuWritesAtTheOffsetAndLeavesTheRestUntouched)
    {
        auto buffer = make_filled(64, 0xAA);
        const std::vector<uint8_t> patch(8, 0x11);

        Buffers::upload_to_gpu(patch.data(), patch.size(), buffer, nullptr, 16);

        const auto bytes = read_all(buffer);
        for (size_t i = 0; i < bytes.size(); ++i) {
            EXPECT_EQ(bytes[i], (i >= 16 && i < 24) ? 0x11 : 0xAA) << "byte " << i;
        }
    }

    TEST_P(StagingUtilsDeviceTest, UploadToGpuIgnoresAnEmptyPayload)
    {
        auto buffer = make_filled(32, 0xAA);

        Buffers::upload_to_gpu(nullptr, 0, buffer);

        for (const auto byte : read_all(buffer)) {
            EXPECT_EQ(byte, 0xAA);
        }
    }

    TEST_P(StagingUtilsDeviceTest, UploadToGpuRejectsAPayloadPastTheBufferCapacity)
    {
        auto buffer = make_filled(32, 0xAA);
        const std::vector<uint8_t> payload(16, 0x11);
        const std::vector<uint8_t> oversized(40, 0x11);

        EXPECT_THROW(Buffers::upload_to_gpu(payload.data(), payload.size(), buffer, nullptr, 24), std::runtime_error);
        EXPECT_THROW(Buffers::upload_to_gpu(oversized.data(), oversized.size(), buffer), std::runtime_error);

        for (const auto byte : read_all(buffer)) {
            EXPECT_EQ(byte, 0xAA);
        }
    }

    TEST_P(StagingUtilsDeviceTest, UploadToGpuCopiesTheSourceAtCallTime)
    {
        auto buffer = make_buffer(16);
        std::vector<uint8_t> source(16, 0x33);

        Buffers::upload_to_gpu(source.data(), source.size(), buffer);
        std::ranges::fill(source, 0x77);

        for (const auto byte : read_all(buffer)) {
            EXPECT_EQ(byte, 0x33);
        }
    }

    TEST_P(StagingUtilsDeviceTest, ASuppliedStagingBufferCarriesADeviceLocalPayload)
    {
        if (!device_local()) {
            GTEST_SKIP() << "Only a device-local target goes through staging";
        }
        auto buffer = make_filled(64, 0xAA);
        auto staging = Buffers::create_staging_buffer(64);
        track(staging);
        const std::vector<uint8_t> payload(24, 0x5A);

        Buffers::upload_to_gpu(payload.data(), payload.size(), buffer, staging, 8);

        const auto* staged = static_cast<const uint8_t*>(staging->get_buffer_resources().mapped_ptr);
        for (size_t i = 0; i < payload.size(); ++i) {
            EXPECT_EQ(staged[i], 0x5A) << "staging byte " << i;
        }
        const auto bytes = read_all(buffer);
        for (size_t i = 0; i < bytes.size(); ++i) {
            EXPECT_EQ(bytes[i], (i >= 8 && i < 32) ? 0x5A : 0xAA) << "byte " << i;
        }
    }

    TEST_P(StagingUtilsDeviceTest, TheDataVariantEntryPointsWriteAtTheOffset)
    {
        auto buffer = make_filled(32, 0xAA);
        const Kakshya::DataVariant variant(std::vector<uint8_t>(8, 0x22));

        if (device_local()) {
            auto staging = Buffers::create_staging_buffer(8);
            track(staging);
            Buffers::upload_device_local(buffer, staging, variant, 4);
        } else {
            Buffers::upload_host_visible(buffer, variant, 4);
        }

        const auto bytes = read_all(buffer);
        for (size_t i = 0; i < bytes.size(); ++i) {
            EXPECT_EQ(bytes[i], (i >= 4 && i < 12) ? 0x22 : 0xAA) << "byte " << i;
        }
    }

    TEST_P(StagingUtilsDeviceTest, EnsureGpuCapacityGrowsOnlyWhenTooSmall)
    {
        auto buffer = make_buffer(32);

        Buffers::ensure_gpu_capacity(buffer, nullptr, 16);
        EXPECT_EQ(buffer->get_size_bytes(), 32U);

        Buffers::ensure_gpu_capacity(buffer, nullptr, 100, 2.0F);
        EXPECT_EQ(buffer->get_size_bytes(), 200U);
    }

    TEST_P(StagingUtilsDeviceTest, UploadResizingGrowsThenWritesTheWholePayload)
    {
        auto buffer = make_buffer(16);
        const std::vector<uint8_t> data(64, 0x42);

        Buffers::upload_resizing(data.data(), data.size(), buffer, nullptr);

        ASSERT_GE(buffer->get_size_bytes(), data.size());
        const auto bytes = read_all(buffer);
        for (size_t i = 0; i < data.size(); ++i) {
            EXPECT_EQ(bytes[i], 0x42) << "byte " << i;
        }
    }

    TEST_P(StagingUtilsDeviceTest, ClearGpuBufferZeroesTheContents)
    {
        auto buffer = make_filled(64, 0xAA);

        ASSERT_TRUE(Buffers::clear_gpu_buffer(buffer));

        for (const auto byte : read_all(buffer)) {
            EXPECT_EQ(byte, 0);
        }
    }

    TEST_P(StagingUtilsDeviceTest, UploadIntervalsWritesOnlyTheRequestedRanges)
    {
        auto buffer = make_filled(64, 0xAA);
        const std::vector<uint8_t> source(64, 0x55);
        const std::array<std::pair<size_t, size_t>, 2> intervals { { { 8, 16 }, { 32, 40 } } };
        std::shared_ptr<VKBuffer> staging;

        Buffers::upload_intervals(source.data(), intervals, buffer, staging);
        track(staging);

        const auto bytes = read_all(buffer);
        for (size_t i = 0; i < bytes.size(); ++i) {
            const bool inside = (i >= 8 && i < 16) || (i >= 32 && i < 40);
            EXPECT_EQ(bytes[i], inside ? 0x55 : 0xAA) << "byte " << i;
        }
    }

    TEST_P(StagingUtilsDeviceTest, UploadIntervalsReadsEachRangeFromItsOwnOffsetInTheSource)
    {
        auto buffer = make_filled(32, 0x00);
        std::vector<uint8_t> source(32);
        for (size_t i = 0; i < source.size(); ++i) {
            source[i] = static_cast<uint8_t>(i + 1);
        }
        const std::array<std::pair<size_t, size_t>, 2> intervals { { { 4, 8 }, { 20, 24 } } };
        std::shared_ptr<VKBuffer> staging;

        Buffers::upload_intervals(source.data(), intervals, buffer, staging);
        track(staging);

        const auto bytes = read_all(buffer);
        for (size_t i = 0; i < bytes.size(); ++i) {
            const bool inside = (i >= 4 && i < 8) || (i >= 20 && i < 24);
            EXPECT_EQ(bytes[i], inside ? source[i] : 0x00) << "byte " << i;
        }
    }

    TEST_P(StagingUtilsDeviceTest, UploadIntervalsMergesOverlappingAndAdjacentRanges)
    {
        auto buffer = make_filled(64, 0xAA);
        const std::vector<uint8_t> source(64, 0x55);
        const std::array<std::pair<size_t, size_t>, 3> intervals { { { 24, 28 }, { 8, 16 }, { 12, 24 } } };
        std::shared_ptr<VKBuffer> staging;

        Buffers::upload_intervals(source.data(), intervals, buffer, staging);
        track(staging);

        const auto bytes = read_all(buffer);
        for (size_t i = 0; i < bytes.size(); ++i) {
            EXPECT_EQ(bytes[i], (i >= 8 && i < 28) ? 0x55 : 0xAA) << "byte " << i;
        }
    }

    TEST_P(StagingUtilsDeviceTest, UploadIntervalsIgnoresEmptyRangesAndANullSource)
    {
        auto buffer = make_filled(32, 0xAA);
        const std::vector<uint8_t> source(32, 0x55);
        const std::array<std::pair<size_t, size_t>, 2> empty { { { 8, 8 }, { 12, 10 } } };
        const std::array<std::pair<size_t, size_t>, 1> real { { { 0, 4 } } };
        std::shared_ptr<VKBuffer> staging;

        Buffers::upload_intervals(source.data(), empty, buffer, staging);
        Buffers::upload_intervals(nullptr, real, buffer, staging);
        track(staging);

        for (const auto byte : read_all(buffer)) {
            EXPECT_EQ(byte, 0xAA);
        }
    }

    TEST_P(StagingUtilsDeviceTest, UploadIntervalsRejectsARangePastTheBufferCapacity)
    {
        auto buffer = make_filled(32, 0xAA);
        const std::vector<uint8_t> source(64, 0x55);
        const std::array<std::pair<size_t, size_t>, 1> intervals { { { 28, 36 } } };
        std::shared_ptr<VKBuffer> staging;

        EXPECT_THROW(Buffers::upload_intervals(source.data(), intervals, buffer, staging), std::runtime_error);
        track(staging);
    }

    TEST_P(StagingUtilsDeviceTest, FlushUploadsEveryBlockAtItsOffset)
    {
        SliceSet set(k_stride, k_topology);
        set.add(vertices(2, 1));
        set.add(vertices(1, 2));
        set.add(vertices(3, 3));
        auto buffer = make_filled(set.size_bytes(), 0xAA);
        std::shared_ptr<VKBuffer> staging;

        set.flush(buffer, staging);
        track(staging);

        EXPECT_EQ(read_all(buffer), concat({ vertices(2, 1), vertices(1, 2), vertices(3, 3) }));
    }

    TEST_P(StagingUtilsDeviceTest, SecondFlushUploadsOnlyTheDirtyRange)
    {
        SliceSet set(k_stride, k_topology);
        const auto a = set.add(vertices(2, 1));
        set.add(vertices(1, 2));
        set.add(vertices(2, 3));
        auto buffer = make_filled(set.size_bytes(), 0xAA);
        std::shared_ptr<VKBuffer> staging;
        set.flush(buffer, staging);

        const std::vector<uint8_t> marker(2 * k_stride, k_sentinel);
        Buffers::upload_to_gpu(marker.data(), marker.size(), buffer, nullptr, 3 * k_stride);
        set.update(a, vertices(2, 9));
        set.flush(buffer, staging);
        track(staging);

        EXPECT_EQ(read_all(buffer), concat({ vertices(2, 9), vertices(1, 2), vertices(2, k_sentinel) }));
    }

    TEST_P(StagingUtilsDeviceTest, FlushWithNothingChangedLeavesTheBufferAlone)
    {
        SliceSet set(k_stride, k_topology);
        set.add(vertices(2, 1));
        auto buffer = make_filled(set.size_bytes(), 0xAA);
        std::shared_ptr<VKBuffer> staging;
        set.flush(buffer, staging);

        const std::vector<uint8_t> marker(2 * k_stride, k_sentinel);
        Buffers::upload_to_gpu(marker.data(), marker.size(), buffer);
        set.flush(buffer, staging);
        track(staging);

        EXPECT_EQ(read_all(buffer), marker);
    }

    TEST_P(StagingUtilsDeviceTest, ResizingABlockUploadsTheShiftedTail)
    {
        SliceSet set(k_stride, k_topology);
        set.add(vertices(2, 1));
        const auto b = set.add(vertices(1, 2));
        set.add(vertices(2, 3));
        auto buffer = make_filled(set.size_bytes() + 4 * k_stride, 0xAA);
        std::shared_ptr<VKBuffer> staging;
        set.flush(buffer, staging);

        set.update(b, vertices(3, 9));
        set.flush(buffer, staging);
        track(staging);

        const auto bytes = read_all(buffer);
        const auto expected = concat({ vertices(2, 1), vertices(3, 9), vertices(2, 3) });
        EXPECT_TRUE(std::equal(expected.begin(), expected.end(), bytes.begin()));
    }

    TEST_P(StagingUtilsDeviceTest, RemovingABlockCompactsTheUploadedData)
    {
        SliceSet set(k_stride, k_topology);
        set.add(vertices(2, 1));
        const auto b = set.add(vertices(1, 2));
        set.add(vertices(2, 3));
        auto buffer = make_filled(set.size_bytes(), 0xAA);
        std::shared_ptr<VKBuffer> staging;
        set.flush(buffer, staging);

        set.remove(b);
        set.flush(buffer, staging);
        track(staging);

        const auto bytes = read_all(buffer);
        const auto expected = concat({ vertices(2, 1), vertices(2, 3) });
        EXPECT_TRUE(std::equal(expected.begin(), expected.end(), bytes.begin()));
    }

    TEST_P(StagingUtilsDeviceTest, FlushGrowsAnUndersizedTargetAndUploadsEverything)
    {
        SliceSet set(k_stride, k_topology);
        set.add(vertices(4, 1));
        set.add(vertices(6, 2));
        auto buffer = make_filled(16, 0xAA);
        std::shared_ptr<VKBuffer> staging;

        set.flush(buffer, staging);
        track(staging);

        ASSERT_GE(buffer->get_size_bytes(), set.size_bytes());
        const auto bytes = read_all(buffer);
        const auto expected = concat({ vertices(4, 1), vertices(6, 2) });
        EXPECT_TRUE(std::equal(expected.begin(), expected.end(), bytes.begin()));
    }

    TEST_P(StagingUtilsDeviceTest, ManyDirtyBlocksUploadCorrectlyInOneFlush)
    {
        constexpr size_t block_count = 200;
        SliceSet set(k_stride, k_topology);
        std::vector<SliceSet::SliceId> ids;
        for (size_t i = 0; i < block_count; ++i) {
            ids.push_back(set.add(vertices(1, static_cast<uint8_t>(i))));
        }
        auto buffer = make_filled(set.size_bytes(), 0xAA);
        std::shared_ptr<VKBuffer> staging;
        set.flush(buffer, staging);

        for (size_t i = 0; i < block_count; i += 2) {
            set.update(ids[i], vertices(1, static_cast<uint8_t>(255 - i)));
        }
        set.flush(buffer, staging);
        track(staging);

        const auto bytes = read_all(buffer);
        for (size_t i = 0; i < block_count; ++i) {
            const auto expected = static_cast<uint8_t>(i % 2 == 0 ? 255 - i : i);
            EXPECT_EQ(bytes[i * k_stride], expected) << "block " << i;
        }
    }

    INSTANTIATE_TEST_SUITE_P(
        HostAndDevice,
        StagingUtilsDeviceTest,
        ::testing::Bool(),
        [](const ::testing::TestParamInfo<bool>& info) {
            return info.param ? std::string("DeviceLocal") : std::string("HostVisible");
        });

    class StagingUtilsStagingReuseTest : public VulkanDeviceTest {
    protected:
        void TearDown() override
        {
            auto* service = Registry::BackendRegistry::instance()
                                .get_service<Registry::Service::BufferService>();
            for (const auto& buffer : buffers) {
                if (service && buffer && buffer->is_initialized()) {
                    service->destroy_buffer(buffer);
                }
            }
            buffers.clear();
            VulkanDeviceTest::TearDown();
        }

        std::shared_ptr<VKBuffer> device_buffer(size_t bytes)
        {
            auto buffer = std::make_shared<VKBuffer>(
                bytes, VKBuffer::Usage::VERTEX, Kakshya::DataModality::UNKNOWN);
            auto* service = Registry::BackendRegistry::instance()
                                .get_service<Registry::Service::BufferService>();
            EXPECT_NE(service, nullptr);
            service->initialize_buffer(buffer);
            buffers.push_back(buffer);
            return buffer;
        }

        std::vector<std::shared_ptr<VKBuffer>> buffers;
    };

    TEST_F(StagingUtilsStagingReuseTest, DeviceLocalUploadCreatesStagingWhenNoneIsSupplied)
    {
        auto target = device_buffer(64);
        const std::vector<uint8_t> source(64, 0x55);
        const std::array<std::pair<size_t, size_t>, 1> intervals { { { 0, 16 } } };
        std::shared_ptr<VKBuffer> staging;

        Buffers::upload_intervals(source.data(), intervals, target, staging);
        buffers.push_back(staging);

        ASSERT_NE(staging, nullptr);
        EXPECT_GE(staging->get_size_bytes(), 16U);
    }

    TEST_F(StagingUtilsStagingReuseTest, SufficientStagingIsReusedAndASmallOneIsReplaced)
    {
        auto target = device_buffer(256);
        const std::vector<uint8_t> source(256, 0x55);
        auto staging = Buffers::create_staging_buffer(128);
        buffers.push_back(staging);
        const auto original = staging;

        const std::array<std::pair<size_t, size_t>, 1> fits { { { 0, 64 } } };
        Buffers::upload_intervals(source.data(), fits, target, staging);
        EXPECT_EQ(staging, original);

        const std::array<std::pair<size_t, size_t>, 1> overflows { { { 0, 256 } } };
        Buffers::upload_intervals(source.data(), overflows, target, staging);
        buffers.push_back(staging);
        EXPECT_NE(staging, original);
        EXPECT_GE(staging->get_size_bytes(), 256U);
    }

}
}
