# SPDX-License-Identifier: MIT

function(exfat_resize_add_device_test target source test_name)
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(
            device_source
            ${PROJECT_SOURCE_DIR}/src/windows/device.c
            ${PROJECT_SOURCE_DIR}/src/windows/device_path.c
        )
    else()
        set(device_source ${PROJECT_SOURCE_DIR}/src/posix/device.c)
    endif()
    add_executable(
        ${target}
        ${source}
        ${device_source}
    )

    target_include_directories(
        ${target}
        PRIVATE
            ${PROJECT_SOURCE_DIR}/lib
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

if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND MSVC)
    add_executable(
        exfat_resize_partition_fault_test
        ${PROJECT_SOURCE_DIR}/src/cli.c
        ${PROJECT_SOURCE_DIR}/src/windows/device.c
        ${PROJECT_SOURCE_DIR}/src/windows/device_path.c
        ${PROJECT_SOURCE_DIR}/src/windows/entry.c
        ${PROJECT_SOURCE_DIR}/src/windows/partition.c
        windows-partition-fault.c
    )
    target_include_directories(
        exfat_resize_partition_fault_test
        PRIVATE
            ${PROJECT_SOURCE_DIR}/lib
            ${PROJECT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_compile_definitions(
        exfat_resize_partition_fault_test
        PRIVATE
            EXFAT_RESIZE_BUILD_VERSION="${EXFAT_RESIZE_BUILD_VERSION}-partition-fault-test"
    )
    target_compile_options(
        exfat_resize_partition_fault_test
        PRIVATE
            "/FI${CMAKE_CURRENT_SOURCE_DIR}/windows-api-redirect.h"
    )
    target_link_libraries(exfat_resize_partition_fault_test PRIVATE exfat_resize)
    exfat_resize_configure_target(exfat_resize_partition_fault_test)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_executable(
        exfat_resize_windows_device_path_test
        windows-device-path.c
        ${PROJECT_SOURCE_DIR}/src/windows/device_path.c
    )
    target_include_directories(
        exfat_resize_windows_device_path_test
        PRIVATE
            ${PROJECT_SOURCE_DIR}/src
    )
    exfat_resize_configure_target(exfat_resize_windows_device_path_test)
    add_test(
        NAME cli.windows-device-path
        COMMAND exfat_resize_windows_device_path_test
    )
    set_tests_properties(cli.windows-device-path PROPERTIES LABELS cli)

    exfat_resize_add_device_test(
        exfat_resize_device_image_test
        device-image-windows.c
        cli.device-image
    )

    foreach(test_case help recovery-guidance)
        add_test(
            NAME cli.${test_case}
            COMMAND
                ${CMAKE_COMMAND}
                "-DPROGRAM=$<TARGET_FILE:exfat-resize>"
                "-DTEST_CASE=${test_case}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/windows-command-test.cmake"
        )
        set_tests_properties(cli.${test_case} PROPERTIES LABELS cli)
    endforeach()
    return()
endif()

add_library(
    exfat_resize_posix_entry_test_object
    OBJECT
        ${PROJECT_SOURCE_DIR}/src/posix/entry.c
)
target_include_directories(
    exfat_resize_posix_entry_test_object
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
)
target_compile_definitions(
    exfat_resize_posix_entry_test_object
    PRIVATE
        main=exfat_resize_test_posix_entry
)
exfat_resize_configure_target(exfat_resize_posix_entry_test_object)

add_executable(
    exfat_resize_posix_entry_test
    posix-entry.c
    $<TARGET_OBJECTS:exfat_resize_posix_entry_test_object>
)
target_include_directories(
    exfat_resize_posix_entry_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
)
exfat_resize_configure_target(exfat_resize_posix_entry_test)
add_test(NAME cli.posix-entry COMMAND exfat_resize_posix_entry_test)
set_tests_properties(cli.posix-entry PROPERTIES LABELS cli)

add_executable(
    exfat_resize_cli_cancellation_test
    cli-cancellation.c
    ${PROJECT_SOURCE_DIR}/src/cli.c
)
target_include_directories(
    exfat_resize_cli_cancellation_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/lib
        ${PROJECT_SOURCE_DIR}/src
)
target_compile_definitions(
    exfat_resize_cli_cancellation_test
    PRIVATE
        EXFAT_RESIZE_BUILD_VERSION="${EXFAT_RESIZE_BUILD_VERSION}-cancellation-test"
)
target_link_libraries(exfat_resize_cli_cancellation_test PRIVATE exfat_resize)
exfat_resize_configure_target(exfat_resize_cli_cancellation_test)
add_test(
    NAME cli.cancellation
    COMMAND
        ${CMAKE_COMMAND}
        "-DPROGRAM=$<TARGET_FILE:exfat_resize_cli_cancellation_test>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cli-cancellation-test.cmake"
)
set_tests_properties(cli.cancellation PROPERTIES LABELS cli)

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
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    exfat_resize_add_device_test(
        exfat_resize_device_lock_size_test
        device-lock-size.c
        cli.device-lock-size
    )
    target_link_options(
        exfat_resize_device_lock_size_test
        PRIVATE
            "LINKER:--wrap=flock"
    )
endif()

set(
    EXFAT_RESIZE_CLI_TEST_SCRIPTS
    help.sh
    recovery-guidance.sh
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
