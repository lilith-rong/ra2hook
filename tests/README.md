# Ordered startup patch tests

The C++20 tests compile the production `IniPatch.cpp` planner and
`StartupPatch.cpp` adapter. No game, YRpp checkout, disk fixtures, network,
external test framework or new software installation is required.

**This revision was not compiled or executed locally.** Per project workflow,
GitHub Actions builds/runs these tests before the Win32 DLL build. The previous
27 removal-only test groups were for the old two-layer implementation and do
not establish correctness of this ordered version.

## GitHub Actions / optional existing toolchain

CI performs:

```sh
cmake -S tests -B patch-tests -A Win32
cmake --build patch-tests --config Release
ctest --test-dir patch-tests -C Release --output-on-failure
```

For someone independently using an already-installed non-Visual-Studio
C++20 toolchain, omit `-A Win32` and choose a local output directory. No local
build is needed for submitting this change to Actions.

## Coverage

`IniPatchTests.cpp` contains 16 groups covering:

- Mixed, repeated set/append/remove instructions in line and caller-root order.
- Includes expanded at their own line, parent resumption, isolated section state,
  repeated references, relative/fallback resolution and quoted paths.
- Exact special `+`/`-` keys; empty assignments, ordinary values and `$Inherits`
  preserved without implementing extension-specific inheritance.
- UTF-8 BOM, ANSI/GBK bytes, fullwidth whitespace/comments, CRLF/CR/LF.
- Malformed syntax, unsafe removal targets, include errors, NUL/UTF-16 rejection,
  source/line diagnostics, registry writes versus forbidden registry deletion.
- Removal opt-in (safe default off), empty inputs, stale output clearing,
  all-roots preflight failures, recovery, path normalization and include cycles.
- Depth, root count, path/token length, file count, mixed command count,
  per-file byte and aggregate byte limits (including exact boundaries).

`StartupPatchTests.cpp` contains 12 groups compiling the real adapter against
`stubs/`, covering:

- Native invocation order, delete then restore, stored-case names for both
  WriteString and Clear, exact explicit deletion and empty-value behavior.
- Sorted roots, child overrides followed by parent/later-root overrides.
- Repeated `+=`, fresh `RA2Hook_N` keys, explicit generated-key collisions,
  case-insensitive numeric prefixes, namespace exhaustion.
- No partial writes/appends/deletes on a later root/include validation failure.
- Rules-only deletion, registry additions, absent-key no-ops and surviving empty
  sections, no fallback to a cached CurrentSection for absent targets.
- Source resolution priority, missing/unreadable/short/oversized files, cycles,
  empty/failed directory scans, null target handling and invalid prepared plans.
- Sound-style preparation followed by memory-only application after source
  files become unavailable.
- Failed native erase/write stops execution without claiming rollback.

The fake engine checks call arguments and visible behavior, not memory layout,
Ares/Phobos coexistence or actual native game code. Real Win32 DLL compilation
and full-restart game/Dump checks remain necessary. Runtime still uses the
unchanged `IniOverlay` algorithm; these tests do not simulate the Runtime UI,
watcher or game-thread reload machinery.

All assertions remain active in Release/NDEBUG. Logging is disabled in adapter
tests so the tests do not create game log directories or files.
