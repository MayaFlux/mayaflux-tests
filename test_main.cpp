#include <fstream>
#include <string_view>
#include <gtest/gtest.h>

#include "support/test_metadata.hpp"

namespace {

using MayaFlux::Test::Meta::Annotation;
using MayaFlux::Test::Meta::Capability;
using MayaFlux::Test::Meta::Kind;

constexpr std::array<std::string_view, 31> capability_names {
    "writable_temp", "host_toolchain", "repo_assets", "system_font",
    "loopback_tcp", "loopback_udp", "dbus_daemon", "desktop_dialog",
    "display", "vulkan", "vulkan_present", "vulkan_validation",
    "shader_float64", "separate_compute_queue", "pipewire", "coreaudio",
    "wasapi", "audio_output", "audio_input", "coremidi",
    "alsa_sequencer", "winmm_input", "loopmidi", "hid_enumeration",
    "hid_open", "tablet_enumeration", "tablet_open", "silero_weights",
    "hubert_weights", "process_exec", "homebrew_paths"
};
static_assert(capability_names.size() == static_cast<unsigned>(Capability::homebrew_paths) + 1);

struct CaseMetadata {
    std::string name;
    Annotation annotation;
};

std::string cmake_quote(std::string_view value)
{
    std::string equals;
    while (value.find("]" + equals + "]") != std::string_view::npos)
        equals += '=';
    return "[" + equals + "[" + std::string(value) + "]" + equals + "]";
}

std::string ctest_name(const testing::TestInfo& info)
{
    std::string suite = info.test_suite_name();
    std::string name = info.name();
    if (suite.starts_with("DISABLED_"))
        suite.erase(0, 9);
    if (name.starts_with("DISABLED_"))
        name.erase(0, 9);
    if (info.type_param() != nullptr && info.type_param()[0] != '\0') {
        const auto slash = suite.rfind('/');
        if (slash != std::string::npos) {
            const auto index = suite.substr(slash + 1);
            suite.erase(slash);
            name += "<" + index + ">";
        }
    }
    return suite + "." + name;
}

std::string labels_for(Annotation annotation)
{
    std::string labels = annotation.kind == Kind::contract ? "kind_contract" : "kind_integration";
    if (annotation.required == 0)
        labels += ";ci_portable";
    for (std::size_t index = 0; index < capability_names.size(); ++index) {
        if ((annotation.required & (std::uint64_t { 1 } << index)) != 0)
            labels += ";requires_" + std::string(capability_names[index]);
    }
    return labels;
}

std::string locks_for(Annotation annotation)
{
    auto has = [&](Capability capability) {
        return (annotation.required & (std::uint64_t { 1 } << static_cast<unsigned>(capability))) != 0;
    };
    std::string locks;
    auto add = [&](std::string_view lock) {
        if (!locks.empty())
            locks += ';';
        locks += lock;
    };
    if (has(Capability::vulkan))
        add("gpu");
    if (has(Capability::display) || has(Capability::desktop_dialog))
        add("display");
    if (has(Capability::audio_output) || has(Capability::audio_input))
        add("audio_endpoint");
    if (has(Capability::coremidi) || has(Capability::alsa_sequencer)
        || has(Capability::winmm_input) || has(Capability::loopmidi))
        add("midi");
    if (has(Capability::hid_open))
        add("hid_device");
    if (has(Capability::tablet_open))
        add("tablet_device");
    if (has(Capability::loopback_tcp) || has(Capability::loopback_udp))
        add("loopback_network");
    if (has(Capability::dbus_daemon))
        add("dbus_service");
    return locks;
}

bool collect_cases(std::vector<CaseMetadata>& cases, std::string& error)
{
    const auto& declarations = MayaFlux::Test::Meta::declarations();
    std::map<std::string, Annotation> defaults;
    std::map<std::pair<std::string, int>, Annotation> overrides;
    for (const auto& declaration : declarations) {
        if (declaration.file == nullptr || declaration.file[0] == '\0'
            || declaration.line < 0
            || (declaration.annotation.required >> capability_names.size()) != 0) {
            error = "invalid test metadata declaration";
            return false;
        }
        const std::string file = std::filesystem::path(declaration.file).lexically_normal().string();
        if (declaration.line == 0) {
            if (!defaults.emplace(file, declaration.annotation).second) {
                error = "duplicate file metadata: " + file;
                return false;
            }
        } else if (!overrides.emplace(std::pair { file, declaration.line }, declaration.annotation).second) {
            error = "duplicate case metadata: " + file + ":" + std::to_string(declaration.line);
            return false;
        }
    }

    std::set<std::pair<std::string, int>> used_overrides;
    std::set<std::string> names;
    const auto* unit = testing::UnitTest::GetInstance();
    for (int suite_index = 0; suite_index < unit->total_test_suite_count(); ++suite_index) {
        const auto* suite = unit->GetTestSuite(suite_index);
        for (int test_index = 0; test_index < suite->total_test_count(); ++test_index) {
            const auto* info = suite->GetTestInfo(test_index);
            const std::string file = std::filesystem::path(info->file()).lexically_normal().string();
            const std::pair key { file, info->line() };
            const auto override = overrides.find(key);
            const auto fallback = defaults.find(file);
            if (override == overrides.end() && fallback == defaults.end()) {
                error = "missing test metadata: " + std::string(info->test_suite_name()) + "." + info->name()
                    + " at " + file + ":" + std::to_string(info->line());
                return false;
            }
            const auto name = ctest_name(*info);
            if (name.find_first_of("[];") != std::string::npos || !names.insert(name).second) {
                error = "CTest TEST_LIST cannot represent test name: " + name;
                return false;
            }
            cases.push_back({ name, override == overrides.end() ? fallback->second : override->second });
            if (override != overrides.end())
                used_overrides.insert(key);
        }
    }
    for (const auto& [key, annotation] : overrides) {
        if (!used_overrides.contains(key)) {
            error = "case metadata does not match a test: " + key.first + ":" + std::to_string(key.second);
            return false;
        }
    }
    return true;
}

bool emit_ctest(const std::filesystem::path& output, std::string& error)
{
    std::error_code ignored;
    std::filesystem::remove(output, ignored);
    std::vector<CaseMetadata> cases;
    if (!collect_cases(cases, error))
        return false;

    std::ofstream file(output);
    if (!file) {
        error = "cannot write CTest metadata: " + output.string();
        return false;
    }
    file << "if(NOT DEFINED MayaFluxDiscoveredTests)\n"
            "  message(FATAL_ERROR \"Google Test discovery did not provide MayaFluxDiscoveredTests\")\n"
            "endif()\n"
            "list(LENGTH MayaFluxDiscoveredTests _mayaflux_discovered_count)\n"
            "if(NOT _mayaflux_discovered_count EQUAL " << cases.size() << ")\n"
            "  message(FATAL_ERROR \"Google Test and metadata case counts differ: ${_mayaflux_discovered_count} versus "
         << cases.size() << "\")\n"
            "endif()\n";
    for (std::size_t index = 0; index < cases.size(); ++index) {
        file << "list(GET MayaFluxDiscoveredTests " << index << " _mayaflux_discovered_name)\n"
                "if(NOT _mayaflux_discovered_name STREQUAL " << cmake_quote(cases[index].name) << ")\n"
                "  message(FATAL_ERROR \"Google Test and metadata names differ at index " << index << "\")\n"
                "endif()\n";
    }
    for (const auto& test : cases) {
        file << "set_tests_properties(" << cmake_quote(test.name) << " PROPERTIES LABELS "
             << cmake_quote(labels_for(test.annotation));
        const auto locks = locks_for(test.annotation);
        if (!locks.empty())
            file << " RESOURCE_LOCK " << cmake_quote(locks);
        if (test.annotation.kind == Kind::integration || test.annotation.required != 0)
            file << " TIMEOUT 120";
        file << ")\n";
    }
    file.close();
    if (!file) {
        std::filesystem::remove(output, ignored);
        error = "failed writing CTest metadata: " + output.string();
        return false;
    }
    return true;
}

}

int main(int argc, char** argv)
{
    testing::InitGoogleTest(&argc, argv);
    if (argc == 3 && std::string_view(argv[1]) == "--mayaflux-emit-ctest") {
        std::string error;
        if (!emit_ctest(argv[2], error)) {
            std::cerr << error << '\n';
            return 1;
        }
        return 0;
    }
    return RUN_ALL_TESTS();
}
