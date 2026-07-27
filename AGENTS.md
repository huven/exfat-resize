# Agent Instructions

After making any code changes to `.c`, `.cpp`, or `.h` files, run
`clang-format -i` on every changed C/C++ source or header file before
finishing.

Use `clang-format` from `PATH` when available. On macOS, if it is not in
`PATH`, use:

`/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang-format`
