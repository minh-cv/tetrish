---
paths:
  - "**/*.c"
  - "**/*.h"
---

# C Design Conventions

Extracted from the current codebase (2026-07-22), not invented. Where the codebase is inconsistent, that's flagged below instead of silently picking one.

## Consistent — follow these

- **Naming**: `snake_case` for functions, variables, and file-scope names throughout (`read_command`, `verify_server_cert`, `mkdir_archive`).
- **Error handling**: on syscall/libc failure, `perror("<context>")` followed by `return -1` (recoverable) or `exit(1)` / `_exit(1)` (unrecoverable), e.g. `dcheck.c`, `dspawn.c`, `shell.c`. Don't introduce a different error-signaling style (error codes via `errno` output param, `goto fail`, etc.) without reason.
- **Build**: one `add_library`/`add_executable` per module in `CMakeLists.txt`, headers under `include/`, private headers included via `target_include_directories(... PRIVATE include)`.

## Exists but not yet adopted — don't assume it's required

- **`dtor.h`** defines a scope-based cleanup macro system (`DTOR_DEFINE`, `DTOR_WRAPPER_DEFINE`, etc.) but grep across `src/**/*.c` finds **zero uses of any `DTOR_*` macro**. It's available infrastructure, not an established pattern yet — don't assume existing code follows it, and don't introduce it into new code unless asked.
- Several entry points (`src/tetrisd/main.c`, `src/tetrisu/main.c`) are empty stubs (`int main() { return 0; }`). Treat modules like this as unimplemented scaffolding, not as an example of a minimal/finished style.

## Inconsistent — flagged, not resolved

- **Header guards** don't follow one scheme: `dtor.h`/`htttp.h`/`tetrisbrain.h`/`tetrissh.h` use a `TETRISH_<NAME>_H` prefix, but `shell.h` uses bare `SHELL_H`, `perms.h` uses bare `PERMS_H`, and `common.h` uses bare `COMMON_H`. The bare ones risk collisions with system/third-party headers. No rule enforced here — pick `TETRISH_<NAME>_H` for new headers unless told otherwise, since it's the more common form and collision-safe, but this needs an explicit decision to standardize existing headers.
- **File header comments** are not uniform: `common.c`/`common.h` open with a `/** name \n --- \n description */` block; most other `.c` files (e.g. `tetrisbrain.c`, `backup.c`) have no file-level comment at all. Not treated as a convention to replicate.
- **`common.h`/`common.c`** describe themselves as "Shared declarations for the Secure FTP project" — that name doesn't match this repo (`tetrish`). This looks like it may have been carried over from a different project. Worth confirming whether it's intentionally reused code or leftover boilerplate before treating its conventions (OpenSSL wrapper style, Fernet-equivalent naming, etc.) as canonical for `tetrish`.

## Open questions (need a decision, not guessed)

1. Standardize all header guards to `TETRISH_<NAME>_H`, or leave as-is?
2. Is `common.h`/`common.c` intentionally ported from another project, or stale/mislabeled?
3. Is `dtor.h` meant to be adopted going forward, or dead/experimental code?

Update this file once these are resolved so it stops listing them as open.
