#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Transitive/Platform/Toolchain.hpp"

#include <set>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    MAYAFLUX_TEST_WITH(TEST, PlatformCandidatesTest, UnixLibraryDirectoriesKeepTheirOrder, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
#if !defined(MAYAFLUX_PLATFORM_WINDOWS)
        const std::vector<std::string> expected {
            "/usr/local/lib", "/usr/lib", "/lib", "/usr/local/lib64", "/usr/lib64", "/lib64"
        };
        EXPECT_EQ(MayaFlux::Platform::platform_candidates().library_dirs, expected);
#else
        GTEST_SKIP() << "Unix library directories are not used on Windows";
#endif
    }

    MAYAFLUX_TEST_WITH(TEST, PlatformCandidatesTest, UnixIncludeDirectoriesKeepTheirOrder, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
#if !defined(MAYAFLUX_PLATFORM_WINDOWS)
        const std::vector<std::string> expected { "/usr/include", "/usr/local/include" };
        EXPECT_EQ(MayaFlux::Platform::platform_candidates().include_dirs, expected);
#else
        GTEST_SKIP() << "Unix include directories are not used on Windows";
#endif
    }

    MAYAFLUX_TEST_WITH(TEST, PlatformCandidatesTest, DependencyFallbacksEndWithTheSystemIncludeDirectories, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
#if !defined(MAYAFLUX_PLATFORM_WINDOWS)
        const auto fallbacks = MayaFlux::Platform::dependency_fallbacks("eigen3");
        ASSERT_GE(fallbacks.size(), 2U);
        EXPECT_EQ(fallbacks[fallbacks.size() - 2], "/usr/include/eigen3");
        EXPECT_EQ(fallbacks.back(), "/usr/local/include/eigen3");
#else
        GTEST_SKIP() << "Unix include fallbacks are not used on Windows";
#endif
    }

    MAYAFLUX_TEST_WITH(TEST, PlatformCandidatesTest, HomebrewIncludeComesFirstWhenPresent, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
#if defined(MAYAFLUX_PLATFORM_MACOS)
        const auto prefix = MayaFlux::Platform::homebrew_prefix();
        if (!prefix) {
            GTEST_SKIP() << "Homebrew is not installed on this host";
        }
        const auto fallbacks = MayaFlux::Platform::dependency_fallbacks("eigen3");
        ASSERT_FALSE(fallbacks.empty());
        EXPECT_EQ(fallbacks.front(), (*prefix / "include" / "eigen3").string());
#else
        GTEST_SKIP() << "Homebrew lookup is macOS only";
#endif
    }

    MAYAFLUX_TEST_WITH(TEST, PlatformCandidatesTest, UnixCandidatesAreAbsoluteNonEmptyAndUnique, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
#if !defined(MAYAFLUX_PLATFORM_WINDOWS)
        const auto& table = MayaFlux::Platform::platform_candidates();
        for (const auto* list : { &table.library_dirs, &table.include_dirs }) {
            std::set<std::string> seen;
            for (const auto& entry : *list) {
                EXPECT_FALSE(entry.empty());
                EXPECT_TRUE(std::filesystem::path(entry).is_absolute()) << entry;
                EXPECT_TRUE(seen.insert(entry).second) << entry;
            }
        }
#else
        GTEST_SKIP() << "Unix candidate lists are not used on Windows";
#endif
    }

    MAYAFLUX_TEST_WITH(TEST, PlatformCandidatesTest, DependencyFallbacksAcceptHostileLeavesWithoutThrowing, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
#if !defined(MAYAFLUX_PLATFORM_WINDOWS)
        const auto& include_dirs = MayaFlux::Platform::platform_candidates().include_dirs;
        const std::vector<std::string> hostile {
            std::string(), std::string("../escape"), std::string("a/b"),
            std::string(300, 'x'), std::string("eigen\0x", 7)
        };
        for (const auto& leaf : hostile) {
            std::vector<std::string> fallbacks;
            EXPECT_NO_THROW(fallbacks = MayaFlux::Platform::dependency_fallbacks(leaf));
            ASSERT_GE(fallbacks.size(), include_dirs.size());
            const std::size_t offset = fallbacks.size() - include_dirs.size();
            for (std::size_t i = 0; i < include_dirs.size(); ++i) {
                EXPECT_EQ(fallbacks[offset + i], (std::filesystem::path(include_dirs[i]) / leaf).string());
            }
        }
#else
        GTEST_SKIP() << "Unix include fallbacks are not used on Windows";
#endif
    }

}
}
