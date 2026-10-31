#include "gtest/gtest.h"


#include "../../support/text_test_support.hpp"

#include "MayaFlux/Portal/Text/FontDiscovery.hpp"
#include "MayaFlux/Portal/Text/FontFace.hpp"
#include "MayaFlux/Portal/Text/GlyphAtlas.hpp"
#include "MayaFlux/Portal/Text/TypeFaceFoundry.hpp"

namespace MayaFlux::Test {
namespace {

    using Portal::Text::FontFace;
    using Portal::Text::TypeFaceFoundry;

    // ------------------------------------------------------------------
    // Discovery
    // ------------------------------------------------------------------

    TEST(FontDiscoveryTest, ResolvesASystemFontToAnExistingFile)
    {
        const auto& path = system_font_path();
        ASSERT_TRUE(path.has_value()) << "no system font could be resolved on this host";
        EXPECT_TRUE(std::filesystem::is_regular_file(*path)) << *path;
    }

    TEST(FontDiscoveryTest, UnknownFamilyNeverYieldsAMissingPath)
    {
        const auto found = Portal::Text::find_font("NoSuchFamilyZq91");
        if (found) {
            EXPECT_TRUE(std::filesystem::exists(*found)) << *found;
        }
    }

#if defined(__linux__)
    TEST(FontDiscoveryTest, StyleHintResolvesToAnExistingFileOnFontconfig)
    {
        const auto found = Portal::Text::find_font("sans-serif", "Bold");
        ASSERT_TRUE(found.has_value());
        EXPECT_TRUE(std::filesystem::exists(*found)) << *found;
    }
#endif

    // ------------------------------------------------------------------
    // FontFace
    // ------------------------------------------------------------------

    TEST(FontFaceTest, LoadBeforeTheLibraryIsInitializedFails)
    {
        if (TypeFaceFoundry::instance().is_initialized()) {
            GTEST_SKIP() << "FreeType already initialized by an earlier test in this process";
        }
        const auto& path = system_font_path();
        ASSERT_TRUE(path.has_value());

        FontFace f;
        EXPECT_FALSE(f.load(*path));
        EXPECT_FALSE(f.is_loaded());
        EXPECT_TRUE(f.path().empty());
    }

    TEST_F(SystemFontTest, LoadExposesPathAndGlyphs)
    {
        EXPECT_TRUE(face.is_loaded());
        EXPECT_EQ(face.path(), font_path);
        ASSERT_NE(face.get_face(), nullptr);
        EXPECT_GT(face.get_face()->num_glyphs, 0);
    }

    TEST_F(SystemFontTest, LoadOfAMissingPathFailsAndLeavesFaceEmpty)
    {
        FontFace f;
        EXPECT_FALSE(f.load("/no/such/dir/missing.ttf"));
        EXPECT_FALSE(f.is_loaded());
        EXPECT_EQ(f.get_face(), nullptr);
        EXPECT_TRUE(f.path().empty());
    }

    TEST_F(SystemFontTest, LoadOfAnOutOfRangeFaceIndexFails)
    {
        FontFace f;
        EXPECT_FALSE(f.load(font_path, 100000));
        EXPECT_FALSE(f.is_loaded());
    }

    TEST_F(SystemFontTest, FailedReloadUnloadsThePreviousFace)
    {
        ASSERT_TRUE(face.is_loaded());
        EXPECT_FALSE(face.load("/no/such/dir/missing.ttf"));
        EXPECT_FALSE(face.is_loaded());
        EXPECT_TRUE(face.path().empty());
    }

    TEST_F(SystemFontTest, ReloadReplacesTheStoredPath)
    {
        const auto& other = second_system_font_path();
        if (!other) {
            GTEST_SKIP() << "host exposes only one resolvable font file";
        }
        ASSERT_TRUE(face.load(*other));
        EXPECT_EQ(face.path(), *other);
        EXPECT_TRUE(face.is_loaded());
    }

    TEST_F(SystemFontTest, UnloadIsIdempotent)
    {
        face.unload();
        EXPECT_FALSE(face.is_loaded());
        EXPECT_TRUE(face.path().empty());
        face.unload();
        EXPECT_FALSE(face.is_loaded());
    }

    // ------------------------------------------------------------------
    // TypeFaceFoundry
    // ------------------------------------------------------------------

    class TypeFaceFoundryLifecycleTest : public ::testing::Test {
    protected:
        void SetUp() override
        {
            if (TypeFaceFoundry::instance().is_initialized()
                || TypeFaceFoundry::instance().get_default_glyph_atlas()) {
                GTEST_SKIP() << "foundry singleton already used earlier in this process";
            }
        }

        void TearDown() override
        {
            TypeFaceFoundry::instance().shutdown();
        }
    };

    TEST_F(TypeFaceFoundryLifecycleTest, InitializeIsIdempotent)
    {
        auto& foundry = TypeFaceFoundry::instance();
        EXPECT_FALSE(foundry.is_initialized());
        EXPECT_TRUE(foundry.initialize());
        EXPECT_TRUE(foundry.is_initialized());
        EXPECT_NE(foundry.get_library(), nullptr);
        EXPECT_TRUE(foundry.initialize());
        EXPECT_TRUE(foundry.is_initialized());
    }

    TEST_F(TypeFaceFoundryLifecycleTest, ShutdownIsIdempotentAndClearsTheLibrary)
    {
        auto& foundry = TypeFaceFoundry::instance();
        ASSERT_TRUE(foundry.initialize());
        foundry.shutdown();
        EXPECT_FALSE(foundry.is_initialized());
        EXPECT_EQ(foundry.get_library(), nullptr);
        foundry.shutdown();
        EXPECT_FALSE(foundry.is_initialized());
    }

    TEST_F(TypeFaceFoundryLifecycleTest, AddFallbackBeforeADefaultFontFails)
    {
        auto& foundry = TypeFaceFoundry::instance();
        ASSERT_TRUE(foundry.initialize());
        const auto& path = system_font_path();
        ASSERT_TRUE(path.has_value());
        EXPECT_FALSE(foundry.add_fallback_font(*path));
    }

    TEST_F(TypeFaceFoundryLifecycleTest, SetDefaultFontPublishesFaceAndAtlas)
    {
        auto& foundry = TypeFaceFoundry::instance();
        ASSERT_TRUE(foundry.initialize());
        const auto& path = system_font_path();
        ASSERT_TRUE(path.has_value());

        ASSERT_TRUE(foundry.set_default_font(*path, 24, 256));
        ASSERT_NE(foundry.get_default_face(), nullptr);
        ASSERT_NE(foundry.get_default_glyph_atlas(), nullptr);
        EXPECT_EQ(foundry.get_default_face()->path(), *path);
        EXPECT_EQ(foundry.get_default_glyph_atlas()->pixel_size(), 24U);
        EXPECT_EQ(foundry.get_default_glyph_atlas()->atlas_size(), 256U);
    }

    TEST_F(TypeFaceFoundryLifecycleTest, FailedSetDefaultFontKeepsThePreviousDefault)
    {
        auto& foundry = TypeFaceFoundry::instance();
        ASSERT_TRUE(foundry.initialize());
        const auto& path = system_font_path();
        ASSERT_TRUE(path.has_value());
        ASSERT_TRUE(foundry.set_default_font(*path, 24, 256));

        const FontFace* before_face = foundry.get_default_face();
        const auto* before_atlas = foundry.get_default_glyph_atlas();

        EXPECT_FALSE(foundry.set_default_font("/no/such/dir/missing.ttf", 48, 512));
        EXPECT_EQ(foundry.get_default_face(), before_face);
        EXPECT_EQ(foundry.get_default_glyph_atlas(), before_atlas);
        EXPECT_EQ(foundry.get_default_glyph_atlas()->pixel_size(), 24U);
    }

    TEST_F(TypeFaceFoundryLifecycleTest, SetDefaultFontBeforeInitializeFails)
    {
        auto& foundry = TypeFaceFoundry::instance();
        const auto& path = system_font_path();
        ASSERT_TRUE(path.has_value());
        EXPECT_FALSE(foundry.set_default_font(*path, 24, 256));
        EXPECT_EQ(foundry.get_default_glyph_atlas(), nullptr);
    }

    TEST_F(TypeFaceFoundryLifecycleTest, FallbackFontLoadsAndMissingFallbackIsRejected)
    {
        auto& foundry = TypeFaceFoundry::instance();
        ASSERT_TRUE(foundry.initialize());
        const auto& path = system_font_path();
        ASSERT_TRUE(path.has_value());
        ASSERT_TRUE(foundry.set_default_font(*path, 24, 256));

        EXPECT_TRUE(foundry.add_fallback_font(*path));
        EXPECT_FALSE(foundry.add_fallback_font("/no/such/dir/missing.ttf"));
    }

    TEST_F(TypeFaceFoundryLifecycleTest, ShutdownUnloadsTheDefaultFace)
    {
        auto& foundry = TypeFaceFoundry::instance();
        ASSERT_TRUE(foundry.initialize());
        const auto& path = system_font_path();
        ASSERT_TRUE(path.has_value());
        ASSERT_TRUE(foundry.set_default_font(*path, 24, 256));

        foundry.shutdown();
        ASSERT_NE(foundry.get_default_face(), nullptr);
        EXPECT_FALSE(foundry.get_default_face()->is_loaded());
    }

} // namespace
} // namespace MayaFlux::Test
