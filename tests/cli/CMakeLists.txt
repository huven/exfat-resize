# SPDX-License-Identifier: MIT

function(exfat_resize_add_device_test target source test_name)
    add_executable(
        ${target}
        ${source}
        ${PROJECT_SOURCE_DIR}/src/device.c
    )

    target_include_directories(
        ${target}
        PRIVATE
            ${PROJECT_SOURCE_DIR}/src
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
    set_tests_properties(${test_name} PROPERTIES LABELS cli)
endfunction()

exfat_resize_add_device_test(
    exfat_resize_device_bounds_test
    device-bounds.c
    cli.device-bounds
)
exfat_resize_add_device_test(
    exfat_resize_device_errors_test
    device-errors.c
    cli.device-errors
)
exfat_resize_add_device_test(
    exfat_resize_device_lock_test
    device-lock.c
    cli.device-lock
)

set(
    EXFAT_RESIZE_CLI_TEST_SCRIPTS
    help.sh
    grow-image.sh
    preserve-data.sh
    explicit-size.sh
    sector-size.sh
    sector-mapping.sh
    reject-input.sh
    raw-device.sh
    work-buffer.sh
    volume-flags.sh
)

foreach(script IN LISTS EXFAT_RESIZE_CLI_TEST_SCRIPTS)
    string(REGEX REPLACE "\\.sh$" "" test_name "${script}")
    add_test(
        NAME cli.${test_name}
        COMMAND
            ${CMAKE_COMMAND} -E env
                "DEVICE_LOCK_TEST=$<TARGET_FILE:exfat_resize_device_lock_test>"
                "${CMAKE_CURRENT_SOURCE_DIR}/${script}"
                "$<TARGET_FILE:exfat-resize>"
    )
    set_tests_properties(
        cli.${test_name}
        PROPERTIES
            LABELS cli
            SKIP_RETURN_CODE 77
    )
endforeach()
