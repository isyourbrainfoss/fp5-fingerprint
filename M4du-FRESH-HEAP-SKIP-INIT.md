# M4du — SHUTDOWN + START + skip INIT_DEV (2026-09-05)

## Review (M4dt did not run)

Last live dump is still **M4ds** (`20260905T214728Z`, fid `1750352060`):

| Check | Result |
|-------|--------|
| Cue / press | PASS — one flash, REAL DOWN itype=0x2, skip QFINGER |
| Native CAP | **FAIL** 112×88 L1 nopa **ms=28** `rows=88` `app_err=0xffffff37` |
| Residual | `ff_sensor_data_array_new[image.c:105] Out of memory` |
| INIT_DEV | still ran Step1–3 + Base on START −22 reattach |
| AUTH | `broken off` rem=0 |

M4dt (`skip INIT` on `do_start=0`) was installed at 21:49Z but **never loaded**.
GTK stayed on **NO IMAGE** with Register disabled. qsee already `exit` / rmmod.
Even if it had loaded, leftover INIT buffers from M4ds would still fill TEE
heap — skip INIT does not free them. `rmmod` never SHUTDOWN.

## Why
Android INIT Step3 `array_new` **succeeds**; enroll CAPTURE does **not**
`array_new` (0 OOM). pmOS INIT Step3 also succeeds; the **second** 112×88
`array_new` (CAPTURE) OOMs, even at 32×32 once INIT has run.

M4du: **SHUTDOWN** resident focal32 (free TEE mallocs) → **APP_START** →
**skip INIT_DEV** → one M4dr 112×88 CAPTURE on DOWN. CAPTURE is the first
image malloc this TA instance.

## Change
- `shutdown_before_start=1` — SHUTDOWN ids 4/5/6 (+ saved) then START
- `skip_init_dev=1` — no INIT_DEV / CHIP / INIT / SYNC_CALIB / DEAD_PIXEL
- GTK **Start over** after empty fid (Register was a dead button)

## One press
Close the old NO IMAGE screen if it is still up, or tap **Start over**.
Start → Register. Hands off until **one flash**, hold ~2s.

Want dmesg `M4du SHUTDOWN` + `M4du skip INIT_DEV` plus:

| Result | Meaning |
|--------|---------|
| `FRAME RAW M4dr` | heap was the leftover INIT; native bind works without Base |
| OOM ~28ms again | not leftover INIT — CAPTURE `array_new` fails on an empty heap too |
| FDT-slow ~11s / no DOWN | skip INIT left FDT unarmed; need a no-malloc FDT arm next |

Do not keep tapping Register if it is OOM or FDT-slow.

## Hold result (2026-09-05)

Two Registers, **no flash**, instant NO IMAGE.

| Session | What happened |
|---------|----------------|
| `220313Z` | SHUTDOWN (id 4 likely ok) + START **id=0x10**. Skip INIT. PRE_ENROLL ok. **ENROLL_HL SCM -22 killed TA** before `HOLD FINGER` / `PRESS then LIFT`. Cue never fired. |
| `220357Z` | SHUTDOWN 4/5/6/3 only (missed **id 16**). START -22, **no TA ids**, insmod returned in 1s. |

Fix (same cut): SHUTDOWN **1..20**; if START -22 attach leftover; **defer ENROLL_HL** when skip INIT (that command needs INIT and was aborting the wait). UI: ended-before-flash is **FAILED**, not NO IMAGE.
