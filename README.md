# Fairphone 5 — fingerprint (Focaltech)

> **Note**
> This is very experimental. Assume nothing here is written, or necessarily read through or fully understood, by a human unless stated, and that includes this text. Use it at your own risk, but you're encouraged to reuse any parts you find useful. The human work here is mainly ideas, testing in reality, and persistence with a vision.

Linux bring-up for the power-button fingerprint sensor on Fairphone 5
(Qualcomm SM7325 / sc7280, postmarketOS). An out-of-tree module talks to the
signed `focal32` trustlet. The trustlet binary is not in this tree.

Status: **experimental**. Not an official Fairphone or distribution package.

## Where this stands

Enroll on a real finger can finish in RAM: the trustlet counts the remaining
samples from 19 down to 0, with a real finger-down (`itype` `0x2`).

The chip serial file `ft_fp_serial_id.bin` is stored and survives reboot.
Success is the trustlet line `file.c:91, 'ff_file_open' leave.` Two create
boots committed RPMB `cmd=0x103` with `result=0x0`. A later boot read the same
segment and did not write it again. Journals are in `evidence/`. Details are
in `STATUS.md`.

A finger template is not stored yet. When the remaining count hits 0, the
current module sends Android's enroll-completion save: `ff_trustlet_save_data`,
opcode `0x1014`, length 4, flag word `0x40000000` (`dev-SAVE-template`), then
loads the group at `/data/vendor_de/0/fpdata`. Opcode `0x1015` with bit 30 is
the short quality command and does not write the finger. That save has not
been proven with a Register on this tree. Match and Phosh unlock are not done.

## Repository layout

```
oot-qsee/    qsee_fingerpr.c (the module in use) and earlier probe sources
app/         Phosh "FP Fingerprint" session
fprintd/     fprintd-compatible daemon that calls the same enroll helper
walkthrough/ Register helper used by the app
tests/       Source-contract tests for the overlay and the serial-file boots
evidence/    Measured journals for the two serial creates and the reopen
STATUS.md    Pinned boots and the on-disk module
```

Phone sudo passwords are not in this tree.

## Build

`qsee_fingerpr.ko` loads only when `uname -r` is `7.2.0-nfc-test+`.
Build out of tree against that kernel:

```sh
make -C /path/to/linux-sc7280-7.2 M="$PWD/oot-qsee" ARCH=arm64 LLVM=1 qsee_fingerpr.ko
```

Copy the module to `/lib/modules/$(uname -r)/extra/qsee_fingerpr.ko` and reboot
to load it. Do not `rmmod` a live `qsee_fingerpr`. One Register per boot, from
the FP Fingerprint app, and only when the module is not already loaded.

```sh
python3 -m unittest tests/test_qsee_fingerpr_overlay.py
```

## Do not

- Program or erase the RPMB key (`req_resp=1`, listener command `0x101`).
- Unlink the secure-storage segment. A real unlink makes the serial open fail
  with an I/O error. The driver only logs `GPFILE UNLINK`.
- Treat the app mark `template: true` as a stored finger. That mark is the
  in-RAM remaining count.

## License

GPL-2.0-only for the kernel module. See `LICENSE`.
