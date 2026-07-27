# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0177)
    cmake_policy(SET CMP0177 NEW)
endif()

file(
    STRINGS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h"
    EXFAT_RESIZE_VERSION_DEFINE
    REGEX
        "^#define EXFAT_RESIZE_VERSION \"[0-9]+\\.[0-9]+\\.[0-9]+\"$"
    LIMIT_COUNT
        1
)
if(NOT EXFAT_RESIZE_VERSION_DEFINE)
    message(FATAL_ERROR "Could not read EXFAT_RESIZE_VERSION from src/version.h")
endif()
string(
    REGEX REPLACE
        "^#define EXFAT_RESIZE_VERSION \"([0-9]+\\.[0-9]+\\.[0-9]+)\"$"
        "\\1"
        EXFAT_RESIZE_VERSION
        "${EXFAT_RESIZE_VERSION_DEFINE}"
)

project(exfat_resize VERSION ${EXFAT_RESIZE_VERSION} LANGUAGES C)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(EXFAT_RESIZE_BUILD_CLI_DEFAULT ON)
    set(EXFAT_RESIZE_BUILD_TESTS_DEFAULT ON)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
else()
    set(EXFAT_RESIZE_BUILD_CLI_DEFAULT OFF)
    set(EXFAT_RESIZE_BUILD_TESTS_DEFAULT OFF)
endif()

option(
    EXFAT_RESIZE_BUILD_CLI
    "Build the exfat-resize command-line utility"
    ${EXFAT_RESIZE_BUILD_CLI_DEFAULT}
)
option(
    EXFAT_RESIZE_BUILD_TESTS
    "Build the exfat-resize tests"
    ${EXFAT_RESIZE_BUILD_TESTS_DEFAULT}
)
option(
    EXFAT_RESIZE_ENABLE_SANITIZERS
    "Build with AddressSanitizer and UndefinedBehaviorSanitizer"
    OFF
)

function(exfat_resize_configure_target target)
    target_compile_features(${target} PRIVATE c_std_11)
    set_target_properties(${target} PROPERTIES C_EXTENSIONS OFF)

    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(
            ${target}
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )

        if(EXFAT_RESIZE_ENABLE_SANITIZERS)
            target_compile_options(
                ${target}
                PRIVATE
                    -fsanitize=address,undefined
                    -fno-omit-frame-pointer
                    -fno-sanitize-recover=all
            )
            get_target_property(target_type ${target} TYPE)
            if(target_type STREQUAL "STATIC_LIBRARY")
                target_link_options(
                    ${target}
                    INTERFACE
                        -fsanitize=address,undefined
                )
            else()
                target_link_options(
                    ${target}
                    PRIVATE
                        -fsanitize=address,undefined
                        -fno-sanitize-recover=all
                )
            endif()
        endif()
    elseif(EXFAT_RESIZE_ENABLE_SANITIZERS)
        message(FATAL_ERROR "Sanitizers require a Clang or GNU compiler")
    endif()
endfunction()

add_library(
    exfat_resize
    STATIC
        lib/block_device.c
        lib/boot_region.c
        lib/checked_math.c
        lib/endian.c
        lib/geometry.c
        lib/resize.c
        lib/sector_adapter.c
)
add_library(exfat_resize::exfat_resize ALIAS exfat_resize)

target_include_directories(
    exfat_resize
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${PROJECT_SOURCE_DIR}/lib
)

exfat_resize_configure_target(exfat_resize)

if(EXFAT_RESIZE_BUILD_CLI)
    add_executable(
        exfat-resize
        src/device.c
        src/main.c
    )

    target_include_directories(
        exfat-resize
        PRIVATE
            ${PROJECT_SOURCE_DIR}/src
    )

    target_link_libraries(
        exfat-resize
        PRIVATE
            exfat_resize
    )

    exfat_resize_configure_target(exfat-resize)
endif()

configure_package_config_file(
    cmake/exfat_resizeConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/exfat_resizeConfig.cmake
    INSTALL_DESTINATION
        ${CMAKE_INSTALL_LIBDIR}/cmake/exfat_resize
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/exfat_resizeConfigVersion.cmake
    VERSION
        ${PROJECT_VERSION}
    COMPATIBILITY
        SameMajorVersion
)

install(
    TARGETS
        exfat_resize
    EXPORT
        exfat_resizeTargets
    ARCHIVE DESTINATION
        ${CMAKE_INSTALL_LIBDIR}
)

if(EXFAT_RESIZE_BUILD_CLI)
    install(
        TARGETS
            exfat-resize
        RUNTIME DESTINATION
            ${CMAKE_INSTALL_BINDIR}
    )
endif()

install(
    FILES
        include/exfat_resize.h
    DESTINATION
        ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
    FILES
        LICENSE
        README.md
    DESTINATION
        ${CMAKE_INSTALL_DOCDIR}
)

install(
    FILES
        docs/TRANSACTION.md
    DESTINATION
        ${CMAKE_INSTALL_DOCDIR}/docs
)

install(
    EXPORT
        exfat_resizeTargets
    NAMESPACE
        exfat_resize::
    DESTINATION
        ${CMAKE_INSTALL_LIBDIR}/cmake/exfat_resize
)

install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/exfat_resizeConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/exfat_resizeConfigVersion.cmake
    DESTINATION
        ${CMAKE_INSTALL_LIBDIR}/cmake/exfat_resize
)

if(EXFAT_RESIZE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/library)
    add_subdirectory(tests/package)
    if(EXFAT_RESIZE_BUILD_CLI AND NOT CMAKE_CROSSCOMPILING)
        add_subdirectory(tests/cli)
    endif()
endif()
