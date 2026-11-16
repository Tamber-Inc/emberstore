# emberstore dev command runner. Plain cmake/ctest — no wrapper CLI.
# Release is the tested configuration: the perf guards are calibrated for it.

set windows-shell := ["cmd.exe", "/c"]

# Show the recipe list by default.
default:
    @just --list

[doc('Configure + build (Release, warnings as errors); extra args go to the cmake configure')]
build *cmake_args:
    cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON {{cmake_args}}
    cmake --build build --config Release -j

[doc('Build + run the test suite')]
test: build
    ctest --test-dir build -C Release --output-on-failure

# The perf guard is excluded: its timing bounds only mean something
# un-instrumented, and ASan always trips them.
[doc('Build + run the tests under AddressSanitizer')]
test-asan:
    cmake -B build-asan -S . -DCMAKE_BUILD_TYPE=RelWithDebInfo -DEMBERSTORE_ASAN=ON
    cmake --build build-asan --config RelWithDebInfo -j
    ctest --test-dir build-asan -C RelWithDebInfo --output-on-failure -E "1M random rows"

# Extra args go to cmake: `just tidy -DEACP_BUILD_GRAPHICS=OFF` on a Linux box without eacp's graphics libraries.
[doc("Build everything, then run clang-tidy (eacp's checks) over emberstore's sources; fails on any finding")]
[unix]
tidy *cmake_args: (build cmake_args)
    scripts/tidy.sh build

[doc('clang-format check (no changes written)')]
[unix]
lint:
    git ls-files '*.h' '*.cpp' '*.mm' | xargs clang-format --dry-run --Werror

[doc('clang-format all sources in place')]
[unix]
format:
    git ls-files '*.h' '*.cpp' '*.mm' | xargs clang-format -i
