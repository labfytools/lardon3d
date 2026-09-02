# Build

## Status

```text
BUILD_SYSTEM=MESON_NINJA
PUBLIC_API_LANGUAGE=C17
IMPLEMENTATION_LANGUAGES=C17_CXX17
BUILD_PARALLELISM=HOST_AWARE
RESOURCE_UTILIZATION_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF=CANONICAL
```

Meson is the build-system authority. Do not duplicate dependency-version truth
in this document when `meson.build` already enforces it.

## Requirements

Lardon3D targets Linux.

Primary toolchain:

```text
Clang or GCC
Meson
Ninja
pkg-config
```

Major dependencies currently include ncursesw, SQLite, OpenSSL, GIO/GLib,
OpenCV, LibRaw, libexif, libpng, libdeflate and Ceres. Vulkan remains optional
at configuration level.

Public APIs are C17. Implementation is mixed C17/C++17.

## Bootstrap examples

These commands install only the basic compiler/build front end; Meson remains
authoritative for the complete dependency set.

Debian/Ubuntu:

```sh
sudo apt install clang meson ninja-build libncursesw5-dev pkg-config
```

Fedora:

```sh
sudo dnf install clang meson ninja-build ncurses-devel pkg-config
```

Arch Linux:

```sh
sudo pacman -S clang meson ninja ncurses pkgconf
```

## Standard build

First configuration:

```sh
CC=clang CXX=clang++ meson setup build
```

Existing tree:

```sh
meson setup --reconfigure build
meson compile -C build
```

Do not hard-code `-j8` as project policy.

Meson/Ninja should use host-appropriate parallelism unless a specific
validation has a reason to constrain it.

## Build parallelism policy

The build is not governed by a portable fixed job count.

Canonical policy:

```text
preserve the interactive host reserve
then use maximum safe useful throughput
```

A reference host measurement such as 8 or 12 useful jobs is evidence for that
host at that time, not a repository constant.

If memory-heavy compilation or another active workload creates pressure,
reduce build width for that run. Do not convert the temporary reduction into a
global documentation rule.

## Reconfigure versus wipe

Prefer incremental reuse:

```sh
meson setup --reconfigure build
meson compile -C build
```

Use `--wipe` only when a fresh configuration is actually required, such as:

- switching sanitizer configuration in the same directory;
- changing compiler family;
- changing a configuration whose cached state cannot be reused safely;
- reproducing a clean release/global-maintenance proof;
- recovering from a stale or corrupt build directory.

A normal edit/test loop should not wipe the build tree repeatedly.

## Release build

Use an explicit release directory or deliberate reconfiguration.

Example:

```sh
CC=clang CXX=clang++ meson setup build-release --buildtype=release
meson compile -C build-release
```

For LTO:

```sh
CC=clang CXX=clang++ meson setup build-release-lto --buildtype=release -Db_lto=true
meson compile -C build-release-lto
```

Separate directories avoid destroying a useful incremental debug tree.

## Vulkan configuration

Portable CPU-only proof:

```sh
CC=clang CXX=clang++ meson setup build-portable -Dvulkan_orb=disabled
meson compile -C build-portable
```

Vulkan-enabled proof:

```sh
CC=clang CXX=clang++ meson setup build-vulkan -Dvulkan_orb=enabled
meson compile -C build-vulkan
```

A Vulkan-on build is not automatically a proof that every scientific path uses
or should use the GPU.

Current production GPU promotion remains limited by each subsystem's validated
backend contract.

## Validation

Normal configured tests:

```sh
meson test -C build --print-errorlogs
```

Whitespace/style boundary:

```sh
git diff --check
```

Public C header probe:

```sh
cc -x c -std=c17 -fsyntax-only -Iinclude -include lardon3d/<header>.h /dev/null
```

Use `docs/development/testing.md` for sanitizer and validation policy.

## ASan / UBSan build

Example dedicated directory:

```sh
CC=clang CXX=clang++ meson setup build-asan -Db_sanitize=address,undefined
meson compile -C build-asan
meson test -C build-asan --print-errorlogs
```

Do not claim an unqualified full LeakSanitizer pass from the retained global
maintenance checkpoint. The external OpenCL loader qualification documented in
the canonical audit remains part of that evidence.

## TSan build

Use TSan only with the configuration that matches the intended proof.

The retained global maintenance concurrency proof used GCC/G++ with Vulkan
disabled, because the project TSan matrix and the Vulkan runtime validation are
separate evidence boundaries.

Example:

```sh
CC=gcc CXX=g++ meson setup build-tsan -Db_sanitize=thread -Db_lundef=false -Dvulkan_orb=disabled
meson compile -C build-tsan
meson test -C build-tsan --print-errorlogs
```

The exact target subset, suppression qualification and repetition evidence are
documented in `docs/development/concurrency.md` and the global maintenance
audit.

## Current retained maintenance checkpoint

The canonical detailed evidence is:

```text
docs/architecture/global_maintenance_audit.md
GLOBAL_MAINTENANCE_AUDIT=PASS/FROZEN
```

The 2026-09-01 checkpoint retained:

```text
portable Clang/Clang++ build and suite
Vulkan-on Clang/Clang++ build and suite
ASan/UBSan qualified run
portable GCC/G++ TSan matrix
public-header C17/C++17 probes
ABI and application-link checks
independent review
```

Those exact historical counts belong to the audit and should not be duplicated
as a new current build contract.

## Build directory layout

A configured Meson tree typically contains:

```text
build/
  src/
  tests/
  compile_commands.json
```

Exact generated layout is Meson/Ninja output and may evolve.

## Environment

Common variables include:

| Variable | Purpose |
| --- | --- |
| `CC` | C compiler |
| `CXX` | C++ compiler |
| `CFLAGS` | additional C flags |
| `CXXFLAGS` | additional C++ flags |
| `LDFLAGS` | additional linker flags |

Prefer Meson options for project features rather than ad-hoc environment flags
that make builds difficult to reproduce.

## Troubleshooting

Check ncursesw discovery:

```sh
pkg-config --libs ncursesw
```

If Clang is unavailable, GCC is supported where the current Meson checks allow
it.

For a slow build, first preserve the existing build tree and let Ninja use
normal host-aware scheduling. Reduce concurrency only when actual host pressure
or another active workload justifies it.

`ccache` may be used when available, but it is optional operational tooling and
not part of scientific identity.

## Rules

```text
NO_FIXED_GLOBAL_J8=YES
NO_REPEATED_UNCHANGED_WIPE=YES
HOST_AWARE_BUILD_PARALLELISM=YES
```

Build configuration is operational state. It must not silently redefine
scientific formats, fingerprints or persistence contracts.
