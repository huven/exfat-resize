# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)

foreach(required_variable
        EXFAT_RESIZE_SOURCE_DIR
        EXFAT_RESIZE_COMMIT
        EXFAT_RESIZE_VERSION_OUTPUT_DIR
)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} must be provided")
    endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/ExfatResizeVersion.cmake")

exfat_resize_resolve_commit_version(
    "${EXFAT_RESIZE_SOURCE_DIR}"
    "${EXFAT_RESIZE_COMMIT}"
    package_version
    build_version
)

file(MAKE_DIRECTORY "${EXFAT_RESIZE_VERSION_OUTPUT_DIR}")
file(WRITE
    "${EXFAT_RESIZE_VERSION_OUTPUT_DIR}/package-version"
    "${package_version}\n"
)
file(WRITE
    "${EXFAT_RESIZE_VERSION_OUTPUT_DIR}/build-version"
    "${build_version}\n"
)
