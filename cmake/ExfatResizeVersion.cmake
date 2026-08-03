# SPDX-License-Identifier: MIT

include_guard(GLOBAL)

# Require the numeric package version format accepted by project(VERSION).
function(_exfat_resize_validate_package_version value)
    if(NOT value MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR "Invalid package version: '${value}'")
    endif()
endfunction()

# Limit build identities to characters safe for CLI output and archive names.
function(_exfat_resize_validate_build_version value)
    if(NOT value MATCHES "^[0-9A-Za-z][0-9A-Za-z.+-]*$")
        message(FATAL_ERROR "Invalid build version: '${value}'")
    endif()
endfunction()

# Reject an exact version tag when it disagrees with the committed VERSION.
function(_exfat_resize_validate_exact_tag exact_tag package_version)
    if(exact_tag AND NOT exact_tag STREQUAL "v${package_version}")
        message(FATAL_ERROR
            "Tag ${exact_tag} does not match package version ${package_version}"
        )
    endif()
endfunction()

# Convert git describe output into the build identity shown by the CLI.
function(_exfat_resize_format_description description package_version output_variable)
    if(description MATCHES "^v")
        string(SUBSTRING "${description}" 1 -1 build_version)
    else()
        set(build_version "${package_version}-g${description}")
    endif()

    set(${output_variable} "${build_version}" PARENT_SCOPE)
endfunction()

# Describe either the working tree or a specific commit, validating exact tags.
function(_exfat_resize_describe_git source_dir package_version commit output_variable)
    set(commit_argument)
    if(commit)
        list(APPEND commit_argument "${commit}")
    endif()

    execute_process(
        COMMAND
            "${GIT_EXECUTABLE}" -C "${source_dir}" describe
            --exact-match --tags --match "v[0-9]*" ${commit_argument}
        RESULT_VARIABLE exact_tag_result
        OUTPUT_VARIABLE exact_tag
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(exact_tag_result EQUAL 0)
        _exfat_resize_validate_exact_tag("${exact_tag}" "${package_version}")
    endif()

    set(dirty_argument)
    if(NOT commit)
        list(APPEND dirty_argument --dirty)
    endif()
    execute_process(
        COMMAND
            "${GIT_EXECUTABLE}" -C "${source_dir}" describe
            --tags --match "v[0-9]*" --always ${dirty_argument} ${commit_argument}
        RESULT_VARIABLE description_result
        OUTPUT_VARIABLE description
        ERROR_VARIABLE description_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT description_result EQUAL 0)
        message(FATAL_ERROR "Could not describe Git source: ${description_error}")
    endif()

    _exfat_resize_format_description(
        "${description}"
        "${package_version}"
        build_version
    )
    _exfat_resize_validate_build_version("${build_version}")
    set(${output_variable} "${build_version}" PARENT_SCOPE)
endfunction()

# Read the source-tree VERSION used for CMake package compatibility.
function(exfat_resize_read_package_version source_dir output_variable)
    set(version_file "${source_dir}/VERSION")
    if(NOT EXISTS "${version_file}")
        message(FATAL_ERROR "VERSION not found in ${source_dir}")
    endif()

    file(READ "${version_file}" package_version)
    string(STRIP "${package_version}" package_version)
    _exfat_resize_validate_package_version("${package_version}")

    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${version_file}")
    endif()
    set(${output_variable} "${package_version}" PARENT_SCOPE)
endfunction()

# Prefer an archived identity, then Git metadata, then an unknown suffix.
function(exfat_resize_resolve_checkout_build_version
         source_dir package_version output_variable)
    set(tarball_version_file "${source_dir}/.tarball-version")
    if(EXISTS "${tarball_version_file}")
        file(READ "${tarball_version_file}" build_version)
        string(STRIP "${build_version}" build_version)
        _exfat_resize_validate_build_version("${build_version}")
    elseif(EXISTS "${source_dir}/.git")
        find_package(Git QUIET)
        if(GIT_FOUND)
            _exfat_resize_describe_git(
                "${source_dir}"
                "${package_version}"
                ""
                build_version
            )
        else()
            set(build_version "${package_version}-unknown")
        endif()
    else()
        set(build_version "${package_version}-unknown")
    endif()

    set(${output_variable} "${build_version}" PARENT_SCOPE)
endfunction()

# Resolve both identities from the exact commit packaged by make dist.
function(exfat_resize_resolve_commit_version
         source_dir commit package_version_variable build_version_variable)
    find_package(Git QUIET)
    if(NOT GIT_FOUND)
        message(FATAL_ERROR "Git is required to determine a committed version")
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${source_dir}" show "${commit}:VERSION"
        RESULT_VARIABLE package_version_result
        OUTPUT_VARIABLE package_version
        ERROR_VARIABLE package_version_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT package_version_result EQUAL 0)
        message(FATAL_ERROR
            "VERSION not found in commit ${commit}: ${package_version_error}"
        )
    endif()
    _exfat_resize_validate_package_version("${package_version}")

    _exfat_resize_describe_git(
        "${source_dir}"
        "${package_version}"
        "${commit}"
        build_version
    )
    set(${package_version_variable} "${package_version}" PARENT_SCOPE)
    set(${build_version_variable} "${build_version}" PARENT_SCOPE)
endfunction()
