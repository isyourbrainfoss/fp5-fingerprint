# Fingerprint lab status (pin this, not chat memory)

Updated: 2026-10-07. Extra `a80e73f7c916e00cc19d65eed1074881b27e4e39471448af226069f7bdf6260a` is on disk and not loaded. Boot `e3836117-b2f4-4689-af55-b742c948beda`. One Register from the FP Fingerprint app loads it. A finger template is not stored yet. Do not `rmmod`. Do not insmod before that Register.

## Pinned boots (extra 4ffeed7f)

The trustlet success line on all three boots is `file.c:91, 'ff_file_open' leave.` Journals are the byte copies in `lab/fingerprint/evidence/`. None of them contains `SFS_ERROR`, `qsee_sfs_open`, `req_resp=1`, or `cmd=0x101`.

| Boot | Result |
|---|---|
| `f20406f6-20ce-4671-aad5-ae0f1041ebca` (22:43 UTC, create) | Live Alt directory removed before insmod. Leave line. RPMB `cmd=0x103` `result=0x0` write counter `111087..111093`. |
| `ea1a3165-b0a3-443a-9063-25588933c08b` (22:45 UTC, create) | Live Alt directory removed before insmod. Leave line. RPMB `cmd=0x103` `result=0x0` write counter `111101..111107`. Live segment mtime `2026-10-06 22:45:09 +0000`. |
| `118236e1-fa2e-4c1b-b28c-0241cad88add` (22:50 UTC, reopen) | Same segment left in place. Leave line. GPFILE READ n=50088 and n=4096 twice. No `SFS_ERROR`. No `cmd=0x103`. No GPFILE write. Live mtime still `2026-10-06 22:45:09 +0000`. Backup `Wc9lu7Hh.kept-8703` still `2026-10-06 21:42:51 +0000`. |

## Extra on disk (pmOS slot B, `7.2.0-nfc-test+`)

| | |
|---|---|
| Path | `/lib/modules/7.2.0-nfc-test+/extra/qsee_fingerpr.ko` |
| SHA256 | `a80e73f7c916e00cc19d65eed1074881b27e4e39471448af226069f7bdf6260a` |
| Loaded | **no**, boot `e3836117-b2f4-4689-af55-b742c948beda` (2026-10-07). Previous image `4ffeed7f…` was the 22:50 UTC reopen on `118236e1-fa2e-4c1b-b28c-0241cad88add`. Rem=0 now sends opcode `0x1014` plen 4 flag `0x40000000` (`dev-SAVE-template`). No Register on this boot yet. |

`framework_log_level` is 1. The declared INIT_DEV response is still `rsp0=0x12a`. The leave line sits further into the response ion (offset `0x526`). The driver prints that trustlet text and does not invent a `qsee_sfs_open` success. `serial-open outcome: absent` means the buffer had no `qsee_sfs_open` phrase. Success does not print that phrase. There is no `SFS_SUCCESS` string.

## No-finger init (2026-10-06 22:43 and 22:45 UTC, same extra)

Boots `f20406f6-20ce-4671-aad5-ae0f1041ebca` and `ea1a3165-b0a3-443a-9063-25588933c08b`. Extra `4ffeed7f…` loaded by each boot's `insmod` (return 0) after a normal reboot. `skip_enroll=1`. No Register. No `req_resp=1`. No `cmd=0x101`. No `SFS_ERROR`.

An intact serial file is not rewritten, so the live `..._Alt` directory was removed before each `insmod`. `Wc9lu7Hh.kept-8703` was left in place. Each init then created the segment again.

Trustlet text during `INIT_DEV`, same on both boots:

`device.c:118, '__module_serial_sync' enter.`

`'ft_fp_serial_id.bin' not exist, skip.`

`file.c:53, 'ff_file_open' enter.`

`file.c:91, 'ff_file_open' leave.`

`device.c:146, '__module_serial_sync' leave.`

`the new serial is synchronized to the file.`

`efc data not exist, skip.`

`file.c:91, 'ff_file_open' leave.` is the level-1 success line (`%s:%u, '%s' leave.`). The first `%s` is the source basename `file.c`, the number is line 91, and the quoted name is `ff_file_open`. It does not contain `qsee_sfs_open`.

RPMB `cmd=0x103` `result=0x0` at addr 2178: boot 1 write counter `111087..111093`, boot 2 `111101..111107`. GPFILE recreated `RQmyhOdY8ywyaGUyB-TI` (8192 bytes) and the segment file (50088 bytes). The 22:45 directory is the second create.

## Reopen of that segment (2026-10-06 22:50 UTC, same extra, directory left in place)

Boot `118236e1-fa2e-4c1b-b28c-0241cad88add`. Same extra `4ffeed7f…`, `insmod` return 0 after a normal reboot. The 22:45 `..._Alt` directory was not removed. `skip_enroll=1`. No Register. No `req_resp=1`. No `SFS_ERROR`. No `qsee_sfs_open`. No `cmd=0x103`. No GPFILE write.

`INIT_DEV` read the existing files, then the trustlet logged the same leave line:

`GPFILE READ '..._Alt/Wc9lu7HhpDADVWYMRw88zZxgXUmlrsVIzIrCf6-GmUA_Alt' n=50088`

`GPFILE READ '..._Alt/RQmyhOdY8ywyaGUyB-TI' n=4096` (twice)

`file.c:53, 'ff_file_open' enter.`

`file.c:91, 'ff_file_open' leave.`

`device.c:146, '__module_serial_sync' leave.`

`efc data not exist, skip.`

`serial-open listeners fs=0 rpmb=1`. `serial-open outcome: absent` because this success text has no `qsee_sfs_open` phrase.

`stat` before and after `insmod`, and before the reboot, all report the same timestamps. Live segment: `2026-10-06 22:45:09 +0000`, sizes 4096 / 8192 / 50088. Backup `Wc9lu7Hh.kept-8703`: `2026-10-06 21:42:51 +0000`, same sizes.

## Earlier read-back (extra 8e9f68b8, 22:21 and 22:22 UTC)

Disk extra at that time was `8e9f68b8e54200f0e5ac4605e9371c74e15fe4cb9aee59d0aa9714c327e558b6`, loaded on boot `d68886e0-f7be-488c-b62c-5bb6f684a49b`. It is not the loaded image now.

`focal32` `ff_file_open` prints `qsee_sfs_open(...)` only from the `file.c:82` failure format, at log level 5. A non-NULL handle logs level 1 and that line does not contain `qsee_sfs_open`. The logger drops level 1 while `diagnosis.framework_log_level` is above 1. Do not synthesize the open line.

## No-finger init (2026-10-06 22:21 and 22:22 UTC, same extra, read back)

Boots `42127567-71a3-4c08-bc9a-cadf03f2ab4b` and `d68886e0-f7be-488c-b62c-5bb6f684a49b`. Extra `8e9f68b8…` loaded by each boot's `insmod` (return 0). `skip_enroll=1`. No Register. No `req_resp=1`. No `cmd=0x103`.

The segment written at 22:18 was still on disk (`RQmyhOdY8ywyaGUyB-TI` 8192 bytes, segment file 50088 bytes). Each init read it:

`GPFILE READ '.../Wc9lu7HhpDADVWYMRw88zZxgXUmlrsVIzIrCf6-GmUA_Alt' n=50088`

`GPFILE READ '.../RQmyhOdY8ywyaGUyB-TI' n=4096` (twice)

`RPMB cmd=0x102` only (one counter read, one 6-frame read). Then:

`sn num: 0b0a6944171209070800fb120d1e1dff22`

`not exist, skip.`

`serial-open listeners fs=0 rpmb=1`

`serial-open outcome: absent`

`ION HIT dev-INIT_DEV-100b ret=0` `rsp0=0x32a`. No `SFS_ERROR_GENERIC`, no `SFS_NO_FILE`, no `reading 'ft_fp_serial_id.bin' failed`, no `the new serial is synchronized to the file.` The serial was already stored, the chip serial matched, and the trustlet did not write it again. The `not exist, skip` is efc. File mtimes stayed 22:18.

## Create that this pair read (2026-10-06 22:18 UTC, extra f4b85fde)

Boot `d46184c2-1538-4620-b73d-83569f70092d`. SHA256 `f4b85fde09acc330c71c5814501cfe31431a43352a5fc5a58763648bb3fe28e3`. `framework_log_level` was 1, to try to keep the level-1 leave line. `insmod` returned 0. `INIT_DEV` committed RPMB `cmd=0x103` `result=0x0` `wc=111045..111051` and the GPFILE writes above. The response shrank to `rsp0=0x12a` and contained neither `leave.` nor `qsee_sfs_open`. Threshold 1 fills the log before the open result is copied out. The integer is back at 2 on extra `8e9f68b8`.

## Unlink experiment (do not repeat)

Extra `a06d1a17` called `vfs_unlink` on GPFILE act 2. Boot `ba54aaea` deleted `RQmyhOdY8ywyaGUyB-TI` and the trustlet then printed `reading 'ft_fp_serial_id.bin' failed` and `I/O error`. The current extra only logs `GPFILE UNLINK` and leaves the inode.

There is still no finger sample. SAVE at rem=0 stays skipped. `template: true` is not stored data.

Register from the Phosh **FP Fingerprint** app only. Never `install-on-fp5.sh`. Never `qbootctl`. Never `rmmod` a live `qsee_fingerpr`.

`focal32` prints `qsee_sfs_open(...)` only from the `file.c:82` failure format. A working open has no such line. `serial-open outcome: absent` means that phrase was not in the trustlet buffer. The 21:42 / 21:45 boots below are free of `SFS_ERROR_GENERIC`, `SFS_NO_FILE`, and `__module_serial_sync` `Null pointer`. The loaded image is no longer extra `87037568`.

## No-finger init (2026-10-06 21:42 UTC, create)

Boot `043b477b-0539-4db5-835c-eb89b5dc4a30`. Same extra. `insmod` returned 0. `skip_enroll=1`. No Register. No `req_resp=1`.

GPFILE writes need the parent directory. This extra creates it before `filp_open`. During `INIT_DEV` the trustlet printed, in order:

`not exist, skip.`

`sn num: 0b0a6944171209070800fb120d1e1dff22`

`the new serial is synchronized to the file.`

`not exist, skip.`

The second `not exist, skip` is the efc line. The scanner starts at that substring, so the `efc data ` prefix is not in the journal. The create sentence is `__module_serial_sync(..., true)` returning 0. Listeners during that command: `fs=0 rpmb=8 time=0 ssd=0`.

RPMB `cmd=0x103` writes at addr 2178 returned `req_resp=0x300 result=0x0`. The write counter moved `111002..111008` (check-prov on this boot had read `111001`). GPFILE writes:

`GPFILE WRITE '.../Wc9lu7HhpDADVWYMRw88zZxgXUmlrsVIzIrCf6-GmUA_Alt/RQmyhOdY8ywyaGUyB-TI' off=0 n=4096`

`GPFILE WRITE '.../Wc9lu7HhpDADVWYMRw88zZxgXUmlrsVIzIrCf6-GmUA_Alt' off=0 n=50088`

and the same segment again, plus `RQmyhOdY8ywyaGUyB-TI` at `off=4096`.

## No-finger init (2026-10-06 21:45 UTC, read back)

Boot `08760e2b-25f8-4902-96a9-b085b8a2020f`. Same extra, loaded by this boot's `insmod` (return 0). `skip_enroll=1`. No Register. No `req_resp=1`. No `RPMB write resp`.

Check-prov read the counter at `wc=111015`. `INIT_DEV` then read the segment written above:

`RPMB xfer OK off=0x18 place=req rr0=0x400 res0=0x0 nbytes=3072 data0=40 55 42 4c` (`@UBL`)

`GPFILE READ '.../Wc9lu7HhpDADVWYMRw88zZxgXUmlrsVIzIrCf6-GmUA_Alt' n=50088`

`GPFILE READ '.../RQmyhOdY8ywyaGUyB-TI' n=4096` (twice)

Then the trustlet printed `sn num: 0b0a6944171209070800fb120d1e1dff22` and one `not exist, skip.` Listeners: `fs=0 rpmb=1`. It did not print `the new serial is synchronized to the file.` A missing serial file takes the create path and prints that sentence, or the `device.c:136` error. This boot did neither, so the serial file was found, the chip serial matched, and the trustlet did not rewrite it. The remaining `not exist, skip` is efc.

There is still no finger sample. SAVE at rem=0 stays skipped. `template: true` is not stored data.

Register from the Phosh **FP Fingerprint** app only. Never `install-on-fp5.sh`. Never `qbootctl`. Never `rmmod` a live `qsee_fingerpr`.

## No-finger init (2026-09-30, time and SSD before APP_START)

Filesystem, RPMB, SSD, and TIME were registered before `APP_START`, then again before `INIT_DEV`. No finger. No ENROLL.

- `FILE_SERVICE` / `SSD_SERVICE` / `TIME_SERVICE` `(before APP_START) ret=0` (TIME buffer `0x5000`)
- `START focal32 ret=0`
- TZ RPMB check-prov blocked on lid `8192`, then `RPMB xfer OK` `result=0x0` `wc=110858`, then `check-prov OK`
- `serial-open outcome: qsee_sfs_open('.../ft_fp_serial_id.bin') = SFS_ERROR_GENERIC`
- `serial-open listeners fs=0 rpmb=0 time=0 ssd=0`

The trustlet still does not call any of those listeners during the open. A listener reply cannot change this result. Key programming (`RPMB` cmd `0x101` and `req_resp=1`) stays refused. The 2026-09-29 run (extra `7961277d…`, counter `110851`) was the same open failure with only FS and RPMB registered.

## App Register + three Match tries (2026-09-30 19:32–19:34 UTC)

Log dir `/home/user/fp-logs/20260930T193201Z-805704`. Samples 60. `ENROLL_OK` at 19:33:59. fid `0x47ec2254`. rem 19→0 across samples 0–21. Last accepted frame avgv 274. Every enroll CAP stayed `slots=0x1 inte=0`. `HEALTH final er=0`.

Save did not run. At rem 0 the overlay logged `overlay skip SAVE rem=0 (rearm FDT for Match)`. The enrolled mark file is that rem=0 mark. The app score `template: true` is the same mark.

Before enroll, `SET_GROUP` blocked on RPMB lid 8192. One captured frame was `RPMB xfer OK off=0x18 result=0x0 wc=0` with `req_resp=0x4`, then `SET_GROUP` `err=0xfffffffe` and `ENUM` `err=0xffffff35`. Pre-ENROLL registered FS, RPMB, SSD, TIME, and GPFILE, all ret 0. The `serial-open` line is not in this log; the follow starts at `SET_GROUP`.

Match was three kicks on the same loaded extra, 19:34:01, 19:34:14, and 19:34:25. Each one armed `FDT_DOWN_DETECT` and skipped `FDT_DOWN` mode 1. Each one saw leftover `itype=0x201` (ESD) then idle `itype=0x1`, then `overlay AUTH kick no itype 0x1 — no CAP` (that `0x1` is the last interrupt, not the type it was waiting for). No exact `itype 0x2`, no capture. Each ended `AUTH FAIL err=0xfffffff0 tmpl_missing=0 id_ok=0` and `WMODE-SLEEP-auth-fail`. The screen hero is `NO MATCH`.

## Still true from earlier registers

- Enroll in RAM reaches rem 0 with exact itype `0x2`. Save stays skipped or `write=0`.
- Match ends `AUTH FAIL 0xfffffff0` with no picture. Do not CAP itype `0x12`.

## No-finger init (2026-10-06 20:22 UTC, GPFILE on)

Extra `501d1a3f…` (the image that is still running). `skip_enroll=1`, no finger. `START focal32 ret=0`. Listeners registered before `INIT_DEV`, including GPFILE id `0x7000` length `0x7e000`. TZ RPMB check-prov OK, write counter `110872`.

During `qsee_sfs_open` of `ft_fp_serial_id.bin`: one GPFILE command 12 (request bytes at +4 were empty), then three RPMB reads (`cmd=0x102`, `nfr=6`, `req_resp=0x4`, off `0x18`, UFS `result=0x0`, `wc=0`). Then `SFS_ERROR_GENERIC`. File-service requests during the open: 0. No RPMB write (`0x103`). No new file under `/mnt/persist`.

`libdrmfs` answers command 12 with the 8-byte value 2 at request+4 (`QSEECom_send_resp` length 12). The running driver stored a 4-byte 2. `librpmb` `rpmb_ufs_read` writes a multi-frame response at shared-buffer+`0x14` and stores `0x14` at +`0xc`. The running driver left the 6-frame response at +`0x18` and cleared the count at +4.

## No-finger init (2026-10-06 20:44 UTC, disk extra c67eb544)

Reboot, then the same no-finger init. `insmod` returned 0. `START focal32 ret=0`. GPFILE command 12 request was `0c 00 00 00` followed by eight zero bytes. Check-prov write counter `110879` (`rr=0x200`).

The three reads inside the open are real responses, not an echoed request: `addr=2178`, `bcnt=6`, response `req_resp=0x400`, first and last frame `result=0`, data starts `40 55 42 4c 06 00 00 00` (`@UBL`). The driver placed those frames at shared-buffer+`0x14` and set +`0xc` to `0x14`. The open still returned `SFS_ERROR_GENERIC` (`fs=0 rpmb=3`). TrustZone then repeated the same address. No `0x103` write.

Placing the multi-frame response at +`0x14` does not make the open succeed. The 1-frame counter read still succeeds with the response left at +`0x18`.

## No-finger init (2026-10-06 21:24 UTC, disk extra b34ca1ae)

Boot `f4acadc3-7e1d-4103-b255-5adffa0318b9`. `insmod` returned 0. No Register.

The trustlet's own open line is:

`qsee_sfs_open('/data/vendor_de/0/fpdata/ft_fp_serial_id.bin', ..) = 'SFS_NO_FILE: No such file'.`

Same buffer: `not exist, skip.` and `__module_serial_sync[device.c:136]: 'Null pointer'.` The chip serial was read (`sn num` matches the 17-byte `/mnt/persist/qti_fp/ft_fp_serial_id.bin` from Sep 14). The trustlet does not create the SFS file on this path.

RPMB writes do commit. A 2-frame `cmd=0x103` at addr 2178 returns `req_resp=0x300 result=0x0`, and the write counter advances (this boot 110955..110994, 40 writes). The 6-frame read beforehand is still `req_resp=0x400 result=0` data `40 55 42 4c` (`@UBL`). No `req_resp=1`, no listener `0x101`.

How the replies got here, each a different shape:

- `c67eb544` (20:44, boot `622231c6`) copied the 6-frame read to +`0x14`. Open stayed `SFS_ERROR_GENERIC`. No `0x103`.
- `9ccccb12` (21:09, boot `b410c4f5`) left the 6-frame read at the request offset and stored `nfr*512` at +8. Open became `SFS_VERI_HMAC_ERROR`. The trustlet then issued `0x103`, but a one-frame OUT came back `result=0x1` and the counter stayed 110893.
- `92f42c34` sent every write frame, then a `req_resp=5` result read, and returned 512 bytes at +`0x14` with +8 = 512 and +`0xc` = `0x14`. Writes returned `result=0x0` and the counter moved (110908…). Open was `SFS_NO_FILE`.
- `b34ca1ae` is that write path plus logging of every serial line. Same open, same committed writes. `misc/` has no GPFS files. File-service requests during the open: 0.

On this extra, another listener-reply shape does not put `ft_fp_serial_id.bin` into the HMAC segment. `__module_serial_sync(..., false)` skips a missing file instead of creating it. The 21:42 extra above creates the GPFILE parent directory, and that is what lets the trustlet write the file.

## Next

Do not `rmmod` the loaded extra on boot `08760e2b`. Another code change loads only after a normal reboot. Do not Register on this boot. Do not program the RPMB key. Do not add a `0x104` handler. Leave SAVE skipped. The serial file is the stored object. A fingerprint template still needs a Register after this, which this boot must not do.
