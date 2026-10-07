# M4bx — mode 5 invalid; INIT-only Base path (2026-07-27)

## Gold residual (mode 5)

```
chips config mode: 5(0=sleep 1=wait touch 2=wait leave 6=gesture 7=vio_1p8 8=io_strength)
unknown device mode → FDT_DOWN for power
SFS_ERROR_GENERIC / calib_data_save Null
```

Mode **5 is not a valid chips config mode**. Prior "WMODE-5 img calib"
was always the unknown-mode recovery path. Step1 try1–2 was a side-effect;
Base never completed (calib save Null after SFS).

## Fix

- Retire mode 5 for img force
- After FDT (mode 1): mode 7 vio_1p8 → mode 1 wait-touch → SCAN
- INIT_DEVICE ×4 @4M/8M with full residual dump
- Image CAP 0x240 after each INIT

Android Base still needs serial SFS; this removes the invalid-mode dead end.
