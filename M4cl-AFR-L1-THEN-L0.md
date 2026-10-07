# M4cl — REAL DOWN: layout1 image CAP then short L0 fallback

## Why
Last hold: layout=0 AFR burned **~13s** (CAP ~1s + DEAD thrash), residual **f:N**, rem=0.
M4cj: layout=1 enters image path (fast CAP) but full 112×88 can **OOM** then Null;
HEALTH clears sticky (M4ck).

## Change
`try_android_frame_raw`:
1. **L1** full 112×88 flag 0x240 × up to 2 (HEALTH between if Null)
2. Else **one** L0 CAP only (no triple / DEAD)
3. REPORT once

## Hold
Expect: `afr L1 CAP`, residual OOM or frame raw; `afr L0` if fallback; wall ≪ 13s.
