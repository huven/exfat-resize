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

include(CheckCSourceCompiles)
set(exfat_resize_had_try_compile_target_type OFF)
if(DEFINED CMAKE_TRY_COMPILE_TARGET_TYPE)
    set(exfat_resize_had_try_compile_target_type ON)
    set(exfat_resize_saved_try_compile_target_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
endif()
if(EXFAT_RESIZE_SOURCE_DIR)
    set(CMAKE_REQUIRED_INCLUDES "${EXFAT_RESIZE_SOURCE_DIR}/include")
else()
    get_target_property(
        CMAKE_REQUIRED_INCLUDES
        exfat_resize::exfat_resize
        INTERFACE_INCLUDE_DIRECTORIES
    )
endif()
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
unset(EXFAT_RESIZE_LEGACY_OPTIONS_COMPILES CACHE)
check_c_source_compiles(
    "#include <exfat_resize.h>
int main(void) {
    struct exfat_resize_options legacy;
    return (int)sizeof(legacy);
}"
    EXFAT_RESIZE_LEGACY_OPTIONS_COMPILES
)
if(EXFAT_RESIZE_LEGACY_OPTIONS_COMPILES)
    message(FATAL_ERROR "The removed exfat_resize_options tag still compiles")
endif()
unset(EXFAT_RESIZE_FOUR_ARGUMENT_CALL_COMPILES CACHE)
check_c_source_compiles(
    "#include <exfat_resize.h>
int main(void) {
    return (int)exfat_resize(NULL, 1, NULL, NULL);
}"
    EXFAT_RESIZE_FOUR_ARGUMENT_CALL_COMPILES
)
if(EXFAT_RESIZE_FOUR_ARGUMENT_CALL_COMPILES)
    message(FATAL_ERROR "The four-argument exfat_resize call still compiles")
endif()
unset(CMAKE_REQUIRED_INCLUDES)
if(exfat_resize_had_try_compile_target_type)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE "${exfat_resize_saved_try_compile_target_type}")
else()
    unset(CMAKE_TRY_COMPILE_TARGET_TYPE)
endif()
unset(exfat_resize_had_try_compile_target_type)
unset(exfat_resize_saved_try_compile_target_type)

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
