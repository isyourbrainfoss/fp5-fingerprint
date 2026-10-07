# M4cj — AFR layout=1 + img_off + DEAD_PIXEL (no under-finger SPI)

## M4ci result
Under REAL DOWN: SET_SPI 4M/8M + ALT 0x41 still **cidri f:N**, CAP **~1s** FDT
(worse than pre-SPI thrash ~0.64s). QUERY/SPI still saw finger touch.

## M4cj
1. **Drop** under-finger SET_SPI / flag thrash (hurt).
2. AFR uses **cap_ctx_layout=1** (rows kept after CAP; M4ao).
3. Log **capture_ctx pre** (flag/cols/rows).
4. CAP0 FDT-slow → rest frames **img_off=0x100**.
5. Still FDT → **CHECK_DEAD_PIXEL 0x1020** then one CAP (image-adjacent cmd).
6. Keep no skipcap CAP (M4ci).

## Hold
Look for: `layout=1`, `afr ctx pre flag=0x240`, `img_off=0x100`, `afr DEAD`,
`frame raw` / `rem_seen`.
