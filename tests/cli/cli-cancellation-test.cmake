# SPDX-License-Identifier: MIT

if(NOT DEFINED PROGRAM)
    message(FATAL_ERROR "PROGRAM is required")
endif()

foreach(mode before-open after-open during-preflight)
    execute_process(
        COMMAND "${PROGRAM}" "${mode}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    set(combined "${output}${error}")
    if(NOT result EQUAL 130)
        message(FATAL_ERROR
            "${mode} cancellation returned ${result}, expected 130:\n${combined}"
        )
    endif()
    foreach(required_text
            "exfat-resize: interrupted by user"
            "exfat-resize: no filesystem write was attempted"
    )
        string(FIND "${combined}" "${required_text}" offset)
        if(offset EQUAL -1)
            message(FATAL_ERROR
                "${mode} cancellation did not report '${required_text}':\n${combined}"
            )
        endif()
    endforeach()
    string(FIND "${combined}" "resize failed" generic_error_offset)
    if(NOT generic_error_offset EQUAL -1)
        message(FATAL_ERROR "${mode} cancellation used the generic error text:\n${combined}")
    endif()
    string(FIND "${output}" "exfat-resize: checking filesystem" stage_offset)
    if(mode STREQUAL "during-preflight" AND stage_offset EQUAL -1)
        message(FATAL_ERROR "${mode} cancellation did not report the preflight stage:\n${combined}")
    elseif(NOT mode STREQUAL "during-preflight" AND NOT stage_offset EQUAL -1)
        message(FATAL_ERROR "${mode} cancellation entered the library unexpectedly:\n${combined}")
    endif()
endforeach()
