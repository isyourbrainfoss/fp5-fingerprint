# M4db — no INIT DEAD_PIXEL; minimal path CAP

## Why (M4da)
Soft free/rebind did not free heap. Cold INIT still ran **DEAD_PIXEL +
FP_IS_TYPE** after Base — those allocate image path and can leave TEE
heap empty for under-finger `array_new` (even 28×24 OOM).

## Change
1. **`init_dead_pixel=0` (default)**: skip INIT DEAD_PIXEL / FP_IS_TYPE
2. Skip SYNC_STATS on enroll+skip_capture
3. Skip SET_GROUP when defer_enroll+skip_capture
4. Diagnosis quiet (log level 0, logcat trustlet off)
5. AFR: **no CANCEL free thrash** — LATCH + SPI8M + layout1 CAP ×3 only

## Hold expect
- `M4db skip INIT DEAD_PIXEL/FP_IS_TYPE`
- `M4db skip SET_GROUP` (if defer)
- `android-frame-raw M4db` + `M4db LATCH direct CAP`
- Prefer frame raw / rem; still OOM ⇒ Base alone fills heap (next: free Base raw reuse)

## Hold result (2026-07-28 go, SAMPLE banners OK)
| Item | Result |
|------|--------|
| SAMPLE banners | **PASS** (no pre-sample ESD_HANDLE kill) |
| Base sticky | PASS |
| REAL DOWN ×2 | PASS |
| LATCH direct CAP | PASS entry |
| CAP | **FAIL** array_new OOM ms=4–12 |
| rem_seen | 0 |

Path to under-finger CAP is stable again. Heap OOM unchanged.
