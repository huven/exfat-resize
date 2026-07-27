# SPDX-License-Identifier: MIT

add_executable(
    exfat_resize_foundation_tests
    support/memory_block_device.c
    unit/foundation_tests.c
)

target_include_directories(
    exfat_resize_foundation_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/lib
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(
    exfat_resize_foundation_tests
    PRIVATE
        exfat_resize
)

exfat_resize_configure_target(exfat_resize_foundation_tests)

add_test(
    NAME library.foundation
    COMMAND exfat_resize_foundation_tests
)

add_executable(
    exfat_resize_boot_region_tests
    support/memory_block_device.c
    unit/boot_region_tests.c
)

target_include_directories(
    exfat_resize_boot_region_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/lib
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(
    exfat_resize_boot_region_tests
    PRIVATE
        exfat_resize
)

exfat_resize_configure_target(exfat_resize_boot_region_tests)

add_test(
    NAME library.boot_region
    COMMAND exfat_resize_boot_region_tests
)

add_executable(
    exfat_resize_geometry_tests
    unit/geometry_tests.c
)

target_include_directories(
    exfat_resize_geometry_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/lib
)

target_link_libraries(
    exfat_resize_geometry_tests
    PRIVATE
        exfat_resize
)

exfat_resize_configure_target(exfat_resize_geometry_tests)

add_test(
    NAME library.geometry
    COMMAND exfat_resize_geometry_tests
)

add_executable(
    exfat_resize_integration_tests
    support/exfat_fixture.c
    support/memory_block_device.c
    integration/resize_tests.c
)

target_include_directories(
    exfat_resize_integration_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/lib
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(
    exfat_resize_integration_tests
    PRIVATE
        exfat_resize
)

exfat_resize_configure_target(exfat_resize_integration_tests)

add_test(
    NAME library.resize
    COMMAND exfat_resize_integration_tests
)

add_executable(
    exfat_resize_api_tests
    support/exfat_fixture.c
    support/memory_block_device.c
    integration/api_tests.c
)

target_include_directories(
    exfat_resize_api_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/lib
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(
    exfat_resize_api_tests
    PRIVATE
        exfat_resize
)

exfat_resize_configure_target(exfat_resize_api_tests)

add_test(
    NAME library.api
    COMMAND exfat_resize_api_tests
)

add_executable(
    exfat_resize_transaction_tests
    support/exfat_fixture.c
    support/memory_block_device.c
    integration/transaction_tests.c
)

target_include_directories(
    exfat_resize_transaction_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/lib
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(
    exfat_resize_transaction_tests
    PRIVATE
        exfat_resize
)

exfat_resize_configure_target(exfat_resize_transaction_tests)

add_test(
    NAME library.transaction
    COMMAND exfat_resize_transaction_tests
)

set_tests_properties(
    library.foundation
    library.boot_region
    library.geometry
    library.resize
    library.api
    library.transaction
    PROPERTIES
        LABELS library
)
