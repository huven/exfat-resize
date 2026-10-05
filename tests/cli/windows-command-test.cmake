# SPDX-License-Identifier: MIT

if(NOT DEFINED PROGRAM OR NOT DEFINED TEST_CASE)
    message(FATAL_ERROR "PROGRAM and TEST_CASE are required")
endif()

function(require_text output expected)
    string(FIND "${output}" "${expected}" offset)
    if(offset EQUAL -1)
        message(FATAL_ERROR "Command output does not contain '${expected}':\n${output}")
    endif()
endfunction()

function(expect_success)
    execute_process(
        COMMAND "${PROGRAM}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Command failed unexpectedly:\n${output}${error}")
    endif()
    set(command_output "${output}${error}" PARENT_SCOPE)
endfunction()

function(expect_no_write_failure)
    execute_process(
        COMMAND "${PROGRAM}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    set(combined "${output}${error}")
    if(result EQUAL 0)
        message(FATAL_ERROR "Command succeeded unexpectedly:\n${combined}")
    endif()
    require_text("${combined}"
        "exfat-resize: no filesystem write was attempted; correct the error and retry when appropriate"
    )
    string(FIND "${combined}" "restore the verified backup" destructive_offset)
    string(FIND "${combined}" "filesystem checker" checker_offset)
    if(NOT destructive_offset EQUAL -1 OR NOT checker_offset EQUAL -1)
        message(FATAL_ERROR "Command reported destructive recovery guidance:\n${combined}")
    endif()
    set(command_output "${combined}" PARENT_SCOPE)
endfunction()

if(TEST_CASE STREQUAL "help")
    expect_success(--help)
    foreach(required_text
            "Usage: exfat-resize DEVICE [SIZE]"
            "exfat-resize --grow-partition DEVICE SIZE"
            "exfat-resize --shrink-partition DEVICE SIZE"
            "Arguments:"
            "Desired filesystem size in bytes or with an optional"
            "K, M, or G suffix (powers of 1024)"
            "Options:"
            "Safety:"
            "Documentation:"
            "Make and verify a backup"
            "Ctrl+C and Ctrl+Break request cooperative cancellation at the next safe boundary"
            "Read the safety requirements"
            "README.md distributed with exfat-resize"
            "https://github.com/huven/exfat-resize#safety"
            "drive letter such as E:"
            "--grow-partition"
            "Grow a basic partition to explicit SIZE when needed"
            "Partition options require a drive specifier or volume-GUID path"
            [[Physical-disk paths such as \\.\PhysicalDrive0 are not supported]]
    )
        require_text("${command_output}" "${required_text}")
    endforeach()
elseif(TEST_CASE STREQUAL "recovery-guidance")
    expect_no_write_failure()
    expect_no_write_failure(--unknown)
    expect_no_write_failure(--grow-partition [[E:]])
    require_text("${command_output}" "--grow-partition requires an explicit SIZE")
    expect_no_write_failure(--shrink-partition [[E:]])
    require_text("${command_output}" "--shrink-partition requires an explicit SIZE")
    expect_no_write_failure(--shrink-partition --grow-partition [[E:]] 1G)
    require_text("${command_output}" "mutually exclusive")
    foreach(option --grow-partition --shrink-partition)
        foreach(target
                "${CMAKE_CURRENT_BINARY_DIR}/missing.exfat"
                [[E:\images\missing.exfat]]
                [[\\?\C:\images\missing.exfat]]
        )
            expect_no_write_failure("${option}" "${target}" 8M)
            require_text("${command_output}"
                "${option} requires a logical Windows volume target"
            )
        endforeach()
    endforeach()
    expect_no_write_failure("${CMAKE_CURRENT_BINARY_DIR}/missing.exfat" 0)
    require_text("${command_output}" "invalid size: 0")
    expect_no_write_failure("${CMAKE_CURRENT_BINARY_DIR}/missing.exfat" 1G)
    require_text("${command_output}" "missing.exfat")
    expect_no_write_failure("${CMAKE_CURRENT_BINARY_DIR}/missing.exfat" 1T)
    require_text("${command_output}" "invalid size: 1T")
    expect_no_write_failure("${CMAKE_CURRENT_BINARY_DIR}/missing.exfat")
    expect_no_write_failure("${CMAKE_CURRENT_BINARY_DIR}/exfat-resize-Ω-missing.exfat")
    require_text("${command_output}" "exfat-resize-Ω-missing.exfat")
    expect_no_write_failure([[\\.\PhysicalDrive0]])
    require_text("${command_output}" "unsupported Windows device path")
    foreach(option -h --help -V --version)
        expect_success("${option}")
    endforeach()
elseif(TEST_CASE STREQUAL "partition-options")
    if(NOT DEFINED FIXTURE_PROGRAM)
        message(FATAL_ERROR "FIXTURE_PROGRAM is required")
    endif()
    set(image "${CMAKE_CURRENT_BINARY_DIR}/partition-options-image.exfat")
    execute_process(
        COMMAND "${FIXTURE_PROGRAM}" "${image}"
        RESULT_VARIABLE result
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Cannot create the Windows image fixture")
    endif()
    file(SHA256 "${image}" original_hash)
    foreach(option --grow-partition --shrink-partition)
        # First request growth that fits, then shrink, then growth beyond capacity.
        foreach(size 8192000 5120000 12288000)
            expect_no_write_failure("${option}" "${image}" "${size}")
            require_text("${command_output}"
                "${option} requires a logical Windows volume target"
            )
            file(SHA256 "${image}" rejected_hash)
            if(NOT rejected_hash STREQUAL original_hash)
                message(FATAL_ERROR "Rejected partition option changed the image")
            endif()
        endforeach()
    endforeach()
    # The same fitting target remains valid without a partition option.
    expect_success("${image}" 8192000)
    require_text("${command_output}" "exfat-resize: resized")
    file(SHA256 "${image}" resized_hash)
    if(resized_hash STREQUAL original_hash)
        message(FATAL_ERROR "Ordinary image growth did not change the fixture")
    endif()
    file(REMOVE "${image}")
else()
    message(FATAL_ERROR "Unknown TEST_CASE: ${TEST_CASE}")
endif()
