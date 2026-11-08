#include "gtest/gtest.h"

#include "../../support/gpu_dispatch_test_support.hpp"

#include "MayaFlux/Yantra/OperationSpec/OperationRegistry.hpp"
#include "MayaFlux/Yantra/Sorters/BitonicSorter.hpp"

#include <random>

namespace MayaFlux::Test {
namespace {

    using namespace GpuDispatch;
    using Yantra::BitonicSorter;
    using Yantra::Datum;
    using Yantra::SortingDirection;
    using Yantra::SortingStrategy;
    using Yantra::StandardDataSorter;
    using Yantra::StandardSorterConfig;

    constexpr double k_inf = std::numeric_limits<double>::infinity();
    constexpr double k_nan = std::numeric_limits<double>::quiet_NaN();

    StandardSorterConfig direction_config(bool descending)
    {
        StandardSorterConfig config;
        config.direction = descending ? SortingDirection::DESCENDING : SortingDirection::ASCENDING;
        return config;
    }

    Datum<std::vector<Kakshya::DataVariant>> channels_of(std::initializer_list<std::vector<double>> channels)
    {
        Datum<std::vector<Kakshya::DataVariant>> input;
        for (const auto& channel : channels) {
            input.data.emplace_back(channel);
        }
        return input;
    }

    std::vector<size_t> order_of(BitonicSorter<>& sorter, const std::vector<double>& values)
    {
        return sorter.order(values);
    }

    std::vector<size_t> expected_order(const std::vector<double>& values, bool descending)
    {
        std::vector<size_t> indices(values.size());
        std::iota(indices.begin(), indices.end(), size_t { 0 });
        std::ranges::stable_sort(indices, [&](size_t a, size_t b) {
            const double x = values[a];
            const double y = values[b];
            const bool x_nan = std::isnan(x);
            const bool y_nan = std::isnan(y);
            if (x_nan || y_nan) {
                return descending ? (x_nan && !y_nan) : (!x_nan && y_nan);
            }
            return descending ? x > y : x < y;
        });
        return indices;
    }

    std::vector<double> random_values(size_t count, uint64_t seed, bool with_ties)
    {
        std::mt19937_64 rng(seed);
        std::uniform_real_distribution<double> spread(-1.0, 1.0);
        std::uniform_int_distribution<int> small(-3, 3);
        std::vector<double> values(count);
        for (auto& v : values) {
            v = with_ties ? static_cast<double>(small(rng)) : spread(rng);
        }
        return values;
    }

    const std::vector<double>& doubles_of(const Datum<std::vector<Kakshya::DataVariant>>& result, size_t channel)
    {
        return std::get<std::vector<double>>(result.data.at(channel));
    }

    TEST(BitonicSorterRegistryTest, IsAVulkanCandidateBesideTheCpuSorters)
    {
        static_assert(Yantra::target_of_v<BitonicSorter<>> == Yantra::targets::vulkan);

        const auto bindings = Yantra::make_default_registry().resolve(
            Yantra::ComputationSignature(Yantra::SortingAlgorithm::STANDARD), nullptr);

        const auto count_of = [&](Yantra::Target target) {
            return std::ranges::count_if(bindings, [&](const auto& binding) { return binding.target == target; });
        };
        EXPECT_EQ(count_of(Yantra::targets::vulkan), 1);
        EXPECT_GE(count_of(Yantra::targets::cpu), 1);
    }

    TEST(BitonicSorterDeclineTest, DeclinesOnlyWhatTheDeviceCannotRun)
    {
        const auto accepts = [](SortingDirection direction, SortingStrategy strategy, size_t chunk_size = 1024) {
            StandardSorterConfig config;
            config.direction = direction;
            config.strategy = strategy;
            config.chunk_size = chunk_size;
            return BitonicSorter<>(config).validate_input(channels_of({ { 1.0 } }));
        };

        EXPECT_TRUE(accepts(SortingDirection::ASCENDING, SortingStrategy::COPY_SORT));
        EXPECT_TRUE(accepts(SortingDirection::DESCENDING, SortingStrategy::INDEX_ONLY));
        EXPECT_TRUE(accepts(SortingDirection::ASCENDING, SortingStrategy::PARALLEL_SORT));
        EXPECT_TRUE(accepts(SortingDirection::BIDIRECTIONAL, SortingStrategy::COPY_SORT));
        EXPECT_TRUE(accepts(SortingDirection::ASCENDING, SortingStrategy::CHUNKED_SORT, 64));
        EXPECT_FALSE(accepts(SortingDirection::CUSTOM, SortingStrategy::COPY_SORT));
        EXPECT_FALSE(accepts(SortingDirection::ASCENDING, SortingStrategy::CHUNKED_SORT, 0));
    }

    TEST(BitonicSorterDeclineTest, EveryAlgorithmIsAccepted)
    {
        for (const auto algorithm : { Yantra::SortingAlgorithm::STANDARD, Yantra::SortingAlgorithm::STABLE,
                 Yantra::SortingAlgorithm::PARTIAL, Yantra::SortingAlgorithm::NTH_ELEMENT,
                 Yantra::SortingAlgorithm::HEAP, Yantra::SortingAlgorithm::PARALLEL,
                 Yantra::SortingAlgorithm::RADIX, Yantra::SortingAlgorithm::COUNTING,
                 Yantra::SortingAlgorithm::BUCKET, Yantra::SortingAlgorithm::MERGE_EXTERNAL,
                 Yantra::SortingAlgorithm::QUICK_OPTIMIZED, Yantra::SortingAlgorithm::LAZY_STREAMING,
                 Yantra::SortingAlgorithm::PREDICTIVE_ML, Yantra::SortingAlgorithm::EIGEN_OPTIMIZED,
                 Yantra::SortingAlgorithm::GPU_ACCELERATED }) {
            StandardSorterConfig config;
            config.algorithm = algorithm;
            EXPECT_TRUE(BitonicSorter<>(config).validate_input(channels_of({ { 1.0 } })))
                << static_cast<int>(algorithm);
        }
    }

    template <typename Fixture>
    class BitonicSorterGpuTest : public Fixture { };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(BitonicSorterGpuTest, HardwareOptions);

    TYPED_TEST(BitonicSorterGpuTest, FitnessGrowsWithTheLongestChannel)
    {
        const auto small = BitonicSorter<>::fitness(channels_of({ std::vector<double>(16, 1.0) }));
        const auto large = BitonicSorter<>::fitness(channels_of({ std::vector<double>(16, 1.0), std::vector<double>(40960, 1.0) }));

        EXPECT_GT(small, 0.0F);
        EXPECT_LT(small, 1.0F);
        EXPECT_GT(large, 1.0F);
    }

    TYPED_TEST(BitonicSorterGpuTest, SingleElementKeepsItsIdentity)
    {
        BitonicSorter<> sorter(direction_config(false));

        EXPECT_EQ(order_of(sorter, { 5.0 }), (std::vector<size_t> { 0 }));
    }

    TYPED_TEST(BitonicSorterGpuTest, AscendingMatchesAStableCpuSortAtEverySize)
    {
        BitonicSorter<> sorter(direction_config(false));

        for (const size_t count : { 2U, 3U, 7U, 8U, 9U, 33U, 64U, 100U, 257U, 1000U }) {
            const auto values = random_values(count, 1000 + count, false);
            EXPECT_EQ(order_of(sorter, values), expected_order(values, false)) << "count " << count;
        }
    }

    TYPED_TEST(BitonicSorterGpuTest, DescendingMatchesAStableCpuSortAtEverySize)
    {
        BitonicSorter<> sorter(direction_config(true));

        for (const size_t count : { 2U, 5U, 16U, 31U, 130U, 513U }) {
            const auto values = random_values(count, 2000 + count, false);
            EXPECT_EQ(order_of(sorter, values), expected_order(values, true)) << "count " << count;
        }
    }

    TYPED_TEST(BitonicSorterGpuTest, EqualValuesKeepTheirInputOrder)
    {
        BitonicSorter<> ascending(direction_config(false));
        BitonicSorter<> descending(direction_config(true));

        for (const size_t count : { 6U, 40U, 300U }) {
            const auto values = random_values(count, 3000 + count, true);
            EXPECT_EQ(order_of(ascending, values), expected_order(values, false)) << "ascending, count " << count;
            EXPECT_EQ(order_of(descending, values), expected_order(values, true)) << "descending, count " << count;
        }
    }

    TYPED_TEST(BitonicSorterGpuTest, SignedZerosAreEqualAndKeepTheirInputOrder)
    {
        BitonicSorter<> sorter(direction_config(false));

        EXPECT_EQ(order_of(sorter, { 0.0, -0.0, 0.0, -0.0, -1.0, 1.0 }), (std::vector<size_t> { 4, 0, 1, 2, 3, 5 }));
    }

    TYPED_TEST(BitonicSorterGpuTest, InfinitiesBoundTheRangeAndNaNSortsLast)
    {
        BitonicSorter<> sorter(direction_config(false));

        const std::vector<double> values { 2.0, k_nan, -k_inf, 0.5, k_inf, k_nan, -7.0 };
        EXPECT_EQ(order_of(sorter, values), (std::vector<size_t> { 2, 6, 3, 0, 4, 1, 5 }));
    }

    TYPED_TEST(BitonicSorterGpuTest, NaNSortsFirstWhenDescending)
    {
        BitonicSorter<> sorter(direction_config(true));

        const std::vector<double> values { 2.0, k_nan, -k_inf, 0.5, k_inf, k_nan, -7.0 };
        EXPECT_EQ(order_of(sorter, values), (std::vector<size_t> { 1, 5, 4, 0, 3, 6, 2 }));
    }

    TYPED_TEST(BitonicSorterGpuTest, ValuesThatDifferBeyondFloatPrecisionStayOrdered)
    {
        BitonicSorter<> sorter(direction_config(false));

        const double tiny = std::ldexp(1.0, -40);
        const std::vector<double> values {
            1.0 + tiny,
            1.0,
            1.0 - tiny,
            1.0 + std::ldexp(1.0, -52),
            1.0 - std::ldexp(1.0, -53),
        };
        EXPECT_EQ(order_of(sorter, values), (std::vector<size_t> { 2, 4, 1, 3, 0 }));
    }

    TYPED_TEST(BitonicSorterGpuTest, OneSorterHandlesInputsOfChangingSize)
    {
        BitonicSorter<> sorter(direction_config(false));

        for (const size_t count : { 300U, 5U, 1024U, 12U, 700U }) {
            const auto values = random_values(count, 4000 + count, false);
            EXPECT_EQ(order_of(sorter, values), expected_order(values, false)) << "count " << count;
        }
    }

    TYPED_TEST(BitonicSorterGpuTest, SortedCopyMatchesTheCpuSorterOnEveryChannel)
    {
        StandardSorterConfig config;
        config.direction = SortingDirection::DESCENDING;
        BitonicSorter<> gpu(config);
        StandardDataSorter cpu(config);

        const auto input = channels_of({ random_values(200, 6001, false), random_values(129, 6002, true) });
        const auto gpu_result = gpu.apply_operation(input);
        const auto cpu_result = cpu.apply_operation(input);

        ASSERT_EQ(gpu_result.data.size(), 2U);
        EXPECT_EQ(doubles_of(gpu_result, 0), doubles_of(cpu_result, 0));
        EXPECT_EQ(doubles_of(gpu_result, 1), doubles_of(cpu_result, 1));
    }

    TYPED_TEST(BitonicSorterGpuTest, EveryAlgorithmGivesTheSortedResult)
    {
        const auto input = channels_of({ random_values(150, 8001, false) });
        for (const auto algorithm : { Yantra::SortingAlgorithm::STANDARD, Yantra::SortingAlgorithm::STABLE,
                 Yantra::SortingAlgorithm::HEAP, Yantra::SortingAlgorithm::PARALLEL,
                 Yantra::SortingAlgorithm::RADIX, Yantra::SortingAlgorithm::GPU_ACCELERATED }) {
            StandardSorterConfig config;
            config.algorithm = algorithm;
            const auto result = BitonicSorter<>(config).apply_operation(input);
            EXPECT_TRUE(std::ranges::is_sorted(doubles_of(result, 0))) << static_cast<int>(algorithm);
        }
    }

    TYPED_TEST(BitonicSorterGpuTest, PartialAndNthElementAreSatisfiedByTheFullSort)
    {
        const auto input = channels_of({ random_values(150, 8002, false) });
        for (const auto algorithm : { Yantra::SortingAlgorithm::PARTIAL, Yantra::SortingAlgorithm::NTH_ELEMENT }) {
            StandardSorterConfig config;
            config.algorithm = algorithm;
            const auto result = BitonicSorter<>(config).apply_operation(input);
            const auto& values = doubles_of(result, 0);
            const auto first_half = std::vector<double>(values.begin(), values.begin() + 75);
            auto expected = std::get<std::vector<double>>(input.data[0]);
            std::ranges::sort(expected);
            EXPECT_EQ(first_half, std::vector<double>(expected.begin(), expected.begin() + 75)) << static_cast<int>(algorithm);
        }
    }

    TYPED_TEST(BitonicSorterGpuTest, BidirectionalOrderMatchesTheCpuSorterOnEveryChannel)
    {
        StandardSorterConfig config;
        config.direction = SortingDirection::BIDIRECTIONAL;
        BitonicSorter<> gpu(config);
        StandardDataSorter cpu(config);

        const auto input = channels_of({ random_values(200, 9001, false), random_values(33, 9002, false), random_values(1, 9003, false) });
        const auto gpu_result = gpu.apply_operation(input);
        const auto cpu_result = cpu.apply_operation(input);

        ASSERT_EQ(gpu_result.data.size(), 3U);
        for (size_t channel = 0; channel < 3; ++channel) {
            EXPECT_EQ(doubles_of(gpu_result, channel), doubles_of(cpu_result, channel)) << "channel " << channel;
        }
    }

    TYPED_TEST(BitonicSorterGpuTest, BidirectionalPermutationMatchesKinesisForDistinctValues)
    {
        StandardSorterConfig config;
        config.direction = SortingDirection::BIDIRECTIONAL;
        BitonicSorter<> sorter(config);

        for (const size_t count : { 2U, 5U, 16U, 101U }) {
            auto values = random_values(count, 9100 + count, false);
            std::vector<double> source = values;
            const auto expected = Kinesis::Discrete::span_sort_indices(source, Kinesis::Discrete::SortOrder { SortingDirection::BIDIRECTIONAL });
            EXPECT_EQ(sorter.order(values), expected) << "count " << count;
        }
    }

    TYPED_TEST(BitonicSorterGpuTest, ChunkedSortMatchesTheCpuSorterIncludingAShortLastChunk)
    {
        for (const auto direction : { SortingDirection::ASCENDING, SortingDirection::DESCENDING }) {
            StandardSorterConfig config;
            config.strategy = SortingStrategy::CHUNKED_SORT;
            config.chunk_size = 64;
            config.direction = direction;
            BitonicSorter<> gpu(config);
            StandardDataSorter cpu(config);

            const auto input = channels_of({ random_values(200, 9201, false), random_values(200, 9202, true) });
            const auto gpu_result = gpu.apply_operation(input);
            const auto cpu_result = cpu.apply_operation(input);

            ASSERT_EQ(gpu_result.data.size(), 2U);
            EXPECT_EQ(doubles_of(gpu_result, 0), doubles_of(cpu_result, 0));
            EXPECT_EQ(doubles_of(gpu_result, 1), doubles_of(cpu_result, 1));
        }
    }

    TYPED_TEST(BitonicSorterGpuTest, ChunkedResultIsSortedPerChunkAndRecordsItsPath)
    {
        StandardSorterConfig config;
        config.strategy = SortingStrategy::CHUNKED_SORT;
        config.chunk_size = 10;
        BitonicSorter<> gpu(config);

        const auto result = gpu.apply_operation(channels_of({ random_values(25, 9301, false) }));
        const auto& values = doubles_of(result, 0);

        ASSERT_EQ(values.size(), 25U);
        EXPECT_TRUE(std::is_sorted(values.begin(), values.begin() + 10));
        EXPECT_TRUE(std::is_sorted(values.begin() + 10, values.begin() + 20));
        EXPECT_TRUE(std::is_sorted(values.begin() + 20, values.end()));

        const auto* record = result.facets.get<Yantra::SortRecord>();
        ASSERT_NE(record, nullptr);
        EXPECT_EQ(static_cast<Yantra::SortPath>(record->path), Yantra::SortPath::CHUNKED_MERGED);
    }

    TYPED_TEST(BitonicSorterGpuTest, SortedCopyKeepsTheSettingsRecord)
    {
        StandardSorterConfig config;
        BitonicSorter<> gpu(config);

        const auto result = gpu.apply_operation(channels_of({ random_values(20, 7001, false) }));

        const auto* record = result.facets.get<Yantra::SortRecord>();
        ASSERT_NE(record, nullptr);
        EXPECT_EQ(static_cast<Yantra::SortPath>(record->path), Yantra::SortPath::COPY);
    }

} // namespace
} // namespace MayaFlux::Test
