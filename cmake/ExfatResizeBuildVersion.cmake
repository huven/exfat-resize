# SPDX-License-Identifier: MIT

include("${CMAKE_CURRENT_LIST_DIR}/ExfatResizeVersion.cmake")
exfat_resize_read_package_version("${EXFAT_RESIZE_VERSION_SOURCE_DIR}" package_version)
exfat_resize_resolve_checkout_build_version(
    "${EXFAT_RESIZE_VERSION_SOURCE_DIR}"
    "${package_version}"
    EXFAT_RESIZE_BUILD_VERSION
)

# These outputs change only when their contents do, avoiding unnecessary rebuilds.
configure_file(
    "${EXFAT_RESIZE_VERSION_SOURCE_DIR}/src/build_version.h.in"
    "${EXFAT_RESIZE_VERSION_BINARY_DIR}/exfat_resize_build_version.h"
    @ONLY
)
file(CONFIGURE
    OUTPUT "${EXFAT_RESIZE_VERSION_BINARY_DIR}/exfat_resize_build_version.cmake"
    CONTENT "set(EXFAT_RESIZE_BUILD_VERSION \"@EXFAT_RESIZE_BUILD_VERSION@\")\n"
    @ONLY
)
if(EXFAT_RESIZE_INSTALL_MANPAGE)
    configure_file(
        "${EXFAT_RESIZE_VERSION_SOURCE_DIR}/docs/exfat-resize.8.in"
        "${EXFAT_RESIZE_VERSION_BINARY_DIR}/exfat-resize.8"
        @ONLY
    )
endif()
