#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include <random>

#include "../../support/gpu_dispatch_test_support.hpp"

#include "MayaFlux/Yantra/Executors/Audio/LaggedPair.hpp"
#include "MayaFlux/Yantra/Executors/Audio/SegmentedReductionContext.hpp"
#include "MayaFlux/Yantra/Executors/Audio/WordPairMap.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test {
namespace {

    using namespace GpuDispatch;
    using Portal::Graphics::WordPairOp;
    using Yantra::GpuBufferBinding;
    using Yantra::GpuDispatchRecorder;
    using Portal::Graphics::KernelOp;
    using Portal::Graphics::PairMap;
    using Portal::Graphics::ReductionMap;
    using Yantra::SegmentedReductionContext;
    using Yantra::SegmentGeometry;
    using Yantra::WordPairOperand;
    using Yantra::lagged_pair_spec;
    using Yantra::lagged_pair_stage;
    using Yantra::word_pair_map_spec;
    using Yantra::word_pair_map_stage;
    using Yantra::WordPairExecutionContext;

    constexpr auto k_word = GpuBufferBinding::ElementType::VEC2_U32;
    constexpr uint32_t k_product_result = 2;
    constexpr uint32_t k_root_operand = 0;
    constexpr uint32_t k_root_result = 1;

    struct Pass {
        std::unique_ptr<WordPairExecutionContext> context;
        Yantra::GpuComputeConfig config;
        WordPairOp op;
    };

    Pass make_pass(WordPairOp op, const std::shared_ptr<GpuDispatchRecorder>& recording = nullptr)
    {
        const auto spec = word_pair_map_spec(op);
        auto config = Yantra::config_from_spec(spec);
        auto context = std::make_unique<WordPairExecutionContext>(config, Yantra::bindings_from_spec(spec), false, "SharedResources");
        if (recording) {
            context->set_recording(recording);
        }
        return { std::move(context), std::move(config), op };
    }

    std::vector<double> positive_values(size_t count, uint64_t seed)
    {
        std::mt19937_64 engine(seed);
        std::vector<double> out(count);
        for (auto& value : out) {
            const uint64_t draw = engine();
            value = std::bit_cast<double>((static_cast<uint64_t>(1023 - 40 + (draw >> 52) % 81) << 52) | (draw & ((1ULL << 52) - 1ULL)));
        }
        return out;
    }

    bool same(double got, double want)
    {
        return (std::isnan(got) && std::isnan(want)) || std::bit_cast<uint64_t>(got) == std::bit_cast<uint64_t>(want);
    }

    void expect_roots_of_products(const std::vector<double>& got, const std::vector<double>& a, const std::vector<double>& b)
    {
        ASSERT_EQ(got.size(), a.size());
        size_t wrong = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            if (!same(got[i], std::sqrt(a[i] * b[i]))) {
                ++wrong;
            }
        }
        EXPECT_EQ(wrong, 0U) << wrong << " of " << a.size() << " roots differ from the host";
    }

    template <typename Fixture>
    class SharedResourcesGpuTest : public Fixture { };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(SharedResourcesGpuTest, HardwareOptions);

    TYPED_TEST(SharedResourcesGpuTest, AnOwnedBufferDeclaresItsContractAndCountsEachReplacement)
    {
        auto owner = make_pass(WordPairOp::Mul);

        EXPECT_FALSE(owner.context->shared_buffer_info(0, k_product_result).has_value());

        owner.context->ensure_shared_buffer(0, k_product_result, 16, k_word);
        const auto first = owner.context->shared_buffer_info(0, k_product_result);
        ASSERT_TRUE(first.has_value());
        EXPECT_EQ(first->element_type, k_word);
        EXPECT_EQ(first->element_count, 16U);
        EXPECT_EQ(first->byte_size, 128U);
        EXPECT_EQ(first->generation, 1U);
        EXPECT_NE(owner.context->shared_buffer_address(0, k_product_result), 0U);

        owner.context->ensure_shared_buffer(0, k_product_result, 8, k_word);
        EXPECT_EQ(owner.context->shared_buffer_info(0, k_product_result)->generation, 1U);

        owner.context->ensure_shared_buffer(0, k_product_result, 64, k_word);
        const auto grown = owner.context->shared_buffer_info(0, k_product_result);
        ASSERT_TRUE(grown.has_value());
        EXPECT_GE(grown->element_count, 64U);
        EXPECT_EQ(grown->generation, 2U);
    }

    TYPED_TEST(SharedResourcesGpuTest, ASharedResultIsWrittenAndReadBackThroughTheSharedBuffer)
    {
        auto pass = make_pass(WordPairOp::Mul);
        const auto a = positive_values(3000, 1);
        const auto b = positive_values(3000, 2);
        pass.context->ensure_shared_buffer(0, k_product_result, a.size(), k_word);

        pass.context->record_stages({ word_pair_map_stage(*pass.context, pass.config, WordPairOp::Mul, a, b) });
        const auto got = pass.context->download_words(k_product_result, a.size());

        ASSERT_EQ(got.size(), a.size());
        for (size_t i = 0; i < a.size(); ++i) {
            ASSERT_TRUE(same(got[i], a[i] * b[i])) << "element " << i;
        }
    }

    TYPED_TEST(SharedResourcesGpuTest, AnImportedResultFeedsASecondContextWithoutAHostRoundTrip)
    {
        const auto recording = std::make_shared<GpuDispatchRecorder>(true);
        auto producer = make_pass(WordPairOp::Mul, recording);
        auto consumer = make_pass(WordPairOp::Sqrt, recording);
        auto a = positive_values(5000, 3);
        auto b = positive_values(5000, 4);
        a[0] = 0.0;
        a[1] = std::numeric_limits<double>::infinity();
        a[2] = 1.0e300;
        b[2] = 1.0e300;

        producer.context->ensure_shared_buffer(0, k_product_result, a.size(), k_word);
        consumer.context->import_shared(0, k_root_operand, *producer.context, 0, k_product_result, k_word);

        producer.context->record_stages({ word_pair_map_stage(*producer.context, producer.config, WordPairOp::Mul, a, b) });
        consumer.context->record_stages({ word_pair_map_stage(*consumer.context, consumer.config, WordPairOp::Sqrt, a.size()) });
        const auto roots = consumer.context->download_words(k_root_result, a.size());

        expect_roots_of_products(roots, a, b);
    }

    TYPED_TEST(SharedResourcesGpuTest, AnImportAliasesTheOwnersBufferAndItsContract)
    {
        const auto recording = std::make_shared<GpuDispatchRecorder>(true);
        auto producer = make_pass(WordPairOp::Mul, recording);
        auto consumer = make_pass(WordPairOp::Sqrt, recording);
        producer.context->ensure_shared_buffer(0, k_product_result, 32, k_word);
        consumer.context->import_shared(0, k_root_operand, *producer.context, 0, k_product_result, k_word);

        const auto owned = producer.context->shared_buffer_info(0, k_product_result);
        const auto seen = consumer.context->shared_buffer_info(0, k_root_operand);
        ASSERT_TRUE(owned.has_value());
        ASSERT_TRUE(seen.has_value());
        EXPECT_EQ(seen->generation, owned->generation);
        EXPECT_EQ(seen->byte_size, owned->byte_size);
        EXPECT_EQ(consumer.context->shared_buffer_address(0, k_root_operand), producer.context->shared_buffer_address(0, k_product_result));
    }

    TYPED_TEST(SharedResourcesGpuTest, AnImportFollowsTheOwnerWhenItReplacesTheBuffer)
    {
        const auto recording = std::make_shared<GpuDispatchRecorder>(true);
        auto producer = make_pass(WordPairOp::Mul, recording);
        auto consumer = make_pass(WordPairOp::Sqrt, recording);

        const auto a_small = positive_values(64, 5);
        const auto b_small = positive_values(64, 6);
        producer.context->ensure_shared_buffer(0, k_product_result, a_small.size(), k_word);
        consumer.context->import_shared(0, k_root_operand, *producer.context, 0, k_product_result, k_word);
        producer.context->record_stages({ word_pair_map_stage(*producer.context, producer.config, WordPairOp::Mul, a_small, b_small) });
        consumer.context->record_stages({ word_pair_map_stage(*consumer.context, consumer.config, WordPairOp::Sqrt, a_small.size()) });
        expect_roots_of_products(consumer.context->download_words(k_root_result, a_small.size()), a_small, b_small);
        const auto before = consumer.context->shared_buffer_info(0, k_root_operand);

        const auto a_large = positive_values(6000, 7);
        const auto b_large = positive_values(6000, 8);
        producer.context->ensure_shared_buffer(0, k_product_result, a_large.size(), k_word);
        producer.context->record_stages({ word_pair_map_stage(*producer.context, producer.config, WordPairOp::Mul, a_large, b_large) });
        consumer.context->record_stages({ word_pair_map_stage(*consumer.context, consumer.config, WordPairOp::Sqrt, a_large.size()) });
        expect_roots_of_products(consumer.context->download_words(k_root_result, a_large.size()), a_large, b_large);

        const auto after = consumer.context->shared_buffer_info(0, k_root_operand);
        ASSERT_TRUE(before.has_value());
        ASSERT_TRUE(after.has_value());
        EXPECT_GT(after->generation, before->generation);
        EXPECT_GE(after->element_count, a_large.size());
    }

    TYPED_TEST(SharedResourcesGpuTest, AnImportedBufferOutlivesItsOwner)
    {
        const auto recording = std::make_shared<GpuDispatchRecorder>(true);
        auto producer = make_pass(WordPairOp::Mul, recording);
        auto consumer = make_pass(WordPairOp::Sqrt, recording);
        const auto a = positive_values(2000, 9);
        const auto b = positive_values(2000, 10);
        producer.context->ensure_shared_buffer(0, k_product_result, a.size(), k_word);
        consumer.context->import_shared(0, k_root_operand, *producer.context, 0, k_product_result, k_word);
        producer.context->record_stages({ word_pair_map_stage(*producer.context, producer.config, WordPairOp::Mul, a, b) });
        consumer.context->record_stages({ word_pair_map_stage(*consumer.context, consumer.config, WordPairOp::Sqrt, a.size()) });
        static_cast<void>(consumer.context->download_words(k_root_result, a.size()));

        producer.context.reset();

        EXPECT_TRUE(static_cast<bool>(consumer.context->shared_buffer_handle(0, k_root_operand)));
        EXPECT_THROW(consumer.context->ensure_shared_buffer(0, k_root_operand, 1U << 20, k_word), std::invalid_argument);
        consumer.context->record_stages({ word_pair_map_stage(*consumer.context, consumer.config, WordPairOp::Sqrt, a.size()) });
        expect_roots_of_products(consumer.context->download_words(k_root_result, a.size()), a, b);
    }

    TYPED_TEST(SharedResourcesGpuTest, AnImportIsRefusedWithTheReason)
    {
        const auto recording = std::make_shared<GpuDispatchRecorder>(true);
        auto producer = make_pass(WordPairOp::Mul, recording);
        auto consumer = make_pass(WordPairOp::Sqrt, recording);

        EXPECT_THROW(consumer.context->import_shared(0, k_root_operand, *producer.context, 0, k_product_result, k_word), std::invalid_argument);

        producer.context->ensure_shared_buffer(0, k_product_result, 16, k_word);
        EXPECT_THROW(consumer.context->import_shared(0, k_root_operand, *producer.context, 0, k_product_result, GpuBufferBinding::ElementType::FLOAT32), std::invalid_argument);

        auto elsewhere = make_pass(WordPairOp::Sqrt, std::make_shared<GpuDispatchRecorder>(true));
        EXPECT_THROW(elsewhere.context->import_shared(0, k_root_operand, *producer.context, 0, k_product_result, k_word), std::invalid_argument);

        auto unrecorded = make_pass(WordPairOp::Sqrt);
        EXPECT_THROW(unrecorded.context->import_shared(0, k_root_operand, *producer.context, 0, k_product_result, k_word), std::invalid_argument);

        auto occupied = make_pass(WordPairOp::Sqrt, recording);
        occupied.context->ensure_shared_buffer(0, k_root_operand, 4, k_word);
        EXPECT_THROW(occupied.context->import_shared(0, k_root_operand, *producer.context, 0, k_product_result, k_word), std::invalid_argument);

        EXPECT_NO_THROW(consumer.context->import_shared(0, k_root_operand, *producer.context, 0, k_product_result, k_word));
        EXPECT_NO_THROW(consumer.context->import_shared(0, k_root_operand, *producer.context, 0, k_product_result, k_word));
    }

    TYPED_TEST(SharedResourcesGpuTest, OnlyTheOwnerGrowsAnImportedBuffer)
    {
        const auto recording = std::make_shared<GpuDispatchRecorder>(true);
        auto producer = make_pass(WordPairOp::Mul, recording);
        auto consumer = make_pass(WordPairOp::Sqrt, recording);
        producer.context->ensure_shared_buffer(0, k_product_result, 16, k_word);
        consumer.context->import_shared(0, k_root_operand, *producer.context, 0, k_product_result, k_word);

        EXPECT_NO_THROW(consumer.context->ensure_shared_buffer(0, k_root_operand, 16, k_word));
        EXPECT_THROW(consumer.context->ensure_shared_buffer(0, k_root_operand, 4096, k_word), std::invalid_argument);
        EXPECT_EQ(producer.context->shared_buffer_info(0, k_product_result)->generation, 1U);
    }

    TYPED_TEST(SharedResourcesGpuTest, UploadFillAndDownloadOfAnOwnedBufferStillRoundTrip)
    {
        auto owner = make_pass(WordPairOp::Mul);
        owner.context->ensure_shared_buffer(0, k_product_result, 8, k_word);

        owner.context->fill_shared(0, k_product_result, 0xFFFFFFFFU);
        std::vector<uint32_t> words(16, 0);
        owner.context->download_shared(0, k_product_result, words.data(), words.size() * sizeof(uint32_t));
        for (const uint32_t word : words) {
            EXPECT_EQ(word, 0xFFFFFFFFU);
        }

        std::vector<uint32_t> sent(16);
        std::iota(sent.begin(), sent.end(), 100U);
        owner.context->upload_shared_raw(0, k_product_result, reinterpret_cast<const uint8_t*>(sent.data()), sent.size() * sizeof(uint32_t));
        std::vector<uint32_t> back(16, 0);
        owner.context->download_shared(0, k_product_result, back.data(), back.size() * sizeof(uint32_t));
        EXPECT_EQ(back, sent);
    }

    TYPED_TEST(SharedResourcesGpuTest, ManyPassesReadOneSharedUploadAndAgreeWithPassesThatEachUploadIt)
    {
        std::mt19937_64 engine(11);
        std::uniform_real_distribution<double> distribution(-1.0, 1.0);
        std::vector<double> values(5000);
        for (auto& value : values) {
            value = distribution(engine);
        }
        const SegmentGeometry geometry { .length = 512, .stride = 256, .offset = 0, .count = 18 };
        constexpr uint32_t first_lag = 20;
        constexpr uint32_t lag_count = 60;

        const auto recording = std::make_shared<GpuDispatchRecorder>(true);
        SegmentedReductionContext energy(KernelOp::Sum, ReductionMap::Square);
        SegmentedReductionContext peak(KernelOp::Max, ReductionMap::Abs);
        const auto lagged = lagged_pair_spec(PairMap::Product, true);
        const auto lagged_config = Yantra::config_from_spec(lagged);
        WordPairExecutionContext correlation(lagged_config, Yantra::bindings_from_spec(lagged), false, "LaggedPair");
        for (Yantra::GpuDispatchCore* context : std::initializer_list<Yantra::GpuDispatchCore*> { &energy, &peak, &correlation }) {
            context->set_recording(recording);
        }
        const auto words = Yantra::OperationHelper::encode_word_pairs(values, 0, false, false);
        energy.ensure_shared_buffer(0, 0, values.size(), k_word);
        energy.upload_shared_raw(0, 0, words.data(), words.size());
        peak.import_shared(0, 0, energy, 0, 0, k_word);
        correlation.import_shared(0, 0, energy, 0, 0, k_word);

        energy.stage(geometry);
        peak.stage(geometry);
        const Yantra::Datum<std::vector<Kakshya::DataVariant>> none {};
        static_cast<void>(energy.dispatch_async(none));
        static_cast<void>(peak.dispatch_async(none));
        correlation.record_stages({ lagged_pair_stage(correlation, lagged_config, geometry, first_lag, lag_count) });
        const auto got_energy = energy.decode_primary(energy.collect_result());
        const auto got_peak = peak.decode_primary(peak.collect_result());
        const auto got_rows = correlation.download_words(1, static_cast<size_t>(geometry.count) * lag_count);

        SegmentedReductionContext energy_alone(KernelOp::Sum, ReductionMap::Square);
        SegmentedReductionContext peak_alone(KernelOp::Max, ReductionMap::Abs);
        WordPairExecutionContext correlation_alone(lagged_config, Yantra::bindings_from_spec(lagged), false, "LaggedPair");
        const auto want_energy = energy_alone.reduce(values, geometry);
        const auto want_peak = peak_alone.reduce(values, geometry);
        correlation_alone.record_stages({ lagged_pair_stage(correlation_alone, lagged_config, values, geometry, first_lag, lag_count) });
        const auto want_rows = correlation_alone.download_words(1, static_cast<size_t>(geometry.count) * lag_count);

        ASSERT_EQ(got_energy.size(), want_energy.size());
        ASSERT_EQ(got_peak.size(), want_peak.size());
        ASSERT_EQ(got_rows.size(), want_rows.size());
        for (size_t i = 0; i < want_energy.size(); ++i) {
            EXPECT_TRUE(same(got_energy[i], want_energy[i])) << "energy " << i;
            EXPECT_TRUE(same(got_peak[i], want_peak[i])) << "peak " << i;
        }
        for (size_t i = 0; i < want_rows.size(); ++i) {
            ASSERT_TRUE(same(got_rows[i], want_rows[i])) << "row element " << i;
        }
    }

    TYPED_TEST(SharedResourcesGpuTest, OneOperandCanComeFromTheDeviceAndTheOtherFromTheHost)
    {
        const auto recording = std::make_shared<GpuDispatchRecorder>(true);
        auto producer = make_pass(WordPairOp::Mul, recording);
        auto consumer = make_pass(WordPairOp::Div, recording);
        const auto a = positive_values(4000, 12);
        const auto b = positive_values(4000, 13);
        const auto divisors = positive_values(4000, 14);
        producer.context->ensure_shared_buffer(0, k_product_result, a.size(), k_word);
        consumer.context->import_shared(0, 0, *producer.context, 0, k_product_result, k_word);

        producer.context->record_stages({ word_pair_map_stage(*producer.context, producer.config, WordPairOp::Mul, a, b) });
        consumer.context->record_stages({ word_pair_map_stage(*consumer.context, consumer.config, WordPairOp::Div, a.size(),
            WordPairOperand::on_device(), divisors) });
        const auto got = consumer.context->download_words(2, a.size());

        ASSERT_EQ(got.size(), a.size());
        size_t wrong = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            if (!same(got[i], (a[i] * b[i]) / divisors[i])) {
                ++wrong;
            }
        }
        EXPECT_EQ(wrong, 0U);
    }

    TYPED_TEST(SharedResourcesGpuTest, AHostOperandShorterThanTheCountIsRefused)
    {
        auto pass = make_pass(WordPairOp::Div);
        const auto a = positive_values(10, 15);
        const auto b = positive_values(4, 16);

        EXPECT_THROW(static_cast<void>(word_pair_map_stage(*pass.context, pass.config, WordPairOp::Div, 10, a, b)), std::invalid_argument);
        EXPECT_NO_THROW(static_cast<void>(word_pair_map_stage(*pass.context, pass.config, WordPairOp::Div, 4, a, b)));
    }

} // namespace
} // namespace MayaFlux::Test
