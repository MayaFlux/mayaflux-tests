#include "gtest/gtest.h"

#include "../../support/gpu_dispatch_test_support.hpp"

#include "MayaFlux/Kinesis/Discrete/OrderKey.hpp"
#include "MayaFlux/Yantra/Executors/WordPairExecutionContext.hpp"

namespace MayaFlux::Test {
namespace {

    using namespace GpuDispatch;
    using Portal::Graphics::GpuBufferBinding;
    using Yantra::Datum;
    using Yantra::ExecutionContext;
    using Yantra::WordPairExecutionContext;

    constexpr std::string_view k_copy_words_source = R"(#version 450
layout(local_size_x = 16) in;
layout(set = 0, binding = 0, std430) readonly buffer In { uvec2 in_words[]; };
layout(set = 0, binding = 1, std430) buffer Out { uvec2 out_words[]; };
layout(push_constant) uniform PC { uint count; } pc;
void main()
{
    const uint x = gl_GlobalInvocationID.x;
    if (x < pc.count) {
        out_words[x] = in_words[x];
    }
}
)";

    constexpr double k_inf = std::numeric_limits<double>::infinity();
    constexpr double k_nan = std::numeric_limits<double>::quiet_NaN();

    struct CopyPush {
        uint32_t count;
    };

    GpuBufferBinding word_binding(uint32_t binding, GpuBufferBinding::Direction direction)
    {
        GpuBufferBinding out;
        out.set = 0;
        out.binding = binding;
        out.direction = direction;
        out.element_type = GpuBufferBinding::ElementType::VEC2_U32;
        return out;
    }

    /**
     * Context over a kernel that copies its uvec2 input to its uvec2 output.
     */
    std::unique_ptr<WordPairExecutionContext> copy_context(bool order_key)
    {
        const auto shader = load_compute(k_copy_words_source);
        EXPECT_NE(shader, Portal::Graphics::INVALID_SHADER);

        auto config = compute_config(shader, sizeof(CopyPush));
        config.workgroup_size = { 16, 1, 1 };
        return std::make_unique<WordPairExecutionContext>(
            config,
            std::vector<GpuBufferBinding> {
                word_binding(0, GpuBufferBinding::Direction::INPUT),
                word_binding(1, GpuBufferBinding::Direction::OUTPUT) },
            order_key);
    }

    /**
     * Stages values, copies them through the kernel and returns the words read back.
     */
    std::vector<uint64_t> copied_words(
        WordPairExecutionContext& context, std::span<const double> values, size_t padded_count = 0, bool complement = false)
    {
        const size_t count = std::max(padded_count, values.size());
        context.stage_words(0, values, padded_count, complement);
        context.set_output_size(1, count * sizeof(uint64_t));
        context.push(CopyPush { .count = static_cast<uint32_t>(count) });
        context.execute(Datum<std::vector<Kakshya::DataVariant>> {}, ExecutionContext {});

        const auto words = context.primary_words();
        std::vector<uint64_t> out(words.size() / 2);
        std::memcpy(out.data(), words.data(), out.size() * sizeof(uint64_t));
        return out;
    }

    const std::vector<double> k_mixed {
        3.5, -2.25, 0.0, -0.0, k_inf, -k_inf, 1.0e-300, -1.0e300, 1.0, k_nan, 7.0
    };

    template <typename Fixture>
    class WordPairExecutionContextGpuTest : public Fixture { };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(WordPairExecutionContextGpuTest, HardwareOptions);

    TYPED_TEST(WordPairExecutionContextGpuTest, ReportsItsEncodingAndHasNoOutputBeforeADispatch)
    {
        const auto keyed = copy_context(true);
        const auto raw = copy_context(false);

        EXPECT_TRUE(keyed->order_keyed());
        EXPECT_FALSE(raw->order_keyed());
        EXPECT_TRUE(keyed->primary_words().empty());
    }

    TYPED_TEST(WordPairExecutionContextGpuTest, OrderKeyWordsReachTheShaderUnchanged)
    {
        const auto context = copy_context(true);

        const auto words = copied_words(*context, k_mixed);

        ASSERT_EQ(words.size(), k_mixed.size());
        for (size_t i = 0; i < k_mixed.size(); ++i) {
            EXPECT_EQ(words[i], Kinesis::Discrete::order_key(k_mixed[i])) << "index " << i;
        }
    }

    TYPED_TEST(WordPairExecutionContextGpuTest, OrderKeyWordsSortLikeTheirValues)
    {
        const auto context = copy_context(true);

        const auto words = copied_words(*context, k_mixed);

        for (size_t a = 0; a < k_mixed.size(); ++a) {
            for (size_t b = 0; b < k_mixed.size(); ++b) {
                if (std::isnan(k_mixed[a]) || std::isnan(k_mixed[b])) {
                    continue;
                }
                EXPECT_EQ(words[a] < words[b], k_mixed[a] < k_mixed[b]) << a << " against " << b;
            }
        }
    }

    TYPED_TEST(WordPairExecutionContextGpuTest, SignedZerosShareOneWordAndNaNIsLargest)
    {
        const auto context = copy_context(true);

        const auto words = copied_words(*context, k_mixed);

        EXPECT_EQ(words[2], words[3]);
        EXPECT_EQ(words[9], ~uint64_t { 0 });
        for (size_t i = 0; i < words.size(); ++i) {
            if (i != 9) {
                EXPECT_LT(words[i], words[9]) << "index " << i;
            }
        }
    }

    TYPED_TEST(WordPairExecutionContextGpuTest, RawEncodingStagesTheIeeeBits)
    {
        const auto context = copy_context(false);

        const auto words = copied_words(*context, k_mixed);

        ASSERT_EQ(words.size(), k_mixed.size());
        for (size_t i = 0; i < k_mixed.size(); ++i) {
            EXPECT_EQ(words[i], std::bit_cast<uint64_t>(k_mixed[i])) << "index " << i;
        }
        EXPECT_NE(words[2], words[3]);
    }

    TYPED_TEST(WordPairExecutionContextGpuTest, PaddingWordsAreTheLargestKey)
    {
        const auto context = copy_context(true);
        const std::vector<double> values { 1.0, 2.0, 3.0, 4.0, 5.0 };

        const auto words = copied_words(*context, values, 8);

        ASSERT_EQ(words.size(), 8U);
        for (size_t i = 0; i < values.size(); ++i) {
            EXPECT_EQ(words[i], Kinesis::Discrete::order_key(values[i])) << "index " << i;
        }
        for (size_t i = values.size(); i < words.size(); ++i) {
            EXPECT_EQ(words[i], ~uint64_t { 0 }) << "index " << i;
        }
    }

    TYPED_TEST(WordPairExecutionContextGpuTest, ComplementReversesTheOrderOfTheWords)
    {
        const auto context = copy_context(true);
        const std::vector<double> values { -4.0, 0.5, 9.0, -0.25 };

        const auto words = copied_words(*context, values, 8, true);

        ASSERT_EQ(words.size(), 8U);
        for (size_t i = 0; i < values.size(); ++i) {
            EXPECT_EQ(words[i], ~Kinesis::Discrete::order_key(values[i])) << "index " << i;
        }
        EXPECT_GT(words[0], words[3]);
        EXPECT_GT(words[3], words[1]);
        EXPECT_GT(words[1], words[2]);
        EXPECT_EQ(words[4], ~uint64_t { 0 });
    }

    TYPED_TEST(WordPairExecutionContextGpuTest, NoWordIsLostAtLargeSizes)
    {
        const auto context = copy_context(true);
        std::vector<double> values(5000);
        std::iota(values.begin(), values.end(), -2500.0);

        const auto words = copied_words(*context, values);

        ASSERT_EQ(words.size(), values.size());
        for (size_t i = 0; i < values.size(); ++i) {
            ASSERT_EQ(words[i], Kinesis::Discrete::order_key(values[i])) << "index " << i;
        }
    }

    TYPED_TEST(WordPairExecutionContextGpuTest, OneContextStagesInputsOfChangingSize)
    {
        const auto context = copy_context(true);

        for (const size_t count : { 300U, 5U, 1024U, 12U }) {
            std::vector<double> values(count);
            std::iota(values.begin(), values.end(), 1.0);

            const auto words = copied_words(*context, values);

            ASSERT_EQ(words.size(), count) << "count " << count;
            for (size_t i = 0; i < count; ++i) {
                ASSERT_EQ(words[i], Kinesis::Discrete::order_key(values[i])) << "count " << count << " index " << i;
            }
        }
    }

} // namespace
} // namespace MayaFlux::Test
