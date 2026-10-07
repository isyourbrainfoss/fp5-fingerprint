# M4k — Auth path after SPI open (2026-07-26)

## Device path (focal32) is healthy

With nested JSON SYNC (`cfg_style=3`), clean boot:

| Cmd | err | Notes |
|-----|-----|--------|
| INIT_SPI / PROBE / INIT_DEV / INIT / CHIP / HEALTH | **0** | SN `0b0a6944171209070800fb…`, foundry nexchip |
| **SET_ACTIVE_GROUP (0x2007)** | **−2** (`0xfffffffe`) | **“loading templates for gid=0… there is 0 template loaded.”** |
| START_SCANNING (0x1012) | **0** | Accepted |
| QUERY_EVENT (0x101c) | **0** | |
| AUTHENTICATE (0x2008) on focal32 | SCM **−22** | Wrong TA or framing |
| AUTHENTICATE on fingerpr | SCM OK, app **`0xffffffff`** | Framing still wrong for algorithm TA |

Evidence: `lab/fingerprint/qsee-auth-path-full.txt`, SPI: `qsee-SPI-WORKS-nested-json.txt`

## Command map (HAL constant pool)

| Opcode | Name |
|--------|------|
| 0x1012 | START_SCANNING (plen 0x30) |
| 0x101f | CONFIG_WORK_MODE (plen 4) |
| 0x1022 | CHIP_PARAMETER_INIT |
| 0x2007 | SET_ACTIVE_GROUP (gid u32 + path string) |
| 0x2008 | AUTHENTICATE (plen 0xe) |

## Interpretation

1. **SPI + device stack = working** on pmOS (L3+).
2. **Template store is empty** for all paths tried on clean boot:
   - `/data/vendor_de/0/fpdata` (Android)
   - `/data/vendor/focaltech`
   - `/var/lib/fp5-fp`
   → always **“0 template loaded”**. Android enrollments are not visible under these SFS keys from pmOS (or TZ templates are slot/user-scoped elsewhere).
3. **Unlock path:** enroll on pmOS (`PRE_ENROLL` / `ENROLL` / `POST_ENROLL` + finger on sensor), then AUTH.
4. **fingerpr AUTH** returns app `0xffffffff` (framing); focal32 AUTH is SCM −22 (device TA doesn’t take 0x2008).

## Module

`qsee_fingerpr.ko`: default `cfg_style=3` (nested JSON); after device init tries SET_GROUP paths + SCAN + AUTH.

## Next

1. ~~Clean-boot SET_GROUP~~ → always 0 templates (see M4l).  
2. ~~PRE_ENROLL~~ → **done** (challenge returned). **ENROLL SCM −22 kills TA** — see `M4l-ENROLL-PATH.md`.  
3. Implement QSEECom listener / blocked-on-listener handling, or low-level `CAPTURE_IMAGE` path.  
4. Greeter glue once AUTH returns fid.
