# M4dd — early heap probe + slim post-Base INIT

## Why (M4dc)
Idle CAP **28×24 OOM** was logged as “heap dead post-Base”, but the probe ran
**after** CHIP_INIT + INIT + DEAD_PIXEL + SYNC_CALIB + SLEEP + HEALTH — not
immediately after INIT_DEV Base. Soft free never helped. Need to cut:

| early @INIT_DEV Base | late @post-SLEEP | Meaning |
|----------------------|------------------|---------|
| OOM | OOM | Base alone fills TEE heap → reuse Base / free Base work bufs |
| soft | OOM | post-Base cmds kill heap → slim works if CAP improves |
| soft | soft | heap free until FDT arm / finger CAP |

## Change
1. **`m4dd_probe_28`** — one layout1 28×24 CAP at a named stage
2. **Early probe** right after INIT_DEV Base (enroll+skip_capture)
3. **`slim_post_base=1` (default)**: after Base, **break** cold INIT — skip
   LOGCAT1, CHIP_INIT, INIT, LOGCAT2, SYNC_CALIB, DEAD_PIXEL, FP_IS_TYPE
4. Skip **SYNC_CALIB-post-base** when slim
5. **Late probe** after SLEEP (same as M4dc site) + verdict line

Under-finger AFR unchanged (M4db LATCH direct CAP).

## Hold expect
- `M4dd probe 28x24 @early-INIT_DEV …`
- `M4dd slim_post_base — skip CHIP/INIT/DEAD after Base`
- `M4dd skip SYNC_CALIB-post-base`
- `M4dd probe 28x24 @late-post-SLEEP …`
- `M4dd verdict: …`
- SAMPLE banners + REAL DOWN + CAP (hope: soft early → under-finger frame)

## Decision after hold
- Base alone OOM → next: Base buffer reuse / free intermediate Step1 arrays
- Post-Base thrash OOM → keep slim; if under-finger still OOM → Step1 re-entry block
- Heap alive → attack FDT arm / finger CAP path only

## Hold result (2026-07-28 go)

| Stage | Result |
|-------|--------|
| Base after INIT_DEV | **OK** base_ok=1 |
| early @INIT_DEV 28×24 | **OOM** `array_new` |
| slim_post_base | **applied** (skip CHIP/INIT/DEAD + SYNC_CALIB-post-base) |
| late @post-SLEEP 28×24 | **OOM** |
| **verdict** | **`heap dead at INIT_DEV Base alone`** |
| Sample 0/1 REAL DOWN | **PASS** (touch, LATCH) |
| under-finger CAP | **OOM** both bursts ms=4 |
| rem_seen | **0** |
| AUTH | er=0 rem=0 |

### Decision
CHIP/INIT/DEAD_PIXEL are **not** the primary heap eaters — **INIT_DEV Base (Step1–3 + Base update) already leaves TEE heap empty** for a second `array_new`.

Slim does not unlock CAP. Next SW must **reuse Base raw** (no second array_new) or **free Base work buffers** after sticky Base, not more post-Base trimming.
