# Planning & roadmap (develop-only)

This directory holds hyphasync's **planning, roadmap, status, and design** docs. It is
maintained on the **`develop`** branch and is intentionally **absent from `main`**, which is
the publicly consumable branch.

## Branch model

| Branch | Audience | Carries |
|--------|----------|---------|
| `main` | Public / users | `README.md` + the public reference docs in `docs/` (`functions.md`, `fingerprinting.md`, `upgrading-duckdb.md`, `UPDATING.md`, `r.md`). **No `docs/planning/`.** |
| `develop` | Maintainers / agents | Everything `main` has **plus** this `docs/planning/` tree (working roadmap, status, design notes, history). |

Public reference docs are deliberately self-contained: nothing under `docs/` (top level) links
into `docs/planning/`, so `main` never has a dangling link. Keep it that way — if a public doc
needs to mention direction, say "tracked on the `develop` branch" rather than linking a planning
file.

## What lives here

| File | Role | Maintained? |
|------|------|-------------|
| [PROJECT_STATUS_AND_RECOMMENDATIONS.md](PROJECT_STATUS_AND_RECOMMENDATIONS.md) | **Canonical** status, gaps, prioritized M#/S#/N# items, milestones | ✅ live |
| [TODO.md](TODO.md) | Running checklist mirroring the canonical doc's item state | ✅ live |
| [dynamic-chunk-sizing.md](dynamic-chunk-sizing.md) | Design note for an unimplemented feature (M5/S5) | ✅ live (until built) |
| [archive/](archive/) | Frozen historical docs (old ROADMAP, STATUS, NEXT, May benchmark postmortem) | ❄️ history only |

Edit the canonical doc + TODO; leave `archive/` alone.

## Promoting changes to `main`

`main` should receive public-facing changes **without** this directory. Two simple recipes:

```sh
# Recipe A — merge develop, then drop planning in the same commit
git checkout main
git merge --no-ff develop
git rm -r docs/planning
git commit --amend --no-edit

# Recipe B — bring over only the public paths (no merge of planning)
git checkout main
git checkout develop -- README.md src test scripts \
  docs/functions.md docs/fingerprinting.md docs/upgrading-duckdb.md \
  docs/UPDATING.md docs/r.md
git commit
```

> **One-time cleanup:** if `main` still has the pre-split flat planning docs
> (`docs/PROJECT_STATUS_AND_RECOMMENDATIONS.md`, `docs/ROADMAP.md`, `docs/TODO.md`,
> `docs/STATUS.md`, `docs/NEXT.md`, `docs/ROADMAP-benchmark.md`, `docs/dynamic-chunk-sizing.md`),
> remove them on `main` once, along with the README/fingerprinting links that pointed at them
> (already done on `develop`).
