file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
)

if(APPLE)
    file(GLOB_RECURSE TEST_OBJCXX_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/*.mm
    )
    list(APPEND TEST_SOURCES ${TEST_OBJCXX_SOURCES})
    set_source_files_properties(${TEST_OBJCXX_SOURCES}
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON
        COMPILE_OPTIONS "-include;${CMAKE_SOURCE_DIR}/cmake/pch.h;-fobjc-arc"
    )
endif()

add_executable(MayaFluxTests ${TEST_SOURCES})

if(WIN32)
    target_sources(MayaFluxTests PRIVATE
        "${CMAKE_SOURCE_DIR}/src/MayaFlux/IO/Image/TinyExrImpl.cpp"
    )
endif()

set_target_properties(MayaFluxTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

find_package(GTest REQUIRED)

target_precompile_headers(MayaFluxTests PUBLIC ${CMAKE_SOURCE_DIR}/cmake/pch.h)

if(MSVC)
    set_property(TARGET MayaFluxTests PROPERTY
        MSVC_RUNTIME_LIBRARY MultiThreadedDLL
    )
endif()

target_link_libraries(MayaFluxTests PRIVATE
    MayaFluxLib
    GTest::gtest
)

if(UNIX AND NOT APPLE AND MAYAFLUX_AUDIO_BACKEND STREQUAL "PIPEWIRE_BACKEND")
    target_link_libraries(MayaFluxTests PRIVATE PkgConfig::PIPEWIRE)
endif()

if(UNIX AND NOT APPLE AND MAYAFLUX_WINDOWING_BACKEND STREQUAL "WAYLAND_BACKEND")
    target_include_directories(MayaFluxTests PRIVATE
        "${CMAKE_BINARY_DIR}/src/MayaFlux/wayland_generated"
    )
endif()

if(UNIX AND NOT APPLE)
    target_include_directories(MayaFluxTests PRIVATE ${DBUS_INCLUDE_DIRS})
    target_link_libraries(MayaFluxTests PRIVATE ${DBUS_LIBRARIES})
endif()

if(APPLE AND MAYAFLUX_AUDIO_BACKEND STREQUAL "COREAUDIO_BACKEND")
    target_link_libraries(MayaFluxTests PRIVATE
        "-framework CoreAudio"
        "-framework AudioUnit"
        "-framework AudioToolbox"
    )
endif()

if(APPLE AND MAYAFLUX_MIDI_BACKEND STREQUAL "COREMIDI_BACKEND")
    target_link_libraries(MayaFluxTests PRIVATE
        "-framework CoreMIDI"
        "-framework CoreFoundation"
    )
endif()

if(APPLE AND MAYAFLUX_WINDOWING_BACKEND STREQUAL "COCOA_BACKEND")
    target_link_libraries(MayaFluxTests PRIVATE
        "-framework AppKit"
        "-framework QuartzCore"
        "-framework CoreFoundation"
        "-framework CoreGraphics"
        "-framework UniformTypeIdentifiers"
    )
endif()

include(GoogleTest)
gtest_discover_tests(MayaFluxTests
    NO_PRETTY_TYPES
    NO_PRETTY_VALUES
    TEST_LIST MayaFluxDiscoveredTests
    DISCOVERY_MODE POST_BUILD
)

set(MAYAFLUX_TEST_METADATA_FILE
    "${CMAKE_CURRENT_BINARY_DIR}/MayaFluxTests_metadata_$<CONFIG>.cmake"
)

add_custom_command(TARGET MayaFluxTests POST_BUILD
    COMMAND $<TARGET_FILE:MayaFluxTests>
        --mayaflux-emit-ctest "${MAYAFLUX_TEST_METADATA_FILE}"
    VERBATIM
)

set(MAYAFLUX_TEST_METADATA_INCLUDE
    "${CMAKE_CURRENT_BINARY_DIR}/MayaFluxTests_metadata_include.cmake"
)

get_property(MAYAFLUX_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(MAYAFLUX_MULTI_CONFIG)
    set(MAYAFLUX_CTEST_CONFIG "\${CTEST_CONFIGURATION_TYPE}")
else()
    set(MAYAFLUX_CTEST_CONFIG "${CMAKE_BUILD_TYPE}")
endif()

file(WRITE "${MAYAFLUX_TEST_METADATA_INCLUDE}"
    "if(DEFINED MayaFluxDiscoveredTests)\n  set(_mayaflux_metadata_file \"${CMAKE_CURRENT_BINARY_DIR}/MayaFluxTests_metadata_${MAYAFLUX_CTEST_CONFIG}.cmake\")\n  if(NOT EXISTS \"\${_mayaflux_metadata_file}\")\n    message(FATAL_ERROR \"MayaFluxTests metadata was not generated\")\n  endif()\n  include(\"\${_mayaflux_metadata_file}\")\nendif()\n"
)

set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
    "${MAYAFLUX_TEST_METADATA_INCLUDE}"
)
