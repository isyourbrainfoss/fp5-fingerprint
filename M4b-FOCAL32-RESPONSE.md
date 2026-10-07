# M4b breakthrough — `focal32` returns real TA text (2026-07-26)

## Headline

**Mainline SEND to `focal32` with the Android SO header is understood by the TA.**

Response (clean boot, load order **focal32 then fingerpr**):

```text
START focal32  → app_id=4
START fingerpr → app_id=5

SEND hdr=0x00000205 0x00000002 0 0  →  ret=0
rsp:
  2a 02 00 00 00 66 6f 63 61 6c 74 65 63 68 3a 74 61 3a 62 61 73 65 00 ...
  76 65 72 73 69 6f 6e 3a 20 27 76 ...

Decoded strings:
  "focaltech:ta:base"
  "version: 'v..."   (truncated in 48-byte dump; more in buffer)
```

First word `0x22a` (554) is likely **payload length** or a positive status/info code — **not** the failure pattern `0xffffffff`.

## What failed vs worked

| Target | Header `0x205 / 0x2` | Other cmds / wrong w1 |
|--------|----------------------|------------------------|
| **focal32** (id 4) | **version banner** | often `st=0xde` |
| **fingerpr** (id 5) | still `0xffffffff` | still `0xffffffff` |

So:

1. **Protocol family is confirmed** for FocalTech base TA (`focal32`).
2. **`fingerpr`** likely needs a different command set, prior INIT via `focal32`, or kernel/SPI sideband (`/dev/focaltech_fp`).

## Load order

App IDs depend on START order:

| Order | focal32 | fingerpr |
|-------|---------|----------|
| fingerpr first | 5 | 4 |
| **focal32 first** | **4** | **5** |

Prefer **focal32 first** (base / sensor library), then **fingerpr** (match).

## Re-test

```sh
# clean boot recommended
scp artifacts/qsee-modules/qsee_probe_fp5.ko artifacts/qsee-modules/qsee_focal_ok.ko fp5-pmos:/tmp/
ssh fp5-pmos 'sudo sh -c "
  mount -o ro /dev/disk/by-partlabel/modem_a /mnt/modem_a
  cp /mnt/modem_a/image/focal32* /mnt/modem_a/image/fingerpr* /lib/firmware/qsee/
  insmod /tmp/qsee_probe_fp5.ko force_pdev=1
  insmod /tmp/qsee_focal_ok.ko
  dmesg | grep qsee_ok
"'
```

Expect: `f32-base` line with `focaltech:ta:base` and `version:`.

## Next

1. Dump full 256–2048 byte rsp for `f32-base` → complete version string.
2. Map `0x205/0x2` meaning (likely “get info” / logcat-style) vs CREATE/INIT opcodes used by HAL after base is up.
3. Drive **fingerpr** after successful focal32 dialogue + L2 GPIO power/IRQ.
4. AUTH / templates / Phosh.

## Artifacts

- `artifacts/qsee-modules/qsee_focal_ok.ko`
- `artifacts/qsee-modules/qsee_session.ko` (contiguous buffer + region notify experiments)
- Source: `lab/fingerprint/oot-qsee/qsee_focal_ok.c`, `qsee_session.c`
