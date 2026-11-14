#pragma once

namespace MayaFlux::Test::Meta {

enum class Kind : std::uint8_t {
    contract,
    integration
};

enum class Capability : std::uint8_t {
    writable_temp,
    host_toolchain,
    repo_assets,
    system_font,
    loopback_tcp,
    loopback_udp,
    dbus_daemon,
    desktop_dialog,
    display,
    vulkan,
    vulkan_present,
    vulkan_validation,
    shader_float64,
    separate_compute_queue,
    pipewire,
    coreaudio,
    wasapi,
    audio_output,
    audio_input,
    coremidi,
    alsa_sequencer,
    winmm_input,
    loopmidi,
    hid_enumeration,
    hid_open,
    tablet_enumeration,
    tablet_open,
    silero_weights,
    hubert_weights,
    process_exec,
    homebrew_paths
};

template <Capability... Values>
constexpr std::uint64_t requirements()
{
    return (std::uint64_t { 0 } | ... | (std::uint64_t { 1 } << static_cast<unsigned>(Values)));
}

struct Annotation {
    Kind kind;
    std::uint64_t required;
};

struct Declaration {
    const char* file;
    int line;
    Annotation annotation;
};

inline std::vector<Declaration>& declarations()
{
    static std::vector<Declaration> values;
    return values;
}

inline bool declare_file(const char* file, Annotation annotation)
{
    declarations().push_back({ file, 0, annotation });
    return true;
}

inline bool declare_case(const char* file, int line, Annotation annotation)
{
    declarations().push_back({ file, line, annotation });
    return true;
}

}

#define MAYAFLUX_TEST_JOIN_IMPL(left, right) left##right
#define MAYAFLUX_TEST_JOIN(left, right) MAYAFLUX_TEST_JOIN_IMPL(left, right)

#define MAYAFLUX_TEST_FILE(annotation) \
    namespace { [[maybe_unused]] const bool MAYAFLUX_TEST_JOIN(mayaflux_test_file_, __COUNTER__) = \
        ::MayaFlux::Test::Meta::declare_file(__FILE__, annotation); }

#define MAYAFLUX_TEST_WITH(test_macro, suite, name, annotation) \
    namespace { [[maybe_unused]] const bool MAYAFLUX_TEST_JOIN(mayaflux_test_case_, __COUNTER__) = \
        ::MayaFlux::Test::Meta::declare_case(__FILE__, __LINE__, annotation); } \
    test_macro(suite, name)
