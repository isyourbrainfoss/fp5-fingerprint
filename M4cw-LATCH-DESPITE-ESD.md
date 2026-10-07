# M4cw — LATCH CAP despite sticky ESD

## Why (M4cv)
Soft ESD preserved Base but left `esd=1`. AFR LATCH required `!esd`:
```
REAL DOWN touch=1 esd=1 itype=0x2 → latched=false → settle → skip image
```
No CAP at all. M4cu had esd=0 after L2 clear → LATCH → OOM (but CAP ran).

## Change
1. LATCH if REAL DOWN (**ignore sticky esd**)
2. If latched + had_esd: one soft **ESD_HANDLE** only (no QUERY)
3. Force sticky touch/itype for CAP; clear local esd flag
4. Keep M4cv SYNC_CALIB+SPI8M + preserve Base (no L2)

## Hold expect
- `android-frame-raw M4cw`
- `afr LATCH+softESD` (or LATCH skip settle)
- CAP BURST with/without Step1 — want frame raw, not skip image

## Hold result (2026-07-28)
**M4cw LATCH path not exercised.** TA died before samples:

```
pre-hold arm esd=1 → soft esd2 “ok” → SCAN-armed2 SCM -22
→ all QEV/CAP -22; IRQ edge but no touch; FORCE idle CAP -22
AUTH -22 rem_seen=0
```

ENROLL start still OK (new fid). Death on second SCAN after soft ESD re-arm.

**Next:** abort/rearm on SCM -22; avoid double SCAN after soft esd2; or soft ESD without extra FDT re-arm thrash before sample wait.
