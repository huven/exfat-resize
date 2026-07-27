# Contributing

Build and test instructions are in the
[README](README.md#build-and-test). Run the relevant tests before submitting a
change.

## C identifier namespaces

The project owns the `exfat_resize_` and `EXFAT_RESIZE_` namespaces.

Functions and objects used by only one translation unit must be `static`.
Cross-file internal functions and objects must use the `exfat_resize_` prefix
and be declared only in private headers. They are not public API unless
documented by `include/exfat_resize.h`.
