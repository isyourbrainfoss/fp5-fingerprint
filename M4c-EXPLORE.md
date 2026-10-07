# M4c — focal32 command surface + fingerpr (2026-07-26)

## Live version identity (focal32) — full string

Header `0x00000205 / 0x00000002` on **focal32** (load order: focal32 first → id **4**):

```text
focaltech:ta:base
version: 'v2.5.3.0-cdf0f80-230614@0928'.
TEE environment checking...
testing timing...0(0)ms.
testing msleep...
```

- First dword **0x22a (554)** ≈ total useful payload length  
- Multi-record log: records separated by `21 03 00 00`  
- TEE self-checks **pass** on mainline SCM path  
- Build id: **cdf0f80**, date **230614@0928** (2023-06-14)

## Second known command: `0x1011`

From Android `fingerprint.default.so` post-send path. On focal32:

```text
hdr=0x00001011 0 0 0  →  st=0x32a (810)
strings:
  focaltech:ta:entry
  keymaster identity: ''.
```

Empty keymaster identity is expected without KM binding on pmOS.

## Command scan summary (focal32)

| Pattern | Typical status | Notes |
|---------|----------------|-------|
| `0x205 / 0x2 / 0 / 0` (first call) | **0x22a** + version log | One-shot “base info” |
| Same header again / w2≠0 | **0xde (222)** | Reject / bad state after first use |
| `0x1011` | **0x32a** + entry log | Second channel |
| Most other w0/w1/w2 | **0xde** | |

## fingerpr (id 5 after focal32-first)

All scanned headers still **0xffffffff**, including `0x205/0x2` and `0x1011`.

Combined with L2 this run:

- `/dev/focaltech_fp` present, `listen=1`, IRQ 34 registered  
- Did **not** change fingerpr SEND outcomes  

So fingerpr needs either a different wire format, prior multi-step init via focal32, or TA-mediated SPI that our L2 chardev does not yet expose to the TA.

## App IDs (this run)

| TA | app_id |
|----|--------|
| focal32 | 4 |
| fingerpr | 5 |

## Artifacts

- `artifacts/qsee-modules/qsee_explore.ko`  
- Full dmesg capture: session implementer `qsee-explore-run.txt`  
- Source: `lab/fingerprint/oot-qsee/qsee_explore.c`  

## Re-test

```sh
# clean reboot if START returns -22
scp artifacts/qsee-modules/qsee_{probe_fp5,explore}.ko artifacts/fp-modules/focaltech_fp_life.ko fp5-pmos:/tmp/
ssh fp5-pmos 'sudo sh -c "
  mount -o ro /dev/disk/by-partlabel/modem_a /mnt/modem_a
  cp /mnt/modem_a/image/{focal32,fingerpr}* /lib/firmware/qsee/
  insmod /tmp/qsee_probe_fp5.ko force_pdev=1
  insmod /tmp/focaltech_fp_life.ko; echo 1 > /sys/class/focaltech_fp/focaltech_fp/listen
  insmod /tmp/qsee_explore.ko
  dmesg | grep -E \"version:|ta:entry|done f32\"
"'
```

## Next

1. Treat `0x205/0x2` as **log/info** and reverse how HAL issues **CREATE/INIT** after base (likely different first-word or shared ion session).  
2. Trace **fingerprint_invoke_command** in `fingerprint.default.so` for buffer layouts after start_app.  
3. Try **fingerpr** with session buffer size `0x80040` + region notify before any SEND.  
4. If needed: Android strace of one unlock for exact `QSEECom_send_cmd` lengths.  

## Roadmap

| Milestone | Status |
|-----------|--------|
| M4b focal32 version string | **Done** (full `v2.5.3.0-cdf0f80-230614@0928`) |
| M4c command map + fingerpr | **Partial** — map growing; fingerpr still closed |
| M5 session unlock | Pending |
