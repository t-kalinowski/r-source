# Knowledge Base Layout (Agent-Readable)

Goal: keep repository knowledge structured, discoverable, and mechanically maintainable.

## Layout

```text
threaded-r/
├── AGENTS.md
├── agents/
├── docs/
│   ├── AGENT_FIRST_PATTERNS.md
│   ├── CURRENT_IMPLEMENTATION_DESIGN.md
│   ├── INVARIANTS_CHECKLIST.md
│   ├── KNOWLEDGE_BASE_LAYOUT.md
│   ├── THREADING_PRODUCT_DIRECTION.md
│   ├── design-docs/
│   ├── exec-plans/
│   │   ├── active/
│   │   └── completed/
│   ├── generated/
│   └── references/
├── plans/
├── benchmarks/
└── tests/
```

## Content ownership

- `AGENTS.md`: short map and global rules only.
- `agents/*.md`: scoped technical navigation notes.
- `docs/design-docs/`: durable architecture/design decisions.
- `docs/exec-plans/active/`: in-flight execution plans.
- `docs/exec-plans/completed/`: completed plans with outcomes.
- `docs/generated/`: generated docs (schemas, inventories, reports).
- `docs/references/`: external reference notes distilled for in-repo use.
- `plans/`: roadmap-level sequencing and phase checklists.

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
