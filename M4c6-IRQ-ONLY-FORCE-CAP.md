# M4c6 — IRQ-only wait + force CAP (2026-07-27)

## M4c5 hold failure
All samples: `no IRQ/DOWN — skip CAP`. Mid-wait `poll-esd` + periodic QUERY
thrashed FDT; no edges; never entered AFR/image CAP. rem_seen=0.

## Fix
1. Sample wait: **poll irq_count only** — no mid-wait QUERY/esd_clear.
2. On IRQ: QUERY once; CAP on edge even if sticky esd (assume).
3. On timeout + `enroll_force_cap=1`: **FORCE MINIMAL CAP** so image path runs.
4. `sample_esd_clear` default **false**.

## Hold
```sh
sh /tmp/do-hold-m4c6.sh
# press 6× power FP ~1s/lift when cued; force CAP still exercises path without IRQ
```
Success: `FORCE MINIMAL` or IRQ edge → `afr CAP` → `frame raw` / `rem_seen=1`.
