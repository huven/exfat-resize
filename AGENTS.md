# Agent Instructions

After making any code changes to `.c`, `.cpp`, or `.h` files, run
`clang-format -i` on every changed C/C++ source or header file before
finishing.

Use `clang-format` from `PATH` when available. On macOS, if it is not in
`PATH`, use:

`/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang-format`

After making changes to `.sh` files, format them with the same 100-character
line limit configured in `.clang-format`. Keep commands and conditions on one
line when they fit within that limit, and wrap them only when necessary. Apply
the same rule to shell code in YAML `run` blocks, counting the YAML indentation
toward the line limit.
