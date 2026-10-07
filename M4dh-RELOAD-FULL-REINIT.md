# M4dh — TA reload + full re-INIT + pre/post Base probes

## Why (M4dg)
SHUTDOWN+START worked, but light re-attach (no INIT_DEV) → **vio fail** →
28×24 CAP sticky-Null → SCM **-22** → no SAMPLE banners.

Not a clean “fresh heap” test. Need working device + clean cut:

| Probe on **new** TA | Meaning |
|---------------------|---------|
| pre-INIT_DEV soft | heap free before Base |
| pre OOM | heap dead without Base (TZ global / listeners / TA image) |
| post-INIT_DEV OOM | Base fills heap on this instance too |
| post soft | Base does **not** kill heap — CAP path bug |

## Change
1. Keep SHUTDOWN+START after first Base  
2. **L2 VDD cycle + IC reset** after START  
3. Full attach: 205/1011/SYNC/INIT_SPI/PROBE  
4. **pre-INIT 28×24** only if vio OK (else skip — avoid sticky Null)  
5. **INIT_DEV** → Base  
6. **post-INIT 28×24** + HEALTH  
7. SLEEP + HEALTH → sample path (must stay alive)  
8. Verdict line comparing pre/post  

## Hold expect
- `M4dh reload begin` / `reloaded focal32 id=N`  
- `M4dh post-PROBE vio=1`  
- `M4dh pre-INIT_DEV 28x24 oom=0|1`  
- `M4dh Base after INIT_DEV-reload`  
- `M4dh post-INIT_DEV 28x24 oom=0|1`  
- `M4dh verdict: …`  
- `READY` / SAMPLE banners (not TIMEOUT)  

## Hold result (2026-07-28 go) — **decisive**

| Step | Result |
|------|--------|
| SHUTDOWN/START | PASS id 4→5 |
| post-PROBE | **vio=1** spi_fail=0 (L2 power fix works) |
| **pre-INIT_DEV 28×24** | **OOM** `array_new` (Base **not** run yet) |
| INIT_DEV Base | **OK** base_ok=1 |
| post-INIT_DEV 28×24 | **OOM** |
| **verdict** | **`heap dead even before Base (fresh TA)`** |
| SAMPLE 0/1 REAL DOWN | PASS itype=0x2 + lift |
| under-finger CAP | OOM both |
| rem_seen | 0 |
| READY / hold | **PASS** (not TIMEOUT) |

### Pivot
M4dd “heap dead at Base alone” was **incomplete**. On a **brand-new** TA after only SYNC+PROBE (vio OK, **no INIT_DEV**), layout1 **0x240** CAP already OOMs.

So either:
1. TEE heap is already empty after TA load + PROBE (listeners / TA image / PROBE allocs), or  
2. Image-flag CAP **always** fails `array_new` on our bind path (not truly “out of memory” from Base footprint alone)

M4ck: **16×16 flag=0x1** soft CAP worked (FDT path). Image flag **0x240** is the path that OOMs.

### Next
- Probe **FDT soft** (flag 0x1, tiny) vs **image 0x240** pre-INIT on fresh TA  
- Or CAP without layout1 / different flag that avoids `array_new`  
- Or grow TZ heap / shrink PROBE footprint / listener SB pressure  
