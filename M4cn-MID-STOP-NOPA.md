# M4cn — mid ladder, stop on OOM, no-PA retry

## Why (M4cm hold)
| Dim | Result |
|-----|--------|
| 16×16 flag 0x240 | soft app_err=0 rows=0 **ms≈11s** (FDT waste) |
| ≥32×32 | instant **OOM** array_new |
| climb after OOM | useless |
| wall | ~13s, rem=0 |

## Change
1. **CANCEL + HEALTH** pre-free
2. Mid ladder only **28×24 → 32×24** (no 16×16, no 112×88 climb)
3. **Stop on first OOM** + HEALTH
4. Soft ms≥200 after return → abort climb (no further rungs)
5. **One no-PA** CAP at last mid dim (`cap_ctx_suppress_pa`)
6. One L0; REPORT; residual only if wall &lt; 3s

## Hold expect
- `android-frame-raw M4cn`
- `afr L1 mid0` … OOM or frame; `afr L1 nopa`
- wall ≪ 13s unless L0 slow
