# Knowledge Base Layout (Agent-Readable)

Goal: keep repository knowledge structured, discoverable, and mechanically maintainable.

## Layout

```text
threaded-r/
├── AGENTS.md
├── r-source/
├── builds/
├── artifacts/
├── harness/
├── agents/
├── docs/
│   ├── AGENT_FIRST_PATTERNS.md
│   ├── CROSSCHECK_AGENT_FIRST.md
│   ├── CURRENT_IMPLEMENTATION_DESIGN.md
│   ├── SUBINTERPRETER_API.md
│   ├── INVARIANTS_CHECKLIST.md
│   ├── KNOWLEDGE_BASE_LAYOUT.md
│   ├── QUALITY_SCORE.md
│   ├── THREADING_PRODUCT_DIRECTION.md
│   ├── design-docs/
│   │   ├── index.md
│   │   └── core-beliefs.md
│   ├── exec-plans/
│   │   ├── active/
│   │   ├── completed/
│   │   └── tech-debt-tracker.md
│   ├── generated/
│   └── references/
├── plans/
├── benchmarks/
└── tests/
```

Operational sub-layout:

```text
builds/
├── mtl/
├── ref/
└── mtl-clang/

artifacts/
├── bench/
│   ├── latest/
│   └── history/
└── checks/
    ├── latest/
    └── history/

harness/
├── scripts/
├── checks/
└── abi/
```

## Content ownership

- `AGENTS.md`: short map and global rules only.
- `agents/*.md`: scoped technical navigation notes.
- `docs/design-docs/`: durable architecture/design decisions.
- `docs/exec-plans/active/`: in-flight execution plans.
- `docs/exec-plans/completed/`: completed plans with outcomes.
- `docs/exec-plans/tech-debt-tracker.md`: known debt with owner/exit criteria.
- `docs/generated/`: generated docs (schemas, inventories, reports).
- `docs/references/`: external reference notes distilled for in-repo use.
- `plans/`: roadmap-level sequencing and phase checklists.
- `harness/`: staged validation ladder and runtime guardrails.
- `artifacts/`: generated outputs (benchmark CSVs, rendered reports, stage status files).

## Update rules

1. For non-trivial changes, update at least one of:
   - `docs/design-docs/*`
   - `docs/exec-plans/*`
   - `docs/INVARIANTS_CHECKLIST.md`
2. Keep file references concrete and repo-relative.
3. When a plan is completed, move it from `active/` to `completed/`.
4. If a repeated failure mode appears, add a mechanical check (test/lint/script), then document the new invariant.

## Freshness policy

- Avoid long narrative docs that are not tied to concrete files/checks.
- Prefer short, composable docs with explicit links to source and scripts.
- Prune stale docs continuously via small maintenance PRs.
