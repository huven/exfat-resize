# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0177)
    cmake_policy(SET CMP0177 NEW)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExfatResizeVersion.cmake")
exfat_resize_read_package_version(
    "${CMAKE_CURRENT_SOURCE_DIR}"
    EXFAT_RESIZE_VERSION
)

project(exfat_resize VERSION ${EXFAT_RESIZE_VERSION} LANGUAGES C)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

set(EXFAT_RESIZE_CLI_SUPPORTED OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR
   CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
   CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(EXFAT_RESIZE_CLI_SUPPORTED ON)
endif()

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(EXFAT_RESIZE_BUILD_CLI_DEFAULT ${EXFAT_RESIZE_CLI_SUPPORTED})
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

if(EXFAT_RESIZE_BUILD_CLI AND NOT EXFAT_RESIZE_CLI_SUPPORTED)
    message(FATAL_ERROR "The exfat-resize CLI supports only macOS, Linux, and Windows")
endif()

set(EXFAT_RESIZE_BUILD_VERSION "${PROJECT_VERSION}-unknown")
if(EXFAT_RESIZE_BUILD_CLI)
    exfat_resize_resolve_checkout_build_version(
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROJECT_VERSION}"
        EXFAT_RESIZE_BUILD_VERSION
    )
endif()

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
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(
            EXFAT_RESIZE_CLI_SOURCES
            src/cli.c
            src/windows/device.c
            src/windows/entry.c
        )
    else()
        set(
            EXFAT_RESIZE_CLI_SOURCES
            src/cli.c
            src/posix/device.c
            src/posix/entry.c
        )
    endif()

    add_executable(
        exfat-resize
        ${EXFAT_RESIZE_CLI_SOURCES}
    )

    target_include_directories(
        exfat-resize
        PRIVATE
            ${PROJECT_SOURCE_DIR}/lib
            ${PROJECT_SOURCE_DIR}/src
    )

    target_compile_definitions(
        exfat-resize
        PRIVATE
            EXFAT_RESIZE_BUILD_VERSION="${EXFAT_RESIZE_BUILD_VERSION}"
    )

    target_link_libraries(
        exfat-resize
        PRIVATE
            exfat_resize
    )

    exfat_resize_configure_target(exfat-resize)

    if(MINGW)
        target_link_options(exfat-resize PRIVATE -municode)
    endif()
endif()

set(EXFAT_RESIZE_INSTALL_MANPAGE OFF)
if(EXFAT_RESIZE_BUILD_CLI AND NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    configure_file(
        docs/exfat-resize.8.in
        ${CMAKE_CURRENT_BINARY_DIR}/exfat-resize.8
        @ONLY
    )
    set(EXFAT_RESIZE_INSTALL_MANPAGE ON)
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
    COMPONENT
        Development
)

if(EXFAT_RESIZE_BUILD_CLI)
    install(
        TARGETS
            exfat-resize
        RUNTIME DESTINATION
            ${CMAKE_INSTALL_BINDIR}
        COMPONENT
            Runtime
    )
    if(EXFAT_RESIZE_INSTALL_MANPAGE)
        install(
            FILES
                ${CMAKE_CURRENT_BINARY_DIR}/exfat-resize.8
            DESTINATION
                ${CMAKE_INSTALL_MANDIR}/man8
            COMPONENT
                Runtime
        )
    endif()
endif()

install(
    FILES
        include/exfat_resize.h
    DESTINATION
        ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT
        Development
)

install(
    FILES
        LICENSE
        README.md
    DESTINATION
        ${CMAKE_INSTALL_DOCDIR}
    COMPONENT
        Runtime
)

install(
    FILES
        docs/TRANSACTION.md
    DESTINATION
        ${CMAKE_INSTALL_DOCDIR}/docs
    COMPONENT
        Runtime
)

install(
    EXPORT
        exfat_resizeTargets
    NAMESPACE
        exfat_resize::
    DESTINATION
        ${CMAKE_INSTALL_LIBDIR}/cmake/exfat_resize
    COMPONENT
        Development
)

install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/exfat_resizeConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/exfat_resizeConfigVersion.cmake
    DESTINATION
        ${CMAKE_INSTALL_LIBDIR}/cmake/exfat_resize
    COMPONENT
        Development
)

if(EXFAT_RESIZE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/library)
    add_subdirectory(tests/package)
    if(EXFAT_RESIZE_BUILD_CLI AND NOT CMAKE_CROSSCOMPILING)
        add_subdirectory(tests/cli)
    endif()
endif()
