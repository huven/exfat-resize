# SPDX-License-Identifier: MIT

if(NOT DEFINED PROGRAM)
    message(FATAL_ERROR "PROGRAM is required")
endif()

foreach(mode diagnostic-orphan diagnostic-fat-chain diagnostic-directory-checksum)
    execute_process(
        COMMAND "${PROGRAM}" "${mode}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    string(REPLACE "\r\n" "\n" output "${output}")
    string(REPLACE "\r\n" "\n" error "${error}")
    if(NOT result EQUAL 1)
        message(FATAL_ERROR "${mode} returned ${result}, expected 1:\n${output}${error}")
    endif()
    if(NOT output STREQUAL "exfat-resize: checking filesystem\n")
        message(FATAL_ERROR "${mode} printed unexpected stdout:\n${output}${error}")
    endif()
    if(mode STREQUAL "diagnostic-orphan")
        set(diagnostic
            "cluster 9 is allocated in the bitmap but has no recognized owner (FAT=0xffffffff)"
        )
    elseif(mode STREQUAL "diagnostic-fat-chain")
        set(diagnostic
            "FAT chain ends before its declared length at cluster 3 (next=0xffffffff)"
        )
    else()
        set(diagnostic
            "invalid directory entry at byte offset 64 in directory starting at cluster 2: entry-set checksum does not match"
        )
    endif()
    string(FIND "${error}" "exfat-resize: ${diagnostic}" diagnostic_offset)
    string(FIND "${error}"
        "exfat-resize: resize failed: invalid or corrupt exFAT filesystem" failure_offset
    )
    string(FIND "${error}"
        "exfat-resize: no filesystem write was attempted" recovery_offset
    )
    if(diagnostic_offset EQUAL -1 OR failure_offset LESS diagnostic_offset
            OR recovery_offset LESS failure_offset)
        message(FATAL_ERROR "${mode} omitted or reordered its diagnostics:\n${error}")
    endif()
    string(REGEX MATCHALL "exfat-resize:" prefixes "${error}")
    list(LENGTH prefixes prefix_count)
    if(NOT prefix_count EQUAL 3)
        message(FATAL_ERROR "${mode} emitted duplicate or unexpected diagnostics:\n${error}")
    endif()
endforeach()
