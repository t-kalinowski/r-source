# Package ABI + Native Code (Agent Scope)

Focus: drop-in package compatibility with existing binaries and source packages.

Key constraints:

- no package author code changes required,
- existing `.Call`/`.External` code should keep working,
- package load/registration may be main-thread serialized initially,
- calling already-registered native routines from workers should be supported.

Key files:

- `src/main/dotcode.c`
- `src/main/Rdynload.c`
- `src/main/registration.c`
- `src/main/main.c`

Smoke targets:

- load binary packages from standard library paths,
- worker-side calls into native-heavy packages,
- RStudio embedded `rsession` startup/linking smoke.
