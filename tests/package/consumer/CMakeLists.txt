# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)

project(exfat_resize_consumer LANGUAGES C CXX)

if(EXFAT_RESIZE_SOURCE_DIR)
    add_subdirectory("${EXFAT_RESIZE_SOURCE_DIR}" exfat-resize)
elseif(EXFAT_RESIZE_FIND_VERSION AND EXFAT_RESIZE_FIND_EXACT)
    find_package(exfat_resize ${EXFAT_RESIZE_FIND_VERSION} EXACT CONFIG REQUIRED)
elseif(EXFAT_RESIZE_FIND_VERSION)
    find_package(exfat_resize ${EXFAT_RESIZE_FIND_VERSION} CONFIG REQUIRED)
else()
    find_package(exfat_resize CONFIG REQUIRED)
endif()

if(EXFAT_RESIZE_EXPECTED_VERSION AND
   NOT "${exfat_resize_VERSION}" VERSION_EQUAL "${EXFAT_RESIZE_EXPECTED_VERSION}")
    message(
        FATAL_ERROR
        "Expected exfat_resize ${EXFAT_RESIZE_EXPECTED_VERSION}, found ${exfat_resize_VERSION}"
    )
endif()

add_executable(exfat_resize_c_consumer consumer.c)
target_compile_features(exfat_resize_c_consumer PRIVATE c_std_11)
target_link_libraries(exfat_resize_c_consumer PRIVATE exfat_resize::exfat_resize)

add_executable(exfat_resize_cxx_consumer consumer.cpp)
target_compile_features(exfat_resize_cxx_consumer PRIVATE cxx_std_11)
target_link_libraries(exfat_resize_cxx_consumer PRIVATE exfat_resize::exfat_resize)

enable_testing()
add_test(NAME consumer.c COMMAND exfat_resize_c_consumer)
add_test(NAME consumer.cxx COMMAND exfat_resize_cxx_consumer)
