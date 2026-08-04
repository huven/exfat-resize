# SPDX-License-Identifier: MIT

function(exfat_resize_add_library_test target test_name)
    add_executable(${target} ${ARGN})

    target_include_directories(
        ${target}
        PRIVATE
            ${PROJECT_SOURCE_DIR}/lib
            ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_link_libraries(
        ${target}
        PRIVATE
            exfat_resize
    )

    exfat_resize_configure_target(${target})

    add_test(
        NAME ${test_name}
        COMMAND ${target}
    )
    set_tests_properties(${test_name} PROPERTIES LABELS library)
endfunction()

exfat_resize_add_library_test(
    exfat_resize_foundation_tests
    library.foundation
    support/memory_block_device.c
    support/test_allocator.c
    unit/foundation_tests.c
)

exfat_resize_add_library_test(
    exfat_resize_boot_region_tests
    library.boot_region
    support/memory_block_device.c
    unit/boot_region_tests.c
)

exfat_resize_add_library_test(
    exfat_resize_geometry_tests
    library.geometry
    unit/geometry_tests.c
)

exfat_resize_add_library_test(
    exfat_resize_integration_tests
    library.resize
    support/exfat_fixture.c
    support/memory_block_device.c
    support/test_allocator.c
    integration/resize_tests.c
)

exfat_resize_add_library_test(
    exfat_resize_api_tests
    library.api
    support/exfat_fixture.c
    support/memory_block_device.c
    support/test_allocator.c
    integration/api_tests.c
)

exfat_resize_add_library_test(
    exfat_resize_transaction_tests
    library.transaction
    support/exfat_fixture.c
    support/memory_block_device.c
    support/test_allocator.c
    integration/transaction_tests.c
)
