# M4dl — under-finger image climb (L1 + 0x240)

## Why (M4dk)
After Base: **all CAP forms soft** (no OOM), including L1+0x240 16×16.  
Under finger we auto-picked **FDT 0x1** → soft ~1s, rows=0, no frame.

## Change
1. Prefer **image** soft in matrix (L1img → L0img → FDT)  
2. AFR after Base: **always L1 + flag 0x240**  
3. Dim climb: **16×16 → 28×24 → 56×44 → 112×88×3**  
4. Soft no-frame → next dim; OOM → stop (one PA retry first)  

## Hold expect
```
M4dl prefer L1 flag=0x240 for AFR
M4dl LATCH image CAP L1 flag=0x240 climb dims
afr M4dl 16x16 … soft no-frame — climb
…
*** ANDROID FRAME RAW M4dl 112x88 ***   OR OOM at some dim
```

## Decision
| Result | Next |
|--------|------|
| frame raw any dim | rem/AUTH path |
| soft all dims no frame | img_scan bind / finger state |
| OOM at 16 | regression vs M4dk idle soft |

## Hold result (2026-07-28 go)

### post-Base matrix
All soft again; **prefer L1 flag=0x240** (image preferred).

### Sample 0 (REAL DOWN)
| Dim | Result |
|-----|--------|
| **16×16** L1 0x240 nopa | **soft** app_err=0 ms~11s vio=1 rows=0 — climb |
| **28×24** nopa | **OOM** ms=4 + residual **Step1 re-entry** |
| 28×24 PA retry | OOM |

### Sample 1
| Dim | Result |
|-----|--------|
| 16×16 nopa | OOM (FDT dac residual, rows=0) |
| 16×16 PA | OOM + vio fail / SPI mode fail |

rem_seen=0. No frame raw.

### Verdict
1. Prefer L1+0x240 is correct.  
2. Under finger **16×16 image soft once** (no frame) — same class as idle.  
3. Climbing to **28×24 re-enters Step1** and OOMs — **do not climb** past soft tiny.  
4. Full 112×88 never reached. Next: **stay at 16×16 / meta 0x240 under finger** (burst ×3, SPI, settle) without larger dims; block Step1 re-entry.  
