# M4dn — last FP cut today: img flag sweep @ 16×16

## Why (M4dm)
Stable soft CAP under finger, residual **FDT dac only** (~11s), no frame raw.
Need img_scan path without growing dims (Step1 OOM).

## Change
1. Stay **16×16 L1 + PA**  
2. No SYNC_CALIB / no climb  
3. Flag sweep: **0x240, 0x200, 0x40, 0x41** ×2  
4. One **DEAD_PIXEL 0x1020** image try if still no frame  
5. Log fast vs FDT-slow soft  

## Hold expect
```
M4dn LATCH img-flag sweep 16x16 L1 PA
afr M4dn flag=0x240 … ms=…
*** ANDROID FRAME RAW M4dn ***  OR soft FDT-slow / OOM next flag
```

## After this hold
Park FP work. Next project: **main camera** (Android oracle → pmOS).

## Hold result (2026-07-28 final go) — park FP

| Item | Result |
|------|--------|
| REAL DOWN ×2 | PASS |
| flags 0x240/0x200/0x40/0x41 ×2 | all **soft FDT-slow** ~11.2s, app_err=0, frame=0 |
| DEAD_PIXEL | er=0 frame=0 |
| frame raw / rem | **0** |
| OOM | none (stayed 16×16) |

### Parked conclusion
- CAP soft + Base + touch path are **green**.  
- All image-flag bits still take **FDT path** (~11s), not Android ~37ms img_scan.  
- Resume later: force img_scan / transmit_image state machine (not more free thrash).  
