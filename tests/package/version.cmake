# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)

foreach(required_variable
        EXFAT_RESIZE_PROJECT_ROOT
        EXFAT_RESIZE_VERSION_TEST_DIR
)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} must be provided")
    endif()
endforeach()

include("${EXFAT_RESIZE_PROJECT_ROOT}/cmake/ExfatResizeVersion.cmake")

if(DEFINED EXFAT_RESIZE_VERSION_TEST_MODE)
    if(EXFAT_RESIZE_VERSION_TEST_MODE STREQUAL "checkout")
        exfat_resize_read_package_version(
            "${EXFAT_RESIZE_VERSION_TEST_REPOSITORY}"
            package_version
        )
        exfat_resize_resolve_checkout_build_version(
            "${EXFAT_RESIZE_VERSION_TEST_REPOSITORY}"
            "${package_version}"
            build_version
        )
    elseif(EXFAT_RESIZE_VERSION_TEST_MODE STREQUAL "commit")
        exfat_resize_resolve_commit_version(
            "${EXFAT_RESIZE_VERSION_TEST_REPOSITORY}"
            "${EXFAT_RESIZE_VERSION_TEST_COMMIT}"
            package_version
            build_version
        )
    else()
        message(FATAL_ERROR
            "Unknown version test mode: ${EXFAT_RESIZE_VERSION_TEST_MODE}"
        )
    endif()
    return()
endif()

find_package(Git QUIET)
if(NOT GIT_FOUND)
    message(FATAL_ERROR "Git is required for the version test")
endif()

set(repository "${EXFAT_RESIZE_VERSION_TEST_DIR}/repository")

function(run_git)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${repository}" ${ARGN}
        RESULT_VARIABLE git_result
        OUTPUT_VARIABLE git_output
        ERROR_VARIABLE git_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT git_result EQUAL 0)
        message(FATAL_ERROR
            "Git command failed: ${ARGN}\n${git_output}${git_error}"
        )
    endif()
endfunction()

function(read_head source_dir output_variable)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${source_dir}" rev-parse HEAD
        RESULT_VARIABLE git_result
        OUTPUT_VARIABLE commit
        ERROR_VARIABLE git_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT git_result EQUAL 0)
        message(FATAL_ERROR "Could not read test commit: ${git_error}")
    endif()
    set(${output_variable} "${commit}" PARENT_SCOPE)
endfunction()

function(expect_checkout source_dir expected_package_version expected_build_version)
    exfat_resize_read_package_version("${source_dir}" package_version)
    exfat_resize_resolve_checkout_build_version(
        "${source_dir}"
        "${package_version}"
        build_version
    )
    if(NOT package_version STREQUAL expected_package_version OR
       NOT build_version STREQUAL expected_build_version)
        message(FATAL_ERROR
            "Checkout version in ${source_dir}: expected "
            "'${expected_package_version}' and '${expected_build_version}', got "
            "'${package_version}' and '${build_version}'"
        )
    endif()
endfunction()

function(expect_commit source_dir commit expected_package_version expected_build_version)
    exfat_resize_resolve_commit_version(
        "${source_dir}"
        "${commit}"
        package_version
        build_version
    )
    if(NOT package_version STREQUAL expected_package_version OR
       NOT build_version STREQUAL expected_build_version)
        message(FATAL_ERROR
            "Commit version in ${source_dir}: expected "
            "'${expected_package_version}' and '${expected_build_version}', got "
            "'${package_version}' and '${build_version}'"
        )
    endif()
endfunction()

function(expect_tag_rejected mode commit)
    set(test_arguments
        "-DEXFAT_RESIZE_PROJECT_ROOT=${EXFAT_RESIZE_PROJECT_ROOT}"
        "-DEXFAT_RESIZE_VERSION_TEST_DIR=${EXFAT_RESIZE_VERSION_TEST_DIR}"
        "-DEXFAT_RESIZE_VERSION_TEST_MODE=${mode}"
        "-DEXFAT_RESIZE_VERSION_TEST_REPOSITORY=${repository}"
    )
    if(mode STREQUAL "commit")
        list(APPEND test_arguments
            "-DEXFAT_RESIZE_VERSION_TEST_COMMIT=${commit}"
        )
    endif()

    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" ${test_arguments}
            -P "${CMAKE_CURRENT_LIST_FILE}"
        RESULT_VARIABLE test_result
        OUTPUT_VARIABLE test_output
        ERROR_VARIABLE test_error
    )
    if(test_result EQUAL 0)
        message(FATAL_ERROR
            "${mode} accepted a tag that does not match VERSION"
        )
    endif()

    string(TOLOWER "${test_output}${test_error}" test_log)
    string(FIND
        "${test_log}"
        "tag v2.0.0 does not match package version 1.3.0"
        mismatch_offset
    )
    if(mismatch_offset EQUAL -1)
        message(FATAL_ERROR
            "${mode} rejection did not explain the tag mismatch:\n${test_log}"
        )
    endif()
endfunction()

file(REMOVE_RECURSE "${EXFAT_RESIZE_VERSION_TEST_DIR}")
file(MAKE_DIRECTORY "${repository}")
run_git(init --quiet)
run_git(config user.name "exfat-resize test")
run_git(config user.email "test@example.invalid")
file(WRITE "${repository}/VERSION" "1.2.3\n")
file(WRITE "${repository}/NOTES" "clean\n")
run_git(add VERSION NOTES)
run_git(commit --quiet -m "Matching version")
read_head("${repository}" matching_commit)
run_git(tag v1.2.3)

expect_checkout("${repository}" "1.2.3" "1.2.3")
expect_commit("${repository}" "${matching_commit}" "1.2.3" "1.2.3")

execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
file(TOUCH_NOCREATE "${repository}/NOTES")
expect_checkout("${repository}" "1.2.3" "1.2.3")

file(APPEND "${repository}/NOTES" "dirty\n")
expect_checkout("${repository}" "1.2.3" "1.2.3-dirty")
expect_commit("${repository}" "${matching_commit}" "1.2.3" "1.2.3")
run_git(checkout -- NOTES)

file(WRITE "${repository}/VERSION" "1.3.0\n")
run_git(add VERSION)
run_git(commit --quiet -m "Version transition")
read_head("${repository}" transition_commit)
string(SUBSTRING "${transition_commit}" 0 12 transition_id)
set(transition_version "1.3.0-g${transition_id}")

expect_checkout("${repository}" "1.3.0" "${transition_version}")
expect_commit(
    "${repository}"
    "${transition_commit}"
    "1.3.0"
    "${transition_version}"
)

file(APPEND "${repository}/NOTES" "dirty\n")
expect_checkout("${repository}" "1.3.0" "${transition_version}-dirty")
expect_commit(
    "${repository}"
    "${transition_commit}"
    "1.3.0"
    "${transition_version}"
)
run_git(checkout -- NOTES)

set(shallow_repository "${EXFAT_RESIZE_VERSION_TEST_DIR}/shallow-repository")
execute_process(
    COMMAND
        "${GIT_EXECUTABLE}" clone --quiet --no-local --depth 1
        "${repository}" "${shallow_repository}"
    RESULT_VARIABLE clone_result
    OUTPUT_VARIABLE clone_output
    ERROR_VARIABLE clone_error
)
if(NOT clone_result EQUAL 0)
    message(FATAL_ERROR "Could not create shallow clone: ${clone_output}${clone_error}")
endif()
if(NOT EXISTS "${shallow_repository}/.git/shallow")
    message(FATAL_ERROR "Version test clone is not shallow")
endif()
read_head("${shallow_repository}" shallow_commit)
if(NOT shallow_commit STREQUAL transition_commit)
    message(FATAL_ERROR "Full and shallow version tests use different commits")
endif()

expect_checkout("${shallow_repository}" "1.3.0" "${transition_version}")
expect_commit(
    "${shallow_repository}"
    "${transition_commit}"
    "1.3.0"
    "${transition_version}"
)
file(APPEND "${shallow_repository}/NOTES" "dirty\n")
expect_checkout("${shallow_repository}" "1.3.0" "${transition_version}-dirty")
expect_commit(
    "${shallow_repository}"
    "${transition_commit}"
    "1.3.0"
    "${transition_version}"
)

run_git(tag v1.3.0 "${transition_commit}")
expect_checkout("${repository}" "1.3.0" "1.3.0")
expect_commit("${repository}" "${transition_commit}" "1.3.0" "1.3.0")
file(APPEND "${repository}/NOTES" "dirty\n")
expect_checkout("${repository}" "1.3.0" "1.3.0-dirty")
expect_commit("${repository}" "${transition_commit}" "1.3.0" "1.3.0")
run_git(checkout -- NOTES)

file(WRITE "${repository}/change" "mismatch\n")
run_git(add change)
run_git(commit --quiet -m "Mismatching tag")
read_head("${repository}" mismatching_commit)
run_git(tag v2.0.0)

expect_tag_rejected(checkout "${mismatching_commit}")
expect_tag_rejected(commit "${mismatching_commit}")

file(REMOVE_RECURSE "${EXFAT_RESIZE_VERSION_TEST_DIR}")
message(STATUS "version: passed")
