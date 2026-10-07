# M4ck — layout=1 CAP OOM/Null (idle, pure-SW)

**Date:** 2026-07-28  
**Scope:** Idle only (no finger). Dual-boot safe.

## Root cause

| Layout | Form | Full 112×88 + image/soft | Result |
|--------|------|--------------------------|--------|
| 0 | img_off@+0xc, nbytes@+0x10 | FDT soft | soft success, rows often 0 |
| 1 | nbytes@+0xc, img_off@+0x10 | 112×88 | **`ff_sensor_data_array_new` OOM → capture Null `0xffffff37`** |
| 1 | same | **16×16 soft** | **app_err=0**, no OOM/Null |

Large-plen CAP (`plen=0x24+image`, M4ap) before layout1 image CAP **sticky-Nulls** later APP_SEND. Force multi-flag thrash after Null made it worse.

## Fix (`qsee_fingerpr.c`)

1. **`cap_big_plen=0` default** — never large-plen CAP.
2. **Layout1 zero image plane** in shared ion when filling ctx.
3. **PA@+0x14** only for image flags or `cap_img_pa=1` (always-on PA Null'd soft CAP).
4. **`idle_layout1_cap=1`**: after FDT arm, run **tiny (16×16) layout1 CAP** + residual + HEALTH + QUERY; skip force thrash when `cap_ctx_layout=1`.
5. Force-image aborts on first Null + HEALTH recover.
6. AFR no longer forces layout=1 (full 112×88 OOMs TA heap).

## Idle smoke

```sh
# phone: dual-boot safe L2 + qsee
scp artifacts/qsee-modules/qsee_fingerpr.ko artifacts/fp-modules/focaltech_fp_life.ko \
    lab/fingerprint/fp-idle-layout1-smoke.sh fp5-pmos:/tmp/
ssh fp5-pmos 'sh /tmp/fp-idle-layout1-smoke.sh'
# expect: M4ck idle layout1 CAP OK; oom=0; HEALTH/QUERY app_err=0
```

## Evidence (accept)

| Marker | Result |
|--------|--------|
| `dev-cap-m4ck-tiny` app_err | **0** |
| `Out of memory` / `ff_sensor_data_array_new` | **0** on accept path |
| HEALTH/QUERY after CAP | **app_err=0** (not sticky) |
| qbootctl / pinmux modules | not used |

Prior residual (pre-fix full 112×88):  
`error at ff_sensor_data_array_new[image.c:105]: 'Out of memory'` + capture Null.

## Remaining gap (not this goal)

Full **112×88** layout1 image CAP still needs larger TA heap or true Android builtin-ion bind; not claimed fixed. Finger rem/AUTH still needs user `go`.
