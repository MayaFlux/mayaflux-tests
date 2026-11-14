#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "../../support/gpu_dispatch_test_support.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test {
namespace {

    using namespace GpuDispatch;
    using Portal::Graphics::GpuBufferBinding;
    using Portal::Graphics::INVALID_FENCE;
    using Portal::Graphics::INVALID_SHADER;
    using Portal::Graphics::IndirectDispatch;
    using Yantra::GpuDispatchRecorder;
    using Yantra::GpuResourceManager;

    constexpr uint32_t k_words = 16;
    constexpr size_t k_bytes = k_words * sizeof(uint32_t);
    constexpr std::array<uint32_t, 3> k_one { 1, 1, 1 };

    constexpr std::string_view k_scale_source = R"(#version 450
layout(local_size_x = 16) in;
layout(set = 0, binding = 0, std430) buffer Data { uint values[]; };
layout(push_constant) uniform PC { uint factor; } pc;
void main()
{
    values[gl_GlobalInvocationID.x] *= pc.factor;
}
)";

    constexpr std::string_view k_accumulate_source = R"(#version 450
layout(local_size_x = 16) in;
layout(set = 0, binding = 0, std430) readonly buffer In { uint in_values[]; };
layout(set = 0, binding = 1, std430) buffer Acc { uint acc[]; };
void main()
{
    acc[gl_GlobalInvocationID.x] += in_values[gl_GlobalInvocationID.x];
}
)";

    constexpr std::string_view k_pair_source = R"(#version 450
layout(local_size_x = 16) in;
layout(set = 0, binding = 0, std430) buffer Data { uint values[]; };
layout(set = 1, binding = 0, std430) readonly buffer Extra { uint extra[]; };
void main()
{
    values[gl_GlobalInvocationID.x] += extra[gl_GlobalInvocationID.x];
}
)";

    constexpr std::string_view k_pointer_source = R"(#version 450
#extension GL_EXT_buffer_reference : require
layout(local_size_x = 16) in;
layout(buffer_reference, std430) buffer Words { uint values[]; };
layout(push_constant) uniform PC { Words words; uint addend; } pc;
void main()
{
    pc.words.values[gl_GlobalInvocationID.x] += pc.addend;
}
)";

    constexpr std::string_view k_far_set_source = R"(#version 450
layout(local_size_x = 16) in;
layout(set = 2, binding = 0, std430) buffer Data { uint values[]; };
void main()
{
    values[gl_GlobalInvocationID.x] += 1u;
}
)";

    struct PointerPush {
        uint64_t words {};
        uint32_t addend {};
        uint32_t padding {};
    };

    std::vector<uint8_t> pointer_push(uint64_t address, uint32_t addend)
    {
        const PointerPush push { .words = address, .addend = addend };
        std::vector<uint8_t> out(sizeof(push));
        std::memcpy(out.data(), &push, sizeof(push));
        return out;
    }

    bool init_pointer(GpuResourceManager& manager)
    {
        return manager.initialise("pointer", compute_config(load_compute(k_pointer_source), sizeof(PointerPush)), {});
    }

    bool init_scale(GpuResourceManager& manager)
    {
        return manager.initialise("scale", compute_config(load_compute(k_scale_source), sizeof(uint32_t)),
            { storage_binding(0, 0) });
    }

    bool init_accumulate(GpuResourceManager& manager)
    {
        return manager.initialise("accumulate", compute_config(load_compute(k_accumulate_source), 0),
            { storage_binding(0, 0), storage_binding(0, 1) });
    }

    bool init_pair(GpuResourceManager& manager)
    {
        return manager.initialise("pair", compute_config(load_compute(k_pair_source), 0),
            { storage_binding(0, 0), storage_binding(1, 0) });
    }

    template <typename Fixture>
    class GpuResourceManagerTest : public Fixture {
    };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(GpuResourceManagerTest, HardwareOptions);

    // ------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------

    TYPED_TEST(GpuResourceManagerTest, InitialiseMakesTheUnitReadyAndKeepsItWhenCalledAgain)
    {
        GpuResourceManager manager;

        EXPECT_FALSE(manager.is_ready("scale"));
        ASSERT_TRUE(init_scale(manager));
        EXPECT_TRUE(manager.is_ready("scale"));
        EXPECT_TRUE(init_scale(manager));
        EXPECT_TRUE(manager.is_ready("scale"));
    }

    TYPED_TEST(GpuResourceManagerTest, UnknownKeysAreNotReadyAndUsingThemThrows)
    {
        GpuResourceManager manager;

        EXPECT_FALSE(manager.is_ready("missing"));
        EXPECT_THROW(manager.dispatch("missing", k_one, nullptr, 0), std::runtime_error);
        EXPECT_THROW(manager.ensure_buffer("missing", 0, k_bytes), std::runtime_error);
    }

    TYPED_TEST(GpuResourceManagerTest, ReleasedKeyIsNotReadyAndCanBeInitialisedAgain)
    {
        GpuResourceManager manager;
        ASSERT_TRUE(init_scale(manager));

        manager.release("scale");
        EXPECT_FALSE(manager.is_ready("scale"));
        EXPECT_TRUE(init_scale(manager));
        EXPECT_TRUE(manager.is_ready("scale"));
    }

    TYPED_TEST(GpuResourceManagerTest, ImageBindingsAreRefusedWhileIsolated)
    {
        GpuResourceManager manager;
        GpuBufferBinding image = storage_binding(0, 0);
        image.element_type = GpuBufferBinding::ElementType::IMAGE_STORAGE;

        EXPECT_THROW(manager.initialise("image", compute_config(INVALID_SHADER, 0), { image }),
            std::invalid_argument);
        EXPECT_FALSE(manager.is_ready("image"));
    }

    TYPED_TEST(GpuResourceManagerTest, EnsureBufferOnlyGrows)
    {
        GpuResourceManager manager;
        ASSERT_TRUE(init_scale(manager));

        manager.ensure_buffer("scale", 0, 64);
        EXPECT_EQ(manager.buffer_allocated_bytes("scale", 0), 64U);
        manager.ensure_buffer("scale", 0, 16);
        EXPECT_EQ(manager.buffer_allocated_bytes("scale", 0), 64U);
        manager.ensure_buffer("scale", 0, 128);
        EXPECT_EQ(manager.buffer_allocated_bytes("scale", 0), 128U);
    }

    TYPED_TEST(GpuResourceManagerTest, CleanupSubmitsAnOpenRecording)
    {
        auto recording = std::make_shared<GpuDispatchRecorder>(true);
        GpuResourceManager manager;
        manager.set_recording(recording);
        ASSERT_TRUE(init_scale(manager));
        manager.ensure_buffer("scale", 0, k_bytes);
        upload_words(manager, "scale", 0, ascending(k_words));
        const std::vector<GpuBufferBinding> bindings { storage_binding(0, 0) };
        manager.bind_descriptors_batch("scale", bindings);
        manager.record_stage("scale", k_one, push_bytes(2), {});
        EXPECT_TRUE(recording->is_open());

        manager.cleanup();

        EXPECT_FALSE(recording->is_open());
        EXPECT_FALSE(manager.is_ready("scale"));
    }

    // ------------------------------------------------------------------
    // Dispatch without a recording
    // ------------------------------------------------------------------

    TYPED_TEST(GpuResourceManagerTest, DispatchRunsTheKernelOverUploadedWords)
    {
        GpuResourceManager manager;
        ASSERT_TRUE(init_scale(manager));
        manager.ensure_buffer("scale", 0, k_bytes);
        upload_words(manager, "scale", 0, ascending(k_words));
        const std::vector<GpuBufferBinding> bindings { storage_binding(0, 0) };
        manager.bind_descriptors_batch("scale", bindings);
        const auto factor = push_bytes(3);

        manager.dispatch("scale", k_one, factor.data(), factor.size());

        EXPECT_EQ(download_words(manager, "scale", 0, k_words), scaled(ascending(k_words), 3));
    }

    TYPED_TEST(GpuResourceManagerTest, RecordStageRunsImmediatelyWithoutARecording)
    {
        GpuResourceManager manager;
        ASSERT_TRUE(init_scale(manager));
        EXPECT_FALSE(manager.has_recording());
        manager.ensure_buffer("scale", 0, k_bytes);
        upload_words(manager, "scale", 0, ascending(k_words));
        const std::vector<GpuBufferBinding> bindings { storage_binding(0, 0) };
        manager.bind_descriptors_batch("scale", bindings);

        manager.record_stage("scale", k_one, push_bytes(2), {});

        EXPECT_EQ(download_words(manager, "scale", 0, k_words), scaled(ascending(k_words), 2));
    }

    TYPED_TEST(GpuResourceManagerTest, DispatchAsyncReturnsAFenceThatCompletesWithResults)
    {
        GpuResourceManager manager;
        ASSERT_TRUE(init_scale(manager));
        manager.ensure_buffer("scale", 0, k_bytes);
        upload_words(manager, "scale", 0, ascending(k_words));
        const std::vector<GpuBufferBinding> bindings { storage_binding(0, 0) };
        manager.bind_descriptors_batch("scale", bindings);
        const auto factor = push_bytes(4);

        const auto fence = manager.dispatch_async("scale", k_one, factor.data(), factor.size());
        ASSERT_NE(fence, INVALID_FENCE);
        foundry().wait_for_fence(fence);
        EXPECT_TRUE(foundry().is_fence_signaled(fence));
        foundry().release_fence(fence);

        EXPECT_EQ(download_words(manager, "scale", 0, k_words), scaled(ascending(k_words), 4));
    }

    TYPED_TEST(GpuResourceManagerTest, IndirectStageReadsItsGroupCountsFromABuffer)
    {
        GpuResourceManager manager;
        ASSERT_TRUE(init_scale(manager));
        manager.ensure_buffer("scale", 0, 2 * k_bytes);
        upload_words(manager, "scale", 0, ascending(2 * k_words));
        const std::vector<GpuBufferBinding> bindings { storage_binding(0, 0) };
        manager.bind_descriptors_batch("scale", bindings);
        manager.ensure_shared_buffer(0, 3, 3, GpuBufferBinding::ElementType::UINT32,
            Portal::Graphics::BufferUsageHint::INDIRECT);
        upload_shared_words(manager, 0, 3, { 2, 1, 1 });
        const IndirectDispatch indirect { .buffer = manager.shared_buffer_handle(0, 3).buffer };

        manager.record_stage("scale", k_one, push_bytes(5), indirect);

        EXPECT_EQ(download_words(manager, "scale", 0, 2 * k_words), scaled(ascending(2 * k_words), 5));
    }

    // ------------------------------------------------------------------
    // Second set through the allocated descriptor set
    // ------------------------------------------------------------------

    TYPED_TEST(GpuResourceManagerTest, SecondSetBindsTheSharedBufferThroughItsAllocatedSet)
    {
        GpuResourceManager manager;
        ASSERT_TRUE(init_pair(manager));
        manager.ensure_buffer("pair", 0, k_bytes);
        upload_words(manager, "pair", 0, ascending(k_words));
        manager.ensure_shared_buffer(1, 0, k_words, GpuBufferBinding::ElementType::UINT32);
        upload_shared_words(manager, 1, 0, ascending(k_words, 100));
        const std::vector<GpuBufferBinding> bindings { storage_binding(0, 0), storage_binding(1, 0) };
        manager.bind_descriptors_batch("pair", bindings);

        manager.dispatch("pair", k_one, nullptr, 0);

        EXPECT_EQ(download_words(manager, "pair", 0, k_words),
            summed(ascending(k_words), ascending(k_words, 100)));
    }

    TYPED_TEST(GpuResourceManagerTest, RegrowingTheSharedSecondSetRebindsTheSet)
    {
        GpuResourceManager manager;
        ASSERT_TRUE(init_pair(manager));
        manager.ensure_buffer("pair", 0, k_bytes);
        upload_words(manager, "pair", 0, ascending(k_words));
        manager.ensure_shared_buffer(1, 0, k_words, GpuBufferBinding::ElementType::UINT32);
        upload_shared_words(manager, 1, 0, ascending(k_words, 100));
        const std::vector<GpuBufferBinding> bindings { storage_binding(0, 0), storage_binding(1, 0) };
        manager.bind_descriptors_batch("pair", bindings);
        manager.dispatch("pair", k_one, nullptr, 0);

        manager.ensure_shared_buffer(1, 0, 2 * k_words, GpuBufferBinding::ElementType::UINT32);
        upload_shared_words(manager, 1, 0, ascending(2 * k_words, 200));
        manager.bind_descriptors_batch("pair", bindings);
        manager.dispatch("pair", k_one, nullptr, 0);

        EXPECT_EQ(download_words(manager, "pair", 0, k_words),
            summed(summed(ascending(k_words), ascending(k_words, 100)), ascending(k_words, 200)));
    }

    // ------------------------------------------------------------------
    // Device addresses and the layout rule
    // ------------------------------------------------------------------

    TYPED_TEST(GpuResourceManagerTest, ShaderReadsAndWritesASharedBufferThroughItsDeviceAddress)
    {
        GpuResourceManager manager;
        ASSERT_TRUE(init_pointer(manager));
        manager.ensure_shared_buffer(1, 0, k_words, GpuBufferBinding::ElementType::UINT32);
        upload_shared_words(manager, 1, 0, ascending(k_words));
        const auto push = pointer_push(manager.shared_buffer_address(1, 0), 5);

        manager.dispatch("pointer", k_one, push.data(), push.size());

        EXPECT_EQ(download_shared_words(manager, 1, 0, k_words), summed(ascending(k_words), filled(k_words, 5)));
    }

    TYPED_TEST(GpuResourceManagerTest, DeviceAddressIsZeroUntilTheBufferExistsAndFollowsARegrowth)
    {
        GpuResourceManager manager;
        ASSERT_TRUE(init_pointer(manager));

        EXPECT_EQ(manager.shared_buffer_address(1, 0), 0U);

        manager.ensure_shared_buffer(1, 0, k_words, GpuBufferBinding::ElementType::UINT32);
        const uint64_t first = manager.shared_buffer_address(1, 0);
        EXPECT_NE(first, 0U);

        manager.ensure_shared_buffer(1, 0, k_words / 2, GpuBufferBinding::ElementType::UINT32);
        EXPECT_EQ(manager.shared_buffer_address(1, 0), first);

        manager.ensure_shared_buffer(1, 0, 2 * k_words, GpuBufferBinding::ElementType::UINT32);
        ASSERT_NE(manager.shared_buffer_address(1, 0), 0U);
        upload_shared_words(manager, 1, 0, ascending(2 * k_words));
        const auto push = pointer_push(manager.shared_buffer_address(1, 0), 7);

        manager.dispatch("pointer", { 2, 1, 1 }, push.data(), push.size());

        EXPECT_EQ(download_shared_words(manager, 1, 0, 2 * k_words),
            summed(ascending(2 * k_words), filled(2 * k_words, 7)));
    }

    TYPED_TEST(GpuResourceManagerTest, LaterSetBindingsTheShaderDoesNotUseAreNotHeld)
    {
        GpuResourceManager manager;
        const std::vector<GpuBufferBinding> bindings { storage_binding(0, 0), storage_binding(1, 0) };

        EXPECT_TRUE(manager.holds_binding("scale", 1, 0));
        ASSERT_TRUE(manager.initialise("scale", compute_config(load_compute(k_scale_source), sizeof(uint32_t)), bindings));

        EXPECT_TRUE(manager.holds_binding("scale", 0, 0));
        EXPECT_FALSE(manager.holds_binding("scale", 1, 0));

        manager.ensure_buffer("scale", 0, k_bytes);
        upload_words(manager, "scale", 0, ascending(k_words));
        manager.ensure_shared_buffer(1, 0, k_words, GpuBufferBinding::ElementType::UINT32);
        manager.bind_descriptors_batch("scale", bindings);
        const auto factor = push_bytes(3);

        manager.dispatch("scale", k_one, factor.data(), factor.size());

        EXPECT_EQ(download_words(manager, "scale", 0, k_words), scaled(ascending(k_words), 3));
    }

    TYPED_TEST(GpuResourceManagerTest, ShaderUsingOnlyALaterSetKeepsItsSetNumberAcrossTheGap)
    {
        GpuResourceManager manager;
        const std::vector<GpuBufferBinding> bindings { storage_binding(2, 0) };
        ASSERT_TRUE(manager.initialise("far", compute_config(load_compute(k_far_set_source), 0), bindings));
        EXPECT_TRUE(manager.holds_binding("far", 2, 0));

        manager.ensure_shared_buffer(2, 0, k_words, GpuBufferBinding::ElementType::UINT32);
        upload_shared_words(manager, 2, 0, ascending(k_words));
        manager.bind_descriptors_batch("far", bindings);

        manager.dispatch("far", k_one, nullptr, 0);

        EXPECT_EQ(download_shared_words(manager, 2, 0, k_words), summed(ascending(k_words), filled(k_words, 1)));
    }

    // ------------------------------------------------------------------
    // Recording
    // ------------------------------------------------------------------

    TYPED_TEST(GpuResourceManagerTest, RecordedDispatchesRunOnSubmitAndDownloadWaits)
    {
        auto recording = std::make_shared<GpuDispatchRecorder>(true);
        GpuResourceManager manager;
        manager.set_recording(recording);
        ASSERT_TRUE(init_scale(manager));
        manager.ensure_buffer("scale", 0, k_bytes);
        upload_words(manager, "scale", 0, ascending(k_words));
        const std::vector<GpuBufferBinding> bindings { storage_binding(0, 0) };
        manager.bind_descriptors_batch("scale", bindings);
        const auto factor = push_bytes(3);

        EXPECT_EQ(manager.dispatch_async("scale", k_one, factor.data(), factor.size()), INVALID_FENCE);
        EXPECT_TRUE(recording->is_open());
        EXPECT_NE(recording->submit(), INVALID_FENCE);
        EXPECT_FALSE(recording->is_open());
        recording->wait();

        EXPECT_EQ(download_words(manager, "scale", 0, k_words), scaled(ascending(k_words), 3));
    }

    TYPED_TEST(GpuResourceManagerTest, UploadsBetweenRecordedDispatchesAreSeenInOrder)
    {
        for (const bool recorded : { false, true }) {
            SCOPED_TRACE(recorded ? "recorded" : "immediate");
            auto recording = std::make_shared<GpuDispatchRecorder>(true);
            GpuResourceManager manager;
            manager.set_recording(recorded ? recording : nullptr);
            ASSERT_TRUE(init_accumulate(manager));
            manager.ensure_buffer("accumulate", 0, k_bytes);
            manager.ensure_buffer("accumulate", 1, k_bytes);
            const std::vector<GpuBufferBinding> bindings { storage_binding(0, 0), storage_binding(0, 1) };
            manager.bind_descriptors_batch("accumulate", bindings);
            upload_words(manager, "accumulate", 1, filled(k_words, 0));

            upload_words(manager, "accumulate", 0, ascending(k_words));
            manager.record_stage("accumulate", k_one, {}, {});
            upload_words(manager, "accumulate", 0, ascending(k_words, 100));
            manager.record_stage("accumulate", k_one, {}, {});

            EXPECT_EQ(download_words(manager, "accumulate", 1, k_words),
                summed(ascending(k_words), ascending(k_words, 100)));
        }
    }

    TYPED_TEST(GpuResourceManagerTest, FillSharedRunsInOrderWithTheDispatchesAroundIt)
    {
        for (const bool recorded : { false, true }) {
            SCOPED_TRACE(recorded ? "recorded" : "immediate");
            auto recording = std::make_shared<GpuDispatchRecorder>(true);
            GpuResourceManager manager;
            manager.set_recording(recorded ? recording : nullptr);
            ASSERT_TRUE(init_accumulate(manager));
            manager.ensure_buffer("accumulate", 0, k_bytes);
            manager.ensure_shared_buffer(0, 1, k_words, GpuBufferBinding::ElementType::UINT32);
            const std::vector<GpuBufferBinding> bindings { storage_binding(0, 0), storage_binding(0, 1) };
            manager.bind_descriptors_batch("accumulate", bindings);
            manager.fill_shared(0, 1, 0);

            upload_words(manager, "accumulate", 0, ascending(k_words));
            manager.record_stage("accumulate", k_one, {}, {});
            manager.fill_shared(0, 1, 5);
            upload_words(manager, "accumulate", 0, ascending(k_words, 100));
            manager.record_stage("accumulate", k_one, {}, {});

            EXPECT_EQ(download_shared_words(manager, 0, 1, k_words),
                summed(filled(k_words, 5), ascending(k_words, 100)));
        }
    }

    TYPED_TEST(GpuResourceManagerTest, RegrowingABufferMidRecordingKeepsTheResultCorrect)
    {
        auto recording = std::make_shared<GpuDispatchRecorder>(true);
        GpuResourceManager manager;
        manager.set_recording(recording);
        ASSERT_TRUE(init_scale(manager));
        const std::vector<GpuBufferBinding> bindings { storage_binding(0, 0) };
        manager.ensure_buffer("scale", 0, k_bytes);
        upload_words(manager, "scale", 0, ascending(k_words));
        manager.bind_descriptors_batch("scale", bindings);
        manager.record_stage("scale", k_one, push_bytes(3), {});

        manager.ensure_buffer("scale", 0, 2 * k_bytes);
        upload_words(manager, "scale", 0, ascending(2 * k_words));
        manager.bind_descriptors_batch("scale", bindings);
        manager.record_stage("scale", { 2, 1, 1 }, push_bytes(3), {});

        EXPECT_EQ(download_words(manager, "scale", 0, 2 * k_words), scaled(ascending(2 * k_words), 3));
    }

    // ------------------------------------------------------------------
    // Chained dispatch
    // ------------------------------------------------------------------

    TYPED_TEST(GpuResourceManagerTest, DispatchBatchedAppliesEachPassWithItsOwnPushConstants)
    {
        GpuResourceManager manager;
        ASSERT_TRUE(init_scale(manager));
        manager.ensure_buffer("scale", 0, k_bytes);
        upload_words(manager, "scale", 0, ascending(k_words));
        const std::vector<GpuBufferBinding> bindings { storage_binding(0, 0) };
        manager.bind_descriptors_batch("scale", bindings);
        std::vector<uint32_t> visited;
        Yantra::ExecutionContext ctx;
        ctx.parameters = Yantra::ChainedParams {
            .pass_count = 3,
            .pc_updater = [&visited](uint32_t pass, void* out) {
                const uint32_t factor = pass + 2;
                visited.push_back(pass);
                std::memcpy(out, &factor, sizeof(factor));
            },
            .passes_per_batch = 2,
        };

        manager.dispatch_batched("scale", k_one, sizeof(uint32_t), ctx);

        EXPECT_EQ(visited, (std::vector<uint32_t> { 0, 1, 2 }));
        EXPECT_EQ(download_words(manager, "scale", 0, k_words), scaled(ascending(k_words), 24));
    }

    TYPED_TEST(GpuResourceManagerTest, DispatchBatchedWithARecordingAppendsEveryPassBeforeSubmit)
    {
        auto recording = std::make_shared<GpuDispatchRecorder>(true);
        GpuResourceManager manager;
        manager.set_recording(recording);
        ASSERT_TRUE(init_scale(manager));
        manager.ensure_buffer("scale", 0, k_bytes);
        upload_words(manager, "scale", 0, ascending(k_words));
        const std::vector<GpuBufferBinding> bindings { storage_binding(0, 0) };
        manager.bind_descriptors_batch("scale", bindings);
        Yantra::ExecutionContext ctx;
        ctx.parameters = Yantra::ChainedParams {
            .pass_count = 3,
            .pc_updater = [](uint32_t pass, void* out) {
                const uint32_t factor = pass + 2;
                std::memcpy(out, &factor, sizeof(factor));
            },
            .passes_per_batch = 2,
        };

        manager.dispatch_batched("scale", k_one, sizeof(uint32_t), ctx);
        EXPECT_TRUE(recording->is_open());

        EXPECT_EQ(download_words(manager, "scale", 0, k_words), scaled(ascending(k_words), 24));
        EXPECT_FALSE(recording->is_open());
    }

    TYPED_TEST(GpuResourceManagerTest, DispatchBatchedIndirectUsesTheSeededGroupCounts)
    {
        for (const bool recorded : { false, true }) {
            SCOPED_TRACE(recorded ? "recorded" : "immediate");
            auto recording = std::make_shared<GpuDispatchRecorder>(true);
            GpuResourceManager manager;
            manager.set_recording(recorded ? recording : nullptr);
            ASSERT_TRUE(init_scale(manager));
            manager.ensure_buffer("scale", 0, 2 * k_bytes);
            upload_words(manager, "scale", 0, ascending(2 * k_words));
            const std::vector<GpuBufferBinding> bindings { storage_binding(0, 0) };
            manager.bind_descriptors_batch("scale", bindings);
            manager.ensure_shared_buffer(0, 3, 3, GpuBufferBinding::ElementType::UINT32,
                Portal::Graphics::BufferUsageHint::INDIRECT);
            std::vector<uint32_t> visited;
            Yantra::ExecutionContext ctx;
            ctx.parameters = Yantra::ChainedIndirectParams {
                .pass_count = 2,
                .pc_updater = [&visited](uint32_t pass, uint32_t, void* out) {
                    const uint32_t factor = 3;
                    visited.push_back(pass);
                    std::memcpy(out, &factor, sizeof(factor));
                },
            };

            manager.dispatch_batched_indirect("scale", 0, 3, { 2, 1, 1 }, sizeof(uint32_t), ctx);

            EXPECT_EQ(visited, (std::vector<uint32_t> { 0, 1 }));
            EXPECT_EQ(download_words(manager, "scale", 0, 2 * k_words), scaled(ascending(2 * k_words), 9));
        }
    }

    // ------------------------------------------------------------------
    // Recording compatibility and release
    // ------------------------------------------------------------------

    TYPED_TEST(GpuResourceManagerTest, RecordingWithADifferentIsolationIsRefused)
    {
        GpuResourceManager manager;

        EXPECT_THROW(manager.set_recording(std::make_shared<GpuDispatchRecorder>(false)), std::invalid_argument);
        EXPECT_FALSE(manager.has_recording());
    }

    TYPED_TEST(GpuResourceManagerTest, ReleasingAUnitWithRecordedWorkLeavesNoValidationError)
    {
        auto recording = std::make_shared<GpuDispatchRecorder>(true);
        GpuResourceManager manager;
        manager.set_recording(recording);
        ASSERT_TRUE(init_scale(manager));
        manager.ensure_buffer("scale", 0, k_bytes);
        upload_words(manager, "scale", 0, ascending(k_words));
        const std::vector<GpuBufferBinding> bindings { storage_binding(0, 0) };
        manager.bind_descriptors_batch("scale", bindings);
        manager.record_stage("scale", k_one, push_bytes(2), {});

        manager.release("scale");
        recording->wait();

        EXPECT_FALSE(manager.is_ready("scale"));
        EXPECT_FALSE(recording->is_open());
    }

} // namespace
} // namespace MayaFlux::Test
