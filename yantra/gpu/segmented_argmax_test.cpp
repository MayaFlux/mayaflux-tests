#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include <random>

#include "../../support/gpu_dispatch_test_support.hpp"

#include "MayaFlux/Yantra/Executors/Audio/LaggedPair.hpp"
#include "MayaFlux/Yantra/Executors/Audio/SegmentedArgMax.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test {
namespace {

    using namespace GpuDispatch;
    using Portal::Graphics::PairMap;
    using Yantra::ArgMaxResult;
    using Yantra::GpuBufferBinding;
    using Yantra::GpuDispatchRecorder;
    using Yantra::lagged_pair_spec;
    using Yantra::lagged_pair_stage;
    using Yantra::no_argmax_index;
    using Yantra::read_argmax;
    using Yantra::SegmentGeometry;
    using Yantra::segmented_argmax_spec;
    using Yantra::segmented_argmax_stage;
    using Yantra::WordPairExecutionContext;

    struct Pass {
        std::unique_ptr<WordPairExecutionContext> context;
        Yantra::GpuComputeConfig config;
    };

    Pass make_select(uint32_t workgroup, const std::shared_ptr<GpuDispatchRecorder>& recording = nullptr)
    {
        const auto spec = segmented_argmax_spec(workgroup);
        auto config = Yantra::config_from_spec(spec);
        auto context = std::make_unique<WordPairExecutionContext>(config, Yantra::bindings_from_spec(spec), false, "SegmentedArgMax");
        if (recording) {
            context->set_recording(recording);
        }
        return { std::move(context), std::move(config) };
    }

    std::vector<ArgMaxResult> select(Pass& pass, std::span<const double> values, const SegmentGeometry& geometry)
    {
        pass.context->record_stages({ segmented_argmax_stage(*pass.context, pass.config, values, geometry) });
        return read_argmax(*pass.context, geometry.count);
    }

    ArgMaxResult host_argmax(std::span<const double> segment)
    {
        ArgMaxResult best;
        for (size_t i = 0; i < segment.size(); ++i) {
            const double v = segment[i];
            if (std::isnan(v)) {
                continue;
            }
            if (!best.found() || v > best.value) {
                best = { .value = v, .index = static_cast<uint32_t>(i) };
            }
        }
        return best;
    }

    bool same(double got, double want)
    {
        return (std::isnan(got) && std::isnan(want)) || std::bit_cast<uint64_t>(got) == std::bit_cast<uint64_t>(want);
    }

    void expect_matches_host(Pass& pass, const std::vector<double>& values, const SegmentGeometry& geometry)
    {
        const auto got = select(pass, values, geometry);
        ASSERT_EQ(got.size(), geometry.count);
        size_t wrong = 0;
        for (uint32_t s = 0; s < geometry.count; ++s) {
            const std::span<const double> segment(values.data() + geometry.offset + static_cast<size_t>(s) * geometry.stride, geometry.length);
            const auto want = host_argmax(segment);
            if (got[s].index != want.index || !same(got[s].value, want.value)) {
                ++wrong;
                ADD_FAILURE() << "segment " << s << ": index " << got[s].index << " want " << want.index;
            }
        }
        EXPECT_EQ(wrong, 0U);
    }

    std::vector<double> tied_values(size_t count, uint64_t seed)
    {
        std::mt19937_64 engine(seed);
        const std::array<double, 9> pool { -3.0, -1.0, -0.0, 0.0, 1.0, 2.0, 2.0, 2.0, 5.0 };
        std::vector<double> out(count);
        for (auto& value : out) {
            const uint64_t draw = engine();
            value = (draw & 3ULL) == 0 ? std::bit_cast<double>(engine()) : pool[(draw >> 8) % pool.size()];
            if (std::isnan(value)) {
                value = 1.0;
            }
        }
        return out;
    }

    template <typename Fixture>
    class SegmentedArgMaxGpuTest : public Fixture { };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(SegmentedArgMaxGpuTest, HardwareOptions);

    TYPED_TEST(SegmentedArgMaxGpuTest, TheFirstMaximumOfEverySegmentMatchesAHostScanAtEveryWorkgroupSize)
    {
        const auto values = tied_values(20000, 1);
        for (const uint32_t workgroup : { 2U, 32U, 256U, 1024U }) {
            auto pass = make_select(workgroup);
            expect_matches_host(pass, values, { .length = 1000, .stride = 700, .offset = 13, .count = 25 });
            expect_matches_host(pass, values, { .length = 3, .stride = 3, .offset = 0, .count = 500 });
            expect_matches_host(pass, values, { .length = 257, .stride = 257, .offset = 5, .count = 40 });
            expect_matches_host(pass, values, { .length = 1, .stride = 1, .offset = 0, .count = 100 });
        }
    }

    TYPED_TEST(SegmentedArgMaxGpuTest, EqualValuesSelectTheLowestIndexAndSignedZerosAreEqual)
    {
        auto pass = make_select(64);
        std::vector<double> values(64, 7.0);
        std::vector<double> zeros(64, 0.0);
        zeros[5] = -0.0;
        zeros[0] = -0.0;

        const auto flat = select(pass, values, { .length = 64, .stride = 64, .offset = 0, .count = 1 });
        const auto signed_zeros = select(pass, zeros, { .length = 64, .stride = 64, .offset = 0, .count = 1 });

        ASSERT_EQ(flat.size(), 1U);
        EXPECT_EQ(flat[0].index, 0U);
        EXPECT_EQ(flat[0].value, 7.0);
        ASSERT_EQ(signed_zeros.size(), 1U);
        EXPECT_EQ(signed_zeros[0].index, 0U);
        EXPECT_TRUE(std::signbit(signed_zeros[0].value));
    }

    TYPED_TEST(SegmentedArgMaxGpuTest, ANaNIsNeverSelectedAndInfinitiesAreOrdered)
    {
        constexpr double nan = std::numeric_limits<double>::quiet_NaN();
        constexpr double inf = std::numeric_limits<double>::infinity();
        auto pass = make_select(8);
        const std::vector<double> values {
            nan, 2.0, nan, 3.0, nan, 1.0, nan, nan,
            nan, nan, nan, nan, nan, nan, nan, nan,
            -inf, nan, nan, nan, nan, nan, nan, nan,
            1.0, 2.0, inf, 5.0, inf, nan, nan, nan,
            -5.0, -inf, -6.0, nan, nan, nan, nan, nan
        };

        const auto got = select(pass, values, { .length = 8, .stride = 8, .offset = 0, .count = 5 });

        ASSERT_EQ(got.size(), 5U);
        EXPECT_EQ(got[0].index, 3U);
        EXPECT_EQ(got[0].value, 3.0);
        EXPECT_FALSE(got[1].found());
        EXPECT_TRUE(std::isnan(got[1].value));
        EXPECT_EQ(got[1].index, no_argmax_index);
        EXPECT_EQ(got[2].index, 0U);
        EXPECT_EQ(got[2].value, -inf);
        EXPECT_EQ(got[3].index, 2U);
        EXPECT_EQ(got[3].value, inf);
        EXPECT_EQ(got[4].index, 0U);
        EXPECT_EQ(got[4].value, -5.0);
    }

    TYPED_TEST(SegmentedArgMaxGpuTest, AnEmptySegmentHoldsNothingToSelect)
    {
        auto pass = make_select(32);
        const std::vector<double> values(64, 1.0);

        const auto got = select(pass, values, { .length = 0, .stride = 4, .offset = 0, .count = 3 });

        ASSERT_EQ(got.size(), 3U);
        for (const auto& result : got) {
            EXPECT_FALSE(result.found());
            EXPECT_TRUE(std::isnan(result.value));
        }
    }

    TYPED_TEST(SegmentedArgMaxGpuTest, ALaggedPairResultOnTheDeviceIsSelectedWithoutLeavingIt)
    {
        const auto recording = std::make_shared<GpuDispatchRecorder>(true);
        const auto lagged = lagged_pair_spec(PairMap::Product, true);
        auto lagged_config = Yantra::config_from_spec(lagged);
        WordPairExecutionContext correlation(lagged_config, Yantra::bindings_from_spec(lagged), false, "LaggedPair");
        correlation.set_recording(recording);
        auto select_pass = make_select(256, recording);

        std::mt19937_64 engine(5);
        std::vector<double> signal(6000);
        for (size_t i = 0; i < signal.size(); ++i) {
            signal[i] = std::sin(0.11 * static_cast<double>(i)) + 0.2 * std::uniform_real_distribution<double>(-1.0, 1.0)(engine);
        }
        const SegmentGeometry windows { .length = 512, .stride = 256, .offset = 0, .count = 21 };
        constexpr uint32_t first_lag = 20;
        constexpr uint32_t lag_count = 200;
        correlation.ensure_shared_buffer(0, 1, static_cast<size_t>(windows.count) * lag_count, GpuBufferBinding::ElementType::VEC2_U32);
        select_pass.context->import_shared(0, 0, correlation, 0, 1, GpuBufferBinding::ElementType::VEC2_U32);

        correlation.record_stages({ lagged_pair_stage(correlation, lagged_config, signal, windows, first_lag, lag_count) });
        const SegmentGeometry searched { .length = lag_count, .stride = lag_count, .offset = 0, .count = windows.count };
        select_pass.context->record_stages({ segmented_argmax_stage(*select_pass.context, select_pass.config, searched) });
        const auto got = read_argmax(*select_pass.context, windows.count);
        const auto rows = correlation.download_words(1, static_cast<size_t>(windows.count) * lag_count);

        ASSERT_EQ(got.size(), windows.count);
        ASSERT_EQ(rows.size(), static_cast<size_t>(windows.count) * lag_count);
        for (uint32_t w = 0; w < windows.count; ++w) {
            const auto want = host_argmax(std::span<const double>(rows.data() + static_cast<size_t>(w) * lag_count, lag_count));
            EXPECT_EQ(got[w].index, want.index) << "window " << w;
            EXPECT_TRUE(same(got[w].value, want.value)) << "window " << w;
        }
    }

} // namespace
} // namespace MayaFlux::Test
