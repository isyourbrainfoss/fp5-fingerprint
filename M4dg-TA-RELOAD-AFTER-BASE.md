# M4dg — SHUTDOWN+START after Base, reload calib

## Why (M4df)
Warm Base / nopa / slim / soft free all still `array_new` OOM. Android
**SLEEP**s after Base and still frame-raws — same TA process, so the
differentiator is not “keep Step3 raw warm.” Base Step1–3 **owns TEE heap**
for the life of the TA instance; CAP always needs a new alloc.

TA strings: `ff_sensor_data_free`, `ff_calibration_data_*`,
`FF_CMD_TA_SYNC_CALIB_DATA`, load `ff_calibration-*.dat`.

Hypothesis: **export calib → kill TA → restart → import calib** frees Step1–3
working arrays while restoring Base coeffs without re-running img Step1–3.

## Change
After Base (enroll+skip_capture), if `ta_reload_after_base=1` (default):
1. `SYNC_CALIB` (export attempt)
2. SLEEP
3. `app_shutdown` focal32
4. `app_start` focal32 (new id)
5. **memset session** (no false Base from old residual)
6. re-attach: 0x205 + 0x1011 + SYNC_CFG + INIT_SPI + PROBE×2
7. **no INIT_DEV** (would re-fill heap)
8. `SYNC_CALIB` import + LOGCAT + ion Base scan
9. one **28×24** probe: `oom=0` ⇒ fresh heap; `oom=1` ⇒ still dead
10. continue FDT sample loop with new id

## Hold expect
- `M4dg reload begin` / `SHUTDOWN` / `reloaded focal32 id=N`
- `M4dg Base after calib reload` **or** `no Base from SYNC_CALIB`
- `M4dg post-reload 28x24 oom=0|1 base=0|1`
- SAMPLE + REAL DOWN + CAP

| post-reload 28×24 | base | Meaning |
|-------------------|------|---------|
| soft/frame | any | **heap free** — under-finger CAP may win |
| OOM | 1 | calib reloaded but heap still tight |
| OOM | 0 | fresh TA still OOM (not Base-only) / attach broken |
| soft + no base | 0 | free heap without Base — CAP may work; rem maybe not |

## Decision
- Heap free after reload → keep path; polish CAP/rem  
- Still OOM → TZ global heap not in this TA instance, or attach re-allocates same  
- START/SHUTDOWN fail → need different free path  

## Hold result (2026-07-28 go)

| Step | Result |
|------|--------|
| Base (pre-reload) | **OK** INIT_DEV |
| SHUTDOWN id=4 | **PASS** ret=0 |
| START → id=5 | **PASS** |
| PROBE2-reload | **vio 1.8v fail** (no INIT_DEV) |
| Base from SYNC_CALIB | **no** base_ok=0 |
| post-reload 28×24 | **OOM** (but device half-dead) |
| Sample hold | **ABORT** sticky Null → SCM **-22** / TA dead |
| READY_SAMPLE0 | **TIMEOUT** (no sample wait) |

### Verdict
1. **SHUTDOWN+START works** on mainline.  
2. **SYNC_CALIB alone does not restore Base** without INIT_DEV (no SFS calib export usable yet — serial SFS still GENERIC).  
3. **Light re-attach without INIT_DEV is invalid** for CAP test: vio fail → Null thrash → TA death.  
4. post-reload OOM is **not clean** “fresh heap still empty” evidence — device not fully open.

### Next
- **M4dh:** reload then **INIT_DEV** (get Base+vio) and compare heap probe **before** vs **after** INIT_DEV on the **new** TA instance; or  
- Fix calib **export to SFS** so reload+SYNC can restore Base without Step1–3; or  
- Default `ta_reload_after_base=0` until re-attach is solid (restore SAMPLE path).
