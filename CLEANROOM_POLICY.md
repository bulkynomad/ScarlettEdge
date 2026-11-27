# ScarlettEdge Clean-Room & IP Hygiene Policy

**ScarlettEdge** is an independent, clean-room Xbox Series X|S (Gen9) runtime compatibility layer and research project. Every contributor and automated workflow **must** follow the rules below without exception.

---

## 1. Strictly Clean-Room Source Code

- **Allowed inputs:**
  - Publicly documented APIs, specifications, and open standards (Direct3D 12, Vulkan, SPIR-V, DXIL, RFC 1951 DEFLATE, FIPS-197 AES, FIPS 180-4 SHA-256, Win32, WinRT).
  - Clean-room behavioral observations and synthetic test harnesses authored from scratch.
  - Original upstream clean-room code from [Freminet/ScarlettX](https://github.com/Freminet/ScarlettX) with explicit attribution.
- **Strictly prohibited inside this repository:**
  - Proprietary Microsoft GDK headers, import libraries (`.lib`), DLLs (`.dll`), NuGet packages (`.nupkg`), or SDK sample source code.
  - Commercial game executables, assets, shaders, audio/video files, or package containers (`.xvc`, `.msixvc`, `.xvd`).
  - Cryptographic keys, title keys, `.cik`, `.ekb`, `.lekb` files, retail certificates, or private key blocks (`-----BEGIN ... PRIVATE KEY-----`).
  - Disassembly, decompilation, or copied source excerpts from copyrighted binaries.

## 2. Strict Workspace Isolation

All external tools, official GDK packages, ATG samples, and user-supplied legal game dumps **must live outside the Git repository** (for example, in a sibling directory outside `ScarlettX/`). Never copy, symlink, or stage external SDK files or game dumps inside the repository tree.

## 3. Trademark & Keyword Hygiene

- Synthetic vertical-slice tests, environment variables, CMake targets, documentation, and commit messages must use **generic technical terminology** (e.g., `openworld_slice`, `ue5_slice`, `hello_scarlett`, `rt_showcase`).
- Do not use third-party publisher trademarks, game titles, or codenames in synthetic test identifiers or commit messages.

## 4. Honest Compatibility Reporting

- Passing synthetic CTests or official GDK sample suites proves ABI and silicon readiness—it does **not** imply compatibility with untested commercial retail games.
- Commercial title status may only be recorded in [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md) after direct hardware verification using a legally obtained, user-supplied dump, and without shipping any copyrighted assets or screenshots of unreleased material.

## 5. Automated Enforcement

These rules are enforced automatically on every push and pull request by [`tools/check_cleanroom.py`](tools/check_cleanroom.py) in [`.github/workflows/ci.yml`](.github/workflows/ci.yml):
- Scans tracked text files and commit messages for prohibited patterns and private key material.
- Blocks tracked binary/container/key extensions (`.xvc`, `.msixvc`, `.xvd`, `.cik`, `.ekb`, `.lekb`, `.nupkg`, `.pdb`, `.exe`, `.dll`, `.lib`).
- Enforces a `2 MB` per-file size budget for tracked assets (with an explicit allowlist for verified documentation images).
