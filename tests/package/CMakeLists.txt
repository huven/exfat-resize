# SPDX-License-Identifier: MIT

configure_file(
    install_test.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/install_test.cmake
    @ONLY
)

add_test(
    NAME package.install-documentation
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/install_test.cmake
)
set_tests_properties(package.install-documentation PROPERTIES LABELS package)

add_test(
    NAME package.version
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/version.sh
)
set_tests_properties(package.version PROPERTIES LABELS package)
