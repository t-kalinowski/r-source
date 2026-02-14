# Core Beliefs

These principles constrain design choices for threaded R in this repository.

1. Serial parity is non-negotiable
- If threadpool is idle, serial main-thread performance must remain near baseline.

2. Correctness before throughput
- Threading wins are rejected if they introduce instability, flaky behavior, or package breakage.

3. Repository is the system of record
- Decisions, invariants, plans, and benchmark outcomes must live in-repo.

4. Enforce invariants mechanically
- Critical constraints should be encoded as scripts/tests, not only prose.

5. Progressive disclosure for agents
- Keep top-level context small (`AGENTS.md`), route detail to scoped docs.

6. Existing package ecosystem compatibility is a product requirement
- ABI/package compatibility checks are blockers, not optional.

7. Small reversible deltas
- Prefer incremental, easy-to-revert changes with checkpointed benchmark evidence.
