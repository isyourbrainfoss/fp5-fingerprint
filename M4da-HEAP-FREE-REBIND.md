# M4da — free/rebind before layout1 CAP

## Why (M4cz)
Layout1 image CAP OOMs at **28×24** (~1.3 KB). Not frame-size limited —
TEE heap empty for `array_new`. Base still warm; buffers likely held.

## Change
1. After Base SLEEP: **HEALTH** free try
2. Under LATCH before CAP:
   - **CANCEL_HL** (when `defer_enroll`)
   - **SLEEP → ESD_HANDLE → FDT_DOWN** (no SCAN)
   - Re-force sticky DOWN (no QUERY)
   - SPI 8M
3. layout1 **112×88 ×3** PA BURST
4. Config back to Android burst=3, process cols/rows=0

## Hold expect
- `M4da HEALTH after Base SLEEP`
- `M4da free/rebind CANCEL+SLEEP+ESD+FDT`
- Prefer frame raw; residual **nav base null** = free too aggressive; still OOM = free not enough

## Hold result (2026-07-28)
| Item | Result |
|------|--------|
| HEALTH post-Base | PASS |
| free/rebind under LATCH | PASS (CANCEL+SLEEP+ESD+FDT, touch sticky) |
| REAL DOWN ×2 | PASS |
| CAP | **FAIL** array_new OOM (ms=8–24) |
| nav base null | **not** seen (free did not drop Base) |
| rem_seen | 0 |

**Verdict:** Soft free/rebind does **not** free enough TEE heap for CAP
`array_new`. Not over-freeing Base (no null base residual). Heap still
exhausted or CAP always allocates new without reusing Base raw.
