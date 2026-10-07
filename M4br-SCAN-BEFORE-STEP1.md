# M4br — SCAN before WMODE-5 AutoSDac (2026-07-27)

## Diagnosis

M4bp reached Step1 + try1–2 (middle 862–868) but never `newDAC`.
AutoSDac (`ft93xx_img_auto_sdac_update_navmode`) runs **inside** the WMODE-5
APP_SEND. Prior order:

1. WMODE-5 → AutoSDac try1–2 finishes (SPI not streaming)
2. START_SCANNING (too late)

Android INIT keeps SPI continuous from FDT into Step1.

Also M4bq VDD cycle before Step1 wiped FDT base (try0 step=0).

## Fix

1. LOGCAT drain pre-Step1 (clean residual)
2. SLEEP → PWR → **START_SCANNING** → **WMODE-5**
3. Immediate dual LOGCAT + full STEP1-RES dump
4. Soft ESD only (no VDD)
5. Markers: effective range / Left_Point / case / restore last dac

## Success

`newDAC` / `get image dac [success]` / `Base update success`
