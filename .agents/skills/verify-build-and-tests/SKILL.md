---
name: verify-build-and-tests
description: Run the deterministic MSVC Ninja build, zero-dirty incremental check, parallel CTest suite, CLI diagnostics, and clean-room IP gate after C++ or CMake edits.
---

# Build, Test & Clean-Room Verification Procedure

1. Configure (if `build/` is absent) and compile with zero new `/W4` warnings:
   `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build`
2. Verify incremental build graph cleanliness (must output `ninja: no work to do`):
   `ninja -C build -n`
3. Execute the parallel CTest suite and flag any test exceeding `1.5s`:
   `ctest --test-dir build --parallel 8 --output-on-failure`
4. Run runtime diagnostics and clean-room compliance checks:
   `./build/scarlettx diag --full`
   `python tools/check_cleanroom.py --commit-range origin/main..HEAD`
5. Confirm zero temporary test artifacts polluted `build/` or the working tree:
   `git status --short`
