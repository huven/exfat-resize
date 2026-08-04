# SPDX-License-Identifier: MIT

# Each component provides its own Debian synopsis. CPack otherwise prepends its
# global project summary to both component descriptions.
unset(CPACK_PACKAGE_DESCRIPTION_SUMMARY)
