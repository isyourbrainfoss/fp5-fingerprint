"""Shipped overlay extra: 8M-class spi_capture_bps key stays; idle skip_enroll CAP."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / "oot-qsee" / "qsee_fingerpr.c"
STATUS = Path(__file__).resolve().parents[1] / "STATUS.md"
EVIDENCE = Path(__file__).resolve().parents[1] / "evidence"
LEAVE = "file.c:91, 'ff_file_open' leave."


class OverlayExtra(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.src = SRC.read_text(encoding="utf-8", errors="replace")

    def test_source_exists(self) -> None:
        self.assertTrue(SRC.is_file(), SRC)

    def test_persist_pmos_contract_extra_and_register_helper(self) -> None:
        """Shipped extra + Register helper: TEE persist contract on pmos."""
        helper = (
            Path(__file__).resolve().parents[1] / "walkthrough" /
            "fp-walkthrough-enroll.sh"
        ).read_text()
        self.assertIn('listeners_register_enabled("before INIT_DEV")', self.src)
        self.assertNotIn('listeners_register_enabled("pre-SAVE")', self.src)
        self.assertIn("dev-SET_GROUP-persist", self.src)
        self.assertIn(
            "overlay SAVE template rem=0 opcode=0x1014 flag=0x40000000 (rearm FDT for Match)",
            self.src,
        )
        self.assertNotIn("dev-SAVE-data-rem0", self.src)
        self.assertNotIn("dev-WMODE-FDT_UP-presave", self.src)
        self.assertIn("overlay persist no WRITE (SET_GROUP is load-only)", self.src)
        self.assertIn("overlay SET_GROUP persist load after rem=0 (gid=0)", self.src)
        self.assertIn('const char *gpath = "/data/vendor_de/0/fpdata"', self.src)
        self.assertNotIn("overlay skip LOGCAT rem=0", self.src)
        self.assertNotIn("dev-WMODE-SLEEP-rem0", self.src)
        self.assertIn("/dev/disk/by-partlabel/persist", helper)
        self.assertIn("mount_persist", helper)
        self.assertIn("enable_fs_listener=1", helper)
        self.assertIn("enable_rpmb_listener=1", helper)
        open_flags = re.search(
            r"case 0x02: /\* open[^*]*\*/ \{\s*"
            r"/\*[^*]*never O_TRUNC[^*]*\*/\s*"
            r"int oflags = ([^;]+);",
            self.src,
        )
        self.assertIsNotNone(open_flags)
        self.assertNotIn("O_TRUNC", open_flags.group(1))
        self.assertIn("O_RDWR", open_flags.group(1))
        self.assertIn("O_CREAT", open_flags.group(1))
        self.assertIn("O_SYNC", open_flags.group(1))
        map_fn = re.search(
            r"static void fs_map_path\(const char \*in.*?\n\}",
            self.src,
            re.S,
        )
        self.assertIsNotNone(map_fn)
        self.assertIn("/data/vendor_de/0/fpdata", map_fn.group(0))
        self.assertIn("/mnt/persist/qti_fp", map_fn.group(0))
        reg_opts = re.search(
            r'else\n  ENROLL_OPTS="([^"]*skip_enroll=0[^"]*)"',
            helper,
        )
        self.assertIsNotNone(reg_opts)
        self.assertIn("defer_listeners=0", reg_opts.group(1))
        self.assertIn("enable_fs_listener=1", reg_opts.group(1))
        self.assertIn("enable_rpmb_listener=1", reg_opts.group(1))
        gpaths = re.search(
            r"static const char \*const gpaths\[\] = \{\s*"
            r"\"([^\"]+)\"",
            self.src,
        )
        self.assertIsNotNone(gpaths)
        self.assertEqual(gpaths.group(1), "/data/vendor_de/0/fpdata")
        get_info = re.search(
            r"case RPMB_CMD_GET_INFO:.*?ufs_rpmb_sec_xfer\(frm, RPMB_FRAME_SIZE, false\);",
            self.src,
            re.S,
        )
        self.assertIsNotNone(get_info)
        self.assertIn("ufs_rpmb_sec_xfer", get_info.group(0))
        self.assertNotIn("soft-ok", get_info.group(0))
        self.assertNotIn("GET_INFO soft-ok", self.src)
        self.assertIn("hdr_off = le32_to_cpu(*(__le32 *)(sb + 0xc))", self.src)
        self.assertIn("if (rr < 1 || rr > 5)", self.src)
        self.assertIn("nfr = le32_to_cpu(*(__le32 *)(sb + 0x4))", self.src)
        xfer = re.search(
            r"static int ufs_rpmb_sec_xfer\(void \*buf, size_t len, bool send\)\s*\{.*?\n\}",
            self.src,
            re.S,
        )
        self.assertIsNotNone(xfer)
        self.assertIn("scsi_execute_cmd", xfer.group(0))
        self.assertNotIn("unlocked_ioctl", xfer.group(0))
        self.assertNotIn("SG_IO", xfer.group(0))
        self.assertIn("QSEECOM_TZ_CMD_RPMB_CHECK_PROV", self.src)
        self.assertIn("TZ RPMB check-prov", self.src)
        self.assertIn("scm_os_rpmb_check_prov", self.src)
        self.assertNotIn("QSEECOM_TZ_CMD_RPMB_PROVISION", self.src)
        self.assertNotIn("QSEECOM_TZ_CMD_RPMB_ERASE", self.src)
        self.assertIn("Never 1=provision or 2=erase", self.src)

    def test_overlay_auth_once_has_no_msleep_1500(self) -> None:
        """Gating: one cue LED, 1s hold, enroll-flag CAP. No QEV/irq storm."""
        auth_once = re.search(
            r"static void overlay_auth_once\(void\)\s*\{.*?\nauth_have_frame:",
            self.src,
            re.S,
        )
        self.assertIsNotNone(auth_once)
        body = auth_once.group(0)
        self.assertNotIn("msleep(1500)", body)
        self.assertNotIn("dev-QEV-auth-kick", body)
        self.assertIn("dev-QEV-auth-down", body)
        self.assertNotIn('"dev-WMODE-FDT_DOWN-auth"', body)
        self.assertIn("overlay AUTH skip FDT_DOWN-auth (mode 1 ESD)", body)
        self.assertIn("dev-WMODE-FDT_DOWN_DETECT-auth", body)
        self.assertIn("overlay AUTH FDT_DOWN_DETECT-pre-wait (same as enroll)", body)
        self.assertNotIn("dev-WMODE-SLEEP-auth-before", body)
        self.assertNotIn("white:flash/brightness", body)
        self.assertIn("focaltech_fp/irq_count", body)
        self.assertNotIn("msleep(1000)", body)
        self.assertIn("overlay AUTH skip leftover itype=0x%x esd=%d", body)
        self.assertIn("overlay AUTH QEV itype=0x%x esd=%d irq=%u->%u", body)
        self.assertIn("last_send_itype == 0x2 && !last_send_esd", body)
        self.assertNotIn("(last_send_itype & 0x7) == 0x2", body)
        self.assertIn(
            "sample %u REAL DOWN QEV-s (touch=%d itype=0x%x) — skip QFINGER",
            self.src,
        )
        self.assertIn("overlay AUTH REAL DOWN itype=0x%x — enroll-flag CAP", body)
        self.assertIn("/vendor/focaltech", self.src)
        self.assertNotIn('persist_data_home\\":\\"/mnt/persist/qti_fp\\"', self.src)
        self.assertIn("overlay_enroll_flag_caps", body)
        self.assertIn("overlay AUTH kick empty avgv=%u — no match CAP", body)

    def test_nested_json_keeps_spi_capture_bps_key(self) -> None:
        self.assertIn(r'"\"spi_capture_bps\":%d,"', self.src)
        self.assertRegex(
            self.src,
            r"static int spi_capture_bps = 8030000;",
        )
        omit = re.search(
            r"Android omits spi_capture_bps[\s\S]{0,80}\"vio_is_1p8\"",
            self.src,
        )
        self.assertIsNone(omit, "nested JSON must not omit spi_capture_bps")

    def test_skipcap_overlay_does_not_set_spi_4m(self) -> None:
        self.assertIn(
            "skip WMODE/SCAN/SET_SPI-skipcap (no FDT 112/96/80)",
            self.src,
        )
        m = re.search(
            r"if \(cap_hal_overlay \|\| skip_enroll\) \{\s*"
            r"pr_info\(\"qsee_fp: skip WMODE/SCAN/SET_SPI-skipcap",
            self.src,
        )
        self.assertIsNotNone(m)
        self.assertNotIn("dev-SCAN-skipcap", self.src)
        self.assertNotIn("dev-WMODE-FDT-skipcap", self.src)

    def test_sfs_open_line_is_trustlet_text_and_rpmb_key_refused(self) -> None:
        """Open outcome is copied from the trustlet. Key programming stays refused.

        Rem=0 sends opcode 0x1014 with flag 0x40000000. The open line is still trustlet text.
        """
        self.assertIn(
            "Never synthesize an open line the TA did not print",
            self.src,
        )
        self.assertNotIn(
            "qsee_sfs_open('/data/vendor_de/0/fpdata/ft_fp_serial_id.bin') = synchronized",
            self.src,
        )
        self.assertNotIn("= SFS_SUCCESS", self.src)
        outcome = self.src.find('qsee_fp: serial-open outcome: %s')
        self.assertGreater(outcome, 0)
        self.assertIn("last_sfs_open_txt", self.src[outcome:outcome + 120])
        self.assertIn('qsee_fp: serial-open outcome: absent', self.src)
        note = self.src.find("static void note_ta_sfs_open")
        self.assertGreater(note, 0)
        note_body = self.src[note:note + 900]
        self.assertIn("Keep the last qsee_sfs_open in this buffer", note_body)
        self.assertNotIn("if (last_sfs_open_txt[0])", note_body)
        prov = self.src.find("if (cmd == RPMB_CMD_PROVISION)")
        self.assertGreater(prov, 0)
        prov_body = self.src[prov:prov + 280]
        self.assertIn("RPMB refuse PROVISION (key program)", prov_body)
        self.assertNotIn("ufs_rpmb_sec_xfer", prov_body)
        self.assertIn("RPMB refuse key-program req_resp=1", self.src)
        self.assertIn(
            "overlay SAVE template rem=0 opcode=0x1014 flag=0x40000000 (rearm FDT for Match)",
            self.src,
        )
        write = self.src.find("qsee_fp: RPMB write off=0x%x nfr=%u out_n=%u w4=%u")
        self.assertGreater(write, 0)
        write_body = self.src[write:write + 1600]
        self.assertIn("cpu_to_be16(0x0005)", write_body)
        self.assertIn("ufs_rpmb_sec_xfer(frm, out_len, true)", write_body)
        self.assertIn("memcpy(sb + 0x14, resp, RPMB_FRAME_SIZE)", write_body)
        self.assertIn("cpu_to_le32(0x14)", write_body)
        self.assertNotIn("RPMB_CMD_PROVISION", write_body)
        gp = self.src.find("static u32 handle_gpfile_listener")
        self.assertGreater(gp, 0)
        gp_body = self.src[gp:gp + 2200]
        parents = gp_body.find("gp_mkdir_parents(full)")
        opened = gp_body.find(
            "filp_open(full, O_RDWR | O_CREAT | O_SYNC, 0600)"
        )
        self.assertGreater(parents, 0)
        self.assertGreater(opened, parents)
        self.assertIn("start_creating_path(AT_FDCWD, name, &path, LOOKUP_DIRECTORY)", self.src)
        self.assertIn("vfs_mkdir(&nop_mnt_idmap, d_inode(path.dentry), dentry, 0700,", self.src)
        self.assertNotIn("vfs_unlink(", self.src)
        self.assertIn("GPFILE UNLINK '%s'", self.src)
        self.assertIn("reading 'ft_fp_serial_id.bin' failed", self.src)

    def test_scored_boots_require_leave_line_writes_and_reopen(self) -> None:
        """Success is the trustlet leave line, plus the two measured writes.

        The driver must not compose that line. The reopen journal must
        show the same line with the 22:45 segment left untouched.
        """
        self.assertIn(
            "Never synthesize an open line the TA did not print",
            self.src,
        )
        self.assertNotIn("= SFS_SUCCESS", self.src)
        self.assertNotIn(LEAVE, self.src)
        self.assertIn(
            "overlay SAVE template rem=0 opcode=0x1014 flag=0x40000000 (rearm FDT for Match)",
            self.src,
        )
        self.assertIn("RPMB refuse key-program req_resp=1", self.src)
        self.assertIn("RPMB refuse PROVISION (key program)", self.src)

        status = STATUS.read_text(encoding="utf-8")
        self.assertIn(
            "f20406f6-20ce-4671-aad5-ae0f1041ebca",
            status,
        )
        self.assertIn(
            "ea1a3165-b0a3-443a-9063-25588933c08b",
            status,
        )
        self.assertIn(
            "118236e1-fa2e-4c1b-b28c-0241cad88add",
            status,
        )
        self.assertIn(LEAVE, status)
        self.assertIn("111087..111093", status)
        self.assertIn("111101..111107", status)
        self.assertIn("2026-10-06 22:45:09", status)
        self.assertIn("2026-10-06 21:42:51", status)
        reopen_row = next(
            line for line in status.splitlines()
            if "118236e1-fa2e-4c1b-b28c-0241cad88add" in line
            and "22:45:09" in line
        )
        self.assertIn("No `SFS_ERROR`", reopen_row)
        self.assertIn("No `cmd=0x103`", reopen_row)

        create = {
            "boot-f20406f6-create.journal": range(111087, 111094),
            "boot-ea1a3165-create.journal": range(111101, 111108),
        }
        for name, counters in create.items():
            journal = (EVIDENCE / name).read_text(
                encoding="utf-8", errors="replace"
            )
            self.assertIn(
                "dev-INIT_DEV-100b SFS-WIDE @0x526: " + LEAVE,
                journal,
            )
            self.assertNotIn("SFS_ERROR", journal)
            self.assertNotIn("qsee_sfs_open", journal)
            self.assertNotIn("req_resp=1", journal)
            self.assertNotIn("cmd=0x101", journal)
            self.assertIn("RPMB cmd=0x103", journal)
            for wc in counters:
                self.assertIn(
                    f"RPMB write resp place=0x14 rr=0x300 result=0x0 wc={wc}",
                    journal,
                )

        reopen = (EVIDENCE / "boot-118236e1-reopen.journal").read_text(
            encoding="utf-8", errors="replace"
        )
        self.assertIn(
            "dev-INIT_DEV-100b SFS-WIDE @0x433: " + LEAVE,
            reopen,
        )
        self.assertIn(
            "GPFILE READ '/mnt/persist/misc/Wc9lu7HhpDADVWYMRw88zZxgXUmlrsVIzIrCf6-GmUA_Alt/"
            "Wc9lu7HhpDADVWYMRw88zZxgXUmlrsVIzIrCf6-GmUA_Alt' n=50088",
            reopen,
        )
        self.assertEqual(
            reopen.count(
                "GPFILE READ '/mnt/persist/misc/Wc9lu7HhpDADVWYMRw88zZxgXUmlrsVIzIrCf6-GmUA_Alt/"
                "RQmyhOdY8ywyaGUyB-TI' n=4096"
            ),
            2,
        )
        self.assertNotIn("SFS_ERROR", reopen)
        self.assertNotIn("qsee_sfs_open", reopen)
        self.assertNotIn("req_resp=1", reopen)
        self.assertNotIn("cmd=0x101", reopen)
        self.assertNotIn("cmd=0x103", reopen)
        self.assertNotIn("GPFILE WRITE", reopen)
        self.assertIn("serial-open outcome: absent", reopen)
        self.assertIn(
            "skip_enroll=1 — not sending ENROLL (keeps TA alive)",
            reopen,
        )

    def test_skip_enroll_fs_listener_before_serial_open(self) -> None:
        """No-finger init registers FS before INIT_DEV and logs the SFS result."""
        pre = self.src.find('listeners_register_enabled("before INIT_DEV")')
        init_dev = self.src.find('seq[n].name = "INIT_DEV"')
        self.assertGreater(pre, 0)
        self.assertGreater(init_dev, pre)
        window = self.src[pre - 400:pre + 80]
        self.assertNotIn("if (!skip_enroll)", window)
        self.assertIn("skip_enroll=%d", window)
        self.assertIn('qsee_fp: FILE_SERVICE (%s) ret=%d', self.src)
        self.assertIn(
            "qsee_fp: FS reply cmd=%u path='%.80s' errno=%d",
            self.src,
        )
        self.assertIn(
            "no filesystem request arrived before SFS_ERROR_GENERIC",
            self.src,
        )
        self.assertIn(
            "no RPMB request arrived before SFS_ERROR_GENERIC",
            self.src,
        )
        self.assertIn(
            "RPMB requests during serial open: %u",
            self.src,
        )
        self.assertIn("RPMB refuse PROVISION (key program)", self.src)
        self.assertIn("RPMB refuse key-program req_resp=1", self.src)
        before_start = self.src.find('listeners_register_enabled("before APP_START")')
        self.assertGreater(before_start, 0)
        self.assertLess(before_start, self.src.find('app_start("focal32"', before_start))
        self.assertIn("qsee_fp: TIME_SERVICE (%s) ret=%d", self.src)
        self.assertIn("qsee_fp: SSD_SERVICE (%s) ret=%d", self.src)
        self.assertIn("qsee_fp: TIME cmd=0x%x reqs=%u first16=%*ph", self.src)
        self.assertIn("qsee_fp: serial-open outcome: %s", self.src)
        self.assertIn("qsee_fp: serial-open outcome: absent", self.src)
        self.assertIn(
            "qsee_fp: serial-open listeners fs=%u rpmb=%u time=%u ssd=%u",
            self.src,
        )
        time_branch = self.src.find("if (lid == TIME_SERVICE)")
        fallback = self.src.find(
            "ret = listener_register_id(lid, &lstnr_sb"
        )
        self.assertGreater(time_branch, 0)
        self.assertGreater(fallback, time_branch)
        sfs = self.src.find('qsee_fp: %s SFS/SERIAL: %.120s')
        self.assertGreater(sfs, 0)
        scan = self.src[sfs - 500:sfs]
        self.assertIn("qsee_sfs_open", scan)
        self.assertIn("SFS_ERROR", scan)
        self.assertIn("ft_fp_serial_id.bin", self.src)
        self.assertIn(
            "skip_enroll=1 — not sending ENROLL (keeps TA alive)",
            self.src,
        )
        self.assertNotIn("wait_finger", window)

    def test_skip_enroll_overlay_idle_cap_burst3_no_enroll(self) -> None:
        self.assertIn(
            "skip_enroll overlay idle CAP burst=3 oneshot (no ENROLL, no REPORT)",
            self.src,
        )
        self.assertIn("try_idle_native_cap(id_dev, sess, 3, true)", self.src)
        self.assertIn(
            "if ((!skip_enroll || do_auth) &&",
            self.src,
        )
        self.assertIn(
            "if (skip_enroll && cap_hal_overlay && !do_auth)",
            self.src,
        )
        self.assertIn(
            "skip_enroll=1 — not sending ENROLL (keeps TA alive)",
            self.src,
        )

    def _try_idle_native_cap_body(self) -> str:
        m = re.search(
            r"static bool try_idle_native_cap\(.*?\nstatic int overlay_enroll_flag_caps\(",
            self.src,
            re.S,
        )
        self.assertIsNotNone(m, "try_idle_native_cap body not found")
        return m.group(0)

    def test_overlay_idle_scan_plen_0x24_before_oneshot_cap(self) -> None:
        self.assertRegex(self.src, r"static bool overlay_idle_scan = true;")
        self.assertIn(
            "overlay idle SCAN opcode=0x%x plen=0x24 (HAL capture_ctx before CAP)",
            self.src,
        )
        self.assertIn(
            "try_idle_native_cap(id_dev, sess, 3, true)",
            self.src,
        )
        body = self._try_idle_native_cap_body()
        self.assertIn("FF_TA_START_SCANNING, 0x24", body)
        self.assertIn("FF_TA_CAPTURE_IMAGE, 0x24", body)
        self.assertNotIn("FF_TA_START_SCANNING, 0x30", body)
        self.assertNotIn("0x30, cap", body)
        i_scan = body.find("FF_TA_START_SCANNING, 0x24")
        i_cap = body.find("FF_TA_CAPTURE_IMAGE, 0x24")
        self.assertGreaterEqual(i_scan, 0)
        self.assertGreater(i_cap, i_scan, "SCAN plen 0x24 must precede one-shot CAP")
        pre_scan = body[:i_scan]
        self.assertIn("capture_ctx_fill_hal", pre_scan)
        self.assertIn("FF_HAL_FLAG_FACTORY", pre_scan)
        self.assertNotIn("cpu_to_le32(1);\n\t\t\tsend_ion", body)
        self.assertIn("overlay idle SCAN abort (no CAP, no plen 0x30)", body)
        self.assertIn("M4dy skip START_SCANNING before overlay CAP", self.src)

    def test_overlay_idle_writereg_plen_0x8_before_scan_cap(self) -> None:
        self.assertRegex(self.src, r"static bool overlay_idle_writereg = true;")
        self.assertIn("#define FF_HAL_WRREG_ADDR	0xC6u", self.src)
        self.assertIn("#define FF_HAL_WRREG_VAL	1u", self.src)
        self.assertIn(
            "overlay idle WRITE_REG opcode=0x%x plen=0x8 addr=0x%x val=0x%x",
            self.src,
        )
        body = self._try_idle_native_cap_body()
        self.assertIn("FF_TA_WRITE_REG, 0x8", body)
        self.assertNotRegex(body, r"FF_TA_WRITE_REG,\s*0x(?!8\b)[0-9a-fA-F]+")
        i_wr = body.find("FF_TA_WRITE_REG, 0x8")
        i_scan = body.find("FF_TA_START_SCANNING, 0x24")
        i_cap = body.find("FF_TA_CAPTURE_IMAGE, 0x24")
        self.assertGreaterEqual(i_wr, 0)
        self.assertGreater(i_scan, i_wr, "WRITE_REG must precede SCAN")
        self.assertGreater(i_cap, i_scan, "SCAN must still precede CAP")
        self.assertIn("overlay idle WRITE_REG abort (no SCAN/CAP, no other addrs)", body)
        self.assertNotIn("FF_TA_START_SCANNING, 0x30", body)

    def test_overlay_idle_cap_plus10_is_3_enroll_plus10_is_1(self) -> None:
        self.assertRegex(self.src, r"static uint overlay_idle_plus10 = 3;")
        self.assertIn(
            "overlay idle CAP +0x10=%u burst=%u (HAL 1; this extra 3)",
            self.src,
        )
        body = self._try_idle_native_cap_body()
        self.assertIn("plus10 = overlay_idle_plus10", body)
        self.assertIn("FF_HAL_FLAG_FACTORY, 1, plus10)", body)
        self.assertGreater(body.count("plus10"), 3)
        self.assertIn("FF_TA_WRITE_REG, 0x8", body)
        self.assertIn("FF_TA_START_SCANNING, 0x24", body)
        self.assertNotIn("FF_TA_START_SCANNING, 0x30", body)
        # Enroll-flag +0x10=1 (HAL). CHIP_INIT-before-CAP spent.
        self.assertIn(
            "capture_ctx_fill_hal(sess, 3, fi, FF_HAL_FLAG_ENROLL, 0, 1)",
            self.src,
        )
        self.assertNotIn(
            "capture_ctx_fill_hal(sess, 3, 0, FF_HAL_FLAG_ENROLL, 0, 3)",
            self.src,
        )
        self.assertNotIn("dev-CHIP_INIT-enroll-flag", self.src)
        self.assertIn(
            '"\\"factory\\":{\\"other_press_inte\\":128,\\"dead_pixel_inte\\":128},"',
            self.src,
        )
        self.assertIn("overlay enroll-flag 3-CAP n_ok=%u", self.src)
        self.assertIn("dev-cap-enroll-flag", self.src)
        self.assertIn("overlay enroll-flag CAP frame=%u er=%d app_err=0x%x raw=%d avgv=%u slots=0x%x inte=%u", self.src)
        self.assertNotIn("overlay enroll-flag one-shot burst=3 +0x20=0", self.src)
        self.assertIn('\\"enable_trusted_enrollment\\":false,', self.src)
        self.assertNotIn('\\"enable_trusted_enrollment\\":true,', self.src)
        self.assertNotIn(
            "overlay poll LOGCAT after enroll-flag CAP (drain burst)",
            self.src,
        )
        self.assertIn("last_send_frame_slots |=", self.src)
        self.assertNotIn("overlay img-poll tries=%u raw=%d slots=0x%x inte=%u", self.src)
        self.assertIn("overlay skip LOGCAT drain (REPORT after 3-CAP)", self.src)
        self.assertIn("overlay PRESS WEAK 3-CAP n_ok=%u", self.src)
        self.assertIn("current inte times:", self.src)
        self.assertNotIn("for (pi = 0; pi < 16; pi++)", self.src)
        self.assertIn("overlay REPORT rem-probe", self.src)
        self.assertNotIn("overlay wait after enroll-flag CAP before REPORT", self.src)
        self.assertNotIn("overlay DEADPIX-down plen 0x24 after REAL DOWN", self.src)
        self.assertNotIn("diag_spi_data = true;", self.src)
        self.assertIn("scan_ion_adc_plane(sess, \"enroll-flag\")", self.src)
        self.assertIn(
            "overlay SET_SPI after INIT_SPI spd=%u (inte at 8.03M)",
            self.src,
        )
        self.assertIn(
            "overlay listeners pre-ENROLL (img_scan/SFS)",
            self.src,
        )
        self.assertNotIn("overlay enroll WRITE_REG 0xC6=1", self.src)
        self.assertNotIn("dev-ENROLL-0x1024-after-save", self.src)
        self.assertIn("event_ctx_fill(sess, 0x5, 4, 3, 0)", self.src)

    def test_enroll_report_stays_ev5(self) -> None:
        self.assertIn("event_ctx_fill(sess, 0x5, 4, 3, 0)", self.src)
        self.assertIn("event_ctx_fill(sess, 0x6, 4, 3, 0)", self.src)
        self.assertIn("overlay rem=%u fid=0x%x sample=%u", self.src)
        self.assertIn("overlay skip persist wait rem=%u (until rem=0)", self.src)
        self.assertIn("dev-REPORT-ev6", self.src)
        self.assertRegex(self.src, r"static int enroll_samples = 60;")
        self.assertIn("overlay PRESS OK rem=%u sample=%u", self.src)
        self.assertIn("overlay PRESS EMPTY avgv=%u sample=%u", self.src)
        self.assertIn("overlay_empty_rearm:", self.src)
        self.assertNotIn("overlay wait lift 2s before next LED", self.src)
        self.assertNotIn("msleep(2000)", self.src)
        self.assertIn("overlay wait lift IRQ (no QEV, no 2s)", self.src)
        self.assertIn("overlay_wait_lift_irq", self.src)
        self.assertIn("overlay_wait_lift_irq(irq_b, 80u)", self.src)
        self.assertNotIn("overlay_wait_lift_irq(1000u)", self.src)
        self.assertIn("overlay lift already irq=%u > down=%u", self.src)
        self.assertIn("dev-WMODE-FDT_UP-lift", self.src)
        self.assertIn("overlay skip FDT_UP-post-sample (ev=6 already re-armed)", self.src)
        self.assertIn("overlay skip QEV-lift before sample %u (lift IRQ already)", self.src)
        self.assertIn("overlay skip POST after rem=0 (keep FDT for Match, no SLEEP)", self.src)
        self.assertNotIn("dev-POST-after-rem0", self.src)
        self.assertIn("skip AUTH (Match is a separate press)", self.src)
        self.assertIn("overlay AUTH skip ENROLL (Match one press)", self.src)
        self.assertIn("overlay AUTH er=%d err=0x%x sample=%u", self.src)
        self.assertIn("overlay AUTH FAIL err=0x%x tmpl_missing=%d id_ok=%d (not a match)", self.src)
        self.assertIn("keep sess for Match AUTH kick", self.src)
        self.assertIn("authenticated fid", self.src)
        self.assertIn("overlay SET_GROUP persist load after rem=0 (gid=0)", self.src)
        self.assertIn("skipcap-pre-enroll", self.src)
        self.assertNotIn("pre-SAVE", self.src)
        self.assertNotIn("dev-SAVE-mask-template", self.src)
        self.assertNotIn("*(__le32 *)eusr |= cpu_to_le32(0x40000000u)", self.src)
        rem0_blk = re.search(
            r"if \(overlay_rem == 0\) \{.*?overlay_persist_load\(id_dev, sess\);",
            self.src,
            re.S,
        )
        self.assertIsNotNone(rem0_blk)
        self.assertIn("dev-SAVE-template", rem0_blk.group(0))
        self.assertIn("0x1014, 4, &save_flag", rem0_blk.group(0))
        self.assertIn("overlay_persist_load(id_dev, sess)", rem0_blk.group(0))
        self.assertNotIn("skip SAVE rem=0", rem0_blk.group(0))
        self.assertNotIn("memset(eusr, 0, 0x2e0)", rem0_blk.group(0))
        self.assertIn("listeners_register_enabled(\"before INIT_DEV\")", self.src)
        self.assertIn("before INIT_DEV", self.src)
        helper_enroll = (
            Path(__file__).resolve().parents[1] / "walkthrough" /
            "fp-walkthrough-enroll.sh"
        ).read_text()
        reg_opts = re.search(
            r'else\n  ENROLL_OPTS="([^"]*skip_enroll=0[^"]*)"',
            helper_enroll,
        )
        self.assertIsNotNone(reg_opts)
        self.assertIn("defer_listeners=0", reg_opts.group(1))
        self.assertIn("GPFILE_SERVICE", self.src)
        self.assertIn("enable_gpfile_listener=1",
                      (Path(__file__).resolve().parents[1] / "walkthrough" /
                       "fp-walkthrough-enroll.sh").read_text())
        self.assertIn("dev-SET_GROUP-persist", self.src)
        self.assertIn("/mnt/persist/qti_fp", self.src)
        self.assertIn("overlay rem stall %u empty/weak rem=%u — stop samples", self.src)
        self.assertIn("goto overlay_empty_rearm", self.src)
        self.assertIn("auth_kick", self.src)
        helper = (
            Path(__file__).resolve().parents[1] / "walkthrough" / "fp-walkthrough-enroll.sh"
        ).read_text()
        self.assertIn("Match AUTH kick on kept enroll sess", helper)
        self.assertIn("match-dmesg.txt", helper)
        self.assertIn("auth_kick_window", helper)
        self.assertIn("overlay AUTH kick — wait lift then PRESS", helper)
        self.assertNotIn("overlay AUTH kick empty", helper)
        self.assertIn("overlay AUTH HIT err=0x0 sample=%u", self.src)
        self.assertIn("overlay AUTH kick — wait lift then PRESS", self.src)
        self.assertIn("overlay AUTH kick empty avgv=%u — no match CAP", self.src)
        self.assertNotIn("dev-WMODE-FDT_DOWN-auth-kick", self.src)
        self.assertNotIn('"dev-WMODE-FDT_DOWN-auth"', self.src)
        self.assertIn("overlay AUTH skip FDT_DOWN-auth (mode 1 ESD)", self.src)
        self.assertIn("dev-WMODE-FDT_DOWN_DETECT-auth", self.src)
        self.assertIn("overlay AUTH FDT_DOWN_DETECT-pre-wait (same as enroll)", self.src)
        self.assertNotIn("dev-WMODE-SLEEP-auth-before", self.src)
        self.assertIn("overlay skip POST after rem=0 (keep FDT for Match, no SLEEP)", self.src)
        self.assertNotIn("dev-WMODE-SLEEP-rem0", self.src)
        auth_once = re.search(
            r"static void overlay_auth_once\(void\)\s*\{.*?\nauth_have_frame:",
            self.src,
            re.S,
        )
        self.assertIsNotNone(auth_once)
        self.assertNotIn("msleep(1500)", auth_once.group(0))
        self.assertNotIn("dev-QEV-auth-kick", auth_once.group(0))
        self.assertIn("dev-QEV-auth-down", auth_once.group(0))
        self.assertNotIn('"dev-WMODE-FDT_DOWN-auth"', auth_once.group(0))
        self.assertIn("overlay AUTH skip FDT_DOWN-auth (mode 1 ESD)", auth_once.group(0))
        self.assertIn("dev-WMODE-FDT_DOWN_DETECT-auth", auth_once.group(0))
        self.assertIn("overlay AUTH FDT_DOWN_DETECT-pre-wait (same as enroll)", auth_once.group(0))
        self.assertNotIn("dev-WMODE-SLEEP-auth-before", auth_once.group(0))
        self.assertNotIn("white:flash/brightness", auth_once.group(0))
        self.assertNotIn("msleep(1000)", auth_once.group(0))
        self.assertIn("overlay_enroll_flag_caps", auth_once.group(0))
        self.assertIn("overlay AUTH kick empty avgv=%u — no match CAP", auth_once.group(0))
        self.assertIn(
            "*** PRESS then LIFT finger sample 0/1 (HOLD on power-button sensor) ***",
            auth_once.group(0),
        )
        self.assertIn("overlay AUTH FAIL err=0xfffffff0 tmpl_missing=0 id_ok=0 (not a match)", self.src)
        self.assertIn('\\"enable_duplicated_finger_checking\\":true,', self.src)
        self.assertIn("dev-SET_GROUP-auth", self.src)
        self.assertIn("static bool do_auth", self.src)
        self.assertIn("overlay ARM wait itype sample=%u rem=%u", self.src)
        self.assertIn("overlay skip FDT_UP-post-sample (ev=6 already re-armed)", self.src)
        empty_rearm = re.search(
            r"overlay skip REPORT empty avgv=%u[\s\S]{0,2000}?goto overlay_empty_rearm",
            self.src,
        )
        self.assertIsNotNone(empty_rearm, "empty skip must re-arm ev=6/FDT not skip it")
        self.assertIn("goto overlay_empty_rearm", empty_rearm.group(0))
        self.assertIn("overlay PRESS REJECT rem=%u avgv=%u sample=%u", self.src)
        self.assertIn("parse_avgv_num", self.src)
        self.assertIn("overlay skip REPORT empty avgv=%u", self.src)
        self.assertIn("overlay quiet after empty (wait new press)", self.src)
        quiet_i = self.src.find("overlay quiet after empty (wait new press)")
        press_i = self.src.find(
            "*** PRESS then LIFT finger sample %u/%d (HOLD on power-button sensor) ***"
        )
        self.assertGreater(press_i, quiet_i, "LED must fire after quiet, not before")
        self.assertIn("overlay enroll-flag 3-CAP n_ok=%u slots=0x%x inte=%u", self.src)
        self.assertIn("FF_HAL_FLAG_ENROLL", self.src)
        self.assertIn("0xC0040002", self.src)
        fill = re.search(
            r"static void event_ctx_fill\(.*?\nstatic void capture_dump_hal_image\(",
            self.src,
            re.S,
        )
        self.assertIsNotNone(fill, "event_ctx_fill body not found")
        self.assertNotIn("memset(usr, 0, 0x2e0)", fill.group(0))
        self.assertIn("usr + 0x2c8", fill.group(0))
        self.assertIn("overlay REPORT rem-probe dirty pay+0x10=0x%x pay+0x24=0x%x", self.src)
        self.assertIn("overlay REPORT rem-probe before pay+0x10=0x%x pay+0x24=0x%x", self.src)
        self.assertIn("cpu_to_le32(0xa5a5a5a5u)", self.src)
        self.assertIn("cpu_to_le32(0xaaaaaaaau)", self.src)
        dirty = self.src.find("overlay REPORT rem-probe dirty")
        before = self.src.find("overlay REPORT rem-probe before")
        after = self.src.find("overlay REPORT rem-probe req+0x08")
        send = self.src.find("dev-REPORT-ev5")
        self.assertGreater(before, dirty)
        self.assertGreater(send, before)
        self.assertGreater(after, send)

    def test_enroll_flag_scan_0x24_before_cap_no_cancel(self) -> None:
        m = re.search(
            r"static int overlay_enroll_flag_caps\(.*?\nstatic bool m4dd_probe_28\(",
            self.src,
            re.S,
        )
        self.assertIsNotNone(m, "overlay_enroll_flag_caps body not found")
        body = m.group(0)
        # m4ht: 0x1020 was quality 5. Restore enroll-flag CAP 0x1013.
        # SCAN-armed after ENROLL before PRESS still skipped (killed FDT).
        self.assertNotIn("overlay SCAN-down plen 0x24 after REAL DOWN", body)
        self.assertNotIn("FF_TA_START_SCANNING, 0x24", body)
        self.assertIn("FF_TA_CAPTURE_IMAGE, 0x24", body)
        self.assertIn("dev-cap-enroll-flag", body)
        self.assertNotIn("dev-DEADPIX-down", body)
        self.assertNotIn("dev-cap-enroll-f%u", body)
        self.assertNotIn("dev-WRREG-enroll-flag", body)
        self.assertNotIn("dev-cap-enroll-flag-%u", body)
        self.assertNotIn("FF_TA_START_SCANNING, 0x30", body)
        self.assertIn("skip POST+CANCEL (no rem — keep TA)", self.src)
        self.assertIn("skip POST+CANCEL (TA enrolling, rem not 0)", self.src)
        self.assertNotIn("dev-CANCEL-no-rem", self.src)
        self.assertNotIn("dev-CANCEL-incomplete", self.src)
        self.assertIn("overlay skip QEV-lift before sample %u (lift IRQ already)", self.src)
        self.assertIn("overlay skip QEV-lift post-sample %u (keep TA)", self.src)
        self.assertNotIn(
            "overlay skip WMODE/SCAN-pre-wait sample %u (keep TA)",
            self.src,
        )
        self.assertIn("dev-WMODE-FDT-pre-wait", self.src)
        self.assertNotIn(
            "overlay WMODE-FDT-armed after ENROLL",
            self.src,
        )
        self.assertIn(
            "overlay skip 0x101f after ENROLL (keep auto fdt img_scan)",
            self.src,
        )
        self.assertNotIn(
            "overlay skip 0x101f after ENROLL sample %u (keep FDT_DOWN_DETECT)",
            self.src,
        )
        self.assertNotIn(
            "overlay WMODE-FDT-pre-wait sample %u",
            self.src,
        )
        self.assertIn(
            "overlay WMODE-FDT_DOWN_DETECT-pre-wait sample %u er=%d",
            self.src,
        )
        self.assertIn("dev-WMODE-FDT_DOWN_DETECT-enroll", self.src)
        self.assertIn(
            "overlay skip SCAN-pre-wait sample %u",
            self.src,
        )
        self.assertIn('"\\"firmware_log_level\\":3,"', self.src)
        self.assertIn('\\"framework_log_level\\":1,', self.src)
        self.assertNotIn('\\"framework_log_level\\":\\"info\\"', self.src)
        self.assertNotIn('"\\"firmware_log_level\\":\\"info\\","', self.src)
        self.assertNotIn('"\\"firmware_log_level\\":2,"', self.src)
        self.assertIn('\\"enable_trustlet_native_log\\":%s,', self.src)
        self.assertIn("diag_native_log = true;", self.src)
        self.assertNotIn(
            "overlay skip SYNC-pre-enroll (keep ENROLL auto fdt)",
            self.src,
        )
        self.assertIn(
            "overlay SYNC-pre-enroll clen=%zu (firmware_log_level 3)",
            self.src,
        )
        self.assertNotIn(
            "overlay SYNC-pre-enroll clen=%zu (native log on)",
            self.src,
        )
        self.assertNotIn(
            "overlay SYNC-pre-enroll clen=%zu (spi data log on)",
            self.src,
        )
        self.assertNotIn(
            "overlay SYNC-pre-enroll clen=%zu (firmware_log_level info)",
            self.src,
        )
        self.assertIn(
            "overlay skip SCAN-armed (keep ENROLL auto fdt)",
            self.src,
        )
        self.assertNotIn("overlay SCAN-armed plen 0x24 after ENROLL", self.src)
        self.assertIn("overlay SET_SPI after ENROLL spd=%u", self.src)
        self.assertIn(
            "overlay WMODE-FDT-pre-enroll after SLEEP",
            self.src,
        )
        self.assertNotIn(
            "overlay skip WMODE-FDT-pre-enroll (ENROLL starts FDT)",
            self.src,
        )
        self.assertIn("M4cs SLEEP after base (Android init end)", self.src)
        self.assertIn(
            "g_img_base_ok && cap_hal_overlay",
            self.src,
        )
        self.assertNotIn(
            "g_img_base_ok && skip_enroll && cap_hal_overlay",
            self.src,
        )
        self.assertIn("if (skip_capture_path) {", self.src)
        self.assertNotIn(
            "if (skip_capture_path && (!skip_enroll || cap_hal_overlay))",
            self.src,
        )
        self.assertIn(
            "skip WMODE/SCAN/SET_SPI-skipcap (no FDT 112/96/80)",
            self.src,
        )
        self.assertNotIn(
            "overlay wait 80ms after REAL DOWN before enroll CAP",
            self.src,
        )
        self.assertNotIn(
            "overlay FDT work-mode %u (Android INIT 0x101f=9; old 1)",
            self.src,
        )
        self.assertNotIn(
            "overlay sample %u IRQ down (skip QEV-s keep TA)",
            self.src,
        )
        self.assertIn(
            "overlay skip enroll-flag CAP itype=0x%x (need FDT 0x2; no ev=7)",
            self.src,
        )
        self.assertIn("(tw % 25) == 12 && !cap_hal_overlay", self.src)
        self.assertIn(
            "overlay skip esdclr sample %u (sticky esd, keep FDT)",
            self.src,
        )
        self.assertIn(
            "overlay skip QFINGER-s sample %u (keep itype, wait IRQ)",
            self.src,
        )
        self.assertIn("overlay skip QEV-pre-sample-alive", self.src)
        self.assertIn("overlay skip QEV-pre-hold", self.src)
        self.assertIn("overlay skip esd_clear pre-sample-loop", self.src)
        self.assertIn("overlay skip QEV-pre-enroll-alive", self.src)
        self.assertIn("overlay skip QFINGER pre-enroll", self.src)
        self.assertIn("overlay skip QEVENT2 pre-enroll", self.src)
        self.assertIn("overlay skip QEVENT pre-enroll", self.src)
        self.assertIn("overlay skip HEALTH-post-cap", self.src)
        self.assertIn("overlay skip PWR-skipcap", self.src)
        self.assertIn("overlay skip PRE_ENROLL2", self.src)
        self.assertIn("dev-PRE_ENROLL", self.src)
        self.assertIn("overlay skip QEV-lift before sample %u (lift IRQ already)", self.src)
        self.assertNotIn("dev-HEALTH-pre-report", self.src)
        self.assertIn("overlay REPORT ev=5 killed TA — stop samples", self.src)
        self.assertIn("Do not HEALTH between CAP and REPORT", self.src)
        self.assertNotIn("overlay wait after enroll-flag CAP before REPORT", self.src)
        self.assertNotIn("overlay second enroll-flag CAP after wait", self.src)
        self.assertNotIn("overlay poll LOGCAT after enroll-flag CAP", self.src)
        self.assertIn("overlay skip LOGCAT drain (REPORT after 3-CAP)", self.src)
        self.assertNotIn("dev-SAVE-mask-template", self.src)
        i_rep = self.src.find("dev-REPORT-ev5")
        i_keep = self.src.find("overlay SAVE template rem=0")
        self.assertGreater(i_keep, i_rep, "keep capture FDT after living REPORT")
        self.assertNotIn("dev-SPI-SPEED-enroll-flag", body)
        self.assertNotIn("overlay enroll SET_SPI_SPEED", body)
        self.assertNotIn("after Base, before ENROLL", self.src)
        self.assertNotIn("dev-CHIP_INIT-enroll-flag", body)

    def test_overlay_press_weak_rearm_rem_stall_skip_set_group(self) -> None:
        """WEAK 3-CAP re-arms; rem stall stops samples; skip SET_GROUP-after-post."""
        self.assertIn("overlay PRESS WEAK 3-CAP n_ok=%u", self.src)
        self.assertIn("overlay_empty_rearm:", self.src)
        rearm_log = (
            "overlay PRESS WEAK re-arm stall=%u rem=%u sample=%u" in self.src
        )
        weak_goto = re.search(
            r"if \(eflag == 1\) \{[\s\S]{0,1500}?goto overlay_empty_rearm",
            self.src,
        )
        self.assertTrue(
            rearm_log or weak_goto is not None,
            "PRESS WEAK 3-CAP must log re-arm stall or goto overlay_empty_rearm",
        )
        self.assertIn(
            "overlay rem stall %u empty/weak rem=%u — stop samples",
            self.src,
        )
        self.assertIn(
            "overlay SET_GROUP persist load after rem=0 (gid=0)",
            self.src,
        )
        self.assertIn("dev-SET_GROUP-persist", self.src)
        # never-retry: no REPORT ev=7 without REAL DOWN
        self.assertNotIn("dev-REPORT-ev7", self.src)
        self.assertNotIn("event_ctx_fill(sess, 0x7, 0x80004, 1, 0)", self.src)
        self.assertIn(
            "overlay skip enroll-flag CAP itype=0x%x (need FDT 0x2; no ev=7)",
            self.src,
        )
        self.assertIn("event_ctx_fill(sess, 0x5, 4, 3, 0)", self.src)
        # no SET_SPI enroll-flag next to CAP
        self.assertNotIn("dev-SPI-SPEED-enroll-flag", self.src)
        self.assertNotIn("overlay enroll SET_SPI_SPEED", self.src)
        self.assertIn("SET_SPI here, not next to CAP", self.src)
        # firmware_log_level integer 3, framework 2, trusted false
        self.assertIn('"\\"firmware_log_level\\":3,"', self.src)
        self.assertNotIn('"\\"firmware_log_level\\":\\"info\\","', self.src)
        self.assertNotIn('"\\"firmware_log_level\\":2,"', self.src)
        self.assertIn('\\"framework_log_level\\":1,', self.src)
        self.assertNotIn('\\"framework_log_level\\":\\"info\\"', self.src)
        self.assertIn('\\"enable_trusted_enrollment\\":false,', self.src)
        self.assertNotIn('\\"enable_trusted_enrollment\\":true,', self.src)

    def test_overlay_no_report_ev7_without_real_down(self) -> None:
        self.assertIn(
            "overlay skip enroll-flag CAP itype=0x%x (need FDT 0x2; no ev=7)",
            self.src,
        )
        self.assertNotIn("dev-REPORT-ev7", self.src)
        self.assertNotIn("event_ctx_fill(sess, 0x7, 0x80004, 1, 0)", self.src)
        self.assertIn("event_ctx_fill(sess, 0x5, 4, 3, 0)", self.src)

    def test_app_enroll_helper_no_force_cap_no_rmmod_loaded(self) -> None:
        helper = (
            Path(__file__).resolve().parents[1]
            / "walkthrough"
            / "fp-walkthrough-enroll.sh"
        )
        text = helper.read_text(encoding="utf-8", errors="replace")
        self.assertRegex(text, r'ENROLL_OPTS="[\s\S]*enroll_force_cap=0')
        self.assertNotRegex(text, r'ENROLL_OPTS="[\s\S]*enroll_force_cap=1')
        self.assertIn('SAMPLES="${ENROLL_SAMPLES:-60}"', text)
        self.assertIn("spi_capture_bps=8030000", text)
        self.assertNotIn("spi_capture_bps=4000000", text)
        self.assertNotIn("spi_capture_bps=8000000", text)
        self.assertNotRegex(text, r'ENROLL_OPTS="[\s\S]*skip_init_dev=1')
        self.assertNotIn("shutdown_before_start=1", text)
        self.assertIn("warm INIT skip_enroll (no overlay idle CAP) then attach", text)
        self.assertIn("cap_hal_overlay=0 overlay_idle_scan=0 overlay_idle_writereg=0", text)
        session = Path(__file__).resolve().parents[1] / "app" / "fp5-fp-session.sh"
        sess_text = session.read_text(encoding="utf-8", errors="replace")
        self.assertIn('ENROLL_SAMPLES="${ENROLL_SAMPLES:-60}"', sess_text)
        self.assertIn("DO_AUTH=1", sess_text)
        self.assertIn('if [ "${DO_AUTH:-0}" = 1 ]; then', text)
        self.assertIn("do_auth=1", text)
        self.assertNotIn('ENROLL_SAMPLES="${ENROLL_SAMPLES:-8}"', sess_text)
        self.assertIn("qsee_fingerpr already loaded — do not rmmod", text)
        self.assertIn("ENROLL_FAIL WEDGED_REBOOT", text)
        self.assertIn('if [ "${DO_AUTH:-0}" = 1 ]; then', text)
        self.assertIn("MATCH_KICK=1", text)
        self.assertIn("mount_persist", text)
        self.assertIn("/dev/disk/by-partlabel/persist", text)
        self.assertIn("MATCH_FAIL no kept enroll sess", text)
        self.assertIn("no insmod, no enroll score", text)
        self.assertIn(': >"$LOGDIR/match-ui.txt"', sess_text)
        self.assertIn("overlay AUTH kick — wait lift then PRESS", self.src)
        self.assertIn("ff_template_", self.src)
        self.assertIn("enable_fs_listener=1", text)
        self.assertIn("enable_rpmb_listener=1", text)
        auth_opts = re.search(
            r'if \[ "\$\{DO_AUTH:-0\}" = 1 \]; then\n  ENROLL_OPTS="([^"]*)"',
            text,
        )
        self.assertIsNotNone(auth_opts)
        self.assertNotIn("defer_listeners=0", auth_opts.group(1))
        reg_opts = re.search(
            r'else\n  ENROLL_OPTS="([^"]*skip_enroll=0[^"]*)"',
            text,
        )
        self.assertIsNotNone(reg_opts)
        self.assertIn("defer_listeners=0", reg_opts.group(1))
        self.assertIn("enable_fs_listener=1", reg_opts.group(1))
        self.assertIn("enable_rpmb_listener=1", reg_opts.group(1))
        cue = (
            Path(__file__).resolve().parents[1]
            / "walkthrough"
            / "fp-walkthrough-cue.sh"
        )
        cue_text = cue.read_text(encoding="utf-8", errors="replace")
        self.assertIn("PRESS then LIFT finger sample", cue_text)
        self.assertIn("wait lift then PRESS", cue_text)
        self.assertIn("CUE_RE=", cue_text)
        self.assertIn("Always live dmesg", cue_text)
        self.assertNotIn("HOLD FINGER NOW|PRESS then LIFT", cue_text)
        self.assertIn('last=$(dmesg 2>/dev/null | grep -E "$CUE_RE" | tail -1)', cue_text)
        self.assertLess(
            cue_text.find('last=$(dmesg'),
            cue_text.find("while [ -f \"$SESSION/cue.pid\" ]"),
        )
        logger = (
            Path(__file__).resolve().parents[1]
            / "walkthrough"
            / "fp-walkthrough-logger.sh"
        )
        log_text = logger.read_text(encoding="utf-8", errors="replace")
        self.assertIn("Copying an old f32 onto a new boot_id", log_text)
        self.assertNotIn(
            'f32n=$(grep -E \'^f32=\' /var/tmp/fp5-ta-ids',
            log_text,
        )
        app = Path(__file__).resolve().parents[1] / "app" / "fp5-fp-lab.py"
        app_text = app.read_text(encoding="utf-8", errors="replace")
        self.assertIn("Hands off until the LED flashes", app_text)
        self.assertNotIn("Wait for a flash. Then put a finger", app_text)
        self.assertIn(
            'self._mode == "enroll" and hero in ("FINGER SEEN", "SENSOR READY", "SAVED")',
            app_text,
        )
        self.assertIn("ktime-enroll.txt", text)
        self.assertIn("ktime-start.txt", sess_text)
        self.assertIn('hero == "FINGER SEEN"', app_text)
        self.assertIn("Register again — no reboot", app_text)
        self.assertNotIn("One Register this boot", app_text)
        self.assertIn("Register when you want", app_text)
        self.assertIn("Normal Linux reboot, then Start → Register", app_text)
        self.assertIn("next_enroll_mode", app_text)
        self.assertNotIn("score.get(\"fid_only\")", app_text)
        self.assertNotIn("events.get(\"enroll_partial\")", app_text)
        status = Path(__file__).resolve().parents[1] / "app" / "fp5_fp_status.py"
        status_text = status.read_text(encoding="utf-8", errors="replace")
        self.assertIn("Register again — no reboot", status_text)
        self.assertIn("Normal Linux reboot, then one Register", status_text)
        self.assertIn("event_ctx before REPORT", self.src)
