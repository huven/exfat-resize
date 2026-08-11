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
            "Arguments:"
            "Desired filesystem size in bytes"
            "Options:"
            "Safety:"
            "Documentation:"
            "Make and verify a backup"
            "Read the safety requirements"
            "README.md distributed with exfat-resize"
            "https://github.com/huven/exfat-resize#safety"
            "drive letter such as E:"
            [[Physical-disk paths such as \\.\PhysicalDrive0 are not supported]]
    )
        require_text("${command_output}" "${required_text}")
    endforeach()
elseif(TEST_CASE STREQUAL "recovery-guidance")
    expect_no_write_failure()
    expect_no_write_failure(--unknown)
    expect_no_write_failure("${CMAKE_CURRENT_BINARY_DIR}/missing.exfat" 0)
    expect_no_write_failure("${CMAKE_CURRENT_BINARY_DIR}/missing.exfat")
    expect_no_write_failure("${CMAKE_CURRENT_BINARY_DIR}/exfat-resize-Ω-missing.exfat")
    require_text("${command_output}" "exfat-resize-Ω-missing.exfat")
    expect_no_write_failure([[\\.\PhysicalDrive0]])
    require_text("${command_output}" "unsupported Windows device path")
    foreach(option -h --help -V --version)
        expect_success("${option}")
    endforeach()
else()
    message(FATAL_ERROR "Unknown TEST_CASE: ${TEST_CASE}")
endif()
