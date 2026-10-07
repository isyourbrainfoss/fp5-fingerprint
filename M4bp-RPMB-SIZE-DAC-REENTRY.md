# M4bp — RPMB sb 0x6400 + DAC re-entry (2026-07-27)

## Context

M4bo fixed `image_acquisition_mode` string type (no JSON_INTEGER for that key).
Still: INIT serial `qsee_sfs_open` → SFS_ERROR_GENERIC (no RPMB block);
WMODE-5 AutoSDac try 1–2 with usMiddleTemp 863–868, never newDAC / Step1 /
Base update.

Android INIT (same chip SN): efc 128 → full FDT → Step1→newDAC→Step2→Step3→
Base update success. SFS works only with RPMB path.

## Changes

1. RPMB listener sb size **0x6400** (Android librpmb `rpmb_start`).
2. GET_INFO reports sectors=0x40 + sector_size=512.
3. After RPMB register: HLOS UFS BSG GET_WRITE_COUNTER (`rpmb_hlos_init`).
4. Force calib: re-enter WMODE-5 ×4 with SCAN only (no CAP mid-DAC).
5. Louder markers: newDAC / get image dac / SFS / get fdt dac success.

## Smoke

```
dmesg | grep -E 'rpmb_hlos_init|REGISTER_LISTENER id=8192|DAC-OK|Base update|Step1|newDAC|SFS/SERIAL|force img base done'
```

Do **not** cue go hold until Base update / get image dac success.
