# Testing

## Status

```text
DOCUMENTATION_LANGUAGE=ENGLISH
TEST_POLICY=HOST_AWARE
REPEATED_UNCHANGED_EXPENSIVE_VALIDATION=AVOID
TSAN_OPEN_CV_TBB_QUALIFICATION=REQUIRED
VULKAN_CONCURRENCY_VALIDATION=SEPARATE
```

Lardon3D uses Meson's test runner. Tests live under `tests/` and combine unit,
integration, persistence, restart, resource and real-path validation.

## Normal commands

Run the configured suite:

```sh
meson test -C build --print-errorlogs
```

Run one named test:

```sh
meson test -C build <test-name> --print-errorlogs
```

Verbose execution:

```sh
meson test -C build -v --print-errorlogs
```

Re-run failures only:

```sh
meson test -C build --reprint=failed
```

Use the names registered by the current `meson.build`; this document does not
maintain a second authoritative list of every test target.

## Validation policy

Validation must match the change.

A documentation-only change normally requires:

```text
git diff --check
targeted content checks
targeted link/authority review
```

It does not justify wiping and rebuilding unchanged code.

A code change normally requires, in increasing scope:

```text
targeted build
targeted tests
broader affected suite
sanitizer or concurrency validation when relevant
full suite when the change or release boundary justifies it
```

Do not repeatedly rerun an unchanged expensive suite between documentation
edits merely to create activity.

## Host-aware parallelism

Build and test parallelism are host-aware.

Do not encode a project-wide fixed `-j8`, `--num-processes 1`, or equivalent
constant as canonical policy.

The correct width depends on the current machine, interactive reserve, memory,
toolchain and workload. Use all safe useful host capacity while preserving the
defined interactive reserve.

```text
RESOURCE_UTILIZATION_POLICY=MAXIMUM_SAFE_USEFUL_THROUGHPUT
SERIALISM_REQUIRES_PROOF=CANONICAL
```

If a temporary validation must be serialized for determinism, diagnosis or a
known tool limitation, label that serialization as test-specific evidence
rather than a global default.

## Fresh build policy

Do not use `meson setup --wipe` by default.

Prefer:

```sh
meson setup --reconfigure build
meson compile -C build
```

Create or wipe a build directory when the configuration genuinely needs a
fresh environment, for example:

```text
different sanitizer set
portable Vulkan-off proof
Vulkan-on proof
compiler-family change
known stale/corrupt build directory
release-grade clean proof
```

Repeated wipes of the same unchanged configuration waste time and invalidate
incremental-build advantages.

## Sanitizers

### ASan / UBSan

For memory, lifetime, ownership or undefined-behavior changes, use a dedicated
sanitizer build.

Example configuration:

```sh
CC=clang CXX=clang++ meson setup build-asan -Db_sanitize=address,undefined
meson compile -C build-asan
meson test -C build-asan --print-errorlogs
```

Reconfigure or wipe only when the existing sanitizer directory does not match
the requested configuration.

### LeakSanitizer qualification

The retained global maintenance evidence must not be summarized as
`LSan 64/64`.

The full first leak-enabled run exposed an externally attributed OpenCL loader
leak and two timeout anomalies. The retained qualified result is:

```text
ASan/UBSan full suite: PASS with detect_leaks=0
proved subset without the external loader: LSan PASS
full leak-enabled suite: not an unqualified PASS
```

Preserve that distinction in future reports unless new evidence supersedes it.

## ThreadSanitizer

Concurrency changes require TSan where the instrumented boundary is meaningful.

The retained global maintenance TSan proof used GCC/G++ with Vulkan disabled
and covered the selected concurrent targets plus deterministic repetitions.

The only retained suppression file is:

```text
tests/tsan-opencv.supp
```

Its purpose is limited to external OpenCV/TBB objects. It must not suppress
Lardon3D frames.

Therefore never report a blanket statement such as:

```text
TSan proves the entire Vulkan build race-free
```

The valid qualification is:

```text
project concurrent paths covered by the retained portable TSan matrix
external OpenCV/TBB reports qualified by the narrow suppression boundary
Vulkan concurrency validated separately
```

## Vulkan validation

ORB Vulkan uses a separate validation boundary.

The retained global maintenance evidence includes a Vulkan-on build and suite
on the real Radeon 780M/RADV host, plus dedicated backend/handle/publication
tests.

That evidence is not interchangeable with TSan.

SIFT/RootSIFT feasibility results did not establish a production GPU backend;
do not turn feasibility checks into production validation claims.

## Determinism and repetition

Repeat tests when repetition proves something specific:

```text
deterministic restart
race sensitivity
resource adaptation
ordering stability
flaky regression reproduction
```

Do not repeat unchanged tests without a stated purpose.

When repetition is the evidence, record:

```text
exact test/corpus
run count
relevant configuration
success/failure count
digest or invariant when applicable
```

## Test isolation

Tests should:

- own and clean up their temporary resources;
- avoid depending on another test's execution order;
- avoid network state unless the test explicitly owns that dependency;
- use synthetic/private Resource snapshots where the test is about deterministic
  policy rather than live host telemetry;
- avoid changing global process state without restoring it.

OpenCV thread configuration is process-wide and must be restored on every exit
path in tests that change it.

## Public-header validation

When a public C header changes, run a standalone C17 syntax probe in addition to
normal build coverage.

Conceptually:

```sh
cc -x c -std=c17 -fsyntax-only -Iinclude -include lardon3d/<header>.h /dev/null
```

Use the current supported compiler matrix when the change affects ABI or
C/C++ interoperability.

## Source comments

Source comments explain non-obvious contracts:

```text
invariants
ownership and lifetime
persistence ordering
resource boundaries
recovery behavior
scientific constraints
```

They should not paraphrase obvious code line by line.

Repository source comments are English.

## Current retained global maintenance evidence

The canonical detailed record is:

```text
docs/architecture/global_maintenance_audit.md
GLOBAL_MAINTENANCE_AUDIT=PASS/FROZEN
```

That historical checkpoint includes fresh portable/Vulkan builds, full suites,
sanitizer work, TSan work, public-header probes, ABI/application-link checks and
independent review.

It remains historical evidence. New changes require only the validation
appropriate to the changed surface unless a new global checkpoint is being
created.

## Ticket closure checklist

Before closing a code ticket:

- confirm the requested scope only was changed;
- run `git diff --check`;
- run targeted tests for changed behavior;
- run the affected broader suite when justified;
- run ASan/UBSan for memory/lifetime-sensitive changes;
- run TSan for concurrency-sensitive project code when applicable;
- keep Vulkan validation separate from portable TSan claims;
- preserve exact external-library qualifications;
- avoid fixed host-parallelism constants;
- avoid repeated unchanged clean builds or suites;
- report what actually ran, not a stronger claim.

For documentation-only remediation, use documentation checks rather than
rebuilding unchanged production code.
