# M4cs — keep finger-down through CAP + SLEEP after base

## Why (M4cr)
```
afr settle CLEAN touch=1
afr postarm touch=0   ← WMODE+SCAN cleared touch
CAP residual QES [unknow] + Step1 + array_new OOM
```
Android under finger CAP with **[finger touch]** → frame raw×3.

## Change
1. After INIT Base: **WMODE SLEEP** (Android init end)
2. AFR: **no** WMODE/SCAN after settle
3. precap QUERY log; CAP meta ×3 while touch still live

## Hold expect
- `M4cs SLEEP after base`
- `afr precap ... touch=1` (must stay DOWN)
- BURST residual QES **finger touch** not unknow — or still OOM with touch held
