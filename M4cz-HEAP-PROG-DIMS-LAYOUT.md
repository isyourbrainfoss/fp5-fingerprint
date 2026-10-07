# M4cz — heap ladder: progressive dims + layout0/1

## Why (M4cy)
defer ENROLL + ion BIG plen still `array_new` OOM. TA always mallocs
working buffers. M4cm with ENROLL: ≥32×32 OOM; without ENROLL, retest
smallest image path.

## Change
1. Config: `burst_acquisition_num=1`, `image_processing` **56×44**
2. AFR LATCH: SPI8M, no SYNC, no BIG plen thrash
3. Ladder: layout **0** then **1**; dims **28×24 → 56×44 → 112×88**
4. Soft no-frame (ms≥200) → climb; OOM → HEALTH, next layout
5. One FULL 112×88 if ladder only soft

## Hold expect
- `android-frame-raw M4cz` + `M4cz AFR SPI8M progressive`
- `afr L0 prog0 28x24` …
- Possible `*** ANDROID FRAME RAW L? WxH ***` if any rung fits

## Hold result (2026-07-28)
| Path | Result |
|------|--------|
| L0 all dims | soft FDT only (~1s, err=0, rows=0) — **no image path** |
| L1 28×24 | **instant array_new OOM** (ms=4, rows=24) |
| FULL 112×88 | OOM |
| rem_seen | 0 |

**Verdict:** Any layout1 image CAP (even **28×24 ≈ 1.3 KB**) OOM under finger.
TEE heap for `array_new` is effectively empty; not a “full frame only” size issue.
Need free/reuse of Base-held buffers (or Android bind that skips array_new).
