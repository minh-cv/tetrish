---
paths:
  - "**/*.c"
  - "**/*.h"
  - "CMakeLists.txt"
---

# Testing Rules

No automated test suite exists yet in this repo (checked `main` and `feat/sample-implementation` as of 2026-07-22) — this project is still in early implementation phase. Testing framework and conventions are deferred, not decided.

Until a framework is chosen:
- Do not invent or assume a test framework (no CMocka, Unity, etc.) when writing or suggesting code.
- Do not write test files unless explicitly asked.
- If asked to verify behavior, use manual/inline `assert()` (already used in `dtor.h`, `shell.c`, `main.c`) or ad-hoc CLI checks against the built binaries (`tetrissh`, `tetrisd`, `tetrisctl`, `tetrisu`, `tetrislogd`), not a test suite.

When a testing approach is adopted, update this file with:
- The chosen framework and how it's wired into `CMakeLists.txt` (e.g. `enable_testing()` / `add_test`)
- Test file location and naming convention
- How to run tests locally (e.g. `ctest` command)