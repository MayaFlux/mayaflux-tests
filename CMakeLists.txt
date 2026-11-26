file(GLOB_RECURSE MAYAFLUX_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
)

if(APPLE)
    set(MAYAFLUX_COCOA_EVENT_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/support/cocoa_event_injector.mm"
    )
    set(MAYAFLUX_COCOA_PANEL_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/support/cocoa_panel_driver.mm"
    )
    set_source_files_properties(
        "${MAYAFLUX_COCOA_EVENT_SOURCE}" "${MAYAFLUX_COCOA_PANEL_SOURCE}"
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON
        COMPILE_OPTIONS "-include;${CMAKE_SOURCE_DIR}/cmake/pch.h;-fobjc-arc"
    )
endif()

find_package(GTest REQUIRED)
include(GoogleTest)

get_property(MAYAFLUX_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(MAYAFLUX_MULTI_CONFIG)
    set(MAYAFLUX_CTEST_CONFIG "\${CTEST_CONFIGURATION_TYPE}")
else()
    set(MAYAFLUX_CTEST_CONFIG "${CMAKE_BUILD_TYPE}")
endif()

list(REMOVE_ITEM MAYAFLUX_TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/test_main.cpp")
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/test_main.cpp"
    "${CMAKE_SOURCE_DIR}/src/MayaFlux/IO/Image/TinyExrImpl.cpp"
    PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON
)
foreach(test_source IN LISTS MAYAFLUX_TEST_SOURCES)
    file(RELATIVE_PATH test_relative "${CMAKE_CURRENT_SOURCE_DIR}" "${test_source}")
    if(NOT test_relative MATCHES "^([^/]+)/.+\\.cpp$")
        message(FATAL_ERROR "Test source has no parent folder: ${test_relative}")
    endif()
    set(test_group "${CMAKE_MATCH_1}")
    list(APPEND MAYAFLUX_TEST_GROUPS "${test_group}")
    list(APPEND MAYAFLUX_TEST_SOURCES_${test_group} "${test_source}")
endforeach()
list(REMOVE_DUPLICATES MAYAFLUX_TEST_GROUPS)
list(SORT MAYAFLUX_TEST_GROUPS)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(MAYAFLUX_TEST_COMPILE_OPTIONS_DEFAULT "-O0;-g1")
else()
    set(MAYAFLUX_TEST_COMPILE_OPTIONS_DEFAULT "")
endif()
set(MAYAFLUX_TEST_COMPILE_OPTIONS "${MAYAFLUX_TEST_COMPILE_OPTIONS_DEFAULT}" CACHE STRING
    "Compile options for the test targets, applied after the build type flags")

option(MAYAFLUX_TEST_UNITY "Compile each test target as unity batches" OFF)

set(MAYAFLUX_TEST_PCH_GROUPS_full api buffers kriya models nexus portal vulkan yantra)
set(MAYAFLUX_TEST_PCH_GROUPS_math io kakshya kinesis nodes)

set(MAYAFLUX_TEST_PCH_HEADERS_base
    "${CMAKE_SOURCE_DIR}/cmake/pch.h"
    <gtest/gtest.h>
)
set(MAYAFLUX_TEST_PCH_HEADERS_math
    ${MAYAFLUX_TEST_PCH_HEADERS_base}
    <glm/glm.hpp>
)
set(MAYAFLUX_TEST_PCH_HEADERS_full
    ${MAYAFLUX_TEST_PCH_HEADERS_math}
    <vulkan/vulkan.hpp>
    <Eigen/Dense>
)

foreach(test_group IN LISTS MAYAFLUX_TEST_GROUPS)
    set(test_target "MayaFluxTests_${test_group}")
    set(test_list "MayaFluxDiscovered_${test_group}")
    add_executable(${test_target}
        "${CMAKE_CURRENT_SOURCE_DIR}/test_main.cpp"
        ${MAYAFLUX_TEST_SOURCES_${test_group}}
    )

    if(APPLE AND test_group MATCHES "^(buffers|core|portal|vulkan)$")
        target_sources(${test_target} PRIVATE "${MAYAFLUX_COCOA_EVENT_SOURCE}")
    endif()
    if(APPLE AND test_group STREQUAL "core")
        target_sources(${test_target} PRIVATE "${MAYAFLUX_COCOA_PANEL_SOURCE}")
    endif()
    if(WIN32 AND test_group STREQUAL "io")
        target_sources(${test_target} PRIVATE
            "${CMAKE_SOURCE_DIR}/src/MayaFlux/IO/Image/TinyExrImpl.cpp"
        )
    endif()

    set_target_properties(${test_target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    target_compile_options(${test_target} PRIVATE ${MAYAFLUX_TEST_COMPILE_OPTIONS})

    if(MAYAFLUX_TEST_UNITY)
        set_target_properties(${test_target} PROPERTIES
            UNITY_BUILD ON
            UNITY_BUILD_BATCH_SIZE 8
        )
    endif()

    if(test_group IN_LIST MAYAFLUX_TEST_PCH_GROUPS_full)
        set(test_pch_tier full)
    elseif(test_group IN_LIST MAYAFLUX_TEST_PCH_GROUPS_math)
        set(test_pch_tier math)
    else()
        set(test_pch_tier base)
    endif()
    if(DEFINED test_pch_owner_${test_pch_tier})
        target_precompile_headers(${test_target} REUSE_FROM ${test_pch_owner_${test_pch_tier}})
    else()
        set(test_pch_owner_${test_pch_tier} ${test_target})
        target_precompile_headers(${test_target} PRIVATE ${MAYAFLUX_TEST_PCH_HEADERS_${test_pch_tier}})
    endif()

    if(MSVC)
        set_property(TARGET ${test_target} PROPERTY
            MSVC_RUNTIME_LIBRARY MultiThreadedDLL
        )
    endif()

    target_link_libraries(${test_target} PRIVATE MayaFluxLib GTest::gtest)

    if(UNIX AND NOT APPLE AND MAYAFLUX_AUDIO_BACKEND STREQUAL "PIPEWIRE_BACKEND")
        target_link_libraries(${test_target} PRIVATE PkgConfig::PIPEWIRE)
    endif()
    if(UNIX AND NOT APPLE AND MAYAFLUX_WINDOWING_BACKEND STREQUAL "WAYLAND_BACKEND")
        target_include_directories(${test_target} PRIVATE
            "${CMAKE_BINARY_DIR}/src/MayaFlux/wayland_generated"
        )
    endif()
    if(UNIX AND NOT APPLE)
        target_include_directories(${test_target} PRIVATE ${DBUS_INCLUDE_DIRS})
        target_link_libraries(${test_target} PRIVATE ${DBUS_LIBRARIES})
    endif()

    if(APPLE AND MAYAFLUX_AUDIO_BACKEND STREQUAL "COREAUDIO_BACKEND")
        target_link_libraries(${test_target} PRIVATE
            "-framework CoreAudio" "-framework AudioUnit" "-framework AudioToolbox"
        )
    endif()
    if(APPLE AND MAYAFLUX_MIDI_BACKEND STREQUAL "COREMIDI_BACKEND")
        target_link_libraries(${test_target} PRIVATE
            "-framework CoreMIDI" "-framework CoreFoundation"
        )
    endif()
    if(APPLE AND MAYAFLUX_WINDOWING_BACKEND STREQUAL "COCOA_BACKEND")
        target_link_libraries(${test_target} PRIVATE
            "-framework AppKit" "-framework QuartzCore" "-framework CoreFoundation"
            "-framework CoreGraphics" "-framework UniformTypeIdentifiers"
        )
    endif()

    gtest_discover_tests(${test_target}
        NO_PRETTY_TYPES
        NO_PRETTY_VALUES
        TEST_PREFIX "${test_group}/"
        TEST_LIST ${test_list}
        DISCOVERY_MODE POST_BUILD
    )

    set(test_metadata_file
        "${CMAKE_CURRENT_BINARY_DIR}/${test_target}_metadata_$<CONFIG>.cmake"
    )
    add_custom_command(TARGET ${test_target} POST_BUILD
        COMMAND $<TARGET_FILE:${test_target}>
            --mayaflux-emit-ctest "${test_metadata_file}" "${test_group}"
        VERBATIM
    )

    set(test_metadata_include
        "${CMAKE_CURRENT_BINARY_DIR}/${test_target}_metadata_include.cmake"
    )
    file(WRITE "${test_metadata_include}"
        "if(DEFINED ${test_list})\n  set(_mayaflux_metadata_file \"${CMAKE_CURRENT_BINARY_DIR}/${test_target}_metadata_${MAYAFLUX_CTEST_CONFIG}.cmake\")\n  if(NOT EXISTS \"\${_mayaflux_metadata_file}\")\n    message(FATAL_ERROR \"${test_target} metadata was not generated\")\n  endif()\n  include(\"\${_mayaflux_metadata_file}\")\nendif()\n"
    )
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
        "${test_metadata_include}"
    )
endforeach()

set(MAYAFLUX_COVERAGE_GROUP "io" CACHE STRING "Test group whose binary the coverage target runs")
set(MAYAFLUX_COVERAGE_FILTER "src/MayaFlux/IO/(CodecContext|Audio|Video|Camera)/" CACHE STRING "gcovr source filter for the coverage target")

if(MAYAFLUX_COVERAGE)
    if(NOT TARGET MayaFluxTests_${MAYAFLUX_COVERAGE_GROUP})
        message(FATAL_ERROR "MAYAFLUX_COVERAGE_GROUP '${MAYAFLUX_COVERAGE_GROUP}' is not a test group")
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "MAYAFLUX_COVERAGE needs GCC or Clang, found ${CMAKE_CXX_COMPILER_ID}")
    endif()

    target_compile_options(MayaFluxLib PRIVATE --coverage)
    target_link_options(MayaFluxLib PUBLIC --coverage)

    find_program(MAYAFLUX_GCOVR gcovr)
    if(MAYAFLUX_GCOVR)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            set(MAYAFLUX_GCOV_EXECUTABLE "llvm-cov gcov")
        else()
            set(MAYAFLUX_GCOV_EXECUTABLE "gcov")
        endif()

        add_custom_target(coverage
            COMMAND ${CMAKE_COMMAND}
                -DTEST_EXECUTABLE=$<TARGET_FILE:MayaFluxTests_${MAYAFLUX_COVERAGE_GROUP}>
                -DGCOVR=${MAYAFLUX_GCOVR}
                -DGCOV_EXECUTABLE=${MAYAFLUX_GCOV_EXECUTABLE}
                -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
                -DBUILD_ROOT=${CMAKE_BINARY_DIR}
                -DREPORT_DIR=${CMAKE_BINARY_DIR}/coverage
                -DFILTER=${MAYAFLUX_COVERAGE_FILTER}
                -P ${CMAKE_SOURCE_DIR}/cmake/coverage.cmake
            DEPENDS MayaFluxTests_${MAYAFLUX_COVERAGE_GROUP}
            VERBATIM
        )
    else()
        message(WARNING "MAYAFLUX_COVERAGE is on but gcovr was not found; coverage target not created")
    endif()
endif()
