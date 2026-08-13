---
paths:
  - "**/*.c"
  - "**/*.h"
---

# C Design Conventions

Extracted from the codebase (last verified 2026-07-22 on `feat/sample-implementation`), not invented. Where the codebase is inconsistent, that's flagged below instead of silently picking one.

## Consistent — follow these

- **Naming**: `snake_case` for functions, variables, and file-scope names throughout (`read_command`, `verify_server_cert`, `htttp_serialize`). Library functions are prefixed with their module (`htttp_*`, `config_*`, `tetrish_*` for tetrissh wire helpers).
- **Header guards**: `TETRISH_<MODULE>_<NAME>_H` — all recent headers follow this (`TETRISH_CONFIG_H`, `TETRISH_TETRISD_CLIENT_H`, `TETRISH_TETRISU_CONFIG_VAR_H`). Use it for every new header. Legacy stragglers exist (see below) but don't copy them.
- **Header placement**: public library headers go in `include/`; module-private headers sit next to their sources (`src/tetrisd/*.h`, `src/tetrisu/*.h`) and are exposed via `target_include_directories(<target> PRIVATE src/<module>)`. The shell's internal headers live in `src/tetrish/libs/`.
- **Cleanup on error paths**: the `dtor.h` macro system is the established pattern for functions that acquire resources:
  ```c
  static DTOR_WRAPPER_DEFINE(free)
  static DTOR_WRAPPER_DEFINE(config_free)

  int f(void) {
      DTOR_DEFINE(dtor, 10);
      ...
      if (fail) DTOR_RETURN(dtor, -1);   // runs registered destructors, then returns
      DTOR_INSERT(dtor, free, ptr);      // register cleanup right after acquisition
      ...
      DTOR_RETURN(dtor, 0);
  }
  ```
  Used across `tetrisu`, `tetrisd`, and `libtetrissh`. In functions that use a dtor, **never bare-`return` after the first `DTOR_INSERT`** — that leaks (see commit `4460dce` fixing exactly this). Use `DTOR_RETURN` for every exit.
- **Build**: one `add_library`/`add_executable` per module in `CMakeLists.txt`. Libraries are default (static) — not `SHARED`. `common` links OpenSSL `crypto`. Library include dirs are `PUBLIC` so dependents inherit them.

## Inconsistent — flagged, not resolved

- **Legacy header guards**: `common.h` uses bare `COMMON_H`, `src/tetrish/libs/perms.h` uses bare `PERMS_H`, and `src/tetrish/libs/shell.h` / `system_program.h` have **no include guards at all**. New code must use the `TETRISH_` scheme; whether to retrofit these is an open decision.
- **Error reporting** is two-generation: older code (`shell.c`, system programs) does `perror("<context>")` then `return -1` / `exit(1)`; newer code uses `DTOR_RETURN(dtor, -1)` for cleanup but is uneven about reporting — `epollmanip.c` has many bare `return -1`s with no diagnostic, `tetrisu` sometimes uses `fprintf(stderr, ...)`. For new code: use the DTOR pattern for cleanup, and prefer emitting *some* diagnostic (`perror` for syscall failures) before returning — but don't "fix" existing silent returns unasked.
- **`common.h`/`common.c`** still self-describe as "Shared declarations for the Secure FTP project" — doesn't match this repo (`tetrish`). It's genuinely load-bearing now (linked by `tetrissh`, `tetrisd`, `tetrisu`), so it's reused code, but the header comment is stale. Don't treat its internal style (section-divider comments, `/** */` file headers) as a convention for new files.

## Open questions (need a decision, not guessed)

1. Retrofit legacy headers (`common.h`, `perms.h`, `shell.h`, `system_program.h`) to `TETRISH_` guards — and add the missing guards?
2. Update `common.h`'s "Secure FTP project" description to reflect its role in tetrish?
3. Standardize error *reporting* (not just cleanup) — e.g. require a `perror`/`fprintf(stderr)` before every error return?

Update this file once these are resolved so it stops listing them as open.