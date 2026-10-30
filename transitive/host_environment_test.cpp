#include "gtest/gtest.h"

#include "../support/scoped_env.hpp"
#include "MayaFlux/Transitive/Platform/HostEnvironment.hpp"

#include <fstream>

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

    TEST(HostEnvironmentTest, SafeGetenvReadsValuesAndTreatsUnsetVariablesAsEmpty)
    {
        constexpr char variable[] = "MAYAFLUX_HOST_ENVIRONMENT_TEST_VALUE";
        MayaFlux::Test::ScopedEnv set_value(variable, "fixture-value");
        EXPECT_EQ(MayaFlux::Platform::safe_getenv(variable), "fixture-value");

        MayaFlux::Test::ScopedEnv unset_value(variable, nullptr);
        EXPECT_TRUE(MayaFlux::Platform::safe_getenv(variable).empty());
    }

    TEST(HostEnvironmentTest, ExecCommandCapturesStdoutAndTrimOutputNormalizesIt)
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

    TEST(HostEnvironmentTest, HostDiscoveryUsesPlatformPathsAndTransientCpath)
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

    TEST(HostEnvironmentTest, MagicEnumIncludeDiscoveryFindsTheVendoredSourceHeader)
    {
        const auto& resolved = MayaFlux::Platform::SystemConfig::get_dep_includes("magic_enum");

        ASSERT_FALSE(resolved.empty());
        EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(resolved) / "magic_enum.hpp"));
    }

    TEST(HostEnvironmentTest, DependencyIncludeHintsOnlyReturnExistingDirectories)
    {
        for (const auto* name : { "Eigen", "freetype2" }) {
            const auto& include = MayaFlux::Platform::SystemConfig::get_dep_includes(name);
            if (!include.empty()) {
                EXPECT_TRUE(std::filesystem::exists(include)) << name << ": " << include;
            }
        }
    }

    TEST(HostEnvironmentTest, DependencyLibrarySearchChecksAllPrefixLibraryDirectories)
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

    TEST(HostEnvironmentTest, DependencyLibrarySearchReturnsEmptyForMissingLibraries)
    {
        FixtureTree fixture;
        const auto name = unique_library_name();

        EXPECT_TRUE(MayaFlux::Platform::SystemConfig::find_dep_library(name, fixture.root().string()).empty());
    }

#if defined(MAYAFLUX_PLATFORM_WINDOWS)
    TEST(HostEnvironmentTest, WindowsPathSeparatorIsBackslash)
    {
        EXPECT_EQ(MayaFlux::Platform::PathSeparator, '\\');
    }

    TEST(HostEnvironmentTest, WindowsLibrarySearchDoesNotDuplicateDllExtension)
    {
        FixtureTree fixture;
        const auto name = unique_library_name() + ".dll";
        const auto expected = fixture.make_file(std::filesystem::path("bin") / name);

        const auto& resolved = MayaFlux::Platform::SystemConfig::find_dep_library(name, fixture.root().string());
        EXPECT_EQ(std::filesystem::path(resolved), expected);
    }

    TEST(HostEnvironmentTest, WindowsExecCommandMergesStderrWhenRedirected)
    {
        const auto output = MayaFlux::Platform::exec_command(
            "cmd /c \"echo FixtureError 1>&2\" 2>&1");
        EXPECT_NE(output.find("FixtureError"), std::string::npos);
    }

    TEST(HostEnvironmentTest, WindowsSdkDiscoveryOnlyReturnsExistingDirectories)
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

    TEST(HostEnvironmentTest, WindowsSdkDiscoveryIgnoresNonexistentSdkDirectory)
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

    TEST(HostEnvironmentTest, WindowsMsvcDiscoveryProvidesToolchainHeadersAndLibraries)
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

    TEST(HostEnvironmentTest, LinuxLibrarySearchAddsLibPrefixAndSoExtension)
    {
        FixtureTree fixture;
        const auto name = unique_library_name();
        const auto expected = fixture.make_file(std::filesystem::path("lib") / ("lib" + name + ".so"));

        const auto& resolved = MayaFlux::Platform::SystemConfig::find_dep_library(name, fixture.root().string());
        EXPECT_EQ(std::filesystem::path(resolved), expected);
    }

    TEST(HostEnvironmentTest, LinuxLibrarySearchDoesNotDuplicateSoExtension)
    {
        FixtureTree fixture;
        const auto name = unique_library_name() + ".so";
        const auto expected = fixture.make_file(std::filesystem::path("lib") / name);

        const auto& resolved = MayaFlux::Platform::SystemConfig::find_dep_library(name, fixture.root().string());
        EXPECT_EQ(std::filesystem::path(resolved), expected);
    }

    TEST(HostEnvironmentTest, LinuxLibrarySearchPreservesArchiveExtension)
    {
        FixtureTree fixture;
        const auto name = unique_library_name() + ".a";
        const auto expected = fixture.make_file(std::filesystem::path("lib64") / name);

        const auto& resolved = MayaFlux::Platform::SystemConfig::find_dep_library(name, fixture.root().string());
        EXPECT_EQ(std::filesystem::path(resolved), expected);
    }

    TEST(HostEnvironmentTest, LinuxExecCommandMergesStderrWhenRedirected)
    {
        const auto output = MayaFlux::Platform::exec_command(
            "sh -c 'echo FixtureError >&2' 2>&1");
        EXPECT_NE(output.find("FixtureError"), std::string::npos);
    }

    TEST(HostEnvironmentTest, LinuxLibraryDiscoveryOnlyReturnsExistingPaths)
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

    TEST(HostEnvironmentTest, MacLibrarySearchAddsLibPrefixAndDylibExtension)
    {
        FixtureTree fixture;
        const auto name = unique_library_name();
        const auto expected = fixture.make_file(std::filesystem::path("lib") / ("lib" + name + ".dylib"));

        const auto& resolved = MayaFlux::Platform::SystemConfig::find_dep_library(name, fixture.root().string());
        EXPECT_EQ(std::filesystem::path(resolved), expected);
    }

    TEST(HostEnvironmentTest, MacLibrarySearchDoesNotDuplicateDylibExtension)
    {
        FixtureTree fixture;
        const auto name = unique_library_name() + ".dylib";
        const auto expected = fixture.make_file(std::filesystem::path("lib64") / name);

        const auto& resolved = MayaFlux::Platform::SystemConfig::find_dep_library(name, fixture.root().string());
        EXPECT_EQ(std::filesystem::path(resolved), expected);
    }

    TEST(HostEnvironmentTest, MacLibrarySearchPreservesLibPrefixedFilename)
    {
        FixtureTree fixture;
        const auto name = "lib" + unique_library_name() + ".dylib";
        const auto expected = fixture.make_file(std::filesystem::path("bin") / name);

        const auto& resolved = MayaFlux::Platform::SystemConfig::find_dep_library(name, fixture.root().string());
        EXPECT_EQ(std::filesystem::path(resolved), expected);
    }

    TEST(HostEnvironmentTest, MacExecCommandMergesStderrWhenRedirected)
    {
        const auto output = MayaFlux::Platform::exec_command(
            "sh -c 'echo FixtureError >&2' 2>&1");
        EXPECT_NE(output.find("FixtureError"), std::string::npos);
    }

    TEST(HostEnvironmentTest, MacLibraryDiscoveryOnlyReturnsExistingPaths)
    {
        const auto& libraries = MayaFlux::Platform::SystemConfig::get_system_libraries();

        for (const auto& path : libraries) {
            EXPECT_TRUE(std::filesystem::exists(path)) << path;
        }
    }

    TEST(HostEnvironmentTest, MacHomebrewPathsAreIncludedWhenPresent)
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

    TEST(HostEnvironmentTest, MacSdkDiscoveryReturnsAValidSdkAndFeedsSystemIncludes)
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
