# Dependencies & toolchain
#
# Compiler:   requires a C++23-capable compiler (g++ 13+, clang 16+, or MSVC 19.36+)
#             The Makefile auto-detects the available compiler.
# Build:      `make`            produces build/atomdb (REPL executable) + object files
#             `make run`        builds and launches the REPL
#             `make clean`      wipes build/
# Tests:      Unit tests live under tests/ and run with `make test`
#             (a single-file test runner, no external test framework dependency).
#             The runner compiles to build/test_runner.
#
# No external (third-party) runtime libraries are required for v0.1.
# Standard library only.
