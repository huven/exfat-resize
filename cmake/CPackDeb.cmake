# SPDX-License-Identifier: MIT

if(NOT EXFAT_RESIZE_BUILD_CLI)
    message(FATAL_ERROR "Debian packaging requires EXFAT_RESIZE_BUILD_CLI=ON")
endif()

set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME exfat-resize)
set(CPACK_PACKAGE_VENDOR "exfat-resize contributors")
set(CPACK_PACKAGE_CONTACT "Richard Huveneers <richard@huveneers.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/huven/exfat-resize")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_STRIP_FILES ON)
set(CPACK_PROJECT_CONFIG_FILE
    "${CMAKE_CURRENT_LIST_DIR}/CPackDebProjectConfig.cmake"
)

set(CPACK_COMPONENTS_ALL Runtime Development)
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_GROUPING IGNORE)

set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_RELEASE 1)
set(CPACK_DEBIAN_PACKAGE_PRIORITY optional)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
set(CPACK_DEBIAN_PACKAGE_SOURCE exfat-resize)
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)

set(CPACK_DEBIAN_RUNTIME_PACKAGE_NAME exfat-resize)
set(CPACK_DEBIAN_RUNTIME_PACKAGE_SECTION utils)
set(CPACK_DEBIAN_RUNTIME_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_RUNTIME_PACKAGE_RECOMMENDS exfatprogs)
set(CPACK_DEBIAN_RUNTIME_DESCRIPTION
    "Grow existing exFAT filesystems
 exfat-resize grows an existing exFAT filesystem in a regular file or raw
 block device after its backing object has already been enlarged."
)

set(CPACK_DEBIAN_DEVELOPMENT_PACKAGE_NAME libexfat-resize-dev)
set(CPACK_DEBIAN_DEVELOPMENT_PACKAGE_SECTION libdevel)
set(CPACK_DEBIAN_DEVELOPMENT_PACKAGE_DEPENDS libc6-dev)
set(CPACK_DEBIAN_DEVELOPMENT_DESCRIPTION
    "Development files for exfat-resize
 This package contains the static library, public C header, and CMake package
 files needed to develop software using libexfat-resize."
)

string(
    TIMESTAMP
    EXFAT_RESIZE_DEB_CHANGELOG_DATE
    "%a, %d %b %Y %H:%M:%S +0000"
    UTC
)
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/debian-changelog.in"
    "${CMAKE_CURRENT_BINARY_DIR}/changelog.Debian"
    @ONLY
)
find_program(GZIP_EXECUTABLE gzip REQUIRED)
execute_process(
    COMMAND
        ${GZIP_EXECUTABLE} -9 -n -c
        ${CMAKE_CURRENT_BINARY_DIR}/changelog.Debian
    OUTPUT_FILE
        ${CMAKE_CURRENT_BINARY_DIR}/changelog.Debian.gz
    COMMAND_ERROR_IS_FATAL
        ANY
)
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/changelog.Debian.gz
    DESTINATION
        ${CMAKE_INSTALL_DOCDIR}
    COMPONENT
        Runtime
)
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/changelog.Debian.gz
    DESTINATION
        ${CMAKE_INSTALL_DATAROOTDIR}/doc/libexfat-resize-dev
    COMPONENT
        Development
)

include(CPack)
