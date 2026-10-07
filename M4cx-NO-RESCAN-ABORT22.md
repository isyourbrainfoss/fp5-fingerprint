# M4cx — no re-SCAN after soft ESD; abort on SCM -22

## Why (M4cw)
```
pre-hold arm esd=1 → soft esd2 → SCAN-armed2 SCM -22
→ all QEV/CAP -22; M4cw LATCH never ran
```

## Change
1. Soft esd_clear: **ESD_HANDLE only** (no WMODE/SCAN thrash)
2. Pre-hold ESD: one soft handle, **no SCAN-armed2**
3. Abort samples on first SCM -22 (arm / QEV / pre-wait)

## Keep
- M4cv preserve Base (no L2)
- M4cw LATCH despite sticky ESD + softESD + SYNC/SPI8M CAP

## Hold expect
- `M4cx pre-hold ESD soft handle only` if esd
- No `SCAN-armed2`
- Alive TA through REAL DOWN → `afr LATCH+softESD` → BURST

## Hold result (2026-07-28)
| Item | Result |
|------|--------|
| TA alive (no -22) | **PASS** |
| no SCAN-armed2 | **PASS** |
| M4cx soft handle | PASS |
| REAL DOWN both | **PASS** (esd=0, touch=1) |
| LATCH + SYNC/SPI8M | **PASS** |
| CAP BURST | **FAIL** `array_new` OOM ms=4–8, residual **no Step1** (len=222) |
| rem_seen | 0 |

Progress: path to under-finger CAP is green. Remaining blocker is pure **TA heap array_new** under warm Base + finger touch (not Step1 re-entry, not dual-TA, not L2 wipe).
