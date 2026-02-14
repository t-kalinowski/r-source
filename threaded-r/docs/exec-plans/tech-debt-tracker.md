# Tech Debt Tracker

Track recurring gaps that are known and accepted temporarily.

## Open

1. Add CI wiring for harness stages (`smoke`, `partial`, `full`) with artifact upload.
Owner: unassigned
Why it matters: turns local process into guaranteed gate.
Exit criterion: CI job definitions merged and green.

2. Add mechanical stale-doc checks (broken intra-doc links, unreferenced plan files).
Owner: unassigned
Why it matters: avoids silent knowledge drift.
Exit criterion: `make docs-check` enforces these checks.

3. Add a recurring doc-gardening workflow spec and runbook.
Owner: unassigned
Why it matters: prevents accumulation of stale guidance.
Exit criterion: runbook in docs + scripted command target.

## Closed

- none yet
