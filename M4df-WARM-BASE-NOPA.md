# M4df — warm Base (no SLEEP) + nopa CAP first

## Why (M4dd)
Heap dead **at INIT_DEV Base alone**. Soft free / slim post-Base / tiny CAP all
OOM. Android under finger: frame raw×3, **0** array_new — likely **reuses**
Step3 raw still held in TA, or CAP without a second full alloc.

Hypotheses this cut tests:
1. **SLEEP after Base frees reusable Step3 raw** → CAP must `array_new` → OOM  
2. **Diagnostic CAP probes** sticky-Null / burn last free slots  
3. **PA@+0x14** double-allocs with internal `array_new`

## Change
1. **`warm_base_nosleep=1` (default)**: skip WMODE SLEEP after Base on enroll path  
2. **`heap_probe=0` (default)**: no early/late 28×24 CAP probes  
3. **`afr_nopa_first=1` (default)**: under-finger CAP suppress PA; on first OOM one PA retry then stop  
4. Keep slim_post_base + M4de lift-between-samples  

## Hold expect
- `M4df skip early heap probe`
- `M4df skip SLEEP after Base (keep image buffers warm)`
- `M4df skip late heap probe`
- `android-frame-raw M4df` + `M4df LATCH direct CAP (nopa_first=1…)`
- `afr BURST0 … pa=0` then maybe `M4df nopa OOM — one PA retry` + `pa=1`
- Prefer: **frame raw** / rem; still OOM both nopa+PA → Base holds heap permanently (need TA-internal free/reuse cmd)

## Decision
| Result | Next |
|--------|------|
| frame raw | keep warm path; polish rem/AUTH |
| nopa soft, PA OOM | stay nopa; tune flag/dims |
| both OOM | free Base work bufs via unknown TA op / deeper RE |

## Hold result (2026-07-28 go)

| Item | Result |
|------|--------|
| Base | OK |
| skip early/late probe | applied |
| skip SLEEP after Base | applied |
| Sample 0/1 REAL DOWN | PASS itype=0x2 + lift clear |
| BURST nopa (pa=0) | **OOM** array_new ms=4 |
| PA retry (pa=1) | **OOM** array_new ms=4 |
| rem_seen | 0 |

### Verdict
Keeping Base warm (no SLEEP) + nopa-first does **not** unlock CAP.
Step3 raw is either already freed inside INIT_DEV before we return, or CAP
**always** `array_new`s a new buffer regardless of warm state. Base footprint
still owns TEE heap permanently for a second alloc.

Next: free Base intermediate arrays without dropping sticky Base (unknown TA
cmd / RE of Android post-Base free), or CAP path that never calls array_new
(flag/mode that reuses internal Base pointer — needs RE).
