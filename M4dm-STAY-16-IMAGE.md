# M4dm — stay 16×16 image under finger (no climb)

## Why (M4dl)
- post-Base: L1+0x240 16×16 **soft**  
- under finger 16×16 image **soft no-frame** (~11s)  
- climb **28×24 → Step1 re-entry + OOM**  

Growing size re-opens calib. Stay tiny; burst only.

## Change
1. AFR: **only 16×16**, layout1, flag **0x240**  
2. Burst **×3**; soft no-frame keeps same size  
3. One PA retry on first OOM; **no dim climb**  
4. Light **SYNC_CALIB** before CAP (sticky Base; no WMODE/SCAN)  

## Hold expect
```
M4dm LATCH stay 16x16 L1 flag=0x240 ×3 (no climb base=1)
afr M4dm 16x16 BURST0/3 …
M4dm soft no-frame burst0 — stay 16x16
… or *** ANDROID FRAME RAW M4dm 16x16 ***
```

## Decision
| Result | Next |
|--------|------|
| frame raw / nz>0 | rem path |
| soft ×3 no frame | img_scan timing / finger QES / SPI 8M |
| OOM at 16 | SPI/state thrash after matrix |

## Hold result (2026-07-28 go)

| Item | Result |
|------|--------|
| post-Base matrix | all soft; prefer L1 0x240 |
| Sample 0/1 REAL DOWN | PASS |
| 16×16 ×3 both samples | **6× soft** app_err=0 ms~11.2s vio=1 |
| residual | **FDT dac success** only (no frame raw / Step1) |
| frame / nz / rem | **0** |
| OOM | **none** (no climb) |

### Verdict
Stable **no-OOM image-flag CAP under finger** at 16×16. TA still runs **FDT path** (~11s FDT dac), not img_scan — so flag 0x240 is soft-accepted without producing image. Next: force img_scan path (flag bits, WMODE, finger state) without growing size / Step1 OOM.
