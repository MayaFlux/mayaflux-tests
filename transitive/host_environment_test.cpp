#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/scoped_env.hpp"
#include "../support/scoped_working_dir.hpp"
#include "MayaFlux/Transitive/Platform/HostEnvironment.hpp"

#include <fstream>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    class FixtureTree {
    public:
        FixtureTree()
        {
            static std::atomic<uint64_t> sequence { 0 };
            m_root = std::filesystem::temp_directory_path()
                / ("mayaflux-host-environment-"
                    + std::to_string(sequence.fetch_add(1)));
            std::filesystem::create_directories(m_root);
        }

        ~FixtureTree()
        {
            std::error_code error;
            std::filesystem::remove_all(m_root, error);
        }

        [[nodiscard]] const std::filesystem::path& root() const { return m_root; }

        std::filesystem::path make_file(const std::filesystem::path& relative_path)
        {
            const auto path = m_root / relative_path;
            std::filesystem::create_directories(path.parent_path());
            std::ofstream(path) << "fixture";
            return path;
        }

    private:
        std::filesystem::path m_root;
    };

    std::string shared_library_filename(const std::string& name)
    {
#if defined(MAYAFLUX_PLATFORM_WINDOWS)
        return name.ends_with(".dll") ? name : name + ".dll";
#elif defined(MAYAFLUX_PLATFORM_MACOS)
        if (name.ends_with(".dylib") || name.starts_with("lib")) {
            return name;
        }
        return "lib" + name + ".dylib";
#else
        if (name.ends_with(".so") || name.ends_with(".a")) {
            return name;
        }
        return "lib" + name + ".so";
#endif
    }

    std::string unique_library_name()
    {
        static std::atomic<uint64_t> sequence { 0 };
        return "mayaflux_host_environment_fixture_" + std::to_string(sequence.fetch_add(1));
    }

    TEST(HostEnvironmentTest, ProjectRootCanBeOverriddenAndRestored)
    {
        const auto previous = MayaFlux::Config::project_root();

        MayaFlux::Config::set_project_root("fixture/project");
        EXPECT_EQ(MayaFlux::Config::project_root(), "fixture/project");

        MayaFlux::Config::set_project_root(previous);
        EXPECT_EQ(MayaFlux::Config::project_root(), previous);
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, SafeGetenvReadsValuesAndTreatsUnsetVariablesAsEmpty, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        constexpr char variable[] = "MAYAFLUX_HOST_ENVIRONMENT_TEST_VALUE";
        MayaFlux::Test::ScopedEnv set_value(variable, "fixture-value");
        EXPECT_EQ(MayaFlux::Platform::safe_getenv(variable), "fixture-value");

        MayaFlux::Test::ScopedEnv unset_value(variable, nullptr);
        EXPECT_TRUE(MayaFlux::Platform::safe_getenv(variable).empty());
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, ExecCommandCapturesStdoutAndTrimOutputNormalizesIt, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::process_exec>() }))
    {
#if defined(MAYAFLUX_PLATFORM_WINDOWS)
        auto output = MayaFlux::Platform::exec_command("cmd /c echo FixtureOutput");
#else
        auto output = MayaFlux::Platform::exec_command("printf 'FixtureOutput\\r\\n'");
#endif
        EXPECT_NE(output.find("FixtureOutput"), std::string::npos);

        MayaFlux::Platform::trim_output(output);
        EXPECT_EQ(output, "FixtureOutput");

        std::string whitespace = " \t\r\n";
        MayaFlux::Platform::trim_output(whitespace);
        EXPECT_TRUE(whitespace.empty());
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, HostDiscoveryUsesPlatformPathsAndTransientCpath, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::host_toolchain, ::MayaFlux::Test::Meta::Capability::process_exec, ::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        static FixtureTree fixture;
        const auto include_root = fixture.root() / "temporary-include-root";
        std::filesystem::create_directories(include_root);
        fixture.make_file("temporary-include-root/FixtureHeader.hpp");
        fixture.make_file("temporary-include-root/MayaFlux/thirdparty/magic_enum/magic_enum.hpp");

        MayaFlux::Test::ScopedEnv cpath("CPATH", include_root.string().c_str());

#ifdef MAYAFLUX_PLATFORM_WINDOWS
        const std::string sdk_version = "10.99.12345.0";
        const auto sdk_root = fixture.root() / "windows-sdk";
        for (const auto* component : { "ucrt", "shared", "um", "winrt", "cppwinrt" }) {
            std::filesystem::create_directories(sdk_root / "Include" / sdk_version / component);
        }
        for (const auto* component : { "ucrt", "um" }) {
            std::filesystem::create_directories(sdk_root / "Lib" / sdk_version / component / "x64");
        }
        const std::string sdk_root_string = sdk_root.string();
        const std::string sdk_version_env = sdk_version + "\\";
        MayaFlux::Test::ScopedEnv sdk_dir("WindowsSdkDir", sdk_root_string.c_str());
        MayaFlux::Test::ScopedEnv sdk_version_var("WindowsSDKVersion", sdk_version_env.c_str());
#else
        const auto library_root = fixture.root() / "host-libraries";
        std::filesystem::create_directories(library_root);
        const std::string library_root_string = library_root.string();
        MayaFlux::Test::ScopedEnv library_path("LD_LIBRARY_PATH", library_root_string.c_str());
#endif

        const auto& include_paths = MayaFlux::Platform::SystemConfig::get_system_includes();
        const auto& library_paths = MayaFlux::Platform::SystemConfig::get_system_libraries();

        EXPECT_NE(std::find(include_paths.begin(), include_paths.end(), include_root.string()), include_paths.end());
        EXPECT_TRUE(std::filesystem::exists(include_root / "MayaFlux/thirdparty/magic_enum/magic_enum.hpp"));
        EXPECT_FALSE(MayaFlux::Platform::SystemConfig::get_clang_resource_dir().empty());

#ifdef MAYAFLUX_PLATFORM_WINDOWS
        EXPECT_NE(std::find(include_paths.begin(), include_paths.end(),
                      (sdk_root / "Include" / sdk_version / "ucrt").string()),
            include_paths.end());
        EXPECT_NE(std::find(library_paths.begin(), library_paths.end(),
                      (sdk_root / "Lib" / sdk_version / "um" / "x64").string()),
            library_paths.end());
#else
        EXPECT_NE(std::find(library_paths.begin(), library_paths.end(), library_root.string()), library_paths.end());
#endif

        for (const auto& path : include_paths) {
            EXPECT_FALSE(path.empty());
        }
        for (const auto& path : library_paths) {
            EXPECT_FALSE(path.empty());
        }
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, DependencyResolutionPrefersVendoredRootOverConfigureHint, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        FixtureTree fixture;
        fixture.make_file("vendored/magic_enum.hpp");
        fixture.make_file("hinted/magic_enum.hpp");
        const std::string vendored = (fixture.root() / "vendored").string();
        const std::string hinted = (fixture.root() / "hinted").string();

        MayaFlux::Platform::DependencyHint hint;
        hint.vendored_root = vendored;
        hint.env_hint = hinted;
        hint.marker_file = "magic_enum.hpp";

        EXPECT_EQ(MayaFlux::Platform::SystemConfig::resolve_dependency_include(hint), vendored);
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, DependencyResolutionFallsBackToConfigureHintThenFallbackPaths, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        FixtureTree hinted_tree;
        hinted_tree.make_file("hinted/magic_enum.hpp");
        const std::string hinted = (hinted_tree.root() / "hinted").string();

        MayaFlux::Platform::DependencyHint hinted_hint;
        hinted_hint.vendored_root = (hinted_tree.root() / "missing").string();
        hinted_hint.env_hint = hinted;
        hinted_hint.marker_file = "magic_enum.hpp";
        EXPECT_EQ(MayaFlux::Platform::SystemConfig::resolve_dependency_include(hinted_hint), hinted);

        FixtureTree fallback_tree;
        fallback_tree.make_file("fallback/magic_enum.hpp");
        const std::string fallback = (fallback_tree.root() / "fallback").string();

        const std::string missing = (fallback_tree.root() / "missing").string();

        MayaFlux::Platform::DependencyHint fallback_hint;
        fallback_hint.env_hint = missing;
        fallback_hint.fallback_paths = { fallback };
        fallback_hint.marker_file = "magic_enum.hpp";
        EXPECT_EQ(MayaFlux::Platform::SystemConfig::resolve_dependency_include(fallback_hint), fallback);
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, DependencyResolutionRequiresMarkerOnlyWhenNamed, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        FixtureTree fixture;
        fixture.make_file("dir-without-marker/readme.txt");
        const std::string directory = (fixture.root() / "dir-without-marker").string();

        MayaFlux::Platform::DependencyHint hint;
        hint.vendored_root = directory;
        hint.marker_file = "magic_enum.hpp";
        EXPECT_TRUE(MayaFlux::Platform::SystemConfig::resolve_dependency_include(hint).empty());

        hint.marker_file = {};
        EXPECT_EQ(MayaFlux::Platform::SystemConfig::resolve_dependency_include(hint), directory);
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, DependencyAliasesShareOneEntry, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        EXPECT_EQ(MayaFlux::Platform::SystemConfig::get_dep_includes("Eigen"),
            MayaFlux::Platform::SystemConfig::get_dep_includes("eigen"));
        EXPECT_EQ(MayaFlux::Platform::SystemConfig::get_dep_includes("freetype"),
            MayaFlux::Platform::SystemConfig::get_dep_includes("freetype2"));
        EXPECT_TRUE(MayaFlux::Platform::SystemConfig::get_dep_includes("no_such_dependency").empty());
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, MagicEnumIncludeDiscoveryFindsTheVendoredSourceHeader, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::repo_assets>() }))
    {
        const auto& resolved = MayaFlux::Platform::SystemConfig::get_dep_includes("magic_enum");

        ASSERT_FALSE(resolved.empty());
        EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(resolved) / "magic_enum.hpp"));
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, DependencyIncludeHintsOnlyReturnExistingDirectories, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        for (const auto* name : { "Eigen", "freetype2" }) {
            const auto& include = MayaFlux::Platform::SystemConfig::get_dep_includes(name);
            if (!include.empty()) {
                EXPECT_TRUE(std::filesystem::exists(include)) << name << ": " << include;
            }
        }
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, DependencyLibrarySearchChecksAllPrefixLibraryDirectories, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        for (const auto* subdirectory : { "lib", "lib64", "bin" }) {
            FixtureTree fixture;
            const auto name = unique_library_name();
            const auto filename = shared_library_filename(name);
            const auto expected = fixture.make_file(std::filesystem::path(subdirectory) / filename);

            const auto& resolved = MayaFlux::Platform::SystemConfig::find_dep_library(name, fixture.root().string());
            EXPECT_EQ(std::filesystem::path(resolved), expected);
        }
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, DependencyLibrarySearchReturnsEmptyForMissingLibraries, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        FixtureTree fixture;
        const auto name = unique_library_name();

        EXPECT_TRUE(MayaFlux::Platform::SystemConfig::find_dep_library(name, fixture.root().string()).empty());
    }

    static_assert(std::is_same_v<decltype(MayaFlux::Platform::SystemConfig::get_clang_resource_dir()), std::string>);
    static_assert(std::is_same_v<decltype(MayaFlux::Platform::SystemConfig::get_system_includes()), std::vector<std::string>>);
    static_assert(std::is_same_v<decltype(MayaFlux::Platform::SystemConfig::get_system_libraries()), std::vector<std::string>>);
    static_assert(std::is_same_v<decltype(MayaFlux::Platform::SystemConfig::find_dep_library("x")), std::string>);
    static_assert(std::is_same_v<decltype(MayaFlux::Platform::SystemConfig::get_dep_includes("x")), std::string>);

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, DependencyLibrarySearchKeepsPrefixesApart, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        FixtureTree first;
        FixtureTree second;
        const auto name = unique_library_name();
        const auto filename = shared_library_filename(name);
        const auto in_first = first.make_file(std::filesystem::path("lib") / filename);
        const auto in_second = second.make_file(std::filesystem::path("lib") / filename);

        EXPECT_EQ(std::filesystem::path(MayaFlux::Platform::SystemConfig::find_dep_library(name, first.root().string())), in_first);
        EXPECT_EQ(std::filesystem::path(MayaFlux::Platform::SystemConfig::find_dep_library(name, second.root().string())), in_second);
        EXPECT_EQ(std::filesystem::path(MayaFlux::Platform::SystemConfig::find_dep_library(name, first.root().string())), in_first);
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, DependencyLibrarySearchFindsALibraryThatAppearsAfterAMiss, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        FixtureTree fixture;
        const auto name = unique_library_name();
        const auto prefix = fixture.root().string();

        EXPECT_TRUE(MayaFlux::Platform::SystemConfig::find_dep_library(name, prefix).empty());

        const auto expected = fixture.make_file(std::filesystem::path("lib") / shared_library_filename(name));
        EXPECT_EQ(std::filesystem::path(MayaFlux::Platform::SystemConfig::find_dep_library(name, prefix)), expected);
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, DependencyLibrarySearchSeesALibraryRemovedAfterAHit, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        FixtureTree fixture;
        const auto name = unique_library_name();
        const auto prefix = fixture.root().string();
        const auto file = fixture.make_file(std::filesystem::path("lib") / shared_library_filename(name));

        EXPECT_EQ(std::filesystem::path(MayaFlux::Platform::SystemConfig::find_dep_library(name, prefix)), file);

        std::filesystem::remove(file);
        EXPECT_TRUE(MayaFlux::Platform::SystemConfig::find_dep_library(name, prefix).empty());
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, DependencyLibrarySearchWithoutAPrefixDoesNotLookInTheWorkingDirectory, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        const auto name = unique_library_name();
        const auto filename = shared_library_filename(name);

        MayaFlux::Test::ScopedWorkingDir cwd;
        for (const char* subdirectory : { "lib", "lib64", "bin", "." }) {
            cwd.write(std::filesystem::path(subdirectory) / filename, "fixture");
        }

        EXPECT_TRUE(MayaFlux::Platform::SystemConfig::find_dep_library(name).empty());
        EXPECT_TRUE(MayaFlux::Platform::SystemConfig::find_dep_library(name, "").empty());
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, DependencyLibrarySearchResolvesARelativePrefixToAnAbsolutePath, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        const auto name = unique_library_name();
        const auto filename = shared_library_filename(name);

        MayaFlux::Test::ScopedWorkingDir cwd;
        cwd.write(std::filesystem::path("local") / "lib" / filename, "fixture");

        const std::filesystem::path resolved = MayaFlux::Platform::SystemConfig::find_dep_library(name, "local");

        ASSERT_FALSE(resolved.empty());
        EXPECT_TRUE(resolved.is_absolute());
        EXPECT_EQ(resolved, std::filesystem::current_path() / "local" / "lib" / filename);
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, DependencyLibrarySearchRefusesNamesThatEscapeThePrefix, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        FixtureTree fixture;
        const auto name = unique_library_name();
        const auto filename = shared_library_filename(name);
        fixture.make_file("prefix/lib/keep");
        fixture.make_file(filename);
        fixture.make_file(std::filesystem::path("prefix") / "lib" / "sub" / filename);
        const auto prefix = (fixture.root() / "prefix").string();

        const std::vector<std::string> hostile {
            "../../" + filename,
            "../" + filename,
            "sub/" + filename,
            (fixture.root() / filename).string(),
            ".",
            "..",
            "",
            std::string("lib\0x", 5),
            "lib\tx",
        };
        for (const auto& candidate : hostile) {
            EXPECT_TRUE(MayaFlux::Platform::SystemConfig::find_dep_library(candidate, prefix).empty()) << candidate;
        }
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, DependencyLibrarySearchIgnoresDirectoriesNamedLikeTheLibrary, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        FixtureTree fixture;
        const auto name = unique_library_name();
        const auto filename = shared_library_filename(name);
        for (const char* subdirectory : { "lib", "lib64", "bin" }) {
            std::filesystem::create_directories(fixture.root() / subdirectory / filename);
        }

        EXPECT_TRUE(MayaFlux::Platform::SystemConfig::find_dep_library(name, fixture.root().string()).empty());
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, DependencyLibrarySearchSurvivesHostilePrefixes, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        FixtureTree fixture;
        const auto file = fixture.make_file("not-a-directory");
        const auto name = unique_library_name();

        const std::vector<std::string> hostile {
            file.string(),
            (fixture.root() / "absent").string(),
            std::string(400, 'a'),
            std::string("pre\0fix", 7),
            "/",
        };
        for (const auto& prefix : hostile) {
            std::string resolved = "unset";
            EXPECT_NO_THROW(resolved = MayaFlux::Platform::SystemConfig::find_dep_library(name, prefix));
            EXPECT_TRUE(resolved.empty());
        }
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, DependencyLibrarySearchIsStableWhenManyThreadsUseDifferentPrefixes, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        constexpr size_t thread_count = 8;
        constexpr int rounds = 60;

        const auto name = unique_library_name();
        const auto filename = shared_library_filename(name);

        std::vector<std::unique_ptr<FixtureTree>> fixtures;
        std::vector<std::filesystem::path> expected;
        for (size_t index = 0; index < thread_count; ++index) {
            fixtures.push_back(std::make_unique<FixtureTree>());
            expected.push_back(fixtures.back()->make_file(std::filesystem::path("lib") / filename));
        }

        std::atomic<size_t> mismatches { 0 };
        std::vector<std::thread> threads;
        for (size_t index = 0; index < thread_count; ++index) {
            threads.emplace_back([&, index] {
                const auto prefix = fixtures[index]->root().string();
                for (int round = 0; round < rounds; ++round) {
                    if (std::filesystem::path(MayaFlux::Platform::SystemConfig::find_dep_library(name, prefix)) != expected[index]) {
                        ++mismatches;
                    }
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }

        EXPECT_EQ(mismatches.load(), 0U);
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, DependencyIncludeLookupsAgreeAcrossThreads, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::process_exec>() }))
    {
        constexpr size_t thread_count = 8;
        constexpr int rounds = 200;

        const std::vector<std::string> names { "eigen", "Eigen", "freetype", "freetype2", "magic_enum", "no_such_dependency" };
        std::vector<std::string> expected;
        for (const auto& name : names) {
            const auto* hint = MayaFlux::Platform::find_dependency_hint(name);
            expected.push_back(hint ? MayaFlux::Platform::SystemConfig::resolve_dependency_include(*hint) : std::string {});
        }

        std::atomic<size_t> mismatches { 0 };
        std::vector<std::thread> threads;
        for (size_t index = 0; index < thread_count; ++index) {
            threads.emplace_back([&] {
                for (int round = 0; round < rounds; ++round) {
                    for (size_t name = 0; name < names.size(); ++name) {
                        if (MayaFlux::Platform::SystemConfig::get_dep_includes(names[name]) != expected[name]) {
                            ++mismatches;
                        }
                    }
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }

        EXPECT_EQ(mismatches.load(), 0U);
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, ToolchainLookupsAgreeAcrossThreads, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::process_exec>() }))
    {
        constexpr size_t thread_count = 8;
        constexpr int rounds = 100;

        const auto includes = MayaFlux::Platform::SystemConfig::get_system_includes();
        const auto libraries = MayaFlux::Platform::SystemConfig::get_system_libraries();
        const auto resource_dir = MayaFlux::Platform::SystemConfig::get_clang_resource_dir();

        std::atomic<size_t> mismatches { 0 };
        std::vector<std::thread> threads;
        for (size_t index = 0; index < thread_count; ++index) {
            threads.emplace_back([&] {
                for (int round = 0; round < rounds; ++round) {
                    if (MayaFlux::Platform::SystemConfig::get_system_includes() != includes
                        || MayaFlux::Platform::SystemConfig::get_system_libraries() != libraries
                        || MayaFlux::Platform::SystemConfig::get_clang_resource_dir() != resource_dir) {
                        ++mismatches;
                    }
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }

        EXPECT_EQ(mismatches.load(), 0U);
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, DependencyIncludeLookupSurvivesHostileNames, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        const std::vector<std::string> hostile {
            "", " ", "../eigen", "eigen/", std::string("eigen\0x", 7), std::string(400, 'e'), "EIGEN", "Eigen ",
        };
        for (const auto& name : hostile) {
            std::string resolved = "unset";
            EXPECT_NO_THROW(resolved = MayaFlux::Platform::SystemConfig::get_dep_includes(name));
            EXPECT_TRUE(resolved.empty()) << name;
        }
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, DependencyIncludeLookupGivesTheSameAnswerOnEveryCall, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        for (const auto* name : { "eigen", "freetype2", "magic_enum", "no_such_dependency" }) {
            const std::string first = MayaFlux::Platform::SystemConfig::get_dep_includes(name);
            for (int call = 0; call < 500; ++call) {
                ASSERT_EQ(MayaFlux::Platform::SystemConfig::get_dep_includes(name), first) << name;
            }
        }
    }

#if defined(MAYAFLUX_PLATFORM_WINDOWS)
    TEST(HostEnvironmentTest, WindowsPathSeparatorIsBackslash)
    {
        EXPECT_EQ(MayaFlux::Platform::PathSeparator, '\\');
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, WindowsLibrarySearchDoesNotDuplicateDllExtension, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        FixtureTree fixture;
        const auto name = unique_library_name() + ".dll";
        const auto expected = fixture.make_file(std::filesystem::path("bin") / name);

        const auto& resolved = MayaFlux::Platform::SystemConfig::find_dep_library(name, fixture.root().string());
        EXPECT_EQ(std::filesystem::path(resolved), expected);
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, WindowsExecCommandMergesStderrWhenRedirected, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::process_exec>() }))
    {
        const auto output = MayaFlux::Platform::exec_command(
            "cmd /c \"echo FixtureError 1>&2\" 2>&1");
        EXPECT_NE(output.find("FixtureError"), std::string::npos);
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, WindowsSdkDiscoveryOnlyReturnsExistingDirectories, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        const auto& includes = MayaFlux::Platform::SystemConfig::get_system_includes();
        const auto& libraries = MayaFlux::Platform::SystemConfig::get_system_libraries();

        for (const auto& path : includes) {
            EXPECT_TRUE(std::filesystem::exists(path)) << path;
        }
        for (const auto& path : libraries) {
            EXPECT_TRUE(std::filesystem::exists(path)) << path;
        }
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, WindowsSdkDiscoveryIgnoresNonexistentSdkDirectory, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        static FixtureTree fixture;
        const auto missing_root = fixture.root() / "missing-windows-sdk";
        const std::string missing_root_string = missing_root.string();
        MayaFlux::Test::ScopedEnv sdk_dir("WindowsSdkDir", missing_root_string.c_str());
        MayaFlux::Test::ScopedEnv sdk_version("WindowsSDKVersion", "10.0.0.0\\");

        const auto& includes = MayaFlux::Platform::SystemConfig::get_system_includes();
        const auto& libraries = MayaFlux::Platform::SystemConfig::get_system_libraries();

        for (const auto& path : includes) {
            EXPECT_FALSE(path.starts_with(missing_root_string)) << path;
        }
        for (const auto& path : libraries) {
            EXPECT_FALSE(path.starts_with(missing_root_string)) << path;
        }
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, WindowsMsvcDiscoveryProvidesToolchainHeadersAndLibraries, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::host_toolchain, ::MayaFlux::Test::Meta::Capability::process_exec>() }))
    {
        const auto& includes = MayaFlux::Platform::SystemConfig::get_system_includes();
        const auto& libraries = MayaFlux::Platform::SystemConfig::get_system_libraries();

        const auto has_header = std::any_of(includes.begin(), includes.end(), [](const std::string& path) {
            return std::filesystem::exists(std::filesystem::path(path) / "vcruntime.h");
        });
        if (!has_header) {
            GTEST_SKIP() << "No Visual Studio C++ toolchain located by vswhere or VCINSTALLDIR";
        }

        const auto has_msvc_lib = std::any_of(libraries.begin(), libraries.end(), [](const std::string& path) {
            return std::filesystem::exists(std::filesystem::path(path) / "msvcrt.lib");
        });
        EXPECT_TRUE(has_msvc_lib);
    }
#endif

#if defined(MAYAFLUX_PLATFORM_LINUX)
    TEST(HostEnvironmentTest, LinuxPathSeparatorIsSlash)
    {
        EXPECT_EQ(MayaFlux::Platform::PathSeparator, '/');
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, LinuxLibrarySearchAddsLibPrefixAndSoExtension, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        FixtureTree fixture;
        const auto name = unique_library_name();
        const auto expected = fixture.make_file(std::filesystem::path("lib") / ("lib" + name + ".so"));

        const auto& resolved = MayaFlux::Platform::SystemConfig::find_dep_library(name, fixture.root().string());
        EXPECT_EQ(std::filesystem::path(resolved), expected);
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, LinuxLibrarySearchDoesNotDuplicateSoExtension, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        FixtureTree fixture;
        const auto name = unique_library_name() + ".so";
        const auto expected = fixture.make_file(std::filesystem::path("lib") / name);

        const auto& resolved = MayaFlux::Platform::SystemConfig::find_dep_library(name, fixture.root().string());
        EXPECT_EQ(std::filesystem::path(resolved), expected);
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, LinuxLibrarySearchPreservesArchiveExtension, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        FixtureTree fixture;
        const auto name = unique_library_name() + ".a";
        const auto expected = fixture.make_file(std::filesystem::path("lib64") / name);

        const auto& resolved = MayaFlux::Platform::SystemConfig::find_dep_library(name, fixture.root().string());
        EXPECT_EQ(std::filesystem::path(resolved), expected);
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, LinuxExecCommandMergesStderrWhenRedirected, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::process_exec>() }))
    {
        const auto output = MayaFlux::Platform::exec_command(
            "sh -c 'echo FixtureError >&2' 2>&1");
        EXPECT_NE(output.find("FixtureError"), std::string::npos);
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, LinuxLibraryDiscoveryOnlyReturnsExistingPaths, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        const auto& libraries = MayaFlux::Platform::SystemConfig::get_system_libraries();

        for (const auto& path : libraries) {
            EXPECT_TRUE(std::filesystem::exists(path)) << path;
        }
    }
#endif

#if defined(MAYAFLUX_PLATFORM_MACOS)
    TEST(HostEnvironmentTest, MacPathSeparatorIsSlash)
    {
        EXPECT_EQ(MayaFlux::Platform::PathSeparator, '/');
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, MacLibrarySearchAddsLibPrefixAndDylibExtension, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        FixtureTree fixture;
        const auto name = unique_library_name();
        const auto expected = fixture.make_file(std::filesystem::path("lib") / ("lib" + name + ".dylib"));

        const auto& resolved = MayaFlux::Platform::SystemConfig::find_dep_library(name, fixture.root().string());
        EXPECT_EQ(std::filesystem::path(resolved), expected);
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, MacLibrarySearchDoesNotDuplicateDylibExtension, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        FixtureTree fixture;
        const auto name = unique_library_name() + ".dylib";
        const auto expected = fixture.make_file(std::filesystem::path("lib64") / name);

        const auto& resolved = MayaFlux::Platform::SystemConfig::find_dep_library(name, fixture.root().string());
        EXPECT_EQ(std::filesystem::path(resolved), expected);
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, MacLibrarySearchPreservesLibPrefixedFilename, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        FixtureTree fixture;
        const auto name = "lib" + unique_library_name() + ".dylib";
        const auto expected = fixture.make_file(std::filesystem::path("bin") / name);

        const auto& resolved = MayaFlux::Platform::SystemConfig::find_dep_library(name, fixture.root().string());
        EXPECT_EQ(std::filesystem::path(resolved), expected);
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, MacExecCommandMergesStderrWhenRedirected, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::process_exec>() }))
    {
        const auto output = MayaFlux::Platform::exec_command(
            "sh -c 'echo FixtureError >&2' 2>&1");
        EXPECT_NE(output.find("FixtureError"), std::string::npos);
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, MacLibraryDiscoveryOnlyReturnsExistingPaths, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        const auto& libraries = MayaFlux::Platform::SystemConfig::get_system_libraries();

        for (const auto& path : libraries) {
            EXPECT_TRUE(std::filesystem::exists(path)) << path;
        }
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, MacHomebrewPathsAreIncludedWhenPresent, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::homebrew_paths>() }))
    {
        const std::filesystem::path homebrew_include = "/opt/homebrew/include";
        const std::filesystem::path homebrew_library = "/opt/homebrew/lib";
        const auto& includes = MayaFlux::Platform::SystemConfig::get_system_includes();
        const auto& libraries = MayaFlux::Platform::SystemConfig::get_system_libraries();

        if (!std::filesystem::exists(homebrew_include) && !std::filesystem::exists(homebrew_library)) {
            GTEST_SKIP() << "Homebrew include and library directories are not installed";
        }

        if (std::filesystem::exists(homebrew_include)) {
            EXPECT_NE(std::find(includes.begin(), includes.end(), homebrew_include.string()), includes.end());
        }
        if (std::filesystem::exists(homebrew_library)) {
            EXPECT_NE(std::find(libraries.begin(), libraries.end(), homebrew_library.string()), libraries.end());
        }
    }

    MAYAFLUX_TEST_WITH(TEST, HostEnvironmentTest, MacSdkDiscoveryReturnsAValidSdkAndFeedsSystemIncludes, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::host_toolchain, ::MayaFlux::Test::Meta::Capability::process_exec>() }))
    {
        const auto sdk = MayaFlux::Platform::SystemConfig::get_macos_sdk_path();
        const auto includes = MayaFlux::Platform::SystemConfig::get_system_includes();

        ASSERT_FALSE(sdk.empty()) << "Expected xcrun or the standard SDK locations to resolve the macOS SDK";
        EXPECT_TRUE(std::filesystem::is_directory(sdk));
        const auto sdk_include = std::filesystem::path(sdk) / "usr/include";
        if (std::filesystem::is_directory(sdk_include)) {
            EXPECT_NE(std::find(includes.begin(), includes.end(), sdk_include.string()), includes.end());
        }
        for (const auto& path : includes) {
            EXPECT_FALSE(path.empty());
        }
    }
#endif

} // namespace
} // namespace MayaFlux::Test
