---
name: audit-subsystem-parity
description: Perform an anti-facade code-body parity audit on a C++ subsystem, verifying live guest COM reachability, absence of no-op S_OK stubs, hot-path efficiency, and updating docs/AUDIT_BASELINE.md.
---

# Empirical Anti-Facade Subsystem Parity Audit Procedure

1. **Trace Live Retail Reachability:** Trace callers from `pe_mapper.cpp` (`ResolveSystemShimTarget` / `ResolveFallbackMiddlewareTarget`), `D3D12XboxCreateDevice`, `QueryApiImpl`, `xcrdapi.dll`, or `xaudio2_9.dll` under `SCARLETTX_REAL_TITLE=1`. Flag any export only reachable by synthetic `uint64_t`-handle tests.
2. **Inspect Function Bodies:** Classify each API strictly by executed semantics (`Verified Functional`, `Happy-Path / Partial`, `Stub / No-Op / Facade`, or `0% Roadblock`). Reject any newly claimed API that returns hardcoded `S_OK`/`0` without state mutation or test coverage.
3. **Check Hot-Path Efficiency:** Inspect modified loops and command-recording paths for heap allocations, coarse mutex contention, synchronous I/O, or missing GPU/memory barriers.
4. **Update Baseline:** Run `/verify-build-and-tests` and update the subsystem's row and trap ledger inside `docs/AUDIT_BASELINE.md`.
