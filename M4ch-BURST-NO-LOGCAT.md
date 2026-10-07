# M4ch — true CAP burst (no inter-CAP LOGCAT) under REAL DOWN

## Problem (M4cg hold)

REAL DOWN (`itype=0x2`, `touch=1`) then FAST AFR still FDT-only:

| Metric | M4cg |
|--------|------|
| CAP spacing | ~650–975 ms (not Android ~12–18 ms) |
| Inter-CAP work | LOGCAT 0x1003 + residual dump + pixel scan |
| Residual | `cidri f: 3(=N)` |
| frame raw / rem | 0 |

Android enroll: itype 0x2 + `[finger touch]` → `frame raw[0..2]` in **~55 ms**,
`transmit_image` ~37 ms. No LOGCAT between CAPs.

## Fix

`try_android_frame_raw()`:

1. **No** `0x1003` / residual / full pixel scan **between** CAP frames.
2. Only 0x240 × nfr with **5 ms** gaps.
3. One **post-burst** LOGCAT + `AFR-BURST` residual + pixel scan.
4. Log per-CAP wall `ms=` and total `wall_ms=`.
5. Stash hold itype/touch (each `send_ion` clears parse flags).

Still no pre-CAP QUERY / WMODE / REPORT (M4cf/M4c4/M4bg).

## Expect on next hold

- `android-frame-raw BURST` + `afr CAP … ms=` with CAP ms ≪ 600 if soft path stays light.
- `afr burst done wall_ms=` near Android (~50–150 ms ideal).
- Residual `AFR-BURST` still diagnostic if FDT-only / f:N.

## Hold

```sh
scp artifacts/qsee-modules/qsee_fingerpr.ko lab/fingerprint/fp-go-hold.sh \
    lab/fingerprint/fp-live-short.sh fp5-pmos:/tmp/
# user types go → PRESS on SAMPLE banners
```
