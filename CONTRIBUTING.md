# Contributing to ScarlettEdge

Thank you for contributing to **ScarlettEdge**. Please read [CLEANROOM_POLICY.md](CLEANROOM_POLICY.md) first—all contributions must be 100% clean-room.

---

## Base Engineering Rules

1. **Clean-Room First:** Never commit proprietary headers, binaries, keys, or retail dumps. Run `python tools/check_cleanroom.py` before every push.
2. **Remote Safety:** Never push to `upstream` (`Freminet/ScarlettX`). Keep `upstream` push disabled:
   ```powershell
   git remote set-url --push upstream DISABLED
   ```
   All work is pushed to your fork (`origin`).
3. **Zero-Warning MSVC `/W4` Build:** All C/C++ code must compile cleanly with MSVC x64 (`/W4`) under Ninja Release and pass 100% of CTests.
4. **Deterministic Tests:** Every bug fix or newly wired GDK/D3D12/COM surface should include a clean-room unit or integration test under `tests/`.
5. **Keep Dashboards Honest:** After adding new HLE APIs, tests, or phase milestones, regenerate the status badges from a passing build tree:
   ```powershell
   python tools/generate_status.py --build-dir build
   ```

## Local Build & Verification Workflow

```powershell
# 1. Configure & build (MSVC x64 Developer Command Prompt)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# 2. Run full parallel CTest suite
ctest --test-dir build --parallel 8 --output-on-failure

# 3. Run clean-room & IP hygiene gate
python tools/check_cleanroom.py

# 4. (Optional) Regenerate status dashboards
python tools/generate_status.py --build-dir build
```

## Flight-Recorder Diagnostics & Triage

When testing local out-of-repo GDK samples or user-supplied legal dumps, run with the 4-part Flight Recorder enabled:

```powershell
.\build\scarlettx.exe boot <path-to-package> --flight-recorder <out-dir> --hang-timeout 15 --debug-layer
.\build\scarlettx.exe triage <out-dir>
```

Never commit raw dumps, shaders, or copyrighted logs from external titles into the repository—distill findings into minimal clean-room regression tests instead.
