# M4dx — CAPTURE rsp overlay on image plane (no finger, 2026-09-06)

Hypothesis: HAL `send_cmd` send_len is header+0x24 and resp is `usr+img_off`,
so the frame is the TZ **response** region (skips `array_new`). Lab
`send_ion` always uses req `0x78000` / rsp `@+0x78000`.

Probe: `app_send_ex` CAPTURE `req_sz=0x34`, `rsp=sess+0x34`, `rsp_sz=0x4d00`.

## Hold result

```
M4dx CAP req_sz=0x34 rsp=usr+0x24 rsp_sz=0x4d00
M4dx CAP er=-22 ms=0 app_err=0x0 nz=0
```

TZ **rejects** that split (`SCM … status=-22`, no TA log). The 0x78000/0x8040
send (M4dv) is the one that actually reaches img_scan then `array_new` OOM.
Stop send-length guessing.

Next: kernel `TZMEM_MODE_SHMBRIDGE` + flash boot_b when fastboot exists.
No press. GTK Register parked.
