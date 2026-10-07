# Portable removal tests

These dependency-free C++20 tests compile the production parser and adapter
against in-memory input and small fake engine/file APIs. They require neither
the game nor YRpp, disk fixtures, network access, or a test framework. Parser
tests use a Reader that deliberately does not canonicalize returned paths.

## CMake / CTest (Linux or Windows MSVC)

From the repository root, with a C++20 compiler and CMake installed:

```sh
cmake -S tests -B tests/build -DCMAKE_BUILD_TYPE=Release
cmake --build tests/build --config Release --parallel
ctest --test-dir tests/build -C Release --output-on-failure
```

On Windows, run from a Visual Studio developer shell with the C++ build tools
installed. CMake can use its default Visual Studio generator; `--config Release`
and `-C Release` also support multi-configuration generators. No game SDK is
needed. `CMAKE_BUILD_TYPE` is only relevant to single-configuration generators.

## Direct GCC build (Linux)

```sh
mkdir -p tests/build-gcc
g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -O2 -DNDEBUG \
    -Isrc src/IniRemoval.cpp tests/IniRemovalTests.cpp \
    -o tests/build-gcc/ini_removal_tests
./tests/build-gcc/ini_removal_tests
```

All checks remain active with `NDEBUG`. The executable prints a named result for
each test group, reports mismatched commands/diagnostics or failed conditions,
and returns a nonzero exit status on any failure. CTest runs the whole executable.
Generated `tests/build*` directories are ignored locally.

## Coverage and limits of these tests

- Repeated commands/sections and caller root order; exact case-insensitive names.
- Body-first, depth-first, repeated, named, numbered and `+=` includes, with
  Reader context, relative/fallback lookup, quotes and comment boundaries.
- UTF-8 BOM, ANSI/GBK bytes, ASCII/fullwidth whitespace/comments and CRLF/CR/LF.
- Strict body/include/header validation, empty/wildcard/list properties,
  unclosed quotes, unsupported UTF-16, NUL/control bytes, forbidden registries,
  read failures and missing files.
- Canonical dot/dot-dot, slash and case cycles; exact success/failure boundaries
  for depth, roots, paths, tokens, files, include entries, commands and bytes.
  Repeated large comment files test the 64 MiB aggregate budget without retaining
  64 MiB of distinct fixtures.
- Empty/stale output on every tested failure, including reuse of a previous
  successful plan, error reset, and recovery after failure.
- A `std::map` target simulation that applies only after **all** roots/children
  validate. Invalid later input leaves the target untouched. Successful absent
  or duplicate deletes do not create entries, remove empty sections, affect
  unrelated or similarly prefixed keys, assign values, or restore defaults.

The parser's target simulation does not execute native game code. A second test
executable compiles production `src/RulesRemoval.cpp` unchanged against
`tests/stubs/` APIs and checks its real orchestration: scan failure/empty input,
relative/game/MIX file priority, read failures, parse-before-mutate behavior,
stored-case names passed to Clear, absence/duplicates, preservation of empty
sections, post-delete verification and native failure reporting. There are 20
parser groups and 7 adapter groups; CTest runs both executables.

These fakes do not model the game's memory layout or execute its native Clear
function. The target exe's Clear was separately checked with disassembly/IDA;
full Win32 DLL compilation and game verification are still required. The test
harness uses exceptions for diagnostics, but production parser/adapter source
can also be compiled separately with `-fno-exceptions -fno-rtti`.
