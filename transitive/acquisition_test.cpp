#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Transitive/Platform/Acquisition.hpp"

#include <atomic>
#include <chrono>
#include <fstream>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {
namespace {

    namespace fs = std::filesystem;
    using Reason = MayaFlux::Platform::AcquireFailure::Reason;

    class AcquisitionFixtureTree {
    public:
        AcquisitionFixtureTree()
        {
            static std::atomic<unsigned> counter { 0 };
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            m_root = fs::temp_directory_path()
                / ("mayaflux-acquisition-" + std::to_string(stamp) + "-" + std::to_string(counter++));
            fs::create_directories(m_root);
        }

        ~AcquisitionFixtureTree()
        {
            std::error_code error;
            fs::remove_all(m_root, error);
        }

        AcquisitionFixtureTree(const AcquisitionFixtureTree&) = delete;
        AcquisitionFixtureTree& operator=(const AcquisitionFixtureTree&) = delete;
        AcquisitionFixtureTree(AcquisitionFixtureTree&&) = delete;
        AcquisitionFixtureTree& operator=(AcquisitionFixtureTree&&) = delete;

        [[nodiscard]] const fs::path& root() const { return m_root; }

        fs::path make_file(const fs::path& relative_path, const std::string& content = "fixture")
        {
            const auto path = m_root / relative_path;
            fs::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary) << content;
            return path;
        }

        fs::path make_directory(const fs::path& relative_path)
        {
            const auto path = m_root / relative_path;
            fs::create_directories(path);
            return path;
        }

        bool make_link(const fs::path& target, const fs::path& link_path, bool directory)
        {
            std::error_code ec;
            if (directory) {
                fs::create_directory_symlink(target, link_path, ec);
            } else {
                fs::create_symlink(target, link_path, ec);
            }
            return !ec;
        }

    private:
        fs::path m_root;
    };

    class WorkingDirectoryGuard {
    public:
        explicit WorkingDirectoryGuard(const fs::path& directory)
            : m_saved(fs::current_path())
        {
            fs::current_path(directory);
        }

        ~WorkingDirectoryGuard()
        {
            std::error_code ec;
            fs::current_path(m_saved, ec);
        }

        WorkingDirectoryGuard(const WorkingDirectoryGuard&) = delete;
        WorkingDirectoryGuard& operator=(const WorkingDirectoryGuard&) = delete;
        WorkingDirectoryGuard(WorkingDirectoryGuard&&) = delete;
        WorkingDirectoryGuard& operator=(WorkingDirectoryGuard&&) = delete;

    private:
        fs::path m_saved;
    };

    std::string platform_library_file(const std::string& base_name)
    {
#if defined(MAYAFLUX_PLATFORM_WINDOWS)
        return base_name + ".dll";
#elif defined(MAYAFLUX_PLATFORM_MACOS)
        return "lib" + base_name + ".dylib";
#else
        return "lib" + base_name + ".so";
#endif
    }

    MayaFlux::Platform::LibrarySource from_path(const fs::path& path)
    {
        return MayaFlux::Platform::LibraryFromPath { .path = path };
    }

    MayaFlux::Platform::LibrarySource from_prefix(const fs::path& prefix, const std::string& base_name)
    {
        return MayaFlux::Platform::LibraryFromPrefix { .prefix = prefix, .base_name = base_name };
    }

    MayaFlux::Platform::LibrarySource from_system(const std::string& file_name)
    {
        return MayaFlux::Platform::LibraryFromSystem { .file_name = file_name };
    }

    MayaFlux::Platform::LibrarySource from_directories(std::vector<fs::path> directories, const std::string& base_name)
    {
        return MayaFlux::Platform::LibraryFromDirectories { .directories = std::move(directories), .base_name = base_name };
    }

    template <typename T>
    void expect_refused(const MayaFlux::Platform::Acquired<T>& result, Reason reason)
    {
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().reason, reason);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PathResolvesAnAbsoluteRegularFile, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto file = fixture.make_file("drop/libexplicit.so");

        const auto result = MayaFlux::Platform::acquire_library(from_path(file));

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->path, file);
        EXPECT_EQ(result->file_name, "libexplicit.so");
        EXPECT_FALSE(result->resolved_by_system);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PathResolvesRelativeToTheWorkingDirectory, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        fixture.make_file("drop/libnear.so");
        const WorkingDirectoryGuard cwd(fixture.root());

        const auto result = MayaFlux::Platform::acquire_library(from_path("drop/libnear.so"));

        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(result->path.is_absolute());
        EXPECT_EQ(result->path, fs::current_path() / "drop/libnear.so");
        EXPECT_EQ(result->file_name, "libnear.so");
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PathAcceptsSpacesDotsAndUnicode, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto file = fixture.make_file("my drop \xC3\xBC/lib w\xC3\xAD" "dget.v2.so.1");

        const auto result = MayaFlux::Platform::acquire_library(from_path(file));

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->path, file);
        EXPECT_EQ(result->file_name, "lib w\xC3\xAD" "dget.v2.so.1");
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PathEmptyIsEmptyRequest, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        const auto result = MayaFlux::Platform::acquire_library(from_path({}));

        expect_refused(result, Reason::EmptyRequest);
        EXPECT_TRUE(result.error().tried.empty());
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PathMissingReportsTheAbsolutePathChecked, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto missing = fixture.root() / "nowhere/libabsent.so";

        const auto result = MayaFlux::Platform::acquire_library(from_path(missing));

        expect_refused(result, Reason::Missing);
        ASSERT_EQ(result.error().tried.size(), 1U);
        EXPECT_EQ(result.error().tried.front(), missing);
        EXPECT_EQ(result.error().request, missing.string());
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PathDirectoryIsNotRegularFile, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto directory = fixture.make_directory("libdir.so");

        expect_refused(MayaFlux::Platform::acquire_library(from_path(directory)), Reason::NotRegularFile);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PathZeroByteFileIsAcceptedBecauseExistenceIsTheCheck, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto file = fixture.make_file("drop/libempty.so", "");

        const auto result = MayaFlux::Platform::acquire_library(from_path(file));

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->path, file);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PathFollowsASymlinkToAFile, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto target = fixture.make_file("real/libtarget.so");
        const auto link = fixture.root() / "libalias.so";
        if (!fixture.make_link(target, link, false)) {
            GTEST_SKIP() << "symlinks unavailable";
        }

        const auto result = MayaFlux::Platform::acquire_library(from_path(link));

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->path, link);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PathDanglingSymlinkIsMissing, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto link = fixture.root() / "libdangling.so";
        if (!fixture.make_link(fixture.root() / "gone.so", link, false)) {
            GTEST_SKIP() << "symlinks unavailable";
        }

        expect_refused(MayaFlux::Platform::acquire_library(from_path(link)), Reason::Missing);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PathEmbeddedNulDoesNotThrow, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const fs::path hostile(fixture.root().string() + std::string("\0/libx.so", 9));

        MayaFlux::Platform::Acquired<MayaFlux::Platform::LibraryLocation> result = std::unexpected(MayaFlux::Platform::AcquireFailure {});
        EXPECT_NO_THROW(result = MayaFlux::Platform::acquire_library(from_path(hostile)));
        EXPECT_FALSE(result.has_value());
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PathOverlongComponentDoesNotThrow, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto hostile = fixture.root() / std::string(400, 'a') / "libx.so";

        MayaFlux::Platform::Acquired<MayaFlux::Platform::LibraryLocation> result = std::unexpected(MayaFlux::Platform::AcquireFailure {});
        EXPECT_NO_THROW(result = MayaFlux::Platform::acquire_library(from_path(hostile)));
        EXPECT_FALSE(result.has_value());
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PrefixFindsTheLibDirectory, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto prefix = fixture.root() / "prefix";
        const auto file_name = platform_library_file("widget");
        const auto expected = fixture.make_file("prefix/lib/" + file_name);

        const auto result = MayaFlux::Platform::acquire_library(from_prefix(prefix, "widget"));

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->path, expected);
        EXPECT_EQ(result->file_name, file_name);
        EXPECT_FALSE(result->resolved_by_system);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PrefixFallsBackToLib64ForFedoraLayouts, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto prefix = fixture.root() / "prefix";
        const auto expected = fixture.make_file("prefix/lib64/" + platform_library_file("widget"));

        const auto result = MayaFlux::Platform::acquire_library(from_prefix(prefix, "widget"));

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->path, expected);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PrefixFallsBackToBin, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto prefix = fixture.root() / "prefix";
        const auto expected = fixture.make_file("prefix/bin/" + platform_library_file("widget"));

        const auto result = MayaFlux::Platform::acquire_library(from_prefix(prefix, "widget"));

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->path, expected);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PrefixPriorityIsLibThenLib64ThenBin, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto prefix = fixture.root() / "prefix";
        const auto file_name = platform_library_file("widget");
        fixture.make_file("prefix/bin/" + file_name);
        fixture.make_file("prefix/lib64/" + file_name);
        const auto lib_file = fixture.make_file("prefix/lib/" + file_name);

        const auto result = MayaFlux::Platform::acquire_library(from_prefix(prefix, "widget"));

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->path, lib_file);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PrefixDecoratedNameInAnyDirectoryBeatsTheExactName, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto prefix = fixture.root() / "prefix";
        fixture.make_file("prefix/lib/widget");
        const auto decorated = fixture.make_file("prefix/bin/" + platform_library_file("widget"));

        const auto result = MayaFlux::Platform::acquire_library(from_prefix(prefix, "widget"));

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->path, decorated);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PrefixAcceptsDotsInTheBaseName, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto prefix = fixture.root() / "prefix";
        const auto expected = fixture.make_file("prefix/lib/" + platform_library_file("widget.v2"));

        const auto result = MayaFlux::Platform::acquire_library(from_prefix(prefix, "widget.v2"));

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->path, expected);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PrefixAcceptsALibPrefixedBaseName, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto prefix = fixture.root() / "prefix";
        const auto expected = fixture.make_file("prefix/lib/" + platform_library_file("widget"));

        const auto result = MayaFlux::Platform::acquire_library(from_prefix(prefix, "libwidget"));

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->path, expected);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PrefixAcceptsAnExactFileName, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto prefix = fixture.root() / "prefix";
        const auto expected = fixture.make_file("prefix/lib/custom.bin");

        const auto result = MayaFlux::Platform::acquire_library(from_prefix(prefix, "custom.bin"));

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->path, expected);
        EXPECT_EQ(result->file_name, "custom.bin");
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PrefixIgnoresDirectoriesNamedLikeTheLibrary, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto prefix = fixture.root() / "prefix";
        fixture.make_directory("prefix/lib/" + platform_library_file("widget"));

        expect_refused(MayaFlux::Platform::acquire_library(from_prefix(prefix, "widget")), Reason::Missing);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PrefixSkipsASubdirectoryThatIsAFile, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto prefix = fixture.root() / "prefix";
        fixture.make_file("prefix/lib");
        const auto expected = fixture.make_file("prefix/lib64/" + platform_library_file("widget"));

        const auto result = MayaFlux::Platform::acquire_library(from_prefix(prefix, "widget"));

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->path, expected);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PrefixDoesNotLookOutsideItsThreeSubdirectories, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto prefix = fixture.root() / "prefix";
        fixture.make_directory("prefix");
        fixture.make_file(platform_library_file("widget"));
        fixture.make_file("prefix/lib/extra/" + platform_library_file("widget"));
        fixture.make_file("prefix/share/" + platform_library_file("widget"));

        expect_refused(MayaFlux::Platform::acquire_library(from_prefix(prefix, "widget")), Reason::Missing);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PrefixMissingListsEveryCandidateInOrder, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto prefix = fixture.root() / "prefix";
        fixture.make_directory("prefix");

        const auto result = MayaFlux::Platform::acquire_library(from_prefix(prefix, "widget"));

        expect_refused(result, Reason::Missing);
        const auto decorated = platform_library_file("widget");
        const std::vector<fs::path> expected {
            prefix / "lib" / decorated,
            prefix / "lib64" / decorated,
            prefix / "bin" / decorated,
            prefix / "lib" / "widget",
            prefix / "lib64" / "widget",
            prefix / "bin" / "widget",
        };
        EXPECT_EQ(result.error().tried, expected);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PrefixRelativeResolvesAgainstTheWorkingDirectory, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        fixture.make_file("local/lib/" + platform_library_file("widget"));
        const WorkingDirectoryGuard cwd(fixture.root());

        const auto result = MayaFlux::Platform::acquire_library(from_prefix("local", "widget"));

        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(result->path.is_absolute());
        EXPECT_EQ(result->path, fs::current_path() / "local" / "lib" / platform_library_file("widget"));
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PrefixEmptyIsEmptyRequest, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        expect_refused(MayaFlux::Platform::acquire_library(from_prefix({}, "widget")), Reason::EmptyRequest);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PrefixMissingPrefixIsNotADirectory, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto prefix = fixture.root() / "absent";

        const auto result = MayaFlux::Platform::acquire_library(from_prefix(prefix, "widget"));

        expect_refused(result, Reason::NotDirectory);
        ASSERT_EQ(result.error().tried.size(), 1U);
        EXPECT_EQ(result.error().tried.front(), prefix);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PrefixFileIsNotADirectory, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto prefix = fixture.make_file("not-a-directory");

        expect_refused(MayaFlux::Platform::acquire_library(from_prefix(prefix, "widget")), Reason::NotDirectory);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PrefixSymlinkedDirectoryResolves, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto real = fixture.make_directory("real-prefix");
        const auto expected = fixture.make_file("real-prefix/lib/" + platform_library_file("widget"));
        const auto link = fixture.root() / "linked-prefix";
        if (!fixture.make_link(real, link, true)) {
            GTEST_SKIP() << "symlinks unavailable";
        }

        const auto result = MayaFlux::Platform::acquire_library(from_prefix(link, "widget"));

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->path, link / "lib" / platform_library_file("widget"));
        EXPECT_TRUE(fs::equivalent(result->path, expected));
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PrefixTrailingSeparatorAndSpacesResolve, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto prefix = (fixture.root() / "prefix spaced").string() + "/";
        const auto expected = fixture.make_file("prefix spaced/lib/" + platform_library_file("widget"));

        const auto result = MayaFlux::Platform::acquire_library(from_prefix(prefix, "widget"));

        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(fs::equivalent(result->path, expected));
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PrefixUnicodeBaseNameResolves, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto prefix = fixture.root() / "prefix";
        const std::string base = "w\xC3\xAD" "dget";
        const auto expected = fixture.make_file("prefix/lib/" + platform_library_file(base));

        const auto result = MayaFlux::Platform::acquire_library(from_prefix(prefix, base));

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->path, expected);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PrefixWrongPlatformSpellingIsMissing, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto prefix = fixture.root() / "prefix";
        const auto correct = platform_library_file("widget");
        const std::vector<std::string> wrong { "widget.so", "widget.dll", "widget.dylib", "libwidget", "libwidget.a", "libwidget.so.1" };
        for (const auto& name : wrong) {
            if (name != correct) {
                fixture.make_file("prefix/lib/" + name);
            }
        }

        expect_refused(MayaFlux::Platform::acquire_library(from_prefix(prefix, "widget")), Reason::Missing);
    }

#if defined(MAYAFLUX_PLATFORM_LINUX)
    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PrefixIsCaseSensitiveOnLinux, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto prefix = fixture.root() / "prefix";
        fixture.make_file("prefix/lib/LIBWIDGET.SO");

        expect_refused(MayaFlux::Platform::acquire_library(from_prefix(prefix, "widget")), Reason::Missing);
    }
#endif

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PrefixOverlongNameDoesNotThrow, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto prefix = fixture.make_directory("prefix");

        MayaFlux::Platform::Acquired<MayaFlux::Platform::LibraryLocation> result = std::unexpected(MayaFlux::Platform::AcquireFailure {});
        EXPECT_NO_THROW(result = MayaFlux::Platform::acquire_library(from_prefix(prefix, std::string(300, 'a'))));
        EXPECT_FALSE(result.has_value());
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PrefixNameRefusesSeparators, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto prefix = fixture.make_directory("prefix");

        std::vector<std::string> hostile { std::string("../escape"), std::string("sub/widget"), std::string("/abs/widget") };
#if defined(MAYAFLUX_PLATFORM_WINDOWS)
        hostile.push_back("sub\\widget");
#endif
        for (const auto& name : hostile) {
            expect_refused(MayaFlux::Platform::acquire_library(from_prefix(prefix, name)), Reason::InvalidName);
        }
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PrefixNameRefusesDotNames, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto prefix = fixture.make_directory("prefix");

        expect_refused(MayaFlux::Platform::acquire_library(from_prefix(prefix, ".")), Reason::InvalidName);
        expect_refused(MayaFlux::Platform::acquire_library(from_prefix(prefix, "..")), Reason::InvalidName);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PrefixNameRefusesControlCharactersAndNul, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto prefix = fixture.make_directory("prefix");

        const std::vector<std::string> hostile { std::string("wid\0get", 7), "wid\tget", "wid\nget", std::string("wid\x7Fget") };
        for (const auto& name : hostile) {
            expect_refused(MayaFlux::Platform::acquire_library(from_prefix(prefix, name)), Reason::InvalidName);
        }
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PrefixNameEmptyIsEmptyRequest, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto prefix = fixture.make_directory("prefix");

        expect_refused(MayaFlux::Platform::acquire_library(from_prefix(prefix, "")), Reason::EmptyRequest);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, PrefixNameIsCheckedBeforeThePrefix, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        expect_refused(MayaFlux::Platform::acquire_library(from_prefix("relative/prefix", "../bad")), Reason::InvalidName);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, DirectoriesFindTheLibraryDirectlyInsideTheDirectory, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto directory = fixture.make_directory("system");
        const auto expected = fixture.make_file("system/" + platform_library_file("widget"));

        const auto result = MayaFlux::Platform::acquire_library(from_directories({ directory }, "widget"));

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->path, expected);
        EXPECT_EQ(result->file_name, platform_library_file("widget"));
        EXPECT_FALSE(result->resolved_by_system);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, DirectoriesDoNotLookInLibLib64OrBinBelowTheDirectory, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto directory = fixture.make_directory("system");
        const auto file_name = platform_library_file("widget");
        for (const char* subdirectory : { "lib", "lib64", "bin", "sub" }) {
            fixture.make_file(std::string("system/") + subdirectory + "/" + file_name);
        }

        expect_refused(MayaFlux::Platform::acquire_library(from_directories({ directory }, "widget")), Reason::Missing);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, DirectoriesFirstDirectoryWins, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto first = fixture.make_directory("first");
        const auto second = fixture.make_directory("second");
        const auto file_name = platform_library_file("widget");
        const auto in_first = fixture.make_file("first/" + file_name);
        const auto in_second = fixture.make_file("second/" + file_name);

        const auto forward = MayaFlux::Platform::acquire_library(from_directories({ first, second }, "widget"));
        const auto reversed = MayaFlux::Platform::acquire_library(from_directories({ second, first }, "widget"));

        ASSERT_TRUE(forward.has_value());
        ASSERT_TRUE(reversed.has_value());
        EXPECT_EQ(forward->path, in_first);
        EXPECT_EQ(reversed->path, in_second);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, DirectoriesDecoratedNameInALaterDirectoryBeatsTheExactNameInAnEarlierOne, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto first = fixture.make_directory("first");
        const auto second = fixture.make_directory("second");
        fixture.make_file("first/widget");
        const auto decorated = fixture.make_file("second/" + platform_library_file("widget"));

        const auto result = MayaFlux::Platform::acquire_library(from_directories({ first, second }, "widget"));

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->path, decorated);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, DirectoriesAcceptAnExactFileName, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto directory = fixture.make_directory("system");
        const auto expected = fixture.make_file("system/custom.bin");

        const auto result = MayaFlux::Platform::acquire_library(from_directories({ directory }, "custom.bin"));

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->path, expected);
        EXPECT_EQ(result->file_name, "custom.bin");
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, DirectoriesAcceptALibPrefixedBaseName, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto directory = fixture.make_directory("system");
        const auto expected = fixture.make_file("system/" + platform_library_file("widget"));

        const auto result = MayaFlux::Platform::acquire_library(from_directories({ directory }, "libwidget"));

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->path, expected);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, DirectoriesMissingListsEveryCandidateNameFirstThenDirectory, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto first = fixture.make_directory("first");
        const auto second = fixture.make_directory("second");

        const auto result = MayaFlux::Platform::acquire_library(from_directories({ first, second }, "widget"));

        expect_refused(result, Reason::Missing);
        const auto decorated = platform_library_file("widget");
        const std::vector<fs::path> expected {
            first / decorated,
            second / decorated,
            first / "widget",
            second / "widget",
        };
        EXPECT_EQ(result.error().tried, expected);
        EXPECT_EQ(result.error().request, "widget");
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, DirectoriesIgnoreADirectoryNamedLikeTheLibrary, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto directory = fixture.make_directory("system");
        fixture.make_directory("system/" + platform_library_file("widget"));
        fixture.make_directory("system/widget");

        expect_refused(MayaFlux::Platform::acquire_library(from_directories({ directory }, "widget")), Reason::Missing);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, DirectoriesSkipAnEntryThatIsAFile, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto not_a_directory = fixture.make_file("not-a-directory");
        const auto directory = fixture.make_directory("system");
        const auto expected = fixture.make_file("system/" + platform_library_file("widget"));

        const auto result = MayaFlux::Platform::acquire_library(from_directories({ not_a_directory, directory }, "widget"));

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->path, expected);
        expect_refused(MayaFlux::Platform::acquire_library(from_directories({ not_a_directory }, "widget")), Reason::Missing);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, DirectoriesRecordAndSkipADirectoryThatDoesNotExist, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto absent = fixture.root() / "absent";
        const auto directory = fixture.make_directory("system");
        const auto expected = fixture.make_file("system/" + platform_library_file("widget"));

        const auto found = MayaFlux::Platform::acquire_library(from_directories({ absent, directory }, "widget"));
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(found->path, expected);

        const auto missing = MayaFlux::Platform::acquire_library(from_directories({ absent }, "widget"));
        expect_refused(missing, Reason::Missing);
        const std::vector<fs::path> tried { absent / platform_library_file("widget"), absent / "widget" };
        EXPECT_EQ(missing.error().tried, tried);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, DirectoriesEmptyEntriesAreIgnored, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto directory = fixture.make_directory("system");
        const auto expected = fixture.make_file("system/" + platform_library_file("widget"));

        const auto result = MayaFlux::Platform::acquire_library(from_directories({ fs::path {}, directory, fs::path {} }, "widget"));

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->path, expected);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, DirectoriesNothingToSearchIsEmptyRequest, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        const auto none = MayaFlux::Platform::acquire_library(from_directories({}, "widget"));
        expect_refused(none, Reason::EmptyRequest);
        EXPECT_TRUE(none.error().tried.empty());

        expect_refused(MayaFlux::Platform::acquire_library(from_directories({ fs::path {}, fs::path {} }, "widget")), Reason::EmptyRequest);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, DirectoriesRelativeEntriesResolveAgainstTheWorkingDirectory, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        fixture.make_file("local/" + platform_library_file("widget"));
        const WorkingDirectoryGuard cwd(fixture.root());

        const auto result = MayaFlux::Platform::acquire_library(from_directories({ "local" }, "widget"));

        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(result->path.is_absolute());
        EXPECT_EQ(result->path, fs::current_path() / "local" / platform_library_file("widget"));
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, DirectoriesFollowASymlinkedDirectory, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto real = fixture.make_directory("real");
        const auto expected = fixture.make_file("real/" + platform_library_file("widget"));
        const auto link = fixture.root() / "linked";
        if (!fixture.make_link(real, link, true)) {
            GTEST_SKIP() << "symlinks unavailable";
        }

        const auto result = MayaFlux::Platform::acquire_library(from_directories({ link }, "widget"));

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->path, link / platform_library_file("widget"));
        EXPECT_TRUE(fs::equivalent(result->path, expected));
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, DirectoriesWrongPlatformSpellingIsMissing, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto directory = fixture.make_directory("system");
        const auto correct = platform_library_file("widget");
        const std::vector<std::string> wrong { "widget.so", "widget.dll", "widget.dylib", "libwidget", "libwidget.a", "libwidget.so.1" };
        for (const auto& name : wrong) {
            if (name != correct) {
                fixture.make_file("system/" + name);
            }
        }

        expect_refused(MayaFlux::Platform::acquire_library(from_directories({ directory }, "widget")), Reason::Missing);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, DirectoriesRefuseHostileNamesBeforeLookingAtTheDirectories, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        std::vector<std::string> hostile { std::string("../escape"), std::string("sub/widget"), std::string("/abs/widget"), std::string("."), std::string(".."),
            std::string("wid\0get", 7), std::string("wid\tget"), std::string("wid\nget"), std::string("wid\x7Fget") };
#if defined(MAYAFLUX_PLATFORM_WINDOWS)
        hostile.push_back("sub\\widget");
#endif
        for (const auto& name : hostile) {
            const auto result = MayaFlux::Platform::acquire_library(from_directories({ "relative/directory" }, name));
            expect_refused(result, Reason::InvalidName);
            EXPECT_TRUE(result.error().tried.empty());
        }

        expect_refused(MayaFlux::Platform::acquire_library(from_directories({ "relative/directory" }, "")), Reason::EmptyRequest);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, DirectoriesCannotEscapeThroughTheName, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto directory = fixture.make_directory("inner/system");
        fixture.make_file("inner/outside.so");
        fixture.make_file("outside.so");

        expect_refused(MayaFlux::Platform::acquire_library(from_directories({ directory }, "../outside.so")), Reason::InvalidName);
        expect_refused(MayaFlux::Platform::acquire_library(from_directories({ directory }, "../../outside.so")), Reason::InvalidName);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, DirectoriesAHugeListIsSearchedWithoutThrowing, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto directory = fixture.make_directory("system");
        const auto expected = fixture.make_file("system/" + platform_library_file("widget"));

        std::vector<fs::path> directories;
        for (int index = 0; index < 3000; ++index) {
            directories.push_back(fixture.root() / ("absent-" + std::to_string(index)));
        }
        directories.push_back(directory);

        MayaFlux::Platform::Acquired<MayaFlux::Platform::LibraryLocation> result = std::unexpected(MayaFlux::Platform::AcquireFailure {});
        EXPECT_NO_THROW(result = MayaFlux::Platform::acquire_library(from_directories(directories, "widget")));
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->path, expected);

        directories.pop_back();
        EXPECT_NO_THROW(result = MayaFlux::Platform::acquire_library(from_directories(directories, "widget")));
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().tried.size(), directories.size() * 2);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, DirectoriesOverlongEntriesAndNamesDoNotThrow, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto overlong_directory = fixture.root() / std::string(400, 'a');
        const auto directory = fixture.make_directory("system");

        MayaFlux::Platform::Acquired<MayaFlux::Platform::LibraryLocation> result = std::unexpected(MayaFlux::Platform::AcquireFailure {});
        EXPECT_NO_THROW(result = MayaFlux::Platform::acquire_library(from_directories({ overlong_directory }, "widget")));
        EXPECT_FALSE(result.has_value());
        EXPECT_NO_THROW(result = MayaFlux::Platform::acquire_library(from_directories({ directory }, std::string(300, 'a'))));
        EXPECT_FALSE(result.has_value());
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, DirectoriesAgreeWithPrefixWhenThePrefixLibIsTheDirectory, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto prefix = fixture.root() / "prefix";
        const auto expected = fixture.make_file("prefix/lib/" + platform_library_file("widget"));

        const auto by_prefix = MayaFlux::Platform::acquire_library(from_prefix(prefix, "widget"));
        const auto by_directories = MayaFlux::Platform::acquire_library(from_directories({ prefix / "lib" }, "widget"));

        ASSERT_TRUE(by_prefix.has_value());
        ASSERT_TRUE(by_directories.has_value());
        EXPECT_EQ(by_prefix->path, expected);
        EXPECT_EQ(by_directories->path, by_prefix->path);
        EXPECT_EQ(by_directories->file_name, by_prefix->file_name);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, SystemPassesTheExactNameThrough,(::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        const std::vector<std::string> names { "libvulkan.so.1", "vulkan-1.dll", "libSystem.B.dylib", "lib my.lib.so.1", "w\xC3\xAD" "dget.dll" };
        for (const auto& name : names) {
            const auto result = MayaFlux::Platform::acquire_library(from_system(name));

            ASSERT_TRUE(result.has_value()) << name;
            EXPECT_TRUE(result->path.empty()) << name;
            EXPECT_EQ(result->file_name, name);
            EXPECT_TRUE(result->resolved_by_system) << name;
        }
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, SystemRefusesForwardSlashAndDirectories, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        for (const std::string hostile : { std::string("/usr/lib/libx.so"), std::string("dir/libx.so"), std::string("../libx.so") }) {
            expect_refused(MayaFlux::Platform::acquire_library(from_system(hostile)), Reason::InvalidName);
        }
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, SystemBackslashIsRefusedOnlyOnWindows, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        const auto result = MayaFlux::Platform::acquire_library(from_system("..\\x.dll"));
#if defined(MAYAFLUX_PLATFORM_WINDOWS)
        expect_refused(result, Reason::InvalidName);
#else
        EXPECT_TRUE(result.has_value());
#endif
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, SystemRefusesControlCharactersAndNul, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        for (const std::string hostile : { std::string("libx\0.so", 7), std::string("lib\tx.so"), std::string("libx\n.so") }) {
            expect_refused(MayaFlux::Platform::acquire_library(from_system(hostile)), Reason::InvalidName);
        }
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, SystemRefusesDotNames, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        expect_refused(MayaFlux::Platform::acquire_library(from_system(".")), Reason::InvalidName);
        expect_refused(MayaFlux::Platform::acquire_library(from_system("..")), Reason::InvalidName);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, SystemColonIsRefusedOnlyOnWindows, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        const auto result = MayaFlux::Platform::acquire_library(from_system("C:x.dll"));
#if defined(MAYAFLUX_PLATFORM_WINDOWS)
        expect_refused(result, Reason::InvalidName);
#else
        EXPECT_TRUE(result.has_value());
#endif
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, SystemEmptyIsEmptyRequest, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        expect_refused(MayaFlux::Platform::acquire_library(from_system("")), Reason::EmptyRequest);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, HeaderRootAcceptsAnExistingDirectory, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto directory = fixture.make_directory("include");

        const auto result = MayaFlux::Platform::acquire_header_root(directory);

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(*result, directory);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, HeaderRootResolvesRelativeToTheWorkingDirectory, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        fixture.make_directory("include");
        const WorkingDirectoryGuard cwd(fixture.root());

        const auto result = MayaFlux::Platform::acquire_header_root("include");

        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(result->is_absolute());
        EXPECT_EQ(*result, fs::current_path() / "include");
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, HeaderRootAcceptsATrailingSeparator, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        fixture.make_directory("include");
        const auto with_slash = (fixture.root() / "include").string() + "/";

        const auto result = MayaFlux::Platform::acquire_header_root(with_slash);

        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(fs::equivalent(*result, fixture.root() / "include"));
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, HeaderRootSymlinkToADirectoryResolves, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto real = fixture.make_directory("real-include");
        const auto link = fixture.root() / "include-link";
        if (!fixture.make_link(real, link, true)) {
            GTEST_SKIP() << "symlinks unavailable";
        }

        const auto result = MayaFlux::Platform::acquire_header_root(link);

        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(fs::equivalent(*result, real));
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, HeaderRootRefusesEmptyMissingAndFile, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto file = fixture.make_file("include-file.hpp");

        expect_refused(MayaFlux::Platform::acquire_header_root({}), Reason::EmptyRequest);
        expect_refused(MayaFlux::Platform::acquire_header_root(fixture.root() / "absent"), Reason::Missing);
        expect_refused(MayaFlux::Platform::acquire_header_root(file), Reason::NotDirectory);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, HeaderMarkerMustBeARegularFile, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto directory = fixture.make_directory("magic");

        const auto without = MayaFlux::Platform::acquire_header_root(directory, "magic_enum.hpp");
        expect_refused(without, Reason::MarkerMissing);
        ASSERT_EQ(without.error().tried.size(), 1U);
        EXPECT_EQ(without.error().tried.front(), directory / "magic_enum.hpp");

        const auto directory_marker = fixture.make_directory("magic-dir-marker");
        fixture.make_directory("magic-dir-marker/magic_enum.hpp");
        expect_refused(MayaFlux::Platform::acquire_header_root(directory_marker, "magic_enum.hpp"), Reason::MarkerMissing);

        fixture.make_file("magic/magic_enum.hpp");
        const auto with = MayaFlux::Platform::acquire_header_root(directory, "magic_enum.hpp");
        ASSERT_TRUE(with.has_value());
        EXPECT_TRUE(fs::equivalent(*with, directory));
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, HeaderMarkerDanglingSymlinkIsMissing, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto directory = fixture.make_directory("magic");
        if (!fixture.make_link(fixture.root() / "gone.hpp", directory / "magic_enum.hpp", false)) {
            GTEST_SKIP() << "symlinks unavailable";
        }

        expect_refused(MayaFlux::Platform::acquire_header_root(directory, "magic_enum.hpp"), Reason::MarkerMissing);
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, HeaderMarkerRefusesEscapesAndDotNames, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto directory = fixture.make_directory("include");
        fixture.make_file("escape.hpp");

        std::vector<std::string> hostile { std::string("../escape.hpp"), std::string("sub/x.hpp"), std::string(".."), std::string(".") };
#if defined(MAYAFLUX_PLATFORM_WINDOWS)
        hostile.push_back("sub\\x.hpp");
#endif
        for (const auto& marker : hostile) {
            expect_refused(MayaFlux::Platform::acquire_header_root(directory, marker), Reason::InvalidName);
        }
    }

    MAYAFLUX_TEST_WITH(TEST, AcquisitionTest, HeaderEmptyMarkerSkipsTheMarkerCheck, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        AcquisitionFixtureTree fixture;
        const auto directory = fixture.make_directory("empty-marker");

        EXPECT_TRUE(MayaFlux::Platform::acquire_header_root(directory, "").has_value());
    }

}
}
