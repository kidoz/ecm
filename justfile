# Default recipe
default: build

# Build directory
build_dir := "build"

# Windows toolchains get their own directories: `meson setup` keeps an existing configuration
# and silently ignores a new native file, so sharing one directory could build with the wrong
# compiler
msvc_build_dir := "build-msvc"
clang_build_dir := "build-clang"

# Development builds treat warnings as errors; a plain `meson setup` does not
dev_options := "-Dwerror=true"

# Setup meson build directory
setup:
    meson setup {{build_dir}} {{dev_options}}

# Setup with debug build type
setup-debug:
    meson setup {{build_dir}} --buildtype=debug {{dev_options}}

# Setup with release build type
setup-release:
    meson setup {{build_dir}} --buildtype=release {{dev_options}}

# Build the project
build: setup
    meson compile -C {{build_dir}}

# Run the unit, benchmark, and integration tests
test: build
    meson test -C {{build_dir}} --print-errorlogs

# Rebuild from scratch
rebuild: clean setup build

# Refuse to delete a build directory that holds disc images or archives, which are not build
# output and cannot be regenerated
_guard-build-dir dir:
    @for f in "{{dir}}"/*.[bB][iI][nN] "{{dir}}"/*.[iI][sS][oO] "{{dir}}"/*.[iI][mM][gG] "{{dir}}"/*.[eE][cC][mM] "{{dir}}"/*.[cC][uU][eE] "{{dir}}"/*.[bB][aA][kK]; do if [ -e "$f" ]; then echo "Refusing to delete {{dir}}: it holds files that are not build output, such as $f" >&2; exit 1; fi; done

# Clean build artifacts
clean: (_guard-build-dir build_dir)
    rm -rf {{build_dir}}

# Install binaries
install: build
    meson install -C {{build_dir}}

# Run ecm encoder
run-ecm *args:
    {{build_dir}}/ecm {{args}}

# Run unecm decoder
run-unecm *args:
    {{build_dir}}/unecm {{args}}

# Show project info
info:
    meson introspect {{build_dir}} --projectinfo

# Show all targets
targets:
    meson introspect {{build_dir}} --targets

# Configure with different C standard (e.g., just configure-std c11)
configure-std std:
    meson configure {{build_dir}} -Dc_std={{std}}

# Reconfigure existing build
reconfigure:
    meson setup {{build_dir}} --reconfigure

# Wipe and reconfigure
wipe: (_guard-build-dir build_dir)
    meson setup {{build_dir}} --wipe

# Format source files (requires clang-format)
fmt:
    clang-format -i src/*.c include/*.h tests/*.c tests/*.h

# Check formatting without modifying
fmt-check:
    clang-format --dry-run --Werror src/*.c include/*.h tests/*.c tests/*.h

# Run clang static analyzer
analyze:
    clang --analyze -std=c2x -Xanalyzer -analyzer-output=text -I include -I {{build_dir}} src/ecm.c src/unecm.c

# Setup with Address Sanitizer
setup-asan:
    meson setup {{build_dir}} --buildtype=debug -Db_sanitize=address {{dev_options}}

# Run ecm with Address Sanitizer (build with setup-asan first)
asan-ecm *args:
    {{build_dir}}/ecm {{args}}

# Run unecm with Address Sanitizer (build with setup-asan first)
asan-unecm *args:
    {{build_dir}}/unecm {{args}}

# Run valgrind memory check on ecm (Linux only)
valgrind-ecm *args:
    valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes {{build_dir}}/ecm {{args}}

# Run valgrind memory check on unecm (Linux only)
valgrind-unecm *args:
    valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes {{build_dir}}/unecm {{args}}

# Run performance benchmarks
benchmark: build
    meson test -C {{build_dir}} perf_benchmark --verbose

# Windows: configure a release build with MSVC in build-msvc (run from a Developer Command Prompt or after vcvars64.bat)
setup-windows-msvc:
    meson setup {{msvc_build_dir}} --native-file native/windows-msvc.ini {{dev_options}}

# Windows: configure a release build with clang in build-clang
setup-windows-clang:
    meson setup {{clang_build_dir}} --native-file native/windows-clang.ini {{dev_options}}

# Windows: build with the Visual Studio compiler
build-windows: setup-windows-msvc
    meson compile -C {{msvc_build_dir}}

# Windows: build with clang
build-windows-clang: setup-windows-clang
    meson compile -C {{clang_build_dir}}

# Windows: test the Visual Studio build
test-windows: build-windows
    meson test -C {{msvc_build_dir}} --print-errorlogs

# Windows: test the clang build
test-windows-clang: build-windows-clang
    meson test -C {{clang_build_dir}} --print-errorlogs
