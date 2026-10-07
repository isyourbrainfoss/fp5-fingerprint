# M4cq — Android enroll-shaped AFR (meta CAP ×3)

## Oracle (fp-enroll-live-20260728-150808)
Under finger Android does **not** re-calib / BIG plen / DEAD:
```
FDT_DOWN → itype 0x2 + finger touch
→ frame raw[0..2]  (~37ms transmit_image)
→ enrolling rem=N → FDT_UP
```
`sb_length=0x80040` (= our SESS_BIG). `capture_ctx == g_builtin_buffer->usr`.
**Zero** array_new OOM.

## Change
1. Short settle clean  
2. **No** SYNC_CALIB / SCAN / CANCEL / BIG / DEAD / pull / L0  
3. layout1 meta **plen 0x24** flag `0x240` **×3** (~12ms gaps)  
4. **suppress PA**@+0x14 (builtin usr bind)  
5. Stop burst on first OOM; residual once; REPORT  

## Hold expect
- `android-frame-raw M4cq`
- `afr BURST0/3` … frame raw **or** OOM stop
- wall ≪ 1s if OOM-fast; rem if Android path opens
