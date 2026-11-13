#include "gtest/gtest.h"

#include "MayaFlux/Transitive/Facet/Facets.hpp"

#include "facet_fixtures.hpp"

namespace MayaFlux::Test {
namespace {

    struct Pose {
        double x { 0.0 };
        double y { 0.0 };
        bool operator==(const Pose&) const = default;
    };

    TEST(FacetTest, DeducesTheTypeFromTheInitialValue)
    {
        const auto onset = Facet(0.5F);
        static_assert(std::is_same_v<decltype(onset), const Facet<float>>);

        EXPECT_FLOAT_EQ(onset.initial(), 0.5F);
    }

    TEST(FacetTest, DefaultFormValueInitialises)
    {
        const auto k = Facet<double>();

        EXPECT_DOUBLE_EQ(k.initial(), 0.0);
    }

    TEST(FacetTest, EachConstructionIsADistinctIdentity)
    {
        const auto a = Facet(1);
        const auto b = Facet(1);

        EXPECT_NE(a.id(), b.id());
    }

    TEST(FacetTest, CopiesShareIdentityAndInitialValue)
    {
        const auto original = Facet(std::string { "kick" });
        const auto copy = original;

        EXPECT_EQ(copy.id(), original.id());
        EXPECT_EQ(copy.initial(), "kick");
    }

    TEST(FacetsTest, SetWithoutValueStoresTheInitialValue)
    {
        Facets bag;
        const auto onset = Facet(0.25F);

        bag.set(onset);

        ASSERT_NE(bag.get(onset), nullptr);
        EXPECT_FLOAT_EQ(*bag.get(onset), 0.25F);
    }

    TEST(FacetsTest, SetWithValueConvertsToTheKeyType)
    {
        Facets bag;
        const auto k2 = Facet<double>();

        bag.set(k2, 0.5F);

        ASSERT_NE(bag.get(k2), nullptr);
        EXPECT_DOUBLE_EQ(*bag.get(k2), 0.5);
    }

    TEST(FacetsTest, GetAbsentKeyReturnsNullAndHasIsFalse)
    {
        const Facets bag;
        const auto k = Facet(3);

        EXPECT_EQ(bag.get(k), nullptr);
        EXPECT_FALSE(bag.has(k));
    }

    TEST(FacetsTest, KeysWithEqualValuesDoNotAlias)
    {
        Facets bag;
        const auto a = Facet(1);
        const auto b = Facet(1);

        bag.set(a, 10);
        bag.set(b, 20);

        EXPECT_EQ(*bag.get(a), 10);
        EXPECT_EQ(*bag.get(b), 20);
        EXPECT_EQ(bag.size(), 2U);
    }

    TEST(FacetsTest, CopyOfKeyReadsTheSameAttribute)
    {
        Facets bag;
        const auto k = Facet(1);
        const auto copy = k;

        bag.set(k, 42);

        ASSERT_NE(bag.get(copy), nullptr);
        EXPECT_EQ(*bag.get(copy), 42);
    }

    TEST(FacetsTest, SetReplacesWithoutGrowing)
    {
        Facets bag;
        const auto k = Facet(1);

        bag.set(k, 1);
        bag.set(k, 2);

        EXPECT_EQ(bag.size(), 1U);
        EXPECT_EQ(*bag.get(k), 2);
    }

    TEST(FacetsTest, EraseRemovesOnlyThatKey)
    {
        Facets bag;
        const auto a = Facet(1);
        const auto b = Facet(2);
        bag.set(a, 1);
        bag.set(b, 2);

        EXPECT_TRUE(bag.erase(a));
        EXPECT_FALSE(bag.erase(a));

        EXPECT_FALSE(bag.has(a));
        EXPECT_EQ(*bag.get(b), 2);
        EXPECT_EQ(bag.size(), 1U);
    }

    TEST(FacetsTest, ClearEmptiesTheBag)
    {
        Facets bag;
        const auto k = Facet(1);
        bag.set(k, 1);

        bag.clear();

        EXPECT_TRUE(bag.empty());
        EXPECT_FALSE(bag.has(k));
    }

    TEST(FacetsTest, MergePreservesDistinctKeysAndUsesIncomingValuesForSharedKeys)
    {
        const Facet<int> existing;
        const Facet<int> shared;
        const Facet<int> incoming;
        Facets destination;
        destination.set(existing, 1);
        destination.set(shared, 2);
        Facets source;
        source.set(shared, 3);
        source.set(incoming, 4);

        destination.merge(source);

        ASSERT_NE(destination.get(existing), nullptr);
        ASSERT_NE(destination.get(shared), nullptr);
        ASSERT_NE(destination.get(incoming), nullptr);
        EXPECT_EQ(*destination.get(existing), 1);
        EXPECT_EQ(*destination.get(shared), 3);
        EXPECT_EQ(*destination.get(incoming), 4);
        EXPECT_EQ(*source.get(shared), 3);
        EXPECT_EQ(destination.size(), 3U);
    }

    TEST(FacetsTest, StructValuesRoundTrip)
    {
        Facets bag;
        const auto pose = Facet(Pose { 1.0, 2.0 });

        bag.set(pose, Pose { 3.0, 4.0 });

        ASSERT_NE(bag.get(pose), nullptr);
        EXPECT_EQ(*bag.get(pose), (Pose { 3.0, 4.0 }));
    }

    TEST(FacetsTest, MoveOnlyValuesAreStored)
    {
        Facets bag;
        const auto k = Facet<std::unique_ptr<int>>();

        bag.set(k, std::make_unique<int>(7));

        ASSERT_NE(bag.get(k), nullptr);
        ASSERT_NE(*bag.get(k), nullptr);
        EXPECT_EQ(**bag.get(k), 7);
    }

    TEST(FacetsTest, CopyingABagSharesValuesAndStaysIndependent)
    {
        Facets original;
        const auto a = Facet(1);
        const auto b = Facet(2);
        original.set(a, 10);

        Facets copy = original;
        copy.set(b, 20);
        copy.set(a, 11);

        EXPECT_EQ(*original.get(a), 10);
        EXPECT_FALSE(original.has(b));
        EXPECT_EQ(*copy.get(a), 11);
        EXPECT_EQ(*copy.get(b), 20);
    }

    TEST(FacetsTest, MovingABagLeavesTheValuesReachable)
    {
        Facets original;
        const auto k = Facet(1);
        original.set(k, 5);

        Facets moved = std::move(original);

        ASSERT_NE(moved.get(k), nullptr);
        EXPECT_EQ(*moved.get(k), 5);
    }

    TEST(FacetsTest, ADeadKeyNeverMatchesANewKeyWhileItsValueIsStored)
    {
        Facets bag;
        const void* stored_id = nullptr;
        {
            const auto gone = Facet(1);
            stored_id = gone.id();
            bag.set(gone, 99);
        }

        std::vector<Facet<int>> fresh;
        for (int i = 0; i < 1000; ++i) {
            fresh.push_back(Facet(i));
            EXPECT_NE(fresh.back().id(), stored_id);
            EXPECT_FALSE(bag.has(fresh.back()));
        }
        EXPECT_EQ(bag.size(), 1U);
    }

    TEST(FacetFromTest, WholeStructUsesOneStableTypeFacet)
    {
        Facets bag;
        const Facet<FacetFromSettings> explicit_key;
        EXPECT_EQ(bag.get<FacetFromSettings>(), nullptr);

        bag.set(FacetFromSettings { .window = 1024, .hop = 128, .threshold = 0.5 });
        ASSERT_NE(bag.get<FacetFromSettings>(), nullptr);
        EXPECT_EQ(bag.get<FacetFromSettings>()->window, 1024U);
        EXPECT_EQ(bag.size(), 1U);
        EXPECT_EQ(bag.get(FacetMember<&FacetFromSettings::window> {}), nullptr);

        bag.set(FacetFromSettings { .window = 2048, .hop = 256, .threshold = 0.25 });
        EXPECT_EQ(bag.get<FacetFromSettings>()->window, 2048U);
        EXPECT_EQ(bag.size(), 1U);

        bag.set(explicit_key, FacetFromSettings { .window = 4096 });
        EXPECT_EQ(bag.get<FacetFromSettings>()->window, 2048U);
        EXPECT_EQ(bag.get(explicit_key)->window, 4096U);
        EXPECT_EQ(bag.size(), 2U);
    }

    TEST(FacetFromTest, MergeCopiesWholeStructValues)
    {
        Facets source;
        source.set(FacetFromSettings { .window = 1024 });
        Facets destination;

        destination.merge(source);

        ASSERT_NE(destination.get<FacetFromSettings>(), nullptr);
        EXPECT_EQ(destination.get<FacetFromSettings>()->window, 1024U);
        EXPECT_EQ(destination.size(), 1U);
    }

    TEST(FacetFromTest, BuildsOneEntryPerMember)
    {
        const auto bag = Facets::from(FacetFromSettings {});

        EXPECT_EQ(bag.size(), 3U);
    }

    TEST(FacetFromTest, EmptyDescriptionGivesAnEmptyBag)
    {
        const auto bag = Facets::from(FacetFromNothing {});

        EXPECT_TRUE(bag.empty());
    }

    TEST(FacetFromTest, StoresTheCurrentValueOfEachMember)
    {
        FacetFromSettings s;
        s.window = 1024;
        s.hop = 128;
        s.threshold = 0.5;

        const auto bag = Facets::from(s);

        ASSERT_NE(bag.get(FacetMember<&FacetFromSettings::window> {}), nullptr);
        EXPECT_EQ(*bag.get(FacetMember<&FacetFromSettings::window> {}), 1024U);
        EXPECT_EQ(*bag.get(FacetMember<&FacetFromSettings::hop> {}), 128U);
        EXPECT_DOUBLE_EQ(*bag.get(FacetMember<&FacetFromSettings::threshold> {}), 0.5);
    }

    TEST(FacetFromTest, MergeDescribedFieldsPreservesCustomKeys)
    {
        const Facet<std::string> custom;
        Facets bag;
        bag.set(custom, std::string { "audio" });
        bag.set(FacetMember<&FacetFromSettings::hop> {}, 64U);

        bag.merge(Facets::from(FacetFromSettings {}));

        ASSERT_NE(bag.get(custom), nullptr);
        ASSERT_NE(bag.get(FacetMember<&FacetFromSettings::hop> {}), nullptr);
        EXPECT_EQ(*bag.get(custom), "audio");
        EXPECT_EQ(*bag.get(FacetMember<&FacetFromSettings::hop> {}), 256U);
        EXPECT_EQ(bag.size(), 4U);
    }

    TEST(FacetFromTest, MembersOfTheSameTypeStayDistinct)
    {
        FacetFromSettings s;
        s.window = 7;
        s.hop = 9;

        const auto bag = Facets::from(s);

        EXPECT_EQ(*bag.get(FacetMember<&FacetFromSettings::window> {}), 7U);
        EXPECT_EQ(*bag.get(FacetMember<&FacetFromSettings::hop> {}), 9U);
    }

    TEST(FacetFromTest, KeysAreTheSameForEveryInstance)
    {
        FacetFromSettings a;
        a.window = 11;
        FacetFromSettings b;
        b.window = 22;

        const auto from_a = Facets::from(a);
        const auto from_b = Facets::from(b);

        EXPECT_EQ(*from_a.get(FacetMember<&FacetFromSettings::window> {}), 11U);
        EXPECT_EQ(*from_b.get(FacetMember<&FacetFromSettings::window> {}), 22U);
    }

    TEST(FacetFromTest, SetWritesAMemberKeyAndOverwritesIt)
    {
        Facets bag;

        bag.set(FacetMember<&FacetFromSettings::hop> {}, 64U);
        bag.set(FacetMember<&FacetFromSettings::hop> {}, 32U);

        EXPECT_EQ(bag.size(), 1U);
        ASSERT_NE(bag.get(FacetMember<&FacetFromSettings::hop> {}), nullptr);
        EXPECT_EQ(*bag.get(FacetMember<&FacetFromSettings::hop> {}), 32U);
    }

    TEST(FacetFromTest, AbsentMemberKeyReturnsNull)
    {
        const Facets bag;

        EXPECT_EQ(bag.get(FacetMember<&FacetFromSettings::window> {}), nullptr);
    }

    TEST(FacetFromTest, FromBagIsIndependentOfTheSource)
    {
        FacetFromSettings s;
        auto bag = Facets::from(s);

        s.window = 2048;

        EXPECT_EQ(*bag.get(FacetMember<&FacetFromSettings::window> {}), 512U);
    }

} // namespace
} // namespace MayaFlux::Test
