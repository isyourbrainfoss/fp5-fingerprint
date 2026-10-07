# M4dt — skip INIT_DEV on reattach (2026-09-05)

## Why
M4ds: no diagnostic matrix, still **28ms** 112×88 `array_new` OOM.
Log still ran **INIT_DEV Step1–3** (`get raw img`) on `do_start=0`
reattach. That mallocs nav/raw again; the finger CAPTURE then
`array_new`s a second 112×88 and dies. Cue/press were good.

## Change
If `do_start=0` (TA already in TZ): SYNC_CFG + SPI PROBE + HEALTH only.
Skip INIT_DEV / CHIP / INIT / SYNC_CALIB / DEAD_PIXEL. Assume Base from
this boot. Keep M4dr native CAP.

## One press
Hands off until **one flash**, hold ~2s.
Want `M4dt reattach — skip INIT_DEV` and either `FRAME RAW M4dr` or
the same 28ms OOM (then INIT is not the leftover heap; stop Register).

## Hold result (2026-09-05)
**Did not run.** Ko was on the phone at 21:49Z; GTK **NO IMAGE** had Register
disabled; last dmesg is still M4ds then `qsee_fp: exit`. Skip INIT on a TA
that already ran INIT this boot also would not free the mallocs. Superseded
by **M4du** (SHUTDOWN + START + skip INIT).
