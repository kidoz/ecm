# Default recipe
default: build

# Build directory
build_dir := "build"

# Setup meson build directory
setup:
    meson setup {{build_dir}}

# Setup with debug build type
setup-debug:
    meson setup {{build_dir}} --buildtype=debug

# Setup with release build type
setup-release:
    meson setup {{build_dir}} --buildtype=release

# Build the project
build: setup
    meson compile -C {{build_dir}}

# Rebuild from scratch
rebuild: clean setup build

# Clean build artifacts
clean:
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
wipe:
    meson setup {{build_dir}} --wipe

# Format source files (requires clang-format)
fmt:
    clang-format -i src/*.c include/*.h tests/*.c

# Check formatting without modifying
fmt-check:
    clang-format --dry-run --Werror src/*.c include/*.h tests/*.c

# Run clang static analyzer
analyze:
    clang --analyze -std=c2x -Xanalyzer -analyzer-output=text -I include -I {{build_dir}} src/ecm.c src/unecm.c

# Setup with Address Sanitizer
setup-asan:
    meson setup {{build_dir}} --buildtype=debug -Db_sanitize=address

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
