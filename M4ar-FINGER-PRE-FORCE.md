# M4ar — force-image FIRST under finger (2026-07-27)

## Enroll sample path change

When `ta_down` / finger edge:

1. **`try_force_image_path(..., ta_down=true)` first** (Android: frame raw during  
   `transmit_image` before enrolling rem).
2. If IMAGE PIXELS / rem markers → feed SAVE/REPORT and mark rem.
3. Else continue flag-sweep meta CAP at **img_off=0x24** (was 0x100).
4. **No a5 paint** when finger down.
5. **`capture_ctx_fix_rows`** after CAP (layout 0 zeros rows).
6. Post-CAP force only when **not** already pre-forced under finger.
7. **No large-plen CAP** (sticky Null).

## Smoke (no finger)

`fp-m4ar-smoke.log`: SN, fdt_ok, force-image IMAGE PIXELS (SRAM nz=24), HEALTH 0.

## Hold

```sh
ssh fp5-pmos 'sh /tmp/do-enroll-m4al.sh'
# 6× press ~1s / lift on power-button FP
```

Look for: `PRE-FORCE rem markers`, `frame raw`, `rem_seen=1`, AUTH without `broken off`.
