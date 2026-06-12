# Bestagon Wars — Balance Simulation Harness

Standalone simulations that validate the combat / economy / pacing math
defined in `examples/bestagon-design.md` (the GDD, locked at commit
`1f6f81a`).  The plan that drives this directory lives at
`memory/plan_bestagon_balance_prototyping.md`.

## Why a separate directory

Sims are *pre-production*: they run before Phase 6 (combat resolution
in `bestagon.trx`) ships.  They use a shared resolver — `combat-core.trx`
— so when production combat lands it `require-module`s the same file
rather than reimplementing the formula in parallel.  No throwaway code.

## Files

| File | Phase | Purpose |
| --- | --- | --- |
| `combat-core.trx` | 0.5A | Shared combat resolver: damage table + CO roster + weather + vet + formula.  Public API used by every sim AND by Phase 6 production code. |
| `damage-matrix.trx` | 0.5B | 6-D enumeration over (atk-unit × def-unit × terrain × atk-CO × def-CO × atk-vet × def-HP) -- 1.13M cells; flags one-shots / useless cells; max-damage tracker. |
| `pacing-sim.trx` | 0.5C | 36K Tank-vs-Tank trials with luck on; measures mean exchanges to first Power per atk-CO; compares against §9.2 GDD targets. |
| `luck-variance.trx` | 0.5D | 1.75M `apply-formula` calls with neutral CO/vet/Power; sweeps `(atk, def, def-HP)` × 10K trials; flags cells where luck pushes p-one-shot ≥ 5%. |
| `vet-distribution.trx` | 0.5E | 1000 arena games × 30 turns, 4 factions, attrition without map. Measures vet-tier reach + kill distribution; outputs §7.8 incidence. |
| `economy-curves.trx` | 0.5G | Pure-math sim: per-turn funds + 17-unit affordability bits across 7 `(hq, cities, starting_funds)` scenarios × 30 turns. Outputs milestone matrix (first turn each unit is affordable). |
| `co-matchup.trx` | 5.5A | (post-M5 / AI-dependent) |
| `weather-impact.trx` | 5.5B | (post-M5 / AI-dependent) |
| `super-marginal.trx` | 5.5C | (post-M5 / AI-dependent) |
| `capture-rush.trx` | 5.5D | (post-M5 / AI-dependent) |
| `air-superiority.trx` | 5.5E | (post-M5 / AI-dependent) |
| `out/` |  | CSV / Markdown reports; gitignored |

## Running

`combat-core.trx` implements `--self-test` (116 assertions against GDD
canonical numbers).  The other sims have no flag handling of their
own: each `run`s combat-core, so passing `--self-test` triggers
combat-core's assertions transitively and the sim's full enumeration
still executes afterwards (economy-curves additionally runs its own
15 assertions unconditionally on every start).  Run with the prod
build:

```
./trix.opt examples/bestagon-sim/combat-core.trx --self-test
```

combat-core also has a thin wrapper at `tests/test_bestagon_combat_core.trx`
that hooks the self-test into the standard `tests/run_all.sh` harness,
so any regression breaks the project test suite (not just the bespoke
sim invocation).  Run via:

```
./trix.opt tests/test_bestagon_combat_core.trx
% or, as part of the full harness:
tests/run_all.sh
```

Debug build (`./trix`) works for development but is 10-30× slower
(memory/feedback_debug_build_slow.md).

Sims that produce CSV write to `out/<sim-name>.csv` with the convention
`run_id, params..., result...`; load directly into Sheets / Excel.
