# Install ECM

This guide covers building ECM from source on Linux, macOS, and Windows, and installing
it with the Homebrew and Arch Linux packages.

## Build from source

### Requirements

- A C23 compatible compiler: GCC 14+, Clang 18+, or Visual Studio 2022+ (through the
  native files described under [Windows](#windows))
- [Meson](https://mesonbuild.com/) 1.3 or newer
- [Ninja](https://ninja-build.org/) (recommended)
- Python 3 for the integration test only; Meson skips that test when no interpreter is
  found

### Build, test, and install

```bash
meson setup build
meson compile -C build
meson test -C build
sudo meson install -C build   # optional
```

Warnings are not errors in a plain `meson setup`, so a newer compiler cannot break a
release build. Add `-Dwerror=true` when developing; the `just` recipes and CI do.

The same steps through the [`just`](https://github.com/casey/just) command runner:

```bash
just build      # Configure and build a release
just test       # Build, then run the unit, benchmark, and integration tests
just install    # Build, then meson install
just benchmark  # Build, then run the performance benchmarks
```

`just clean`, `just rebuild`, and `just wipe` refuse to delete a build directory that
holds disc images or archives, since those are not build output.

### Verify the installation

```console
$ ecm --version
ecm 1.3.4
```

## Windows

Native files under `native/` configure a static-CRT release build, so the resulting
`ecm.exe` and `unecm.exe` need no VC++ redistributable.

With the Visual Studio compiler, from a *Developer Command Prompt for VS* (or after
running `vcvars64.bat`):

```bat
meson setup build --native-file native/windows-msvc.ini
meson compile -C build
```

With clang (LLVM for Windows):

```bat
meson setup build --native-file native/windows-clang.ini
meson compile -C build
```

The equivalent `just` recipes are `just build-windows` (MSVC, in `build-msvc`) and
`just build-windows-clang` (in `build-clang`), with `test-windows` and
`test-windows-clang` to run the tests. MSVC has no `nullptr` keyword in C mode yet, so
the MSVC native file selects `/std:clatest` and maps `nullptr` to `NULL`.

The Windows executables embed a manifest that makes UTF-8 their code page, so file names
in any script work on Windows 10 version 1903 and later. The integration test runs on
Python 3, as on Unix; no bash or Git Bash is needed.

## Homebrew (macOS)

From a checkout of this repository, enable Homebrew developer mode to install the local
formula:

```bash
HOMEBREW_DEVELOPER=1 brew install --formula ./packaging/homebrew/ecm.rb
```

## Arch Linux

```bash
cd packaging/archlinux
makepkg -si
```
