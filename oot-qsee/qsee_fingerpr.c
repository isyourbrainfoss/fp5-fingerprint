// SPDX-License-Identifier: GPL-2.0-only
/*
 * Open fingerpr TA: try trustlet-style msgs + large session buffer.
 * Load order: focal32 (base) then fingerpr.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/kprobes.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/firmware/qcom/qcom_tzmem.h>
#include <dt-bindings/firmware/qcom,scm.h>
#include <linux/string.h>
#include <linux/sizes.h>
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/bsg.h>
#include <scsi/sg.h>
#include <scsi/scsi.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>
#include <linux/blkdev.h>
#include <linux/unaligned.h>
#include <linux/ioctl.h>
#include <linux/kthread.h>
#include <linux/moduleparam.h>
#include <linux/namei.h>
#include <linux/mnt_idmapping.h>

/* Match L2 focaltech_fp_life ioctl surface for IC reset after ESD */
#define FF_L2_IOC_MAGIC		'f'
#define FF_L2_IOC_INIT		_IO(FF_L2_IOC_MAGIC, 0x01)
#define FF_L2_IOC_RESET		_IO(FF_L2_IOC_MAGIC, 0x02)
#define FF_L2_IOC_IRQ_EN	_IO(FF_L2_IOC_MAGIC, 0x03)
#define FF_L2_IOC_SPI_EN	_IO(FF_L2_IOC_MAGIC, 0x05)
#define FF_L2_IOC_PWR_EN	_IO(FF_L2_IOC_MAGIC, 0x07)

/* L2 power-button FP: reset pulse after ENROLL ESD ("need to reset ic") */
static void l2_reset_ic(const char *why)
{
	struct file *f;
	long r = -ENODEV;

	f = filp_open("/dev/focaltech_fp", O_RDWR, 0);
	if (IS_ERR(f)) {
		pr_info("qsee_fp: l2_reset_ic open fail %ld (%s)\n", PTR_ERR(f),
			why);
		return;
	}
	if (f->f_op && f->f_op->unlocked_ioctl) {
		/* power + clk then hardware reset pulse (L2 INIT does full cycle) */
		f->f_op->unlocked_ioctl(f, FF_L2_IOC_PWR_EN, 0);
		f->f_op->unlocked_ioctl(f, FF_L2_IOC_SPI_EN, 0);
		r = f->f_op->unlocked_ioctl(f, FF_L2_IOC_RESET, 0);
		f->f_op->unlocked_ioctl(f, FF_L2_IOC_INIT, 0);
		f->f_op->unlocked_ioctl(f, FF_L2_IOC_IRQ_EN, 0);
	}
	fput(f);
	pr_info("qsee_fp: l2_reset_ic ret=%ld (%s)\n", r, why);
	msleep(50);
}

/*
 * M4c8/M4cd: ensure L2 IRQ + SPI clocks without power-cycling the IC.
 * Writing sysfs listen=1 always runs ff_power_cycle() (VDD+RESET) which
 * re-sticks TA [esd] and floods HLOS IRQs (M4cc hold: itype=0x0 esd=1 storm).
 * Prefer ioctl IRQ_EN/SPI_EN; only write listen=1 if currently off.
 */
static void l2_listen_on(const char *why)
{
	struct file *f, *fl;
	loff_t p = 0;
	char cur[4] = { 0 };
	ssize_t n;
	bool need_sysfs = true;

	f = filp_open("/dev/focaltech_fp", O_RDWR, 0);
	if (!IS_ERR(f)) {
		if (f->f_op && f->f_op->unlocked_ioctl) {
			f->f_op->unlocked_ioctl(f, FF_L2_IOC_SPI_EN, 0);
			f->f_op->unlocked_ioctl(f, FF_L2_IOC_IRQ_EN, 0);
		}
		fput(f);
	}
	fl = filp_open("/sys/class/focaltech_fp/focaltech_fp/listen", O_RDWR, 0);
	if (!IS_ERR_OR_NULL(fl)) {
		p = 0;
		n = kernel_read(fl, cur, sizeof(cur) - 1, &p);
		if (n > 0) {
			cur[n] = 0;
			if (cur[0] == '1')
				need_sysfs = false;
		}
		if (need_sysfs) {
			p = 0;
			kernel_write(fl, "1\n", 2, &p);
			pr_info("qsee_fp: l2_listen_on sysfs enable (%s)\n", why);
		} else {
			pr_info("qsee_fp: l2_listen_on irq/spi only — listen already 1 (%s)\n",
				why);
		}
		fput(fl);
	} else {
		pr_info("qsee_fp: l2_listen_on ioctl only (%s)\n", why);
	}
}

#define MAX_QCOM_SCM_ARGS 10
enum { QCOM_SCM_VAL = 0, QCOM_SCM_RO = 1, QCOM_SCM_RW = 2, QCOM_SCM_BUFVAL = 3 };
#define QCOM_SCM_ARGS_IMPL(num, a, b, c, d, e, f, g, h, i, j, ...) (\
	(((a) & 3) << 4) | (((b) & 3) << 6) | (((c) & 3) << 8) | \
	(((d) & 3) << 10) | (((e) & 3) << 12) | (((f) & 3) << 14) | \
	(((g) & 3) << 16) | (((h) & 3) << 18) | (((i) & 3) << 20) | \
	(((j) & 3) << 22) | ((num) & 0xf))
#define QCOM_SCM_ARGS(...) QCOM_SCM_ARGS_IMPL(__VA_ARGS__, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)

struct qcom_scm_desc {
	u32 svc, cmd, arginfo;
	u64 args[MAX_QCOM_SCM_ARGS];
	u32 owner;
};
struct qcom_scm_qseecom_resp {
	u64 result, resp_type, data;
};

#define QSEECOM_RESULT_SUCCESS		0
#define QSEECOM_RESULT_INCOMPLETE	1
#define QSEECOM_RESULT_BLOCKED_ON_LISTENER 2
#define QSEECOM_TZ_OWNER_QSEE_OS	50
#define QSEECOM_TZ_OWNER_TZ_APPS	48
#define QSEECOM_TZ_SVC_APP_MGR		1
#define QSEECOM_TZ_SVC_LISTENER		2
#define QSEECOM_TZ_SVC_APP_ID_PLACEHOLDER 0
#define QSEECOM_TZ_CMD_APP_START	1
/* CAF QSEOS_APP_SHUTDOWN_COMMAND — free TZ heap of a loaded TA */
#define QSEECOM_TZ_CMD_APP_SHUTDOWN	2
#define QSEECOM_TZ_CMD_APP_REGION_NOTIFICATION 5
#define QSEECOM_TZ_CMD_APP_SEND		1
/* Listener cmds under TZ_SVC_LISTENER */
#define QSEECOM_TZ_CMD_LISTENER_REGISTER	1
#define QSEECOM_TZ_CMD_LISTENER_DEREGISTER	2
#define QSEECOM_TZ_CMD_LISTENER_RESPONSE	3
/* TZ_OWNER_QSEE_OS + TZ_SVC_RPMB. cmd 3 = check-prov only. Never 1=provision or 2=erase. */
#define QSEECOM_TZ_SVC_RPMB			4
#define QSEECOM_TZ_CMD_RPMB_CHECK_PROV		3
/* Android qseecomd FILE_SERVICE / TIME_SERVICE / RPMB */
#define FILE_SERVICE	10
#define TIME_SERVICE	11
/*
 * FP5 libdrmtime.so registers a 0x5000-byte buffer and dispatches
 * time_cmd_id 0x302..0x306. Response overwrites the start of that buffer
 * (QSEECom_send_resp), command word included as zero.
 */
#define TIME_SB_SIZE		0x5000
#define TIME_CMD_GET_UTC_SEC	0x302
#define TIME_CMD_GET_SYSTIME	0x303
#define TIME_CMD_GET_TIME_MS	0x304
#define TIME_CMD_GET_HLOS_UTC	0x305
#define TIME_CMD_END		0x306
#define RPMB_SERVICE	0x2000
#define GPFILE_SERVICE	0x7000	/* FP6 QTEE gpfile; try on QSEE too */
#define SSD_SERVICE	0x3000	/* Android qseecomd also registers SSD */
#define LISTENER_SB_SIZE	SZ_64K
/* Android librpmb rpmb_start uses sb_length 0x6400 for RPMB listener */
#define RPMB_SB_SIZE		0x6400
/* RPMB cmds from librpmb.so dispatch (0x101..0x104) */
#define RPMB_CMD_PROVISION	0x101
#define RPMB_CMD_READ		0x102
#define RPMB_CMD_WRITE		0x103
#define RPMB_CMD_GET_INFO	0x104
/*
 * CAF libdrmfs FILE_SERVICE (id=10) shared-buffer layout (RE of libdrmfs.so
 * on FP5 vendor):
 *   +0x000 u32 cmd
 *   +0x004 char path[0x100]     (response status overwrites first u32 of path)
 *   +0x008 (open) u32 fd        (success)
 *   +0x104 s32  size / len
 *   +0x108 u32  open flags/mode
 *   +0x10c u32  write flags
 *   +0x110 u8   data[]          (read/write payload for some cmds)
 *   +0x00c u8   data[]          (used by open-family helper as alt data)
 * Response: status/errno at +4; never clobber cmd at +0.
 * Cmd groups (jump table): 0/4/8 open-family; 1/5/9 r/w-family;
 * 2/6/0xa remove-ish; 3/7/0xb rename-ish; 0xc partition size.
 */
#define FS_CMD_OPEN	0
#define FS_CMD_REMOVE	2
#define FS_CMD_RENAME	3
#define FS_CMD_READ	4
#define FS_CMD_WRITE	5
#define FS_CMD_ERROR	0xFFFFFFFF
#define FS_PATH_OFF	0x4
#define FS_STATUS_OFF	0x4
#define FS_FD_OFF	0x8
#define FS_SIZE_OFF	0x104
#define FS_FLAGS_OFF	0x108
#define FS_DATA_OFF	0x110
#define FS_PATH_MAX	0x100

/* Android QSEECom_start_app("fingerpr") ion layout from fingerprint.default.so */
#define SESS_BIG	0x80040
#define REQ_ION		0x78000	/* send req length */
#define RSP_ION_OFF	0x78000	/* rsp starts here */
#define RSP_ION		0x8040	/* send rsp length */
#define REQ_SMALL	0x40
#define RSP_SMALL	0x800
/* FF_CMD_TA_* from ff_cmd_name jump table */
#define FF_TA_SEGMENTED		0x1000
#define FF_TA_CREATE		0x1001
#define FF_TA_INIT		0x1004
#define FF_TA_INIT_SPI		0x1006
#define FF_TA_PROBE_DEVICE	0x100a
#define FF_TA_INIT_DEVICE	0x100b
#define FF_TA_SYNC_CONFIG	0x100d
#define FF_TA_SYNC_CFG		FF_TA_SYNC_CONFIG
#define FF_TA_SYNC_STATS	0x100e	/* HAL pool plen 0x230 */
#define FF_TA_SYNC_CALIB	0x100f
#define FF_TA_SYNC_TEMPLATE	0x1010
#define FF_TA_SET_KM_KEY	0x1011
#define FF_TA_START_SCANNING	0x1012
#define FF_TA_SET_SPI_SPEED	0x1008
#define FF_TA_XFER_SPI		0x1009	/* HAL ff_trustlet_xfer_spi_data */
#define FF_TA_CAPTURE_IMAGE	0x1013
#define FF_TA_ESD_HANDLE	0x1014	/* HAL pool plen 0x4 — ff_trustlet_esd_handle */
#define FF_TA_SAVE_DATA		0x1015	/* HAL pool plen 0x2e0 — post-capture data */
#define FF_TA_UPDATE_TMPL	0x1015	/* same opcode as SAVE_DATA in some pools */
#define FF_TA_CHECK_HEALTH	0x1016
#define FF_TA_REPORT_EVENT	0x1017	/* HAL pool plen 0x2e0 */
#define FF_TA_WRITE_REG		0x1018	/* plen 0x8: addr, value */
#define FF_TA_READ_REG		0x1019	/* plen 0x4: addr; value returned in buffer */
#define FF_TA_WRITE_SRAM	0x101a
#define FF_TA_READ_SRAM		0x101b	/* plen 0x8 meta; data in shared ion */
#define FF_TA_QUERY_EVENT	0x101c
#define FF_TA_QUERY_FINGER	0x101d
#define FF_TA_CFG_POWER_MODE	0x101e
#define FF_TA_CFG_WORK_MODE	0x101f
/* chips config mode: 0=sleep 1=wait touch(FDT_DOWN) 2=wait leave(FDT_UP) */
#define FF_WMODE_SLEEP		0
#define FF_WMODE_FDT_DOWN	1
#define FF_WMODE_FDT_UP		2
/* Android INIT 0x101f plen 4 first word is 9, not chips-mode 1. */
#define FF_WMODE_FDT_DOWN_DETECT	9
/*
 * TA chips config mode (from residual):
 *  0=sleep 1=wait touch 2=wait leave 6=gesture 7=vio_1p8 8=io_strength
 * Mode 5 is INVALID ("unknown device mode") — do not use for img calib.
 * Step1/Base only from INIT_DEVICE after serial SFS (Android path).
 */
#define FF_WMODE_IMG_CALIB	1	/* wait touch — valid; was 5=unknown */
#define FF_WMODE_VIO_1P8	7
/* HAL pool: 0x1020 plen 0x24 same framing as CAPTURE (dead-pixel / forced image) */
#define FF_TA_CHECK_DEAD_PIXEL	0x1020
#define FF_TA_FINGERPRINT_IS_TYPE 0x1021
#define FF_TA_CHIP_PARAM_INIT	0x1022	/* HAL constant pool */
/* FT9362 image size used by Android frame raw path */
#define FF_IMG_COLS		112
#define FF_IMG_ROWS		88
#define FF_IMG_BYTES		(FF_IMG_COLS * FF_IMG_ROWS * 2)
#define FF_TA_PRE_ENROLL	0x1023
#define FF_TA_ENROLL		0x1024
#define FF_TA_POST_ENROLL	0x1025
#define FF_TA_GET_AUTH_ID	0x1026
#define FF_TA_CANCEL		0x1027
#define FF_TA_ENUMERATE		0x1028
#define FF_TA_REMOVE		0x1029
/* High-level CA ops (HAL constant pool 0x20xx) */
#define FF_TA_PRE_ENROLL_HL	0x2000	/* high-level CA ops */
#define FF_TA_ENROLL_HL		0x2001
#define FF_TA_POST_ENROLL_HL	0x2002
#define FF_TA_CANCEL_HL		0x2004
#define FF_TA_SET_ACTIVE_GROUP	0x2007
#define FF_TA_AUTHENTICATE	0x2008
#define FF_SVC_GET_VERSION	0x0205

static char *ta_dir = "/lib/firmware/qsee";
module_param(ta_dir, charp, 0644);
static int force_f32_id;
module_param(force_f32_id, int, 0644);
static int force_fp_id;
module_param(force_fp_id, int, 0644);
static bool do_start = true;
module_param(do_start, bool, 0644);
/*
 * M4dt/M4du: INIT_DEV Step1–3 mallocs the only 112×88 the TEE heap can hold.
 * Under-finger CAPTURE then array_new OOMs (M4ds: 28ms rows=88). Skip that
 * sequence so CAPTURE is the first image malloc. Pair with
 * shutdown_before_start to drop leftover INIT buffers from earlier Registers.
 */
static bool skip_init_dev;
module_param(skip_init_dev, bool, 0644);
MODULE_PARM_DESC(skip_init_dev,
		 "skip INIT_DEV/CHIP/calib (M4dt reattach / M4du fresh heap)");
static bool shutdown_before_start;
module_param(shutdown_before_start, bool, 0644);
MODULE_PARM_DESC(shutdown_before_start,
		 "SHUTDOWN resident focal32 then APP_START (M4du empty TEE heap)");
/*
 * M4cu: Android device/enroll path is focal32 only (~3MB). fingerpr is ~10MB
 * algorithm TA; loading both leaves global qsee_malloc tight → CAP
 * ff_sensor_data_array_new OOM even at 32×32. Default: do not START fingerpr;
 * if force_fp_id is set, SHUTDOWN that app to reclaim TZ RAM from prior holds.
 */
static bool start_fingerpr;
module_param(start_fingerpr, bool, 0644);
MODULE_PARM_DESC(start_fingerpr,
		 "START fingerpr TA (default false; M4cu heap — focal32 only)");
/*
 * M4cv: after INIT Base, L2 GPIO resets (pre/post ENROLL, esd_clear try≥1)
 * wipe chip img base → under-finger CAP re-enters Step1 → array_new OOM.
 * Android never resets IC between Base and frame raw. When Base is warm,
 * use soft ESD_HANDLE only.
 */
static bool preserve_img_base = true;
module_param(preserve_img_base, bool, 0644);
MODULE_PARM_DESC(preserve_img_base,
		 "skip L2 reset after Base (default true; M4cv Step1 re-entry)");
static int spi_bus_num = 14;
module_param(spi_bus_num, int, 0644);
MODULE_PARM_DESC(spi_bus_num, "driver.spi_bus_num for SYNC_CFG (default 14)");
static bool enable_spidev;
module_param(enable_spidev, bool, 0644);
MODULE_PARM_DESC(enable_spidev, "driver.enable_spidev in SYNC_CFG (default false)");
/* device.spi_mode: 0=mode0 (Android default). Try 1/2/3 if chip_id shifted. */
static int spi_mode;
module_param(spi_mode, int, 0644);
MODULE_PARM_DESC(spi_mode, "device.spi_mode 0..3 in SYNC_CFG (default 0)");
static int spi_default_bps = 4000000;
module_param(spi_default_bps, int, 0644);
MODULE_PARM_DESC(spi_default_bps, "device.spi_default_bps (default 4000000)");
/* Android Base update logs ~8.03M; enroll frame raw uses same image SPI path */
/* Keep the nested JSON key (omit killed Base). 4M forced ~21ms slot-0
 * scans; 8M-class matches INIT so max inte can reach 128. */
static int spi_capture_bps = 8030000;
module_param(spi_capture_bps, int, 0644);
MODULE_PARM_DESC(spi_capture_bps, "device.spi_capture_bps Hz in SYNC_CFG (default 8030000; keep key)");
/* M4av: recover VDD+speed+FDT when configure spi mode failed (no FREE_SPI) */
static bool spi_img_recover = true;
module_param(spi_img_recover, bool, 0644);
MODULE_PARM_DESC(spi_img_recover, "VDD+4M+FDT recover on spi mode fail for image CAP (default true)");
/*
 * Config text style for SYNC_CFG:
 *  3 = nested JSON (DEFAULT — required for SPI open on this TA)
 *  0 = flat dotted JSON (syncs but does NOT install driver.spi_bus_num)
 *  1 = key=value lines
 *  2 = Android logcat "key : value" lines (SCM -22 on this unit)
 */
static int cfg_style = 3;
module_param(cfg_style, int, 0644);
MODULE_PARM_DESC(cfg_style, "0=flat json, 1=kv, 2=colon, 3=nested json (default)");
/* request payload offset after cmd header; Android appears to use 0x10 */
static int pay_off = 0x10;
module_param(pay_off, int, 0644);
MODULE_PARM_DESC(pay_off, "payload byte offset in ion req (default 0x10)");
/*
 * skip_enroll=1 (default): do NOT send ENROLL 0x2001 — it SCM-kills focal32.
 * Run PRE_ENROLL + CAPTURE_IMAGE path instead (M4m).
 */
static bool skip_enroll = true;
module_param(skip_enroll, bool, 0644);
MODULE_PARM_DESC(skip_enroll, "skip ENROLL 0x2001 that kills TA (default true)");
static bool do_auth;
module_param(do_auth, bool, 0644);
MODULE_PARM_DESC(do_auth, "one-press AUTH 0x2008 (Match); skip ENROLL");
/*
 * M4cy heap: ENROLL_HL may allocate large enroll workspace before CAP so
 * array_new OOMs. Default: arm FDT + sample CAP first; ENROLL after samples.
 * Android does ENROLL first — if frame raw appears only with defer, ENROLL
 * was the heap consumer.
 */
static bool defer_enroll = true;
module_param(defer_enroll, bool, 0644);
MODULE_PARM_DESC(defer_enroll,
		 "sample CAP before ENROLL_HL (default true; M4cy heap)");
/*
 * M4bb: long pre-enroll CAPTURE flag/size sweep thrashes SPI mode and leaves
 * RST/ESD. When enrolling, skip that sweep; arm FDT once then sample loop.
 */
static bool skip_capture_path;
module_param(skip_capture_path, bool, 0644);
MODULE_PARM_DESC(skip_capture_path, "skip pre-enroll CAPTURE sweep; FDT arm only (default false)");
/*
 * After FDT soft-success, poll QUERY_EVENT / L2 irq_count for finger down.
 * 0 = no wait. Useful with skip_enroll=0 so ENROLL only fires if finger seen.
 */
static int wait_finger_s;
module_param(wait_finger_s, int, 0644);
MODULE_PARM_DESC(wait_finger_s, "seconds to wait for finger after FDT (0=none)");
/* Only ENROLL if wait_finger_s saw a press (or wait_finger_s=0 and !skip_enroll) */
static bool enroll_needs_finger = true;
module_param(enroll_needs_finger, bool, 0644);
MODULE_PARM_DESC(enroll_needs_finger, "require finger detect before ENROLL (default true)");
/* CAPTURE-only samples after single ENROLL start (Android rem loop) */
static int enroll_samples = 60;
module_param(enroll_samples, int, 0644);
MODULE_PARM_DESC(enroll_samples, "CAPTURE samples after one ENROLL start (default 60; stop at rem=0)");
/*
 * Seconds to poll for IRQ/TA DOWN before each enroll CAP sample.
 * CAP still runs when enroll_force_cap=1 even if no edge (HLOS IRQ may be quiet
 * while TA SPI FDT is active — Android waits IRQ but we must not skip CAP).
 */
static int sample_wait_s = 5;
module_param(sample_wait_s, int, 0644);
MODULE_PARM_DESC(sample_wait_s, "seconds wait per enroll sample for IRQ/DOWN (default 5)");
/* Always CAPTURE after sample wait; IRQ edge is optional enrichment */
/* M4bc: default false — idle CAP thrashes SPI; hold scripts press for ta_down */
static bool enroll_force_cap;
module_param(enroll_force_cap, bool, 0644);
MODULE_PARM_DESC(enroll_force_cap, "CAP even without IRQ/DOWN during enroll samples (default false; M4bc finger-only)");
/*
 * M4z: prefer FDT_DOWN (mode 1) for finger CAPs, dual-path CAP, ADC-range
 * pixel scan. PA in capture_ctx is optional — live smoke showed SCM -22 when
 * stuffing phys into the 0x24 ctx; leave off unless experimenting.
 */
static bool cap_img_bind = true;
module_param(cap_img_bind, bool, 0644);
MODULE_PARM_DESC(cap_img_bind, "FDT_DOWN finger CAP + pixel scan path (default true)");
static bool cap_img_pa;
module_param(cap_img_pa, bool, 0644);
MODULE_PARM_DESC(cap_img_pa, "also write image PA into capture_ctx+0x14 (default false; can SCM-kill)");
/*
 * M4ag: Android enroll stays in FDT_DOWN_DETECT for frame raw; mode-5 is only
 * for post-ENROLL vio/img-calib warmup. Pulsing WMODE-5 before each CAP may
 * divert CAPTURE into calib nav path (no ft9362_img_scan / frame raw).
 * Default off. HAL __svc_capture_image also uses flag bits 0x40/0x200.
 */
static bool cap_img_mode_pulse;
module_param(cap_img_mode_pulse, bool, 0644);
MODULE_PARM_DESC(cap_img_mode_pulse, "WMODE-5 pulse before finger CAP (default false; Android does not)");
/* M4be: default 0x240 = bit6|bit9 (HAL __svc_capture_image image path) */
static uint capture_flag = 0x240;
module_param(capture_flag, uint, 0644);
MODULE_PARM_DESC(capture_flag, "capture_ctx+0 flag (default 0x240 image bits; 1=FDT-only)");
static bool capture_flag_sweep = true;
module_param(capture_flag_sweep, bool, 0644);
MODULE_PARM_DESC(capture_flag_sweep, "on finger-down try 0x240/0x41/0x200/0x40 (default true)");
/* M4ai: force image via CHECK_DEAD_PIXEL 0x1020 + dual img_off layouts */
static bool try_dead_pixel = true;
module_param(try_dead_pixel, bool, 0644);
MODULE_PARM_DESC(try_dead_pixel, "after CAP try FF_TA_CHECK_DEAD_PIXEL 0x1020 (default true)");
/*
 * M4db tried default off for heap; hold showed TA SCM -22 on next ESD after
 * HEALTH when DEAD_PIXEL skipped. Default on again for TA stability / SAMPLE banner.
 */
static bool init_dead_pixel = true;
module_param(init_dead_pixel, bool, 0644);
MODULE_PARM_DESC(init_dead_pixel,
		 "run DEAD_PIXEL+FP_IS_TYPE in cold INIT (default true; needed for TA life)");
/*
 * M4dd: after INIT_DEV Base, skip CHIP_INIT/INIT/LOGCAT2/SYNC_CALIB/DEAD_PIXEL
 * on enroll+skip_capture. M4dc OOM was after those cmds — Base alone vs post-Base
 * thrash is the next cut. Early 28x24 probe runs before the skip.
 */
static bool slim_post_base = true;
module_param(slim_post_base, bool, 0644);
MODULE_PARM_DESC(slim_post_base,
		 "M4dd skip CHIP/INIT/DEAD after INIT_DEV Base (default true)");
/* set by M4dd early probe when enroll path */
static bool m4dd_early_oom;
static bool m4dd_early_ran;
/*
 * M4df: Base alone fills TEE heap (M4dd). Soft free never enough. Hypotheses:
 *  1) SLEEP after Base frees the only reusable Step3 raw → CAP must array_new
 *  2) diagnostic CAP probes sticky-Null / burn last free slots
 *  3) PA@+0x14 double-allocs with array_new
 * Defaults: keep Base warm (no SLEEP), no heap CAP probes, AFR nopa-first.
 */
static bool warm_base_nosleep = true;
module_param(warm_base_nosleep, bool, 0644);
MODULE_PARM_DESC(warm_base_nosleep,
		 "M4df skip SLEEP after Base to keep image bufs (default true)");
static bool heap_probe;
module_param(heap_probe, bool, 0644);
MODULE_PARM_DESC(heap_probe,
		 "M4df run 28x24 CAP probes after Base (default false; burns heap)");
static bool afr_nopa_first = true;
module_param(afr_nopa_first, bool, 0644);
MODULE_PARM_DESC(afr_nopa_first,
		 "M4df under-finger CAP suppress PA first (default true)");
/* Interactive: one 0x240 CAP (~11s) instead of 4 flags × 2 (~90s FDT-slow). */
static bool afr_one_shot;
module_param(afr_one_shot, bool, 0644);
MODULE_PARM_DESC(afr_one_shot,
		 "under-finger AFR: one flag 0x240 try (default false = 8-way sweep)");
/*
 * M4dg/M4dh reload: M4di showed any CAP OOMs on reloaded TA pre-Base.
 * Default **off** (M4dj) — reload doubles INIT cost and never freed CAP heap.
 */
static bool ta_reload_after_base;
module_param(ta_reload_after_base, bool, 0644);
MODULE_PARM_DESC(ta_reload_after_base,
		 "SHUTDOWN+START after Base + re-INIT (default false; M4dj)");
/*
 * M4dj/M4dk: early CAP stages. M4dk: layout0 vs layout1 matrix at entry +
 * immediately after Base (calib can malloc; CAPTURE may not).
 */
static bool early_cap_stages;
module_param(early_cap_stages, bool, 0644);
MODULE_PARM_DESC(early_cap_stages,
		 "M4dk layout matrix at entry+post-Base (default false; burns heap before native CAP)");
static bool defer_listeners = true;
module_param(defer_listeners, bool, 0644);
MODULE_PARM_DESC(defer_listeners,
		 "M4dj: START first; register FS/RPMB after INIT Base, before ENROLL (default true; false = after START, before INIT)");
static bool dual_img_off = true;
module_param(dual_img_off, bool, 0644);
MODULE_PARM_DESC(dual_img_off, "try capture_ctx img_off 0x24 then 0x100 (default true)");
/*
 * M4at: mode-5 prime in force-image re-sticks [esd] and blocks finger_touch
 * during the next sample wait (M4as hold: QES esd for entire 10s windows).
 * Default off — stay FDT_DOWN; enable only for no-finger calib experiments.
 */
static bool force_img_mode5;
module_param(force_img_mode5, bool, 0644);
MODULE_PARM_DESC(force_img_mode5, "mode-5 prime in force-image when !ta_down (default false; causes ESD)");
/*
 * M4ca: painting 0xA5 into capture image plane can mask TA-written frames and
 * inflate "nz" with sentinel. Default true (legacy); set paint_cap_a5=0 for
 * force-image / no-finger experiments to inspect real ion content.
 */
static bool paint_cap_a5 = true;
module_param(paint_cap_a5, bool, 0644);
MODULE_PARM_DESC(paint_cap_a5, "paint 0xA5 into CAP image plane when idle (default true)");
/* Mid-sample ESD clears when QES sticky during wait (default true) */
/* M4c6: default false — mid-wait esd thrash killed IRQ (M4c5 hold skip CAP) */
static bool sample_esd_clear;
module_param(sample_esd_clear, bool, 0644);
MODULE_PARM_DESC(sample_esd_clear, "esd_clear before sample wait if QES esd (default false; M4c6)");
/*
 * M4au: Android does frame raw[0..2] then transmit/enroll (~37ms), then
 * FDT_UP → FDT_DOWN for the next edge. IRQ edge alone is the primary
 * signal (itype 0x2); do not require QES finger_touch when L2 IRQ fires.
 */
static int sample_triple_cap = 3;
module_param(sample_triple_cap, int, 0644);
MODULE_PARM_DESC(sample_triple_cap, "CAPTURE frames per sample under finger (Android=3, default 3)");
/*
 * M4cb: default false. Assuming DOWN on any L2 edge (often ESD/noise) made
 * CAP run with residual finger=N → FDT-only, rem_seen=0. Require QES
 * [finger touch] or itype 0x2 for real_down; optional late-window fallback.
 */
static bool irq_means_down;
module_param(irq_means_down, bool, 0644);
MODULE_PARM_DESC(irq_means_down, "if true, L2 IRQ alone counts as ta_down (default false; M4cb needs QES/itype)");
static int irq_edge_delta = 1;
module_param(irq_edge_delta, int, 0644);
MODULE_PARM_DESC(irq_edge_delta, "min irq_count delta to count as press edge (default 1; was 2)");
/* M4cb: after IRQ, poll QUERY up to this many ×20ms for finger_touch (default 500ms) */
static int touch_wait_ms = 500;
module_param(touch_wait_ms, int, 0644);
MODULE_PARM_DESC(touch_wait_ms, "ms to poll QES after IRQ for finger_touch before CAP (default 500)");
/*
 * M4cc: ESD IRQ storm burned sample windows with full touchwait per edge.
 * Debounce edge handling; short touchwait when sticky esd.
 */
static int irq_debounce_ms = 250;
module_param(irq_debounce_ms, int, 0644);
MODULE_PARM_DESC(irq_debounce_ms, "min ms between processing L2 IRQ edges in sample wait (default 250)");
static int esd_touch_wait_ms = 60;
module_param(esd_touch_wait_ms, int, 0644);
MODULE_PARM_DESC(esd_touch_wait_ms, "ms QES poll after ESD IRQ (default 60)");
/*
 * Re-SYNC_CONFIG + CFG_POWER before ENROLL. Default false: live test showed
 * SYNC/PWR before ENROLL is followed by enroll-CAP "vio 1.8v fail" / spi mode
 * failed, while pre-enroll CAP still had vio success.
 */
static bool enroll_resync;
module_param(enroll_resync, bool, 0644);
MODULE_PARM_DESC(enroll_resync, "re-SYNC+PWR before ENROLL (default false; often breaks VIO)");
/* CREATE (0x1001) historically soft-errs then poisons APP_SEND; optional retry */
static bool try_create;
module_param(try_create, bool, 0644);
MODULE_PARM_DESC(try_create, "send FF_TA_CREATE after SYNC (default false)");
/*
 * Register Android FILE_SERVICE (id=10). Default false: SCM register returns
 * -22 on this firmware via mainline qcom_scm; RW arginfo hard-resets SoC.
 * Infrastructure kept for blocked-on-listener handling when register works.
 */
static bool enable_fs_listener;
module_param(enable_fs_listener, bool, 0644);
MODULE_PARM_DESC(enable_fs_listener, "register FILE_SERVICE=10 before TA work (default false)");
/* RPMB (0x2000) for SFS. Default false — dump-only stub until UFS RPMB IO lands. */
static bool enable_rpmb_listener;
module_param(enable_rpmb_listener, bool, 0644);
MODULE_PARM_DESC(enable_rpmb_listener, "register RPMB_SERVICE=0x2000 (default false)");
/* M4bz: also register SSD=0x3000 (qseecomd set) in case SFS checks full stack */
static bool enable_ssd_listener;
module_param(enable_ssd_listener, bool, 0644);
MODULE_PARM_DESC(enable_ssd_listener, "register SSD_SERVICE=0x3000 (default false)");
/*
 * qseecomd registers FS/RPMB/TIME/SSD before APP_START. Default on when a
 * secure-file listener is enabled. GPFILE is not included: its 0x7e000
 * buffer before START has returned -EIO. Pass 0 and reboot (no rmmod) if
 * START itself returns -EIO.
 */
static bool listeners_before_start = true;
module_param(listeners_before_start, bool, 0644);
MODULE_PARM_DESC(listeners_before_start,
		 "register FS/RPMB/SSD/TIME before APP_START (default true)");
static bool enable_gpfile_listener;
module_param(enable_gpfile_listener, bool, 0644);
MODULE_PARM_DESC(enable_gpfile_listener,
		 "register GPFILE=0x7000 persist store like FP6 (default false)");
static char *fs_root = "/var/lib/fp5-fp";
module_param(fs_root, charp, 0644);
MODULE_PARM_DESC(fs_root, "REE root for TA file paths (default /var/lib/fp5-fp)");
/* 0=full, 1=START only, 2=listener only, 3=one GET_VERSION send after START */
static int probe_level;
module_param(probe_level, int, 0644);
MODULE_PARM_DESC(probe_level, "0=full, 1=START only, 2=listener only, 3=one ion send");
/* Cap send_ion calls (0=unlimited). Use to binary-search crash/block points. */
static int max_sends;
module_param(max_sends, int, 0644);
MODULE_PARM_DESC(max_sends, "stop after N send_ion calls (0=unlimited)");
static int sends_done;
/* 1=dump+ENOENT only (safe); 0=path open/read/write against fs_root */
static bool fs_dump_only;
module_param(fs_dump_only, bool, 0644);
MODULE_PARM_DESC(fs_dump_only, "FS listener: only dump + status ENOENT (default false)");
/* Reduce dmesg flood so ENROLL/rem markers survive ring buffer */
static bool quiet_send;
module_param(quiet_send, bool, 0644);
MODULE_PARM_DESC(quiet_send, "skip full LOGCONT dump on send_ion (default false)");
/* Set by quiet MARK scan when last send saw vio 1.8v success */
static bool last_send_vio_ok;
static bool last_send_img_calib;
static bool last_send_base_update;
static bool last_send_img_dac_ok;
static bool last_send_img_step;
static bool last_send_fdt_ok; /* sticky: get fdt dac / fdt base check success */
/* Sticky across sends — Base update once is enough for the session */
static bool g_img_base_ok;
/* M4bw: diagnosis log levels — drop after FDT so Step1 residual has room for newDAC */
static int diag_fw_log_level = 3;
static int diag_framework_log_level = 3;
/* INIT stays false so Base sticky residual survives. SYNC-pre-enroll sets
 * true (Android diagnosis.enable_trustlet_native_log : on) for img_scan.
 */
static bool diag_native_log;
/* SYNC-pre-enroll only. INIT keeps false so Base sticky residual survives. */
static bool diag_spi_data;
/* TA log "interrupt type: 0xN" with down bit (0x2) set */
static bool last_send_finger_down;
static bool last_send_frame_raw;
static unsigned int last_send_frame_avgv;

static unsigned int parse_avgv_num(const char *p)
{
	while (*p == ' ' || *p == '\t')
		p++;
	return (unsigned int)simple_strtoul(p, NULL, 10);
}
/* Sticky across CAP+LOGCAT: bit0=raw[0], bit1=raw[1], bit2=raw[2]. */
static u8 last_send_frame_slots;
static unsigned int last_send_inte;
static bool last_send_enrolling;
static bool last_send_new_fid;
static bool last_send_qual_err;
static bool last_send_esd;
/* True only when last QUERY_EVENT parse saw a non-[esd] state string */
static bool last_send_qes_ok;
static bool last_send_image_ready;
static bool last_send_sensor_raw;
static bool last_send_spi_mode_fail;
/* Last interrupt type from TA log (0x2=down, 0x200=rst, 0x400=esd, …) */
static u32 last_send_itype;
/* M4ck: TA app err from ion req+8 after APP_SEND (0xffffff37 = Null) */
static u32 last_send_app_err;
static bool last_send_tmpl_missing;
static bool last_send_saw_ff_tmpl;
static bool last_send_auth_id_ok;
/* RPMB req 3 or FS write of a template this session. SET_GROUP is load-only. */
static bool last_persist_write;
static void *g_keep_sess;
static struct qcom_tzmem_pool *g_keep_pool;
static u32 g_keep_id;
static bool g_keep_enrolled;

/* Parse hex after "0x" stopping at first non-hex (TA log appends "(1=idle...") */
static u32 parse_hex_prefix(const char *p)
{
	u32 v = 0;

	if (!p)
		return 0;
	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
		p += 2;
	while (*p) {
		u32 d;

		if (*p >= '0' && *p <= '9')
			d = *p - '0';
		else if (*p >= 'a' && *p <= 'f')
			d = *p - 'a' + 10;
		else if (*p >= 'A' && *p <= 'F')
			d = *p - 'A' + 10;
		else
			break;
		v = (v << 4) | d;
		p++;
	}
	return v;
}

/*
 * capture_ctx @ ion usr (sess+0x10), HAL plen always 0x24:
 * Layout A (default): +0 flag, +4 cols, +8 rows, +0xc img_off, +0x10 nbytes
 * Layout B (cap_ctx_layout=1): +0 flag, +4 cols, +8 rows, +0xc nbytes, +0x10 img_off
 * M4an live: after CAP TA left rows=0 (cols=112 kept) — restore rows if zeroed.
 * flag: HAL __svc_capture bits 6/9 (0x40/0x200).
 */
/*
 * M4ao: layout 1 keeps rows=88 but force CAP on clean boot can Null (0xffffff37).
 * Layout 0: soft CAP success + FDT; TA zeros rows (we restore with fix_rows).
 * Default 0 for safe CAP; set 1 experimentally.
 * M4ck: large-plen CAP (0x24+img) before layout1 image CAP poisons TA → sticky Null.
 * Zero image plane in shared ion for layout1 so TA need not malloc full frame.
 */
static int cap_ctx_layout;
module_param(cap_ctx_layout, int, 0644);
MODULE_PARM_DESC(cap_ctx_layout, "0=off@0xc nb@0x10 (default soft CAP); 1=nb@0xc off@0x10");
/* M4ck: large in-band plen CAP — default off (M4ap sticky Null) */
static bool cap_big_plen;
module_param(cap_big_plen, bool, 0644);
MODULE_PARM_DESC(cap_big_plen, "allow plen=0x24+image CAP (default false; sticky Null risk)");
/* M4ck: after init, one controlled layout=1 CAP + residual + HEALTH (idle smoke) */
static bool idle_layout1_cap = true;
module_param(idle_layout1_cap, bool, 0644);
MODULE_PARM_DESC(idle_layout1_cap, "idle one layout1 CAP smoke after capture path (default true)");
/* M4dv: no-finger 112×88 CAPTURE after INIT to log TA builtin-bind vs array_new */
static bool idle_native_cap;
module_param(idle_native_cap, bool, 0644);
MODULE_PARM_DESC(idle_native_cap,
		 "skip_enroll: one 112x88 L1 nopa CAP after INIT (M4dv bind probe)");
/*
 * M4dy: Android 0x24 capture_ctx is burst/flag, not cols/rows/nbytes.
 * Layout1 112×88 in the request made TA array_new OOM. HAL overlay
 * produced idle frame raw[0] without malloc.
 */
static bool cap_hal_overlay = true;
module_param(cap_hal_overlay, bool, 0644);
MODULE_PARM_DESC(cap_hal_overlay,
		 "Android HAL 0x24 capture_ctx (no cols/rows; default true; M4dy)");
/*
 * Overlay skip_enroll idle: START_SCANNING 0x1012 plen 0x24 with the same
 * HAL capture_ctx as the following factory CAP. Not plen 0x30 first-word 1.
 * Recovery START without this SCAN: overlay_idle_scan=0.
 */
static bool overlay_idle_scan = true;
module_param(overlay_idle_scan, bool, 0644);
MODULE_PARM_DESC(overlay_idle_scan,
		 "skip_enroll overlay idle: SCAN 0x1012 plen 0x24 before CAP (default true)");
/*
 * Overlay skip_enroll idle: WRITE_REG 0x1018 plen 0x8 immediately before
 * SCAN+CAP. HAL ff_trustlet_write_device_register stores addr@+0 value@+4.
 * Only hardcoded HAL pair: invokeCommand writes 0xC6=1 (not a sweep).
 * Recovery START without this WRITE_REG: overlay_idle_writereg=0.
 */
static bool overlay_idle_writereg = true;
module_param(overlay_idle_writereg, bool, 0644);
MODULE_PARM_DESC(overlay_idle_writereg,
		 "skip_enroll overlay idle: WRITE_REG 0x1018 plen 0x8 before SCAN (default true)");
#define FF_HAL_WRREG_ADDR	0xC6u
#define FF_HAL_WRREG_VAL	1u
#define FF_HAL_FLAG_ENROLL	0xC0040002u
#define FF_HAL_FLAG_FACTORY	0xE0000240u
/*
 * Overlay skip_enroll idle CAP: +0x10=3 (burst count) instead of HAL
 * event-thread hardcoded 1. Enroll-flag / under-finger stay +0x10=1.
 * Recovery without this extra: overlay_idle_plus10=1 (or 0 → 1).
 */
static uint overlay_idle_plus10 = 3;
module_param(overlay_idle_plus10, uint, 0644);
MODULE_PARM_DESC(overlay_idle_plus10,
		 "skip_enroll overlay idle CAP +0x10 (default 3; 0/1 = HAL 1)");
/*
 * M4dw: GENERIC tzmem does not shmbridge. Android qseecom registers the
 * start_app ion. Call CREATE on the session buffer only — do not
 * shm_bridge_enable (global; rmtfs reset on some SoCs).
 */
static bool sess_shmbridge = true;
module_param(sess_shmbridge, bool, 0644);
MODULE_PARM_DESC(sess_shmbridge,
		 "qcom_scm_shm_bridge_create on SESS_BIG (M4dw; default true)");
static u64 g_sess_shm_handle;
static bool g_sess_shm_ok;

#define QCOM_SHM_BRIDGE_NUM_VM_SHIFT 9

static void sess_shmbridge_register(void *sess)
{
	phys_addr_t pa;
	u64 pfn, size_flags;
	int ret;

	g_sess_shm_ok = false;
	g_sess_shm_handle = 0;
	if (!sess_shmbridge || !sess)
		return;
	pa = qcom_tzmem_to_phys(sess);
	pfn = (u64)pa | QCOM_SCM_PERM_RW;
	size_flags = ALIGN(SESS_BIG, PAGE_SIZE) | (1ULL << QCOM_SHM_BRIDGE_NUM_VM_SHIFT);
	ret = qcom_scm_shm_bridge_create(pfn, pfn, size_flags, QCOM_SCM_VMID_HLOS,
					 &g_sess_shm_handle);
	pr_info("qsee_fp: M4dw shm_bridge_create pa=0x%llx sz=0x%lx ret=%d handle=0x%llx\n",
		(u64)pa, (unsigned long)ALIGN(SESS_BIG, PAGE_SIZE), ret,
		g_sess_shm_handle);
	if (!ret)
		g_sess_shm_ok = true;
}

static void sess_shmbridge_unregister(void)
{
	if (!g_sess_shm_ok)
		return;
	(void)qcom_scm_shm_bridge_delete(g_sess_shm_handle);
	pr_info("qsee_fp: M4dw shm_bridge_delete handle=0x%llx\n",
		g_sess_shm_handle);
	g_sess_shm_ok = false;
	g_sess_shm_handle = 0;
}
/* M4cn: AFR may suppress image PA@+0x14 (TA may double-alloc array + PA plane) */
static bool cap_ctx_suppress_pa;

static void capture_ctx_fill_flag(void *sess, u32 cols, u32 rows, u32 img_off,
				  u32 nbytes, bool paint_a5, u32 flag)
{
	u8 *usr = (u8 *)sess + 0x10;
	u32 plane;

	memset(usr, 0, 0x24);
	*(__le32 *)(usr + 0) = cpu_to_le32(flag);
	*(__le32 *)(usr + 4) = cpu_to_le32(cols);
	*(__le32 *)(usr + 8) = cpu_to_le32(rows);
	if (cap_ctx_layout == 1) {
		*(__le32 *)(usr + 0xc) = cpu_to_le32(nbytes);
		*(__le32 *)(usr + 0x10) = cpu_to_le32(img_off);
	} else {
		*(__le32 *)(usr + 0xc) = cpu_to_le32(img_off);
		*(__le32 *)(usr + 0x10) = cpu_to_le32(nbytes);
	}
	/*
	 * M4ck: optional image PA at ctx+0x14. Always-on for layout1 caused soft
	 * CAP Null (0xffffff37). Only for image flags (bit6/bit9) or cap_img_pa=1.
	 * M4cn: cap_ctx_suppress_pa skips PA so TA may use only internal array.
	 */
	if (!cap_ctx_suppress_pa &&
	    (cap_img_pa || (cap_ctx_layout == 1 && (flag & 0x240)))) {
		u64 img_pa = (u64)qcom_tzmem_to_phys(sess) + 0x10 + img_off;

		*(__le64 *)(usr + 0x14) = cpu_to_le64(img_pa);
	}
	/*
	 * M4ck: pre-zero shared-ion image plane for layout1.
	 */
	if (cap_ctx_layout == 1 && nbytes && img_off >= 0x24 &&
	    0x10 + img_off < REQ_ION) {
		plane = min_t(u32, nbytes, REQ_ION - 0x10 - img_off);
		if (plane)
			memset(usr + img_off, 0, plane);
	}
	if (paint_a5 && img_off >= 0x24 && 0x10 + img_off + 64 <= REQ_ION) {
		u32 mark = min_t(u32, nbytes ? nbytes : 64, 256);

		if (0x10 + img_off + mark <= REQ_ION)
			memset(usr + img_off, 0xA5, mark);
	}
}

/* If TA zeroed rows after CAP, restore dims so pixel scan / SAVE see a frame. */
static void capture_ctx_fix_rows(void *sess, u32 cols, u32 rows, u32 img_off,
				    u32 nbytes)
{
	u8 *usr = (u8 *)sess + 0x10;
	u32 r = le32_to_cpu(*(__le32 *)(usr + 8));
	u32 c = le32_to_cpu(*(__le32 *)(usr + 4));

	if (r != 0 && c != 0)
		return;
	pr_info("qsee_fp: capture_ctx rows/cols zeroed by TA (c=%u r=%u) — restore %ux%u\n",
		c, r, cols, rows);
	if (c == 0)
		*(__le32 *)(usr + 4) = cpu_to_le32(cols);
	if (r == 0)
		*(__le32 *)(usr + 8) = cpu_to_le32(rows);
	if (cap_ctx_layout == 1) {
		*(__le32 *)(usr + 0xc) = cpu_to_le32(nbytes);
		*(__le32 *)(usr + 0x10) = cpu_to_le32(img_off);
	} else {
		*(__le32 *)(usr + 0xc) = cpu_to_le32(img_off);
		*(__le32 *)(usr + 0x10) = cpu_to_le32(nbytes);
	}
}

static void capture_ctx_fill(void *sess, u32 cols, u32 rows, u32 img_off,
			     u32 nbytes, bool paint_a5)
{
	capture_ctx_fill_flag(sess, cols, rows, img_off, nbytes, paint_a5,
			      capture_flag);
}

static u32 capture_scan_pixels(const void *buf, u32 nbytes, u32 *na5_out,
			       u32 *avg_out, u32 *min_out, u32 *max_out);

/* Android HAL __svc_capture_image 0x24 — image header returns after CAP. */
static void capture_ctx_fill_hal(void *sess, u32 burst, u32 frame, u32 flag,
				 u32 factory, u32 plus10)
{
	u8 *usr = (u8 *)sess + 0x10;

	if (!plus10)
		plus10 = 1;
	memset(usr, 0, 0x24);
	*(__le32 *)(usr + 0x0c) = cpu_to_le32(burst);
	/* Event-thread 0x51748 hardcodes +0x10=1. Idle overlay may use 3. */
	*(__le32 *)(usr + 0x10) = cpu_to_le32(plus10);
	*(__le32 *)(usr + 0x14) = cpu_to_le32(frame);
	*(__le32 *)(usr + 0x1c) = cpu_to_le32(flag);
	*(__le32 *)(usr + 0x20) = cpu_to_le32(factory);
}

/*
 * Android ff_trustlet_event (REPORT 0x1017 plen 0x2e0) after capture.
 * device_event_thread success CAP (0x51a2c): +0x4=5, +0x2c8=burst,
 * +0x2d0=[sp,#0x44]=0, +0x2d8=4 (0x80000 only if NOT capturing).
 * ev=7 is IMAGE_READY when [sp,#0x44]≠0 (unlock), not enroll rem.
 * Putting itype at +0 is not the HAL layout.
 */
static void event_ctx_fill(void *sess, u32 ev_type, u32 flag_2d8, u32 burst,
			  u32 cap_rounds)
{
	u8 *usr = (u8 *)sess + 0x10;

	if (!burst)
		burst = 1;
	/* Overlay HAL 0x51a2c fields on the CAP ion. memset 0x2e0 wipes
	 * usr+0x2c image header (same class as HEALTH-pre-report).
	 */
	*(__le32 *)(usr + 4) = cpu_to_le32(ev_type);
	*(__le32 *)(usr + 0x2c8) = cpu_to_le32(burst);
	*(__le32 *)(usr + 0x2d0) = cpu_to_le32(cap_rounds);
	*(__le32 *)(usr + 0x2d8) = cpu_to_le32(flag_2d8);
}

static void capture_dump_hal_image(void *sess, const char *tag)
{
	u8 *usr = (u8 *)sess + 0x10;
	u32 i, nz44 = 0, avg = 0, mn = 0, mx = 0;
	u32 cols = le32_to_cpu(*(__le32 *)(usr + 0x30));
	u32 rows = le32_to_cpu(*(__le32 *)(usr + 0x34));

	pr_info("qsee_fp: %s hdr +0x2c=%*ph\n", tag, 32, usr + 0x2c);
	for (i = 0x24; i <= 0x44; i += 4)
		pr_info("qsee_fp: %s u32[+0x%02x]=0x%x (%u)\n", tag, i,
			le32_to_cpu(*(__le32 *)(usr + i)),
			le32_to_cpu(*(__le32 *)(usr + i)));
	nz44 = capture_scan_pixels(usr + 0x44, FF_IMG_BYTES, NULL, &avg, &mn, &mx);
	pr_info("qsee_fp: %s pix@+0x44 nz=%u avg=%u min=%u max=%u hdr=%ux%u raw=%d sens=%d\n",
		tag, nz44, avg, mn, mx, cols, rows,
		last_send_frame_raw, last_send_sensor_raw);
}

/* Find whether TA frame raw (ADC ~700–1000) landed in the start_app ion. */
static void scan_ion_adc_plane(void *sess, const char *tag)
{
	const u8 *p = sess;
	u32 off, n700 = 0, first = ~0u, last = 0;
	u32 best_off = 0, best_nz = 0, best_avg = 0, best_mn = 0, best_mx = 0;

	for (off = 0; off + 2 <= REQ_ION; off += 2) {
		u16 v = le16_to_cpu(*(__le16 *)(p + off));

		if (v >= 600 && v <= 1100) {
			n700++;
			if (first == ~0u)
				first = off;
			last = off;
		}
	}
	pr_info("qsee_fp: %s ion u16[600..1100]=%u span=0x%x..0x%x\n",
		tag, n700, first, last);

	for (off = 0; off + 512 <= REQ_ION; off += 0x40) {
		u32 nz = 0, sum = 0, mn = 0xffff, mx = 0, j;
		const __le16 *pix = (const __le16 *)(p + off);

		for (j = 0; j < 256; j++) {
			u16 v = le16_to_cpu(pix[j]);

			if (v >= 600 && v <= 1100) {
				nz++;
				sum += v;
				if (v < mn)
					mn = v;
				if (v > mx)
					mx = v;
			}
		}
		if (nz > best_nz) {
			best_nz = nz;
			best_off = off;
			best_avg = nz ? sum / nz : 0;
			best_mn = (mn == 0xffff) ? 0 : mn;
			best_mx = mx;
		}
	}
	pr_info("qsee_fp: %s best 256win off=0x%x nz=%u avg=%u min=%u max=%u\n",
		tag, best_off, best_nz, best_avg, best_mn, best_mx);
	if (best_nz >= 200 && best_avg >= 600)
		pr_info("qsee_fp: *** %s ADC plane in ion off=0x%x ***\n",
			tag, best_off);
	else
		pr_info("qsee_fp: %s no ADC plane in ion (TA frame stayed TZ-side)\n",
			tag);
}

/*
 * Count ADC-like u16 pixels (Android frame raw ~40–1023). Reject 0xa5a5
 * sentinel and high values that are really logcat ASCII misread as u16.
 */
static u32 capture_scan_pixels(const void *buf, u32 nbytes, u32 *na5_out,
			       u32 *avg_out, u32 *min_out, u32 *max_out)
{
	const u16 *pix = buf;
	u32 npix = nbytes / 2;
	u32 j, nz = 0, na5 = 0, sum = 0, mn = 0xffff, mx = 0;
	u32 lim = min_t(u32, npix, 4096);

	for (j = 0; j < lim; j++) {
		u16 v = le16_to_cpu(pix[j]);
		const u8 *b = (const u8 *)&pix[j];

		if (b[0] != 0xa5 || b[1] != 0xa5)
			na5++;
		/* FT93xx raw is ~10-bit; ignore log-text-looking high words */
		if (v >= 1 && v <= 2047) {
			nz++;
			sum += v;
			if (v < mn)
				mn = v;
			if (v > mx)
				mx = v;
		}
	}
	if (na5_out)
		*na5_out = na5;
	if (avg_out)
		*avg_out = nz ? sum / nz : 0;
	if (min_out)
		*min_out = mn == 0xffff ? 0 : mn;
	if (max_out)
		*max_out = mx;
	return nz;
}

/* Scan req image area + mid-ion. Do NOT treat rsp log window as pixels. */
static u32 capture_scan_all(void *sess, u32 img_off, u32 nbytes, const char *tag,
			    int cap_er)
{
	u8 *usr = (u8 *)sess + 0x10;
	u32 na5 = 0, avg = 0, mn = 0, mx = 0, nz, nz_mid;
	u32 scan_n = min_t(u32, nbytes ? nbytes : 512, 8192);

	/* Failed CAP: don't claim image from stale/log data */
	if (cap_er == -22) {
		pr_info("qsee_fp: %s pix scan skip (CAP SCM -22)\n", tag);
		return 0;
	}
	nz = capture_scan_pixels(usr + img_off, scan_n, &na5, &avg, &mn, &mx);
	nz_mid = capture_scan_pixels((u8 *)sess + 0x1000, min_t(u32, scan_n, 4096),
				     NULL, NULL, NULL, NULL);
	pr_info("qsee_fp: %s pix scan img_off=0x%x nz=%u na5=%u avg=%u min=%u max=%u mid_nz=%u\n",
		tag, img_off, nz, na5, avg, mn, mx, nz_mid);
	/*
	 * Flat constant (e.g. all 514) is not an image. Sparse dynamic ADC
	 * (M4ai post-force RSRAM min=1 max~1800 nz~20+) is partial image activity —
	 * claim IMAGE PIXELS when range is strong even if count is modest.
	 */
	if ((nz > 16 || nz_mid > 16) && (mx > mn + 32))
		pr_info("qsee_fp: *** IMAGE PIXELS %s nz=%u mid=%u avg=%u min=%u max=%u ***\n",
			tag, nz, nz_mid, avg, mn, mx);
	else if (nz > 0 || nz_mid > 0)
		pr_info("qsee_fp: %s weak/flat pixel signal nz=%u mid=%u avg=%u min=%u max=%u\n",
			tag, nz, nz_mid, avg, mn, mx);
	/* Return nz only if dynamic — flat constant buffers don't count as image */
	if (mx <= mn + 16)
		return 0;
	return max(nz, nz_mid);
}

typedef unsigned long (*kln_t)(const char *);
typedef int (*qcall_t)(const struct qcom_scm_desc *, struct qcom_scm_qseecom_resp *);
static kln_t kln;
static qcall_t qcall;

/* Listener shared buffers (one pool, one SB per service id) */
static struct qcom_tzmem_pool *lstnr_pool;
static void *lstnr_sb;		/* FILE_SERVICE */
static unsigned int fs_listener_reqs;
static unsigned int rpmb_listener_reqs;
static unsigned int time_listener_reqs;
static unsigned int ssd_listener_reqs;
static bool last_send_sfs_generic;
/* TA text of this send's qsee_sfs_open, or the synchronized marker. */
static char last_sfs_open_txt[160];
static bool serial_outcome_logged;
static size_t lstnr_sb_len;
static u32 lstnr_id = FILE_SERVICE;
static bool lstnr_registered;
static void *rpmb_sb;
static size_t rpmb_sb_len;
static bool rpmb_registered;
static void *ssd_sb;
static size_t ssd_sb_len;
static bool ssd_registered;
static void *gpfile_sb;
static size_t gpfile_sb_len;
static bool gpfile_registered;
static void *time_sb;
static size_t time_sb_len;
static bool time_registered;
/* Simple fd table for open files from TA */
#define FS_MAX_FD 16
static struct file *fs_files[FS_MAX_FD];

static noinline __attribute__((no_sanitize("kcfi")))
int inv(const struct qcom_scm_desc *d, struct qcom_scm_qseecom_resp *r)
{
	return qcall(d, r);
}

static int resolve(void)
{
	struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };

	if (register_kprobe(&kp))
		return -ENOENT;
	kln = (kln_t)kp.addr;
	unregister_kprobe(&kp);
	qcall = (qcall_t)kln("qcom_scm_qseecom_call");
	return qcall ? 0 : -ENOENT;
}

static int read_file(const char *path, u8 **out, size_t *olen)
{
	struct file *f = filp_open(path, O_RDONLY, 0);
	loff_t sz;
	u8 *b;
	ssize_t n;

	if (IS_ERR(f)) {
		pr_err("qsee_fp: open '%s' err=%ld\n", path, PTR_ERR(f));
		return PTR_ERR(f);
	}
	sz = i_size_read(file_inode(f));
	if (sz <= 0 || sz > 32 << 20) {
		pr_err("qsee_fp: bad size '%s' sz=%lld\n", path, (long long)sz);
		filp_close(f, NULL);
		return -EINVAL;
	}
	b = kvmalloc(sz, GFP_KERNEL);
	if (!b) {
		filp_close(f, NULL);
		return -ENOMEM;
	}
	/*
	 * M4c9/M4ca: retry EINTR/EAGAIN and accumulate partial reads. Also
	 * re-open on n==0 (seen on /lib/firmware/qsee/*.mdt — i_size ok but
	 * first kernel_read returns 0). Prefer staging TAs under /tmp/qsee-ta.
	 */
	{
		loff_t pos = 0;
		size_t got = 0;
		int tries = 0;
		int reopens = 0;

		while (got < (size_t)sz && tries < 64) {
			n = kernel_read(f, b + got, (size_t)sz - got, &pos);
			if (n == -EINTR || n == -EAGAIN) {
				tries++;
				cond_resched();
				continue;
			}
			if (n < 0) {
				pr_err("qsee_fp: read '%s' err=%zd got=%zu want=%lld\n",
				       path, n, got, (long long)sz);
				kvfree(b);
				filp_close(f, NULL);
				return (int)n;
			}
			if (n == 0) {
				if (got == 0 && reopens < 3) {
					filp_close(f, NULL);
					msleep(20);
					f = filp_open(path, O_RDONLY, 0);
					if (IS_ERR(f)) {
						pr_err("qsee_fp: reopen '%s' err=%ld\n",
						       path, PTR_ERR(f));
						kvfree(b);
						return PTR_ERR(f);
					}
					pos = 0;
					reopens++;
					tries++;
					continue;
				}
				break;
			}
			got += (size_t)n;
			tries = 0;
		}
		filp_close(f, NULL);
		if (got != (size_t)sz) {
			pr_err("qsee_fp: short read '%s' n=%zu want=%lld reopens=%d\n",
			       path, got, (long long)sz, reopens);
			kvfree(b);
			return -EIO;
		}
	}
	*out = b;
	*olen = sz;
	return 0;
}

static int build_img(const char *dir, const char *name,
		     u8 **img, size_t *ilen, size_t *mlen)
{
	char path[256];
	u8 *p[9] = {};
	size_t l[9] = {};
	size_t tot = 0, off = 0, mdt;
	u8 *o;
	int i, r;

	snprintf(path, sizeof(path), "%s/%s.mdt", dir, name);
	r = read_file(path, &p[0], &l[0]);
	if (r)
		return r;
	mdt = l[0];
	tot = mdt;
	for (i = 0; i < 8; i++) {
		snprintf(path, sizeof(path), "%s/%s.b0%d", dir, name, i);
		r = read_file(path, &p[i + 1], &l[i + 1]);
		if (r)
			goto err;
		tot += l[i + 1];
	}
	o = kvmalloc(tot, GFP_KERNEL);
	if (!o) {
		r = -ENOMEM;
		goto err;
	}
	for (i = 0; i < 9; i++) {
		memcpy(o + off, p[i], l[i]);
		off += l[i];
		kvfree(p[i]);
	}
	*img = o;
	*ilen = tot;
	*mlen = mdt;
	return 0;
err:
	for (i = 0; i < 9; i++)
		kvfree(p[i]);
	return r;
}

static int app_start(const char *name, u32 *id)
{
	u8 *img;
	size_t il, ml;
	struct qcom_tzmem_pool *pool;
	struct qcom_tzmem_pool_config cfg;
	void *tz;
	struct qcom_scm_desc d = {};
	struct qcom_scm_qseecom_resp r = {};
	int ret;

	ret = build_img(ta_dir, name, &img, &il, &ml);
	if (ret) {
		pr_err("qsee_fp: build_img %s/%s ret=%d\n", ta_dir, name, ret);
		return ret;
	}
	pr_info("qsee_fp: build_img %s il=%zu ml=%zu\n", name, il, ml);
	cfg.initial_size = ALIGN(il + SZ_1M, SZ_2M);
	cfg.policy = QCOM_TZMEM_POLICY_ON_DEMAND;
	cfg.increment = SZ_1M;
	cfg.max_size = ALIGN(il + SZ_4M, SZ_2M);
	pool = qcom_tzmem_pool_new(&cfg);
	if (IS_ERR(pool)) {
		kvfree(img);
		return PTR_ERR(pool);
	}
	tz = qcom_tzmem_alloc(pool, il, GFP_KERNEL);
	if (!tz) {
		qcom_tzmem_pool_free(pool);
		kvfree(img);
		return -ENOMEM;
	}
	memcpy(tz, img, il);
	kvfree(img);
	d.owner = QSEECOM_TZ_OWNER_QSEE_OS;
	d.svc = QSEECOM_TZ_SVC_APP_MGR;
	d.cmd = QSEECOM_TZ_CMD_APP_START;
	d.arginfo = QCOM_SCM_ARGS(3, QCOM_SCM_VAL, QCOM_SCM_VAL, QCOM_SCM_VAL);
	d.args[0] = ml;
	d.args[1] = il;
	d.args[2] = qcom_tzmem_to_phys(tz);
	ret = inv(&d, &r);
	pr_info("qsee_fp: START %s ret=%d res=0x%llx id=0x%llx\n",
		name, ret, r.result, r.data);
	qcom_tzmem_free(tz);
	qcom_tzmem_pool_free(pool);
	if (ret || r.result != QSEECOM_RESULT_SUCCESS)
		return ret ? ret : -EIO;
	*id = (u32)r.data;
	return 0;
}

/* M4cu: QSEECom_shutdown_app equivalent — free a loaded TA's TZ footprint */
static int app_shutdown(u32 app_id, const char *why)
{
	struct qcom_scm_desc d = {};
	struct qcom_scm_qseecom_resp r = {};
	int ret;

	if (!app_id)
		return 0;
	d.owner = QSEECOM_TZ_OWNER_QSEE_OS;
	d.svc = QSEECOM_TZ_SVC_APP_MGR;
	d.cmd = QSEECOM_TZ_CMD_APP_SHUTDOWN;
	d.arginfo = QCOM_SCM_ARGS(1, QCOM_SCM_VAL);
	d.args[0] = app_id;
	ret = inv(&d, &r);
	pr_info("qsee_fp: SHUTDOWN id=%u (%s) ret=%d res=0x%llx data=0x%llx\n",
		app_id, why ? why : "-", ret, r.result, r.data);
	if (ret)
		return ret;
	if (r.result != QSEECOM_RESULT_SUCCESS)
		return -EIO;
	return 0;
}

static int region_notify_try(const char *tag, u32 owner, u32 svc, u32 cmd,
			     u32 arginfo, u64 a0, u64 a1, u64 a2, int nargs)
{
	struct qcom_scm_desc d = {};
	struct qcom_scm_qseecom_resp r = {};
	int ret;

	d.owner = owner;
	d.svc = svc;
	d.cmd = cmd;
	d.arginfo = arginfo;
	d.args[0] = a0;
	d.args[1] = a1;
	d.args[2] = a2;
	ret = inv(&d, &r);
	pr_info("qsee_fp: REGION %s ret=%d res=0x%llx type=0x%llx data=0x%llx\n",
		tag, ret, r.result, r.resp_type, r.data);
	return (ret == 0 && r.result == QSEECOM_RESULT_SUCCESS) ? 0 : -EIO;
}

static void region_notify_probe(u32 app_id, phys_addr_t addr, size_t size)
{
	/*
	 * Only VAL-typed args — RW/TZ_APPS variants can hard-reset the SoC.
	 * Android binds ion at start_app; standalone REGION often returns -22.
	 */
	region_notify_try("2v-addr-sz",
			  QSEECOM_TZ_OWNER_QSEE_OS, QSEECOM_TZ_SVC_APP_MGR, 5,
			  QCOM_SCM_ARGS(2, QCOM_SCM_VAL, QCOM_SCM_VAL),
			  addr, size, 0, 2);
	region_notify_try("2v-sz-addr",
			  QSEECOM_TZ_OWNER_QSEE_OS, QSEECOM_TZ_SVC_APP_MGR, 5,
			  QCOM_SCM_ARGS(2, QCOM_SCM_VAL, QCOM_SCM_VAL),
			  size, addr, 0, 2);
	region_notify_try("3v-id-addr-sz",
			  QSEECOM_TZ_OWNER_QSEE_OS, QSEECOM_TZ_SVC_APP_MGR, 5,
			  QCOM_SCM_ARGS(3, QCOM_SCM_VAL, QCOM_SCM_VAL, QCOM_SCM_VAL),
			  app_id, addr, size, 3);
	region_notify_try("2v-addr-1M",
			  QSEECOM_TZ_OWNER_QSEE_OS, QSEECOM_TZ_SVC_APP_MGR, 5,
			  QCOM_SCM_ARGS(2, QCOM_SCM_VAL, QCOM_SCM_VAL),
			  addr, SZ_1M, 0, 2);
	/*
	 * Global carveout from pmOS DT reserved-memory/sec-apps@808ff000
	 * (Android qseecom_send_app_region uses platform "secapp-region").
	 */
	region_notify_try("dt-sec-apps",
			  QSEECOM_TZ_OWNER_QSEE_OS, QSEECOM_TZ_SVC_APP_MGR, 5,
			  QCOM_SCM_ARGS(2, QCOM_SCM_VAL, QCOM_SCM_VAL),
			  0x808ff000ULL, 0x1000ULL, 0, 2);
	/* nearby larger reserved memory@f8500000 (6MiB) — may be unrelated */
	region_notify_try("dt-mem-f85",
			  QSEECOM_TZ_OWNER_QSEE_OS, QSEECOM_TZ_SVC_APP_MGR, 5,
			  QCOM_SCM_ARGS(2, QCOM_SCM_VAL, QCOM_SCM_VAL),
			  0xf8500000ULL, 0x600000ULL, 0, 2);
}

static void dump_interesting(const char *tag, const u8 *p, size_t n)
{
	char ascii[200];
	size_t i, chunk = 0;
	u32 st = le32_to_cpu(*(__le32 *)p);
	/* rsp[0] often holds log length for device-TA ion framing */
	u32 log_len = st;
	size_t scan = n;

	pr_info("qsee_fp: %s st=0x%x first8=%02x%02x%02x%02x %02x%02x%02x%02x\n",
		tag, st, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
	if (log_len > 4 && log_len < n)
		scan = min_t(size_t, n, (size_t)log_len + 8);
	for (i = 0; i < scan; ) {
		if (p[i] >= 0x20 && p[i] < 0x7f) {
			size_t s = i;

			while (i < scan && p[i] >= 0x20 && p[i] < 0x7f)
				i++;
			if (i - s >= 3) {
				size_t off = s;

				while (off < i) {
					size_t len = min_t(size_t, i - off, sizeof(ascii) - 1);

					memcpy(ascii, p + off, len);
					ascii[len] = 0;
					pr_info("qsee_fp: %s str[%zu]: %s\n", tag, chunk++, ascii);
					if (strnstr(ascii, "ff_template_", len))
						last_send_saw_ff_tmpl = true;
					if (strnstr(ascii, "hasn't been loaded", len) ||
					    strnstr(ascii, "0 template loaded", len))
						last_send_tmpl_missing = true;
					if (last_send_saw_ff_tmpl &&
					    (strnstr(ascii, "not exist", len) ||
					     strnstr(ascii, "not exis", len)))
						last_send_tmpl_missing = true;
					if (strnstr(ascii, "authenticated fid", len) ||
					    strnstr(ascii, "matched fid", len) ||
					    strnstr(ascii, "onAuthenticated", len))
						last_send_auth_id_ok = true;
					off += len;
				}
			}
		} else {
			i++;
		}
	}
	if (!chunk)
		pr_info("qsee_fp: %s str: (none printable)\n", tag);
}

/* Scan ion req+rsp for TA enroll consume (send_ion_seg does not set flags). */
static void scan_enroll_markers(void *sess, const char *tag)
{
	const char *parts[2];
	size_t lens[2];
	unsigned int p;

	if (!sess)
		return;
	parts[0] = sess;
	parts[1] = (const char *)sess + RSP_ION_OFF;
	lens[0] = 4096;
	lens[1] = min_t(size_t, RSP_ION, 4096);
	for (p = 0; p < 2; p++) {
		const char *scan = parts[p];
		size_t n = lens[p], si;

		for (si = 0; si + 18 < n; si++) {
			if (!strncmp(scan + si, "enrolling...", 12))
				pr_info("qsee_fp: %s ENROLL-MARK '%.48s'\n", tag,
					scan + si);
			if (!strncmp(scan + si, "enrolled fid", 12)) {
				last_send_enrolling = true;
				pr_info("qsee_fp: %s REM-MARK '%.48s'\n", tag,
					scan + si);
			} else if (!strncmp(scan + si, "rem = ", 6)) {
				pr_info("qsee_fp: %s REM-TEXT '%.48s'\n", tag,
					scan + si);
			}
			if (!strncmp(scan + si, "new fid", 7)) {
				last_send_new_fid = true;
				pr_info("qsee_fp: %s NEWFID-MARK '%.48s'\n", tag,
					scan + si);
			}
			if (!strncmp(scan + si, "got finger touched", 18) ||
			    !strncmp(scan + si, "got finger released", 19))
				pr_info("qsee_fp: %s TOUCH-MARK '%.48s'\n", tag,
					scan + si);
			if (!strncmp(scan + si, "coverage", 8) ||
			    !strncmp(scan + si, "quality", 7) ||
			    !strncmp(scan + si, "too small", 9) ||
			    !strncmp(scan + si, "partial finger", 14) ||
			    !strncmp(scan + si, "low quality", 11))
				pr_info("qsee_fp: %s QUAL-MARK '%.48s'\n", tag,
					scan + si);
			if (!strncmp(scan + si, "error, ret = -3", 15) ||
			    !strncmp(scan + si, "quality is too low", 18))
				last_send_qual_err = true;
		}
	}
}

/* Forward decl — send_sep must use blocked-listener loop, not mainline app_send */
static int app_send_ex(u32 app_id, void *req, size_t req_sz, void *rsp, size_t rsp_sz,
		       const char *tag);

/* Classic separate-buffer send (uses app_send_ex so FILE_SERVICE blocks work) */
static int send_sep(u32 app_id, const char *tag, const void *reqd, size_t reqn,
		    size_t req_cap, size_t rsp_cap)
{
	struct qcom_tzmem_pool_config cfg = {
		.initial_size = SZ_256K,
		.policy = QCOM_TZMEM_POLICY_ON_DEMAND,
		.increment = SZ_64K,
		.max_size = SZ_512K,
	};
	struct qcom_tzmem_pool *pool;
	void *req, *rsp;
	int ret;
	u32 st;

	if (reqn > req_cap)
		reqn = req_cap;
	pool = qcom_tzmem_pool_new(&cfg);
	if (IS_ERR(pool))
		return PTR_ERR(pool);
	req = qcom_tzmem_alloc(pool, req_cap, GFP_KERNEL);
	rsp = qcom_tzmem_alloc(pool, rsp_cap, GFP_KERNEL);
	if (!req || !rsp) {
		if (req)
			qcom_tzmem_free(req);
		if (rsp)
			qcom_tzmem_free(rsp);
		qcom_tzmem_pool_free(pool);
		return -ENOMEM;
	}
	memset(req, 0, req_cap);
	memset(rsp, 0, rsp_cap);
	memcpy(req, reqd, reqn);
	/* NEVER use mainline qcom_scm_qseecom_app_send with FILE_SERVICE registered —
	 * it does not resume BLOCKED_ON_LISTENER and hard-resets this SoC. */
	ret = app_send_ex(app_id, req, req_cap, rsp, rsp_cap, tag);
	st = le32_to_cpu(*(__le32 *)rsp);
	if (ret == 0 && st != 0xffffffff) {
		pr_info("qsee_fp: HIT %s ret=%d\n", tag, ret);
		dump_interesting(tag, rsp, min_t(size_t, rsp_cap, 256));
	} else {
		pr_info("qsee_fp: %s ret=%d st=0x%x\n", tag, ret, st);
	}
	qcom_tzmem_free(req);
	qcom_tzmem_free(rsp);
	qcom_tzmem_pool_free(pool);
	return ret;
}

/*
 * Android ion session send: one contiguous buffer
 *   req @0 len REQ_ION (0x78000)
 *   rsp @RSP_ION_OFF len RSP_ION (0x8040)
 * Message in req: cmd@0, plen@4, payload@0x10 (ff_trustlet_client_exchange_message)
 */
/* Map Android/SFS paths onto REE fs_root */
static void fs_map_path(const char *in, char *out, size_t outlen)
{
	static const char *const prefixes[] = {
		"/data/vendor_de/0/fpdata",
		"/data/vendor/focaltech",
		"/data/vendor/misc/qti_fp",
		"/data/vendor/misc",
		"/persist/data",
		"/persist/qti_fp",
	};
	unsigned int i;

	if (!in || !in[0]) {
		scnprintf(out, outlen, "%s/empty", fs_root);
		return;
	}
	/* FP6: persist partition is the TEE store. Prefer it when mounted. */
	if (!strncmp(in, "/data/vendor_de/0/fpdata", 24) ||
	    !strncmp(in, "/persist/qti_fp", 15)) {
		const char *rest = !strncmp(in, "/persist/qti_fp", 15) ? in + 15 : in + 24;

		scnprintf(out, outlen, "/mnt/persist/qti_fp%s", rest);
		return;
	}
	for (i = 0; i < ARRAY_SIZE(prefixes); i++) {
		size_t n = strlen(prefixes[i]);

		if (strncmp(in, prefixes[i], n) == 0) {
			scnprintf(out, outlen, "%s%s", fs_root, in + n);
			return;
		}
	}
	/* Fallback: keep only basename-ish under fs_root (avoid nesting /data/...) */
	if (in[0] == '/') {
		const char *base = strrchr(in, '/');

		if (base && base[1])
			scnprintf(out, outlen, "%s/%s", fs_root, base + 1);
		else
			scnprintf(out, outlen, "%s/path", fs_root);
	} else {
		scnprintf(out, outlen, "%s/%s", fs_root, in);
	}
}

static int fs_alloc_fd(struct file *f)
{
	int i;

	for (i = 1; i < FS_MAX_FD; i++) {
		if (!fs_files[i]) {
			fs_files[i] = f;
			return i;
		}
	}
	return -EMFILE;
}

static struct file *fs_get(int fd)
{
	if (fd <= 0 || fd >= FS_MAX_FD)
		return NULL;
	return fs_files[fd];
}

static void fs_put(int fd)
{
	if (fd > 0 && fd < FS_MAX_FD) {
		if (fs_files[fd]) {
			fput(fs_files[fd]);
			fs_files[fd] = NULL;
		}
	}
}

static void dump_sb(const char *tag, void *sb, size_t sb_len, size_t n)
{
	size_t off;

	if (!sb)
		return;
	n = min_t(size_t, n, sb_len);
	pr_info("qsee_fp: LSTNR %s dump %zu bytes pa=0x%llx\n",
		tag, n, (u64)qcom_tzmem_to_phys(sb));
	for (off = 0; off < n; off += 32) {
		char hex[32 * 3 + 4];
		size_t h = 0, j, chunk = min_t(size_t, 32, n - off);

		for (j = 0; j < chunk; j++)
			h += scnprintf(hex + h, sizeof(hex) - h, "%02x ",
				       ((u8 *)sb)[off + j]);
		pr_info("qsee_fp: LSTNR[%03zu]: %s\n", off, hex);
	}
}

static void dump_listener_sb(const char *tag, size_t n)
{
	dump_sb(tag, lstnr_sb, lstnr_sb_len, n);
}

/* Write libdrmfs-style status at +4; never touch cmd@+0. */
static void fs_set_status(u8 *sb, u32 status)
{
	*(__le32 *)(sb + FS_STATUS_OFF) = cpu_to_le32(status);
}

/*
 * FILE_SERVICE handler matching libdrmfs shared-buffer layout.
 * fs_dump_only: dump + ENOENT (safe first contact). Full mode: path ops.
 */
static u32 handle_fs_listener(void)
{
	u8 *sb = lstnr_sb;
	u32 cmd, raw_cmd, ret = 0;
	char ree_path[256];
	char path_in[FS_PATH_MAX + 1];
	int fd;
	struct file *f;
	ssize_t n;
	loff_t pos;
	s32 size;
	u32 flags;

	if (!sb)
		return FS_CMD_ERROR;

	cmd = le32_to_cpu(*(__le32 *)sb);
	raw_cmd = cmd;
	size = (s32)le32_to_cpu(*(__le32 *)(sb + FS_SIZE_OFF));
	flags = le32_to_cpu(*(__le32 *)(sb + FS_FLAGS_OFF));
	/* path is fixed @+4, NUL-terminated C string */
	memcpy(path_in, sb + FS_PATH_OFF, FS_PATH_MAX);
	path_in[FS_PATH_MAX] = 0;
	pr_info("qsee_fp: FS cmd=%u size=%d flags=0x%x path='%.80s'\n",
		cmd, size, flags, path_in[0] ? path_in : "(empty)");
	dump_listener_sb("fs-req", 0x120);

	/*
	 * Observed on FP5 fingerpr/qti_fp traffic (not classic 0..11 libdrmfs):
	 *   cmd = 0x02xx where low byte is op:
	 *     0x02 open (path@+4 → result fd or -errno @+4)
	 *     0x0f mkdir/create path
	 *     0x1c close (fd@+4)
	 * Response overwrites +4 with fd (success) or negative errno.
	 */
	{
		u32 cls = cmd >> 8;
		u32 op = cmd & 0xff;

		if (cls == 2)
			cmd = op; /* normalize */
	}

	if (fs_dump_only) {
		/* Safe: report missing so TA soft-fails optional files */
		fs_set_status(sb, (u32)-ENOENT);
		return 0;
	}

	if (path_in[0] == '/')
		fs_map_path(path_in, ree_path, sizeof(ree_path));
	else if (path_in[0] && path_in[0] >= 0x20 && path_in[0] < 0x7f)
		scnprintf(ree_path, sizeof(ree_path), "%s/%s", fs_root, path_in);
	else
		ree_path[0] = 0;

	if (ree_path[0])
		pr_info("qsee_fp: FS ree '%s'\n", ree_path);

	switch (cmd) {
	case 0x0f: /* mkdir / create path hierarchy */ {
		char *s;

		if (!ree_path[0]) {
			fs_set_status(sb, (u32)-ENOENT);
			break;
		}
		/* Ensure leaf exists as empty file (parents may already be there) */
		s = strrchr(ree_path, '/');
		if (s && strchr(s, '.')) {
			f = filp_open(ree_path, O_RDWR | O_CREAT, 0600);
			if (!IS_ERR(f))
				fput(f);
			else
				pr_info("qsee_fp: FS create '%s' err=%ld\n",
					ree_path, PTR_ERR(f));
		}
		fs_set_status(sb, 0);
		pr_info("qsee_fp: FS mkdir/create done '%s'\n", ree_path);
		break;
	}
	case 0x02: /* open — return fd or -errno at +4 */ {
		/* FP6: O_RDWR|O_CREAT|O_SYNC, never O_TRUNC (truncates templates). */
		int oflags = O_RDWR | O_CREAT | O_SYNC;

		if (!ree_path[0]) {
			fs_set_status(sb, (u32)-ENOENT);
			break;
		}
		(void)flags;
		f = filp_open(ree_path, oflags, 0600);
		if (IS_ERR(f))
			f = filp_open(ree_path, O_RDONLY, 0);
		if (IS_ERR(f)) {
			ret = (u32)PTR_ERR(f);
			pr_info("qsee_fp: FS open '%s' err=%d\n", ree_path, (int)ret);
			fs_set_status(sb, ret);
			break;
		}
		fd = fs_alloc_fd(f);
		if (fd < 0) {
			fput(f);
			fs_set_status(sb, (u32)-EMFILE);
			break;
		}
		pr_info("qsee_fp: FS open ok fd=%d '%s'\n", fd, ree_path);
		/* Success: put positive fd at +4 (overwrites path) */
		fs_set_status(sb, (u32)fd);
		break;
	}
	case 0x0e: /* read by fd: cmd, fd@+4, len@+8?, data follows */ {
		u32 len;
		loff_t rpos = 0;

		fd = (int)le32_to_cpu(*(__le32 *)(sb + 4));
		len = le32_to_cpu(*(__le32 *)(sb + 8));
		if (!len || len > 0x4000)
			len = le32_to_cpu(*(__le32 *)(sb + FS_SIZE_OFF));
		if (!len || len > lstnr_sb_len - 0x40)
			len = 256;
		f = fs_get(fd);
		if (!f) {
			pr_info("qsee_fp: FS readfd bad fd=%d\n", fd);
			fs_set_status(sb, (u32)-EBADF);
			break;
		}
		/* data often at +0x10 or +0x40 after hdr */
		n = kernel_read(f, sb + 0x40, len, &rpos);
		pr_info("qsee_fp: FS readfd fd=%d len=%u n=%zd\n", fd, len, n);
		if (n < 0)
			fs_set_status(sb, (u32)n);
		else {
			fs_set_status(sb, 0);
			*(__le32 *)(sb + 8) = cpu_to_le32((u32)n);
		}
		break;
	}
	case 0x0d: /* write by fd */ {
		u32 len;
		loff_t wpos = 0;

		fd = (int)le32_to_cpu(*(__le32 *)(sb + 4));
		len = le32_to_cpu(*(__le32 *)(sb + 8));
		if (!len || len > lstnr_sb_len - 0x40)
			len = 0;
		f = fs_get(fd);
		if (!f) {
			fs_set_status(sb, (u32)-EBADF);
			break;
		}
		n = len ? kernel_write(f, sb + 0x40, len, &wpos) : 0;
		pr_info("qsee_fp: FS writefd fd=%d len=%u n=%zd\n", fd, len, n);
		if (n > 0)
			last_persist_write = true;
		if (n < 0)
			fs_set_status(sb, (u32)n);
		else {
			fs_set_status(sb, 0);
			*(__le32 *)(sb + 8) = cpu_to_le32((u32)n);
		}
		break;
	}
	case 0x1c: /* close — fd was written at +4 */ {
		fd = (int)le32_to_cpu(*(__le32 *)(sb + FS_STATUS_OFF));
		/* also accept fd if path was clobbered to errno from prior open fail */
		if (fd <= 0)
			fd = (int)le32_to_cpu(*(__le32 *)(sb + 4));
		if (fd > 0)
			fs_put(fd);
		fs_set_status(sb, 0);
		pr_info("qsee_fp: FS close fd=%d\n", fd);
		break;
	}
	case 0: /* classic libdrmfs open */
	case 8: {
		int oflags = O_RDWR | O_CREAT;

		if (!ree_path[0]) {
			fs_set_status(sb, (u32)-ENOENT);
			break;
		}
		f = filp_open(ree_path, oflags, 0600);
		if (IS_ERR(f)) {
			ret = (u32)PTR_ERR(f);
			fs_set_status(sb, ret);
			*(__le32 *)(sb + FS_FD_OFF) = 0;
			break;
		}
		fd = fs_alloc_fd(f);
		if (fd < 0) {
			fput(f);
			fs_set_status(sb, (u32)-EMFILE);
			break;
		}
		fs_set_status(sb, 0);
		*(__le32 *)(sb + FS_FD_OFF) = cpu_to_le32(fd);
		break;
	}
	case 4: /* read-family */ {
		u32 len;

		if (!ree_path[0]) {
			fs_set_status(sb, (u32)-ENOENT);
			break;
		}
		len = (size > 0 && size < (s32)(lstnr_sb_len - FS_DATA_OFF)) ?
			      (u32)size : 0;
		f = filp_open(ree_path, O_RDONLY, 0);
		if (IS_ERR(f)) {
			fs_set_status(sb, (u32)PTR_ERR(f));
			break;
		}
		pos = 0;
		n = len ? kernel_read(f, sb + FS_DATA_OFF, len, &pos) : 0;
		fput(f);
		pr_info("qsee_fp: FS read '%s' n=%zd\n", ree_path, n);
		if (n < 0)
			fs_set_status(sb, (u32)n);
		else {
			fs_set_status(sb, 0);
			*(__le32 *)(sb + FS_FD_OFF) = cpu_to_le32((u32)n);
		}
		break;
	}
	case 5:
	case 9: {
		u32 len;

		if (!ree_path[0]) {
			fs_set_status(sb, (u32)-ENOENT);
			break;
		}
		len = (size > 0 && size < (s32)(lstnr_sb_len - FS_DATA_OFF)) ?
			      (u32)size : 0;
		f = filp_open(ree_path, O_WRONLY | O_CREAT, 0600);
		if (IS_ERR(f)) {
			fs_set_status(sb, (u32)PTR_ERR(f));
			break;
		}
		pos = 0;
		n = len ? kernel_write(f, sb + FS_DATA_OFF, len, &pos) : 0;
		fput(f);
		if (n < 0)
			fs_set_status(sb, (u32)n);
		else {
			fs_set_status(sb, 0);
			*(__le32 *)(sb + FS_FD_OFF) = cpu_to_le32((u32)n);
		}
		break;
	}
	case 1:
	case 6:
	case 0xa:
		fs_set_status(sb, 0);
		break;
	default:
		dump_listener_sb("fs-unknown", 0x140);
		fs_set_status(sb, (u32)-ENOENT);
		break;
	}
	wmb();
	{
		s32 st = (s32)le32_to_cpu(*(__le32 *)(sb + FS_STATUS_OFF));

		fs_listener_reqs++;
		pr_info("qsee_fp: FS reply cmd=%u path='%.80s' errno=%d\n",
			raw_cmd, path_in[0] ? path_in : "(empty)", st);
	}
	return 0; /* listener_respond status=0; detail is in shared buffer */
}

/* FP6 gpfile 0x7000: persist-data is root 2. READ data@+0x00c WRITE@+0x110. */
/* Parent of a GPFILE write. SFS creates misc/<id>_Alt/<segment> and
 * filp_open does not create the missing directory, so the create open
 * comes back SFS_NO_FILE.
 */
static int gp_mkdir(const char *name)
{
	struct dentry *dentry;
	struct path path;

	dentry = start_creating_path(AT_FDCWD, name, &path, LOOKUP_DIRECTORY);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);
	dentry = vfs_mkdir(&nop_mnt_idmap, d_inode(path.dentry), dentry, 0700,
			   NULL);
	end_creating_path(&path, dentry);
	return PTR_ERR_OR_ZERO(dentry);
}

static int gp_mkdir_parents(const char *full)
{
	char *path, *s;
	int err = 0;

	path = kstrdup(full, GFP_KERNEL);
	if (!path)
		return -ENOMEM;
	s = path;
	if (*s == '/')
		s++;
	for (;;) {
		s = strchr(s, '/');
		if (!s)
			break;
		*s = '\0';
		err = gp_mkdir(path);
		if (err == -EEXIST)
			err = 0;
		if (err)
			break;
		*s = '/';
		s++;
	}
	kfree(path);
	return err;
}

static u32 handle_gpfile_listener(void)
{
	u8 *sb = gpfile_sb;
	u32 op, root, act, len, err = 0, got = 0;
	s32 off;
	char rel[256], full[320];
	struct file *f;
	loff_t pos;
	ssize_t n;
	static const char *const roots[] = {
		"tzstorage", "misc", "persist-data", "root3"
	};

	if (!sb || gpfile_sb_len < 0x114)
		return 0;
	op = le32_to_cpu(*(__le32 *)sb);
	pr_info("qsee_fp: GPFILE op=%u first12=%*ph\n", op, 12, sb);
	/*
	 * libdrmfs gpfs thread, cmd 12 only: stur d8,[req+4] then
	 * QSEECom_send_resp length 0xc. d8 is the u64 at file 0x3108,
	 * little-endian 2. A 4-byte store leaves whatever TrustZone
	 * wrote at +8.
	 */
	if (op == 12) {
		put_unaligned_le64(2, sb + 4);
		wmb();
		return 0;
	}
	if (op > 12)
		goto gp_einval;
	root = op >> 2;
	act = op & 3;
	if (root >= ARRAY_SIZE(roots))
		goto gp_einval;
	memcpy(rel, sb + 4, 255);
	rel[255] = 0;
	if (!rel[0] || rel[0] == '/' || strstr(rel, ".."))
		goto gp_einval;
	scnprintf(full, sizeof(full), "/mnt/persist/%s/%s", roots[root], rel);
	off = (s32)le32_to_cpu(*(__le32 *)(sb + 0x104));
	len = le32_to_cpu(*(__le32 *)(sb + 0x108));
	if (len > 0x7d000)
		len = 0x7d000;
	if (act == 1) {
		int mk;

		pos = off > 0 ? off : 0;
		if (0x110 + len > gpfile_sb_len)
			len = gpfile_sb_len > 0x110 ? gpfile_sb_len - 0x110 : 0;
		mk = gp_mkdir_parents(full);
		if (mk)
			pr_info("qsee_fp: GPFILE parents '%s' err=%d\n", full, mk);
		f = filp_open(full, O_RDWR | O_CREAT | O_SYNC, 0600);
		if (IS_ERR(f)) {
			err = (u32)(-PTR_ERR(f));
			pr_info("qsee_fp: GPFILE WRITE open '%s' off=%d len=%u err=%u\n",
				full, off, len, err);
			goto gp_reply;
		}
		n = kernel_write(f, sb + 0x110, len, &pos);
		filp_close(f, NULL);
		if (n < 0)
			err = (u32)(-n);
		else
			got = (u32)n;
		pr_info("qsee_fp: GPFILE WRITE '%s' off=%d n=%zd\n", full, off, n);
	} else if (act == 0) {
		pos = off > 0 ? off : 0;
		if (0x00c + len > gpfile_sb_len)
			len = gpfile_sb_len > 0x00c ? gpfile_sb_len - 0x00c : 0;
		f = filp_open(full, O_RDONLY, 0);
		if (IS_ERR(f)) {
			err = (u32)(-PTR_ERR(f));
			goto gp_reply;
		}
		n = kernel_read(f, sb + 0x00c, len, &pos);
		filp_close(f, NULL);
		if (n < 0)
			err = (u32)(-n);
		else
			got = (u32)n;
		pr_info("qsee_fp: GPFILE READ '%s' n=%zd\n", full, n);
	} else if (act == 2) {
		/* Reply success and keep the inode. Boot ba54aaea deleted
		 * RQmyhOdY8ywyaGUyB-TI here and the trustlet then logged
		 * reading 'ft_fp_serial_id.bin' failed / I/O error.
		 */
		pr_info("qsee_fp: GPFILE UNLINK '%s'\n", full);
	} else {
		goto gp_einval;
	}
	goto gp_reply;
gp_einval:
	err = 22;
gp_reply:
	*(__le32 *)(sb + 4) = cpu_to_le32(err);
	*(__le32 *)(sb + 8) = cpu_to_le32(got);
	wmb();
	return 0;
}


static int listener_respond(u32 lid, u32 status, struct qcom_scm_qseecom_resp *out)
{
	struct qcom_scm_desc d = {};
	struct qcom_scm_qseecom_resp r = {};
	int ret;

	d.owner = QSEECOM_TZ_OWNER_QSEE_OS;
	d.svc = QSEECOM_TZ_SVC_LISTENER;
	d.cmd = QSEECOM_TZ_CMD_LISTENER_RESPONSE;
	d.arginfo = QCOM_SCM_ARGS(2, QCOM_SCM_VAL, QCOM_SCM_VAL);
	d.args[0] = lid;
	d.args[1] = status;
	ret = inv(&d, &r);
	pr_info("qsee_fp: listener_respond lid=%u status=%u ret=%d result=0x%llx type=0x%llx data=0x%llx\n",
		lid, status, ret, r.result, r.resp_type, r.data);
	if (out)
		*out = r;
	return ret;
}

/*
 * Try REGISTER_LISTENER once for (sb, sb_sz). Returns 0 on TZ success.
 * M4c1: RPMB 0x6400 often SCM -22 on clean boot; fallback LISTENER_SB_SIZE.
 */
static int listener_register_try(u32 lid, void *sb, size_t sb_sz,
				 struct qcom_scm_qseecom_resp *rout)
{
	struct qcom_scm_desc d = {};
	struct qcom_scm_qseecom_resp r = {};
	int ret;

	d.owner = QSEECOM_TZ_OWNER_QSEE_OS;
	d.svc = QSEECOM_TZ_SVC_LISTENER;
	d.cmd = QSEECOM_TZ_CMD_LISTENER_REGISTER;
	d.arginfo = QCOM_SCM_ARGS(3, QCOM_SCM_VAL, QCOM_SCM_RW, QCOM_SCM_VAL);
	d.args[0] = lid;
	d.args[1] = qcom_tzmem_to_phys(sb);
	d.args[2] = sb_sz;
	pr_info("qsee_fp: REGISTER_LISTENER attempt id=%u pa=0x%llx len=0x%zx\n",
		lid, (u64)d.args[1], sb_sz);
	wmb();
	ret = inv(&d, &r);
	pr_info("qsee_fp: REGISTER_LISTENER id=%u ret=%d result=0x%llx type=0x%llx data=0x%llx\n",
		lid, ret, r.result, r.resp_type, r.data);

	if (ret || r.result != QSEECOM_RESULT_SUCCESS) {
		memset(&d, 0, sizeof(d));
		memset(&r, 0, sizeof(r));
		d.owner = QSEECOM_TZ_OWNER_QSEE_OS;
		d.svc = QSEECOM_TZ_SVC_LISTENER;
		d.cmd = 6;
		d.arginfo = QCOM_SCM_ARGS(3, QCOM_SCM_VAL, QCOM_SCM_RW, QCOM_SCM_VAL);
		d.args[0] = lid;
		d.args[1] = qcom_tzmem_to_phys(sb);
		d.args[2] = sb_sz;
		ret = inv(&d, &r);
		pr_info("qsee_fp: REGISTER_LISTENER(cmd6) id=%u ret=%d result=0x%llx\n",
			lid, ret, r.result);
	}
	if (rout)
		*rout = r;
	return (ret == 0 && r.result == QSEECOM_RESULT_SUCCESS) ? 0 : (ret ? ret : -EIO);
}

/* Register one listener id with its own tzmem shared buffer. */
static int listener_register_id(u32 lid, void **sb_out, size_t *len_out, bool *reg_out)
{
	struct qcom_tzmem_pool_config cfg = {
		.initial_size = LISTENER_SB_SIZE * 2 + SZ_1M,
		.policy = QCOM_TZMEM_POLICY_ON_DEMAND,
		.increment = SZ_1M,
		.max_size = LISTENER_SB_SIZE * 4 + SZ_4M,
	};
	void *sb;
	int ret;
	/* M4bp/M4c1: Android librpmb uses 0x6400; fall back to 64K if -22 */
	size_t sizes[2];
	unsigned int nsz = 1;
	unsigned int si;

	sizes[0] = (lid == RPMB_SERVICE) ? RPMB_SB_SIZE :
		   (lid == GPFILE_SERVICE) ? 0x7e000u :
		   (lid == TIME_SERVICE) ? TIME_SB_SIZE : LISTENER_SB_SIZE;
	if (lid == RPMB_SERVICE || lid == GPFILE_SERVICE) {
		sizes[1] = LISTENER_SB_SIZE;
		nsz = 2;
	}

	if (*reg_out)
		return 0;

	if (!lstnr_pool) {
		lstnr_pool = qcom_tzmem_pool_new(&cfg);
		if (IS_ERR(lstnr_pool)) {
			long e = PTR_ERR(lstnr_pool);

			pr_err("qsee_fp: listener pool %ld\n", e);
			lstnr_pool = NULL;
			return (int)e;
		}
	}

	for (si = 0; si < nsz; si++) {
		size_t sb_sz = sizes[si];

		sb = qcom_tzmem_alloc(lstnr_pool, sb_sz, GFP_KERNEL);
		if (!sb)
			return -ENOMEM;
		memset(sb, 0, sb_sz);
		ret = listener_register_try(lid, sb, sb_sz, NULL);
		if (ret == 0) {
			*sb_out = sb;
			*len_out = sb_sz;
			*reg_out = true;
			pr_info("qsee_fp: listener id=%u registered OK len=0x%zx\n",
				lid, sb_sz);
			return 0;
		}
		pr_warn("qsee_fp: listener id=%u len=0x%zx failed ret=%d%s\n",
			lid, sb_sz, ret,
			(si + 1 < nsz) ? " — retry larger SB" : "");
		qcom_tzmem_free(sb);
	}
	*sb_out = NULL;
	*len_out = 0;
	pr_err("qsee_fp: listener id=%u register failed (all sizes)\n", lid);
	return ret ? ret : -EIO;
}

static int listener_register(u32 lid)
{
	int ret;

	if (lid == FILE_SERVICE) {
		lstnr_id = FILE_SERVICE;
		return listener_register_id(FILE_SERVICE, &lstnr_sb, &lstnr_sb_len,
					    &lstnr_registered);
	}
	if (lid == RPMB_SERVICE)
		return listener_register_id(RPMB_SERVICE, &rpmb_sb, &rpmb_sb_len,
					    &rpmb_registered);
	if (lid == SSD_SERVICE)
		return listener_register_id(SSD_SERVICE, &ssd_sb, &ssd_sb_len,
					    &ssd_registered);
	if (lid == GPFILE_SERVICE)
		return listener_register_id(GPFILE_SERVICE, &gpfile_sb, &gpfile_sb_len,
					    &gpfile_registered);
	if (lid == TIME_SERVICE)
		return listener_register_id(TIME_SERVICE, &time_sb, &time_sb_len,
					    &time_registered);
	ret = listener_register_id(lid, &lstnr_sb, &lstnr_sb_len, &lstnr_registered);
	if (!ret)
		lstnr_id = lid;
	return ret;
}

/*
 * UFS RPMB via BSG SECURITY PROTOCOL (JESD220). LUN 49476 = SCSI_W_LUN+0x44 (RPMB).
 * Verified userspace GET_WRITE_COUNTER (wc≈70260) on this unit.
 */
#define UFS_RPMB_BSG_PATH	"/dev/bsg/0:0:0:49476"
#define UFS_RPMB_SEC_PROTOCOL	0xEC
#define UFS_RPMB_SEC_PROT_ID	0x01
#define RPMB_FRAME_SIZE		512
#ifndef SECURITY_PROTOCOL_IN
#define SECURITY_PROTOCOL_IN	0xA2
#define SECURITY_PROTOCOL_OUT	0xB5
#endif

/* JEDEC rpmb_frame (512B) */
struct qsee_rpmb_frame {
	u8 stuff[196];
	u8 key_mac[32];
	u8 data[256];
	u8 nonce[16];
	__be32 write_counter;
	__be16 addr;
	__be16 block_count;
	__be16 result;
	__be16 req_resp;
} __packed;

static struct scsi_device *ufs_rpmb_sdev(void)
{
	struct Scsi_Host *shost;
	struct scsi_device *sdev;
	unsigned short h;

	for (h = 0; h < 8; h++) {
		shost = scsi_host_lookup(h);
		if (!shost)
			continue;
		sdev = scsi_device_lookup(shost, 0, 0, 49476ULL);
		scsi_host_put(shost);
		if (sdev)
			return sdev;
	}
	return NULL;
}

static int ufs_rpmb_sec_xfer(void *buf, size_t len, bool send)
{
	struct scsi_device *sdev;
	u8 cdb[12] = { };
	int ret;

	if (len < RPMB_FRAME_SIZE || len > 4096)
		return -EINVAL;
	sdev = ufs_rpmb_sdev();
	if (!sdev) {
		pr_info("qsee_fp: RPMB scsi_device 0:0:0:49476 missing\n");
		return -ENODEV;
	}
	cdb[0] = send ? SECURITY_PROTOCOL_OUT : SECURITY_PROTOCOL_IN;
	cdb[1] = UFS_RPMB_SEC_PROTOCOL;
	cdb[3] = UFS_RPMB_SEC_PROT_ID;
	put_unaligned_be32(len, &cdb[6]);
	ret = scsi_execute_cmd(sdev, cdb, send ? REQ_OP_DRV_OUT : REQ_OP_DRV_IN,
			       buf, (unsigned int)len, 30 * HZ, 0, NULL);
	scsi_device_put(sdev);
	if (ret)
		pr_info("qsee_fp: RPMB scsi_execute_cmd send=%d ret=%d\n",
			send, ret);
	return ret;
}

/*
 * QSEE RPMB listener (librpmb): cmd@0 in 0x101..0x104.
 * When frames are present in the SB, forward to UFS RPMB. Layout refined by dumps.
 */
static u32 handle_rpmb_listener(void)
{
	u8 *sb = rpmb_sb;
	u32 cmd;
	int ret;

	if (!sb)
		return (u32)-ENODEV;
	cmd = le32_to_cpu(*(__le32 *)sb);
	rpmb_listener_reqs++;
	pr_info("qsee_fp: RPMB cmd=0x%x reqs=%u first16=%*ph\n",
		cmd, rpmb_listener_reqs, 16, sb);
	/*
	 * req_resp 1 and listener cmd 0x101 program the UFS RPMB key.
	 * That key is already provisioned (Android SFS). Never send it.
	 */
	if (cmd == RPMB_CMD_PROVISION) {
		pr_info("qsee_fp: RPMB refuse PROVISION (key program)\n");
		*(__le32 *)(sb + 4) = cpu_to_le32((u32)-EPERM);
		wmb();
		return 0;
	}

	switch (cmd) {
	case RPMB_CMD_GET_INFO: /* 0x104 — capacity/info into sb+0x10 */ {
		u8 frm[RPMB_FRAME_SIZE] = { };
		struct qsee_rpmb_frame *rf = (void *)frm;

		/* Real UFS GET_WRITE_COUNTER — do not stub OK without xfer. */
		rf->req_resp = cpu_to_be16(0x0002);
		ret = ufs_rpmb_sec_xfer(frm, RPMB_FRAME_SIZE, true);
		if (!ret)
			ret = ufs_rpmb_sec_xfer(frm, RPMB_FRAME_SIZE, false);
		pr_info("qsee_fp: RPMB GET_INFO ufs xfer ret=%d result=0x%x wc=%u\n",
			ret, be16_to_cpu(rf->result),
			be32_to_cpu(rf->write_counter));
		if (ret) {
			*(__le32 *)(sb + 4) = cpu_to_le32((u32)ret);
			break;
		}
		*(__le32 *)(sb + 4) = 0;
		*(__le32 *)(sb + 0x10) = cpu_to_le32(0x40);
		*(__le32 *)(sb + 0x14) = cpu_to_le32(512);
		*(__le32 *)(sb + 0x18) = cpu_to_le32(0x40 * 512);
		*(__le32 *)(sb + 0x1c) = cpu_to_le32(be32_to_cpu(rf->write_counter));
		break;
	}
	case RPMB_CMD_READ: /* 0x102 */
	case RPMB_CMD_WRITE: /* 0x103 */
	case RPMB_CMD_PROVISION: /* 0x101 */ {
		/*
		 * QSEE header: cmd@0, size@+8 (0x200), frame off@+0xc (0x18).
		 * Live SET_GROUP: we used off=0x14 req_resp=0x6 (a result
		 * code, not a request) and UFS returned result=0x1.
		 * Valid requests: 1=key 2=get_wc 3=write 4=read 5=result.
		 */
		u32 hdr_off = le32_to_cpu(*(__le32 *)(sb + 0xc));
		u32 hdr_sz = le32_to_cpu(*(__le32 *)(sb + 0x8));
		u32 nfr = le32_to_cpu(*(__le32 *)(sb + 0x4));
		u32 offs[6];
		unsigned int n_off = 0, i;
		bool ok = false;
		size_t in_len;

		if (nfr < 1 || nfr > 16)
			nfr = 1;
		in_len = (size_t)nfr * RPMB_FRAME_SIZE;
		if (hdr_sz >= RPMB_FRAME_SIZE &&
		    hdr_off + RPMB_FRAME_SIZE <= rpmb_sb_len)
			offs[n_off++] = hdr_off;
		offs[n_off++] = 0x18;
		offs[n_off++] = 0x10;
		offs[n_off++] = 0x14;
		offs[n_off++] = 0x20;
		offs[n_off++] = 0x8;
		for (i = 0; i < n_off; i++) {
			u32 off = offs[i];
			u8 *frm;
			size_t flen = RPMB_FRAME_SIZE;
			size_t ilen = in_len;
			struct qsee_rpmb_frame *rf;
			u16 rr;

			if (off + flen > rpmb_sb_len)
				continue;
			if (off + ilen > rpmb_sb_len)
				ilen = flen;
			frm = sb + off;
			rf = (void *)frm;
			rr = be16_to_cpu(rf->req_resp);
			if (cmd == RPMB_CMD_READ && rr == 0) {
				memset(frm, 0, flen);
				rf->req_resp = cpu_to_be16(0x0002);
				rr = 0x0002;
			}
			if (rr < 1 || rr > 5)
				continue;
			if (rr == 1) {
				pr_info("qsee_fp: RPMB refuse key-program req_resp=1 off=0x%x\n",
					off);
				continue;
			}
			/*
			 * The 1-frame counter read that TrustZone accepts is
			 * returned in place at the request offset. librpmb
			 * rpmb_ufs_read also stores (frame count << 9) at +8.
			 * A 1-frame reply is already 0x200 there. A 6-frame
			 * reply stays 512 unless this word is updated, so
			 * TrustZone only consumes the first frame.
			 * Copying the 6-frame read to +0x14 failed on
			 * 2026-10-06 20:44.
			 */
			pr_info("qsee_fp: RPMB try off=0x%x req_resp=0x%x nfr=%u ilen=%zu addr=%u bcnt=%u\n",
				off, rr, nfr, ilen,
				be16_to_cpu(rf->addr),
				be16_to_cpu(rf->block_count));
			/*
			 * librpmb rpmb_ufs_write OUTs every request frame, then a
			 * one-frame req_resp=5 result read, then INs 512 bytes to
			 * shared-buffer+0x14 and stores 0x200 at +8. The dispatcher
			 * also sets +0xc to 0x14. A 2-frame write that OUTs only
			 * the first frame returns result=0x1 and the write counter
			 * stays put (2026-10-06 21:09, extra 9ccccb12).
			 */
			if (cmd == RPMB_CMD_WRITE && rr == 3 && nfr > 1 &&
			    off + (size_t)nfr * RPMB_FRAME_SIZE <= rpmb_sb_len) {
				u32 w4 = le32_to_cpu(*(__le32 *)(sb + 0x14));
				u32 out_n = (w4 >= 1 && w4 <= 16) ? w4 : nfr;
				size_t out_len = (size_t)out_n * RPMB_FRAME_SIZE;
				u8 resp[RPMB_FRAME_SIZE];
				struct qsee_rpmb_frame rrq = { };
				struct qsee_rpmb_frame *rsp;
				struct qsee_rpmb_frame *lastf;

				if (out_len > 4096 || off + out_len > rpmb_sb_len)
					continue;
				lastf = (void *)(frm + out_len - RPMB_FRAME_SIZE);
				pr_info("qsee_fp: RPMB write off=0x%x nfr=%u out_n=%u w4=%u addr=%u bcnt=%u last_rr=0x%x\n",
					off, nfr, out_n, w4,
					be16_to_cpu(rf->addr),
					be16_to_cpu(lastf->block_count),
					be16_to_cpu(lastf->req_resp));
				ret = ufs_rpmb_sec_xfer(frm, out_len, true);
				if (ret)
					continue;
				rrq.req_resp = cpu_to_be16(0x0005);
				ret = ufs_rpmb_sec_xfer(&rrq, RPMB_FRAME_SIZE, true);
				if (ret)
					continue;
				ret = ufs_rpmb_sec_xfer(resp, RPMB_FRAME_SIZE, false);
				if (ret)
					continue;
				rsp = (void *)resp;
				pr_info("qsee_fp: RPMB write resp place=0x14 rr=0x%x result=0x%x wc=%u\n",
					be16_to_cpu(rsp->req_resp),
					be16_to_cpu(rsp->result),
					be32_to_cpu(rsp->write_counter));
				memcpy(sb + 0x14, resp, RPMB_FRAME_SIZE);
				*(__le32 *)(sb + 4) = 0;
				*(__le32 *)(sb + 8) = cpu_to_le32(RPMB_FRAME_SIZE);
				*(__le32 *)(sb + 0xc) = cpu_to_le32(0x14);
				if (!be16_to_cpu(rsp->result))
					last_persist_write = true;
				ok = true;
				break;
			}
			if (rr == 3)
				last_persist_write = true;
			ret = ufs_rpmb_sec_xfer(frm, flen, true);
			if (ret)
				continue;
			if (cmd == RPMB_CMD_READ && nfr > 1 &&
			    off + ilen <= rpmb_sb_len) {
				u8 *inb = kmalloc(ilen, GFP_KERNEL);
				struct qsee_rpmb_frame *last;
				u32 nbytes;

				if (!inb)
					continue;
				ret = ufs_rpmb_sec_xfer(inb, ilen, false);
				if (ret) {
					kfree(inb);
					continue;
				}
				last = (void *)(inb + ilen - RPMB_FRAME_SIZE);
				rf = (void *)inb;
				nbytes = (u32)ilen;
				pr_info("qsee_fp: RPMB xfer OK off=0x%x place=req rr0=0x%x res0=0x%x wc0=%u rrL=0x%x resL=0x%x nbytes=%u data0=%*ph\n",
					off, be16_to_cpu(rf->req_resp),
					be16_to_cpu(rf->result),
					be32_to_cpu(rf->write_counter),
					be16_to_cpu(last->req_resp),
					be16_to_cpu(last->result),
					nbytes, 8, rf->data);
				memcpy(sb + off, inb, ilen);
				kfree(inb);
				*(__le32 *)(sb + 4) = 0;
				*(__le32 *)(sb + 8) = cpu_to_le32(nbytes);
			} else {
				ret = ufs_rpmb_sec_xfer(frm, ilen, false);
				if (ret)
					continue;
				pr_info("qsee_fp: RPMB xfer OK off=0x%x result=0x%x wc=%u rr=0x%x\n",
					off, be16_to_cpu(rf->result),
					be32_to_cpu(rf->write_counter),
					be16_to_cpu(rf->req_resp));
				*(__le32 *)(sb + 4) = 0;
			}
			ok = true;
			break;
		}
		if (!ok)
			*(__le32 *)(sb + 4) = cpu_to_le32((u32)-EIO);
		break;
	}
	default:
		/* Soft-fail unknown so TZ may proceed */
		*(__le32 *)(sb + 4) = cpu_to_le32((u32)-EIO);
		*(__le32 *)(sb + 0xc) = cpu_to_le32(0x14);
		break;
	}
	wmb();
	return 0;
}

/*
 * libdrmtime.so (FP5 vendor) shared-buffer replies. clock_gettime(CLOCK_REALTIME)
 * is the path it takes when ATS_MODEM is unset. Do not answer a command that
 * was not in the buffer.
 */
static u32 handle_time_listener(void)
{
	u8 *sb = time_sb;
	u32 cmd;
	struct timespec64 ts;

	if (!sb || time_sb_len < 48)
		return (u32)-ENODEV;
	cmd = get_unaligned_le32(sb);
	time_listener_reqs++;
	pr_info("qsee_fp: TIME cmd=0x%x reqs=%u first16=%*ph\n",
		cmd, time_listener_reqs, 16, sb);
	ktime_get_real_ts64(&ts);
	memset(sb, 0, 64);
	switch (cmd) {
	case TIME_CMD_GET_UTC_SEC:
		put_unaligned_le32((u32)ts.tv_sec, sb + 4);
		pr_info("qsee_fp: TIME utc sec=%u\n", (u32)ts.tv_sec);
		break;
	case TIME_CMD_GET_SYSTIME: {
		struct tm tm;

		time64_to_tm(ts.tv_sec, 0, &tm);
		put_unaligned_le32(tm.tm_sec, sb + 4);
		put_unaligned_le32(tm.tm_min, sb + 8);
		put_unaligned_le32(tm.tm_hour, sb + 12);
		put_unaligned_le32(tm.tm_mday, sb + 16);
		put_unaligned_le32(tm.tm_mon, sb + 20);
		put_unaligned_le32((u32)tm.tm_year, sb + 24);
		put_unaligned_le32(tm.tm_wday, sb + 28);
		put_unaligned_le32(tm.tm_yday, sb + 32);
		put_unaligned_le32(0, sb + 36); /* gmtime: tm_isdst is 0 */
		pr_info("qsee_fp: TIME systime %04ld-%02d-%02d %02d:%02d:%02d\n",
			tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			tm.tm_hour, tm.tm_min, tm.tm_sec);
		break;
	}
	case TIME_CMD_GET_TIME_MS: {
		u64 ms = (u64)(u32)ts.tv_sec * 1000;

		put_unaligned_le64(ms, sb + 4);
		pr_info("qsee_fp: TIME ms=%llu\n", ms);
		break;
	}
	case TIME_CMD_GET_HLOS_UTC:
		put_unaligned_le32((u32)ts.tv_sec, sb + 4);
		put_unaligned_le32((u32)ts.tv_nsec, sb + 8);
		pr_info("qsee_fp: TIME hlos utc sec=%u nsec=%u\n",
			(u32)ts.tv_sec, (u32)ts.tv_nsec);
		break;
	case TIME_CMD_END:
		pr_info("qsee_fp: TIME end\n");
		break;
	default:
		put_unaligned_le32((u32)-1, sb + 4);
		pr_info("qsee_fp: TIME unknown cmd=0x%x\n", cmd);
		break;
	}
	wmb();
	return 0;
}

/* qseecomd SSD listener: status word at +4. Log the command that arrived. */
static void handle_ssd_listener(const char *why)
{
	ssd_listener_reqs++;
	if (!ssd_sb || ssd_sb_len < 16) {
		pr_info("qsee_fp: SSD request (%s) reqs=%u no sb\n",
			why, ssd_listener_reqs);
		return;
	}
	pr_info("qsee_fp: SSD cmd=0x%x (%s) reqs=%u first16=%*ph\n",
		get_unaligned_le32(ssd_sb), why, ssd_listener_reqs, 16, ssd_sb);
	put_unaligned_le32(0, ssd_sb + 4);
	wmb();
}

/*
 * FILE/RPMB/SSD. Android qseecomd has these up for the whole HAL session.
 * M4dj deferred them until after probe_device_ta so INIT CAP had TEE heap —
 * that also meant REPORT/ENROLL ran with no SFS, so rem never persisted.
 * Register after START (defer_listeners=0) or after Base (default) and
 * before ENROLL. Idempotent if already registered.
 */
static int scm_os_rpmb_check_prov(const char *why);

static void listeners_register_enabled(const char *why)
{
	int ret;

	bool want_android = enable_fs_listener || enable_rpmb_listener ||
			    enable_ssd_listener;

	if (!want_android && !enable_gpfile_listener)
		return;
	pr_info("qsee_fp: register listeners (%s) fs=%d rpmb=%d ssd=%d time=%d gpfile=%d\n",
		why, enable_fs_listener, enable_rpmb_listener,
		want_android, want_android, enable_gpfile_listener);
	if (enable_fs_listener) {
		ret = listener_register(FILE_SERVICE);
		pr_info("qsee_fp: FILE_SERVICE (%s) ret=%d\n", why, ret);
	}
	if (enable_rpmb_listener) {
		bool already = rpmb_registered;

		ret = listener_register(RPMB_SERVICE);
		if (ret)
			pr_warn("qsee_fp: RPMB (%s) ret=%d\n", why, ret);
		else if (!already) {
			u8 frm[RPMB_FRAME_SIZE] = { };
			struct qsee_rpmb_frame *rf = (void *)frm;
			int rret;

			rf->req_resp = cpu_to_be16(0x0002);
			rret = ufs_rpmb_sec_xfer(frm, RPMB_FRAME_SIZE, true);
			if (!rret)
				rret = ufs_rpmb_sec_xfer(frm, RPMB_FRAME_SIZE,
							 false);
			pr_info("qsee_fp: rpmb_hlos_init WC xfer (%s) ret=%d req_resp=0x%x result=0x%x wc=%u\n",
				why, rret, be16_to_cpu(rf->req_resp),
				be16_to_cpu(rf->result),
				be32_to_cpu(rf->write_counter));
			(void)scm_os_rpmb_check_prov(why);
		}
	}
	if (want_android) {
		ret = listener_register(SSD_SERVICE);
		pr_info("qsee_fp: SSD_SERVICE (%s) ret=%d\n", why, ret);
		ret = listener_register(TIME_SERVICE);
		pr_info("qsee_fp: TIME_SERVICE (%s) ret=%d\n", why, ret);
	}
	if (enable_gpfile_listener) {
		ret = listener_register(GPFILE_SERVICE);
		if (ret)
			pr_warn("qsee_fp: GPFILE 0x7000 (%s) ret=%d\n", why, ret);
		else
			pr_info("qsee_fp: GPFILE 0x7000 registered (%s) sb=%zu\n",
				why, gpfile_sb_len);
	}
}

static void listener_cleanup(void)
{
	int i;
	struct qcom_scm_desc d = {};
	struct qcom_scm_qseecom_resp r = {};

	for (i = 0; i < FS_MAX_FD; i++)
		fs_put(i);

	if (rpmb_registered) {
		memset(&d, 0, sizeof(d));
		memset(&r, 0, sizeof(r));
		d.owner = QSEECOM_TZ_OWNER_QSEE_OS;
		d.svc = QSEECOM_TZ_SVC_LISTENER;
		d.cmd = QSEECOM_TZ_CMD_LISTENER_DEREGISTER;
		d.arginfo = QCOM_SCM_ARGS(1, QCOM_SCM_VAL);
		d.args[0] = RPMB_SERVICE;
		(void)inv(&d, &r);
		rpmb_registered = false;
	}
	if (lstnr_registered) {
		memset(&d, 0, sizeof(d));
		memset(&r, 0, sizeof(r));
		d.owner = QSEECOM_TZ_OWNER_QSEE_OS;
		d.svc = QSEECOM_TZ_SVC_LISTENER;
		d.cmd = QSEECOM_TZ_CMD_LISTENER_DEREGISTER;
		d.arginfo = QCOM_SCM_ARGS(1, QCOM_SCM_VAL);
		d.args[0] = lstnr_id;
		(void)inv(&d, &r);
		lstnr_registered = false;
	}
	if (rpmb_sb) {
		qcom_tzmem_free(rpmb_sb);
		rpmb_sb = NULL;
	}
	if (lstnr_sb) {
		qcom_tzmem_free(lstnr_sb);
		lstnr_sb = NULL;
	}
	if (time_registered) {
		memset(&d, 0, sizeof(d));
		memset(&r, 0, sizeof(r));
		d.owner = QSEECOM_TZ_OWNER_QSEE_OS;
		d.svc = QSEECOM_TZ_SVC_LISTENER;
		d.cmd = QSEECOM_TZ_CMD_LISTENER_DEREGISTER;
		d.arginfo = QCOM_SCM_ARGS(1, QCOM_SCM_VAL);
		d.args[0] = TIME_SERVICE;
		(void)inv(&d, &r);
		time_registered = false;
	}
	if (ssd_registered) {
		memset(&d, 0, sizeof(d));
		memset(&r, 0, sizeof(r));
		d.owner = QSEECOM_TZ_OWNER_QSEE_OS;
		d.svc = QSEECOM_TZ_SVC_LISTENER;
		d.cmd = QSEECOM_TZ_CMD_LISTENER_DEREGISTER;
		d.arginfo = QCOM_SCM_ARGS(1, QCOM_SCM_VAL);
		d.args[0] = SSD_SERVICE;
		(void)inv(&d, &r);
		ssd_registered = false;
	}
	if (time_sb) {
		qcom_tzmem_free(time_sb);
		time_sb = NULL;
	}
	if (ssd_sb) {
		qcom_tzmem_free(ssd_sb);
		ssd_sb = NULL;
	}
	if (lstnr_pool) {
		qcom_tzmem_pool_free(lstnr_pool);
		lstnr_pool = NULL;
	}
}

/*
 * APP_SEND with incomplete/blocked-on-listener loop (vendor qseecom style).
 * CRITICAL: do NOT re-issue APP_SEND after a block — only LISTENER_RESPONSE
 * advances TZ and returns the next result (re-send caused SoC reset).
 */
static int app_send_ex(u32 app_id, void *req, size_t req_sz, void *rsp, size_t rsp_sz,
		       const char *tag)
{
	struct qcom_scm_desc d = {};
	struct qcom_scm_qseecom_resp r = {};
	int status;
	int loops;

	d.owner = QSEECOM_TZ_OWNER_TZ_APPS;
	d.svc = QSEECOM_TZ_SVC_APP_ID_PLACEHOLDER;
	d.cmd = QSEECOM_TZ_CMD_APP_SEND;
	d.arginfo = QCOM_SCM_ARGS(5, QCOM_SCM_VAL, QCOM_SCM_RW, QCOM_SCM_VAL,
				  QCOM_SCM_RW, QCOM_SCM_VAL);
	d.args[0] = app_id;
	d.args[1] = qcom_tzmem_to_phys(req);
	d.args[2] = req_sz;
	d.args[3] = qcom_tzmem_to_phys(rsp);
	d.args[4] = rsp_sz;

	status = inv(&d, &r);
	if (status) {
		pr_info("qsee_fp: SCM %s status=%d (firmware)\n", tag, status);
		return status;
	}

	for (loops = 0; loops < 48; loops++) {
		if (r.result == QSEECOM_RESULT_SUCCESS)
			return 0;

		if (r.result == QSEECOM_RESULT_INCOMPLETE ||
		    r.result == QSEECOM_RESULT_BLOCKED_ON_LISTENER) {
			u32 lid = (u32)r.data;

			pr_warn("qsee_fp: %s blocked/incomplete result=%llu type=0x%llx lid=%u\n",
				tag, r.result, r.resp_type, lid);
			if (lid == FILE_SERVICE || lid == lstnr_id) {
				dump_listener_sb("blocked", 128);
				(void)handle_fs_listener();
			} else if (lid == RPMB_SERVICE) {
				pr_info("qsee_fp: *** RPMB BLOCKED lid=%u (SFS IO) ***\n",
					lid);
				dump_sb("rpmb-blocked", rpmb_sb, rpmb_sb_len, 128);
				(void)handle_rpmb_listener();
			} else if (lid == SSD_SERVICE) {
				pr_info("qsee_fp: *** SSD BLOCKED lid=%u ***\n", lid);
				handle_ssd_listener(tag);
			} else if (lid == TIME_SERVICE) {
				pr_info("qsee_fp: *** TIME BLOCKED lid=%u ***\n", lid);
				(void)handle_time_listener();
			} else if (lid == GPFILE_SERVICE) {
				pr_info("qsee_fp: *** GPFILE BLOCKED lid=%u ***\n",
					lid);
				(void)handle_gpfile_listener();
			} else {
				pr_warn("qsee_fp: unhandled listener id %u\n", lid);
			}
			/* Vendor: LISTENER_DATA_RSP fills next result — never re-APP_SEND */
			status = listener_respond(lid, 0, &r);
			if (status) {
				pr_err("qsee_fp: %s listener_respond failed %d\n", tag, status);
				return status;
			}
			continue;
		}

		pr_info("qsee_fp: SCM %s result=0x%llx type=0x%llx data=0x%llx\n",
			tag, r.result, r.resp_type, r.data);
		return -EIO;
	}
	pr_err("qsee_fp: %s listener loop exceeded\n", tag);
	return -ELOOP;
}

/*
 * Android qseecomd rpmb_init_service runs CHECK_PROV before SFS.
 * HLOS GET_WRITE_COUNTER never makes TZ talk RPMB. Check-only.
 */
static int scm_os_rpmb_check_prov(const char *why)
{
	struct qcom_scm_desc d = {};
	struct qcom_scm_qseecom_resp r = {};
	int status, loops;

	d.owner = QSEECOM_TZ_OWNER_QSEE_OS;
	d.svc = QSEECOM_TZ_SVC_RPMB;
	d.cmd = QSEECOM_TZ_CMD_RPMB_CHECK_PROV;
	d.arginfo = QCOM_SCM_ARGS(0);
	status = inv(&d, &r);
	pr_info("qsee_fp: TZ RPMB check-prov (%s) scm=%d result=0x%llx type=0x%llx data=0x%llx\n",
		why, status, r.result, r.resp_type, r.data);
	if (status)
		return status;
	for (loops = 0; loops < 48; loops++) {
		if (r.result == QSEECOM_RESULT_SUCCESS) {
			pr_info("qsee_fp: TZ RPMB check-prov OK (%s)\n", why);
			return 0;
		}
		if (r.result == QSEECOM_RESULT_INCOMPLETE ||
		    r.result == QSEECOM_RESULT_BLOCKED_ON_LISTENER) {
			u32 lid = (u32)r.data;

			pr_info("qsee_fp: TZ RPMB check-prov blocked lid=%u\n", lid);
			if (lid == RPMB_SERVICE) {
				pr_info("qsee_fp: *** RPMB BLOCKED lid=%u (check-prov) ***\n",
					lid);
				(void)handle_rpmb_listener();
			} else if (lid == FILE_SERVICE || lid == lstnr_id) {
				(void)handle_fs_listener();
			} else if (lid == TIME_SERVICE) {
				(void)handle_time_listener();
			} else if (lid == SSD_SERVICE) {
				handle_ssd_listener("check-prov");
			}
			status = listener_respond(lid, 0, &r);
			if (status)
				return status;
			continue;
		}
		pr_info("qsee_fp: TZ RPMB check-prov key_status=0x%llx (%s)\n",
			r.result, why);
		return 0;
	}
	return -ELOOP;
}

/*
 * Keep the trustlet's own qsee_sfs_open line. A synchronized success does
 * not include that phrase; record it only when the open line was absent.
 * Never synthesize an open line the TA did not print.
 */
static void note_ta_sfs_open(const u8 *buf, u32 len)
{
	u32 i;

	/*
	 * One INIT_DEV log can contain a read open and a later create
	 * open. Keep the last qsee_sfs_open in this buffer.
	 */
	for (i = 0; i + 13 < len; i++) {
		if (!memcmp(buf + i, "qsee_sfs_open", 13)) {
			u32 j, n = 0;

			for (j = i; j < len && n < sizeof(last_sfs_open_txt) - 1; j++) {
				u8 c = buf[j];

				if (c < 0x20 || c >= 0x7f)
					break;
				last_sfs_open_txt[n++] = c;
			}
			last_sfs_open_txt[n] = 0;
		}
	}
}

/*
 * Declared rsp length is not the end of the trustlet log. Level 1
 * shrinks rsp0 (0x12a on extra f4b85fde) while serial text can sit
 * later in the response ion. Print only serial lines the trustlet
 * wrote. Never synthesize an open line the TA did not print.
 * Format is SFS-WIDE so the first SFS/SERIAL site stays put.
 */
static u32 scan_serial_text(const char *tag, const char *buf, u32 len,
			    u32 already)
{
	u32 si, hits = 0;

	if (!buf || len < 20 || already >= len)
		return 0;
	for (si = already; si + 16 < len; si++) {
		u32 from = si;
		bool hit = false;

		if (!strncmp(buf + si, "qsee_sfs_open", 13) ||
		    !strncmp(buf + si, "SFS_ERROR", 9) ||
		    !strncmp(buf + si, "SFS_NO_FILE", 11) ||
		    !strncmp(buf + si, "SFS_NO_MEMORY", 13) ||
		    !strncmp(buf + si, "SFS_FS_ERROR", 12) ||
		    !strncmp(buf + si, "SFS_VERI_HMAC_ERROR", 19) ||
		    !strncmp(buf + si, "ff_file_open", 12) ||
		    !strncmp(buf + si, "ft_fp_serial", 12) ||
		    !strncmp(buf + si, "__module_serial_sync", 20) ||
		    !strncmp(buf + si, "the new serial is synchronized", 30) ||
		    !strncmp(buf + si, "not exist, skip", 15) ||
		    !strncmp(buf + si, "Get serial from chip failed", 27))
			hit = true;
		if (!hit && !strncmp(buf + si, " leave.", 7)) {
			u32 b = si;
			u32 k;

			while (b > already &&
			       (u8)buf[b - 1] >= 0x20 &&
			       (u8)buf[b - 1] < 0x7f)
				b--;
			for (k = b; k + 12 <= si; k++) {
				if (!memcmp(buf + k, "ff_file_open", 12) ||
				    !memcmp(buf + k, "ft_fp_serial", 12)) {
					hit = true;
					from = b;
					break;
				}
			}
		}
		if (!hit)
			continue;
		/*
		 * The needle can sit mid-line. Success text is one
		 * sprintf: basename, line, then 'ff_file_open' leave.
		 * Printing at the needle drops the filename prefix.
		 */
		if (from == si) {
			u32 guard = 0;

			while (from > 0 && guard < 180 &&
			       (u8)buf[from - 1] >= 0x20 &&
			       (u8)buf[from - 1] < 0x7f) {
				from--;
				guard++;
			}
		}
		pr_info("qsee_fp: %s SFS-WIDE @0x%x: %.180s\n", tag, from,
			buf + from);
		hits++;
		while (from + 1 < len &&
		       (u8)buf[from + 1] >= 0x20 &&
		       (u8)buf[from + 1] < 0x7f)
			from++;
		si = from;
	}
	return hits;
}

static int send_ion(u32 app_id, void *sess, size_t sess_sz,
		    u32 cmd, u32 plen, const void *pay, const char *tag)
{
	void *req = sess;
	void *rsp = (u8 *)sess + RSP_ION_OFF;
	int ret;
	u32 st_req, st_rsp, st_req8, st_rsp8;
	u8 *p;

	if (sess_sz < SESS_BIG)
		return -EINVAL;
	if (max_sends > 0 && sends_done >= max_sends) {
		pr_info("qsee_fp: max_sends=%d reached, skip %s cmd=0x%x\n",
			max_sends, tag, cmd);
		return -EAGAIN;
	}
	sends_done++;
	/*
	 * Android reuses the same ion mapping. Never wipe the whole window —
	 * PRE_ENROLL leaves challenge + TA session state in the shared buffer.
	 * Only overwrite cmd@0 and plen@4; zero err@+8 on send (status slot).
	 * Do NOT touch +0xc (TA flag seen as 1 after PRE) unless writing payload.
	 */
	p = req;
	*(__le32 *)p = cpu_to_le32(cmd);
	*((__le32 *)p + 1) = cpu_to_le32(plen);
	/* err/status @+8 must be 0 on send; writing plen@8 caused SCM -22 */
	*((__le32 *)p + 2) = 0;
	if (pay && plen && (u32)pay_off + plen <= REQ_ION)
		memcpy(p + pay_off, pay, plen);
	/* leave rsp as-is except clear length word so we do not mis-parse stale logs */
	*(__le32 *)rsp = 0;

	ret = app_send_ex(app_id, req, REQ_ION, rsp, RSP_ION, tag);
	st_req = le32_to_cpu(*(__le32 *)req);
	st_rsp = le32_to_cpu(*(__le32 *)rsp);
	st_req8 = le32_to_cpu(*((__le32 *)req + 2));
	st_rsp8 = le32_to_cpu(*((__le32 *)rsp + 2));
	last_send_app_err = st_req8;
	last_send_tmpl_missing = false;
	last_send_saw_ff_tmpl = false;
	last_send_auth_id_ok = false;

	last_send_vio_ok = false;
	last_send_img_calib = false;
	/* M4bq: do NOT clear base/dac/step — sticky once seen (Step1 was lost
	 * when SCAN/LOGCAT reset flags after WMODE-5 logged ******Step1). */
	/* last_send_base_update / last_send_img_dac_ok / last_send_img_step sticky */
	last_send_finger_down = false;
	last_send_frame_raw = false;
	last_send_frame_avgv = 0;
	last_send_enrolling = false;
	last_send_new_fid = false;
	last_send_esd = false;
	last_send_qes_ok = false;
	last_send_image_ready = false;
	last_send_sensor_raw = false;
	last_send_spi_mode_fail = false;
	last_send_itype = 0;
	last_send_sfs_generic = false;
	last_sfs_open_txt[0] = 0;
	/* Bit31 of req[0] set = HAL "completed"; also treat non-ffffffff rsp as hit */
	if (ret == 0 && (st_rsp != 0xffffffff || (st_req & 0x80000000))) {
		/* Full response, not the short length word. A later
		 * qsee_sfs_open in this buffer replaces an earlier one.
		 */
		note_ta_sfs_open((const u8 *)rsp, RSP_ION);
		if (strstr(tag, "INIT_DEV"))
			note_ta_sfs_open((const u8 *)req, REQ_ION);
		if (strcmp(tag, "dev-QEV-auth-kick"))
			pr_info("qsee_fp: ION HIT %s ret=%d req0=0x%x rsp0=0x%x err=0x%x rsp8=0x%x\n",
				tag, ret, st_req, st_rsp, st_req8, st_rsp8);
		if (!quiet_send) {
			/* Full TA log window (rsp0 is length); also scan req for TA log */
			dump_interesting(tag, rsp, min_t(size_t, RSP_ION, 2048));
			dump_interesting(tag, req, min_t(size_t, 2048, REQ_ION));
			/* Continuous log from rsp and req if length looks sane */
			if (st_rsp > 4 && st_rsp < 0x8000) {
				size_t i;
				char line[160];
				size_t li = 0;
				u32 log_n = min_t(u32, st_rsp, 4096);

				pr_info("qsee_fp: %s LOGCONT begin len=%u\n", tag, st_rsp);
				for (i = 4; i < log_n; i++) {
					u8 c = ((u8 *)rsp)[i];

					if (c >= 0x20 && c < 0x7f) {
						if (li < sizeof(line) - 1)
							line[li++] = c;
					} else if (c == 0 || c == '\n' || c == '|') {
						if (li) {
							line[li] = 0;
							pr_info("qsee_fp: %s LOG: %s\n", tag, line);
							li = 0;
						}
					} else {
						if (li) {
							line[li] = 0;
							pr_info("qsee_fp: %s LOG: %s\n", tag, line);
							li = 0;
						}
					}
				}
				if (li) {
					line[li] = 0;
					pr_info("qsee_fp: %s LOG: %s\n", tag, line);
				}
				pr_info("qsee_fp: %s LOGCONT end\n", tag);
			}
		} else {
			/* Quiet path: only print high-value markers from TA log */
			if (st_rsp > 4 && st_rsp < 0x8000) {
				const char *scan = (const char *)rsp;
				u32 log_n = min_t(u32, st_rsp, 4096);
				u32 si;

				for (si = 4; si + 12 < log_n; si++) {
					if (!strncmp(scan + si, "vio 1.8v success", 16))
						last_send_vio_ok = true;
					if (!strncmp(scan + si, "configure spi mode failed", 25) ||
					    !strncmp(scan + si, "config spi mode all failure", 27) ||
					    !strncmp(scan + si, "chip_id is shifted", 18))
						last_send_spi_mode_fail = true;
					if (!strncmp(scan + si, "img calib", 9))
						last_send_img_calib = true;
					if (!strncmp(scan + si, "Base update success", 19) ||
					    !strncmp(scan + si, "Base update", 11))
						last_send_base_update = true;
					if (!strncmp(scan + si, "get image dac", 13) ||
					    !strncmp(scan + si, "get img dac", 11) ||
					    !strncmp(scan + si, "Got Img Dac ok", 14) ||
					    !strncmp(scan + si, "newDAC", 6)) {
						last_send_img_dac_ok = true;
						pr_info("qsee_fp: %s DAC-OK: %.72s\n", tag,
							scan + si);
					}
					if (!strncmp(scan + si, "get fdt dac [success]", 21) ||
					    !strncmp(scan + si, "get fdt dac [", 13)) {
						last_send_fdt_ok = true;
						pr_info("qsee_fp: %s FDT-DAC: %.72s\n", tag,
							scan + si);
					}
					if (!strncmp(scan + si, "SFS_ERROR_GENERIC", 17))
						last_send_sfs_generic = true;
					if (!strncmp(scan + si, "SFS_ERROR", 9) ||
					    !strncmp(scan + si, "qsee_sfs_open", 13) ||
					    !strncmp(scan + si, "not exist, skip", 15) ||
					    !strncmp(scan + si, "Get serial from chip failed", 27) ||
					    !strncmp(scan + si, "qsee_sfs_is_anti", 16) ||
					    !strncmp(scan + si, "__module_serial_sync", 20) ||
					    !strncmp(scan + si, "the new serial is synchronized", 30) ||
					    !strncmp(scan + si, "ff_file_open", 12) ||
					    !strncmp(scan + si, "ft_fp_serial", 12))
						pr_info("qsee_fp: %s SFS/SERIAL: %.120s\n",
							tag, scan + si);
					/* Level-1 success text is "%s:%u, '%s' leave."
					 * and does not contain qsee_sfs_open. Print the
					 * trustlet bytes only, and only when this line
					 * is the serial open.
					 */
					if (!strncmp(scan + si, " leave.", 7)) {
						u32 b = si;
						u32 k;
						bool serial_leave = false;

						while (b > 4 &&
						       (u8)scan[b - 1] >= 0x20 &&
						       (u8)scan[b - 1] < 0x7f)
							b--;
						for (k = b; k + 12 <= si; k++) {
							if (!memcmp(scan + k, "ff_file_open", 12) ||
							    !memcmp(scan + k, "ft_fp_serial", 12)) {
								serial_leave = true;
								break;
							}
						}
						if (serial_leave)
							pr_info("qsee_fp: %s SFS/SERIAL: %.120s\n",
								tag, scan + b);
					}
					if (!strncmp(scan + si, "abnormal state when calibration", 31) ||
					    !strncmp(scan + si, "fail to get img dac", 19) ||
					    !strncmp(scan + si, "find DAC fail", 13) ||
					    !strncmp(scan + si, "skip step2 and step3", 20) ||
					    !strncmp(scan + si, "too many bad pixels", 19) ||
					    !strncmp(scan + si, "sensor may in esd", 17) ||
					    !strncmp(scan + si, "Got image DAC FAIL", 18) ||
					    !strncmp(scan + si, "Too many failures", 17) ||
					    !strncmp(scan + si, "image dac over flow", 19) ||
					    !strncmp(scan + si, "image dac is not in effective range", 35) ||
					    !strncmp(scan + si, "restore last dac", 16) ||
					    !strncmp(scan + si, "Left_Point", 10) ||
					    !strncmp(scan + si, "Right_Point", 11) ||
					    !strncmp(scan + si, "get new better dac", 18) ||
					    !strncmp(scan + si, "case 1 left", 11) ||
					    !strncmp(scan + si, "case 4 left", 11))
						pr_info("qsee_fp: %s IMG-CALIB-DIAG: %.96s\n",
							tag, scan + si);
					if (!strncmp(scan + si, "fdt base check result: [success]", 32) ||
					    !strncmp(scan + si, "fdt base check result", 21)) {
						last_send_fdt_ok = true;
						pr_info("qsee_fp: %s FDT-BASE: %.48s\n", tag,
							scan + si);
					}
					if (!strncmp(scan + si, "find DAC fail", 13))
						pr_info("qsee_fp: %s DAC-FAIL: %.64s\n", tag,
							scan + si);
					if (!strncmp(scan + si, "Step1:", 6) ||
					    !strncmp(scan + si, "Step2:", 6) ||
					    !strncmp(scan + si, "Step3:", 6) ||
					    !strncmp(scan + si, "******Step", 10))
						last_send_img_step = true;
					/* M4br: dump WMODE residual once when try:2 seen without newDAC */
					if (!strncmp(scan + si, "img calibration->try: 2", 23) ||
					    !strncmp(scan + si, "img calibration->try:2", 22)) {
						static bool try2_dumped;
						u32 j, lj = 0;
						char ln[128];

						if (!try2_dumped && !last_send_img_dac_ok) {
							try2_dumped = true;
							pr_info("qsee_fp: %s TRY2 residual dump len=%u (pre-newDAC?)\n",
								tag, log_n);
							for (j = 4; j < log_n; j++) {
								char c = scan[j];

								if (c >= 0x20 && c < 0x7f &&
								    lj < 120)
									ln[lj++] = c;
								else if (lj > 6) {
									ln[lj] = 0;
									pr_info("qsee_fp: TRY2-RES: %s\n",
										ln);
									lj = 0;
								} else {
									lj = 0;
								}
							}
							if (lj > 6) {
								ln[lj] = 0;
								pr_info("qsee_fp: TRY2-RES: %s\n",
									ln);
							}
						}
					}
					if (!strncmp(scan + si, "skip step2 and step3", 20) ||
					    !strncmp(scan + si, "fail to get img dac", 19) ||
					    !strncmp(scan + si, "Got image DAC FAIL", 18) ||
					    !strncmp(scan + si, "Too many failures, stop calibration",
						     35))
						pr_info("qsee_fp: %s IMG-CALIB-FAIL: %.64s\n",
							tag, scan + si);
					if (!strncmp(scan + si, "image buffer = NULL", 19) ||
					    !strncmp(scan + si, "nav base or raw data memory is null",
						     35) ||
					    !strncmp(scan + si, "failed to malloc memory for raw",
						     31))
						pr_info("qsee_fp: %s IMG-BUF-NULL: %.56s\n",
							tag, scan + si);
					if (!strncmp(scan + si, "configure spi mode failed", 25) ||
					    !strncmp(scan + si, "chip_id is shifted", 18) ||
					    !strncmp(scan + si, "config spi mode all failure", 27))
						last_send_spi_mode_fail = true;
					if (!strncmp(scan + si, "frame raw[", 10) &&
					    scan[si + 10] >= '0' && scan[si + 10] <= '7' &&
					    scan[si + 11] == ']') {
						const char *av;

						last_send_frame_raw = true;
						last_send_frame_slots |=
							(u8)(1u << (scan[si + 10] - '0'));
						/* Same MARK line: frame raw[0]: avgv = 863 */
						av = strnstr(scan + si, "avgv =", 80);
						if (av)
							last_send_frame_avgv = parse_avgv_num(av + 6);
					} else if (!strncmp(scan + si, "frame raw", 9) ||
						   !strncmp(scan + si, "avgv =", 6) ||
						   !strncmp(scan + si, "avgv=", 5)) {
						last_send_frame_raw = true;
						if (!strncmp(scan + si, "avgv =", 6))
							last_send_frame_avgv =
								parse_avgv_num(scan + si + 6);
						else if (!strncmp(scan + si, "avgv=", 5))
							last_send_frame_avgv =
								parse_avgv_num(scan + si + 5);
					}
					if (!strncmp(scan + si, "current inte times:", 19))
						last_send_inte = parse_avgv_num(scan + si + 19);
					if (!strncmp(scan + si, "support max image inte", 22))
						pr_info("qsee_fp: %s MAX-INTE: %.72s\n",
							tag, scan + si);
					if (!strncmp(scan + si, "last mode is fdt", 16) ||
					    !strncmp(scan + si, "return out 1(normal fdt", 23) ||
					    !strncmp(scan + si, "return out 2(gesture fdt", 24))
						pr_info("qsee_fp: %s FDT-ONLY early-return: %.48s\n",
							tag, scan + si);
					if (!strncmp(scan + si, "sensor raw data", 15) ||
					    !strncmp(scan + si, "scanning totally", 16))
						last_send_sensor_raw = true;
					if (!strncmp(scan + si, "got image ready", 15) ||
					    !strncmp(scan + si, "image ready event", 17))
						last_send_image_ready = true;
					if (!strncmp(scan + si, "enrolled fid", 12))
						last_send_enrolling = true;
					if (!strncmp(scan + si, "new fid", 7))
						last_send_new_fid = true;
					/* ESD only from newest query-event-state (below) */
					if (!strncmp(scan + si, "frame raw", 9) ||
					    !strncmp(scan + si, "enrolling...", 12) ||
					    !strncmp(scan + si, "rem = ", 6) ||
					    !strncmp(scan + si, "enrolled fid", 12) ||
					    !strncmp(scan + si, "new fid", 7) ||
					    !strncmp(scan + si, "sn num", 6) ||
					    !strncmp(scan + si, "got fdt", 7) ||
					    !strncmp(scan + si, "img calib", 9) ||
					    !strncmp(scan + si, "newDAC", 6) ||
					    !strncmp(scan + si, "get image dac", 13) ||
					    !strncmp(scan + si, "get fdt dac", 11) ||
					    !strncmp(scan + si, "******Step", 10) ||
					    !strncmp(scan + si, "Base update", 11) ||
					    !strncmp(scan + si, "SFS_ERROR", 9) ||
					    !strncmp(scan + si, "successfully init", 17) ||
					    !strncmp(scan + si, "successfully created", 20) ||
					    !strncmp(scan + si, "vio 1.8v success", 16) ||
					    !strncmp(scan + si, "vio 1.8v fail", 13) ||
					    !strncmp(scan + si, "configure spi mode failed", 25) ||
					    !strncmp(scan + si, "chip_id is shifted", 18) ||
					    !strncmp(scan + si, "broken off", 10) ||
					    !strncmp(scan + si, "interrupt type", 14) ||
					    !strncmp(scan + si, "check finger status", 19) ||
					    !strncmp(scan + si, "got finger touched", 18) ||
					    !strncmp(scan + si, "got finger released", 19) ||
					    !strncmp(scan + si, "current inte", 12) ||
					    !strncmp(scan + si, "got image ready", 15) ||
					    !strncmp(scan + si, "sensor raw data", 15) ||
					    !strncmp(scan + si, "query event state", 17) ||
					    !strncmp(scan + si, "finger touch", 12) ||
					    !strncmp(scan + si, "avgv =", 6) ||
					    !strncmp(scan + si, "Null pointer", 12) ||
					    !strncmp(scan + si, "shared memory", 13) ||
					    !strncmp(scan + si, "image buffer", 12) ||
					    !strncmp(scan + si, "last mode is", 12) ||
					    !strncmp(scan + si, "last mode:", 10) ||
					    !strncmp(scan + si, "img_scan", 8) ||
					    !strncmp(scan + si, "ft9362_img", 10) ||
					    !strncmp(scan + si, "GetFpImage", 10) ||
					    !strncmp(scan + si, "read image", 10) ||
					    !strncmp(scan + si, "sensor raw data", 15) ||
					    !strncmp(scan + si, "SETDAC", 6) ||
					    !strncmp(scan + si, "last mode", 9) ||
					    !strncmp(scan + si, "host omit", 9) ||
					    !strncmp(scan + si, "abnormality", 11) ||
					    !strncmp(scan + si, "chip_id is shifted", 18) ||
					    !strncmp(scan + si, "got device id", 13) ||
					    !strncmp(scan + si, "RETURN", 6) ||
					    !strncmp(scan + si, "ENTER", 5) ||
					    !strncmp(scan + si, "nav_mode", 8) ||
					    !strncmp(scan + si, "img_mode", 8) ||
					    !strncmp(scan + si, "First gesture", 13)) {
						/* 96 chars keeps usMiddleTemp: NNN full.
						 * AUTH kick QEV MARKs hung GTK (WAIT).
						 */
						if (strcmp(tag, "dev-QEV-auth-kick"))
							pr_info("qsee_fp: %s MARK: %.96s\n", tag,
								scan + si);
					}
				}
				/*
				 * TA log is a rolling window — old lines linger.
				 * M4ct: prefer sticky finger_touch / itype 0x2 over a trailing
				 * "[unknow]" (M4cs: last was often unknow while MARK still
				 * showed finger touch). Leave clears; esd last wins.
				 * Android finger press: "[finger touch]" / itype 0x2.
				 */
				{
					const char *last_qes = NULL;
					const char *last_itype = NULL;
					bool saw_touch = false;
					bool saw_leave = false;
					bool saw_esd = false;
					bool saw_itype_down = false;
					bool saw_itype_up = false;
					u32 si2, itype_any_down = 0;

					for (si2 = 4; si2 + 24 < log_n; si2++) {
						if (!strncmp(scan + si2, "query event state: [", 20)) {
							const char *st = scan + si2 + 20;

							last_qes = scan + si2;
							if (!strncmp(st, "finger touch]", 13))
								saw_touch = true;
							else if (!strncmp(st, "finger leave]", 13))
								saw_leave = true;
							else if (!strncmp(st, "esd]", 4))
								saw_esd = true;
						}
						if (!strncmp(scan + si2, "interrupt type", 14)) {
							const char *hx = NULL;
							u32 itype, k;

							last_itype = scan + si2;
							for (k = 0; k + 2 < 40 && last_itype[k];
							     k++) {
								if (last_itype[k] == '0' &&
								    (last_itype[k + 1] == 'x' ||
								     last_itype[k + 1] == 'X')) {
									hx = last_itype + k;
									break;
								}
							}
							itype = parse_hex_prefix(hx);
							if (itype & 0x2) {
								saw_itype_down = true;
								itype_any_down = itype;
							}
							if (itype & 0x4)
								saw_itype_up = true;
						}
					}
					if (last_qes) {
						const char *st = last_qes + 20;

						if (!strncmp(st, "esd]", 4) ||
						    (saw_esd && !saw_touch && !saw_itype_down)) {
							last_send_esd = true;
							last_send_qes_ok = false;
							if (strcmp(tag, "dev-QEV-auth-kick"))
								pr_info("qsee_fp: %s QES_LAST esd\n", tag);
						} else if (!strncmp(st, "finger leave]", 13) &&
							   !saw_itype_down) {
							last_send_esd = false;
							last_send_qes_ok = true;
							pr_info("qsee_fp: %s QES_LAST finger_leave\n",
								tag);
						} else if (!strncmp(st, "finger touch]", 13) ||
							   (saw_touch &&
							    !(!strncmp(st, "finger leave]", 13)))) {
							/* last touch, or sticky touch over unknow */
							last_send_finger_down = true;
							last_send_esd = false;
							last_send_qes_ok = true;
							pr_info("qsee_fp: %s QES sticky finger_touch (last='%.16s' saw_touch=%d)\n",
								tag, st, saw_touch);
						} else {
							/* unknow/idle: do not clear if itype saw DOWN */
							last_send_esd = false;
							last_send_qes_ok = true;
							if (saw_itype_down || saw_touch) {
								last_send_finger_down = true;
								pr_info("qsee_fp: %s QES other but sticky DOWN (saw_touch=%d saw_itype_down=%d last='%.16s')\n",
									tag, saw_touch,
									saw_itype_down, st);
							} else {
								pr_info("qsee_fp: %s QES_LAST other '%.24s'\n",
									tag, st);
							}
						}
						(void)saw_leave;
					}
					if (last_itype) {
						const char *hx = NULL;
						u32 itype, k;

						for (k = 0; k + 2 < 40 && last_itype[k]; k++) {
							if (last_itype[k] == '0' &&
							    (last_itype[k + 1] == 'x' ||
							     last_itype[k + 1] == 'X')) {
								hx = last_itype + k;
								break;
							}
						}
						itype = parse_hex_prefix(hx);
						/* sticky: keep DOWN if any itype had 0x2 and last
						 * is not pure UP without DOWN bits
						 */
						if (saw_itype_down && !(itype & 0x4 &&
									!(itype & 0x2))) {
							if (!(itype & 0x2))
								itype |= 0x2;
							if (!itype)
								itype = itype_any_down;
						}
						last_send_itype = itype;
						if (itype & 0x2) {
							last_send_finger_down = true;
							pr_info("qsee_fp: %s FINGER_DOWN itype=0x%x\n",
								tag, itype);
						}
						if (itype & 0x200)
							pr_info("qsee_fp: %s ITYPE_RST 0x%x\n",
								tag, itype);
						if (itype & 0x400) {
							last_send_esd = true;
							pr_info("qsee_fp: %s ITYPE_ESD 0x%x\n",
								tag, itype);
						}
						(void)saw_itype_up;
					} else if (saw_itype_down) {
						last_send_itype = itype_any_down | 0x2;
						last_send_finger_down = true;
					}
				}
			}
		}
		{
			u32 already = 0;
			u32 wide_rsp, wide_req = 0;

			if (st_rsp > 4 && st_rsp < 0x8000)
				already = min_t(u32, st_rsp, 4096);
			wide_rsp = scan_serial_text(tag, (const char *)rsp,
						    RSP_ION, already);
			if (strstr(tag, "INIT_DEV"))
				wide_req = scan_serial_text(tag, (const char *)req,
							    REQ_ION, 0);
			if (strstr(tag, "INIT_DEV"))
				pr_info("qsee_fp: %s SFS-WIDE hits rsp=%u req=%u rsp0=0x%x\n",
					tag, wide_rsp, wide_req, st_rsp);
		}
	} else {
		pr_info("qsee_fp: ION %s ret=%d req0=0x%x rsp0=0x%x err=0x%x\n",
			tag, ret, st_req, st_rsp, st_req8);
	}
	return ret;
}

/*
 * TA-mediated SPI / SRAM image pull (REE alternative to geni_spi pinmux).
 * HAL: XFER 0x1009 layout usr: u32 wlen, u32 rlen, u8 wdata[wlen], then rdata.
 * READ_SRAM 0x101b: u32 addr, u32 len — data lands in shared ion after header.
 */
static int ta_xfer_spi(u32 app_id, void *sess, const u8 *wbuf, u32 wlen,
		       u32 rlen, const char *tag)
{
	u8 *usr = (u8 *)sess + 0x10;
	u32 plen = 8 + wlen + rlen;
	int er;

	if (plen > 0x70000 || !sess)
		return -EINVAL;
	memset(usr, 0, plen);
	*(__le32 *)(usr + 0) = cpu_to_le32(wlen);
	*(__le32 *)(usr + 4) = cpu_to_le32(rlen);
	if (wbuf && wlen)
		memcpy(usr + 8, wbuf, wlen);
	if (rlen)
		memset(usr + 8 + wlen, 0xA5, rlen);
	er = send_ion(app_id, sess, SESS_BIG, FF_TA_XFER_SPI, plen, usr, tag);
	return er;
}

/* Defined later; used by try_force_image_path */
static u32 try_pull_image_ree_ta(u32 app_id, void *sess, const char *why);

/* Full VDD off/on via L2 sysfs — stronger than RESET alone for sticky ESD */
static void l2_vdd_cycle(const char *why)
{
	struct file *fv;
	loff_t p = 0;

	fv = filp_open("/sys/class/focaltech_fp/focaltech_fp/vdd", O_WRONLY, 0);
	if (IS_ERR_OR_NULL(fv)) {
		pr_info("qsee_fp: l2_vdd_cycle open fail (%s)\n", why);
		return;
	}
	kernel_write(fv, "0\n", 2, &p);
	msleep(80);
	p = 0;
	kernel_write(fv, "1\n", 2, &p);
	fput(fv);
	{
		struct file *fs;

		p = 0;
		fs = filp_open("/sys/class/focaltech_fp/focaltech_fp/spiclk",
			       O_WRONLY, 0);
		if (!IS_ERR_OR_NULL(fs)) {
			kernel_write(fs, "1\n", 2, &p);
			fput(fs);
		}
	}
	pr_info("qsee_fp: l2_vdd_cycle done (%s)\n", why);
	msleep(50);
}

/*
 * Sticky [esd] after ENROLL blocks FDT finger_touch (M4x/M4ag holds).
 * M4c4: escalate gently — soft ESD_HANDLE first; L2 reset next; VDD last.
 * M4c1 holds: immediate L2+VDD+WMODE before finger left QES [esd] permanently
 * and AFR's re-WMODE under finger re-induced [esd] (FDT-only CAP).
 */
/*
 * Returns true if finger DOWN (QES touch or itype 0x2) was observed during clear.
 * M4ce: mid-hold saw itype=0x2 during esd_clear but continued and lost the edge.
 */
static bool esd_clear_loop(u32 app_id, void *sess, const char *why, int max_try)
{
	u8 esd_pay[4] = { 0, 0, 0, 0 };
	u8 wm_local[8];
	u8 *usr = (u8 *)sess + 0x10;
	int t;
	bool saw_down = false;
	/* M4cv: after Base, never L2-reset / VDD / soft-CAP thrash */
	bool soft_only = preserve_img_base && g_img_base_ok;

	if (max_try < 1)
		max_try = 1;
	if (max_try > 6)
		max_try = 6;
	if (soft_only && max_try > 3)
		max_try = 3;

	if (soft_only)
		pr_info("qsee_fp: M4cv esd_clear soft-only (preserve Base) (%s)\n",
			why);

	for (t = 0; t < max_try; t++) {
		memset(usr, 0, 8);
		send_ion(app_id, sess, SESS_BIG, FF_TA_QUERY_EVENT, 4, usr,
			 "dev-QEV-esd-check");
		if (last_send_finger_down || (last_send_itype & 0x2)) {
			pr_info("qsee_fp: esd_clear SAW DOWN try=%d itype=0x%x touch=%d (%s)\n",
				t, last_send_itype, last_send_finger_down, why);
			saw_down = true;
			return true;
		}
		/*
		 * Require positive non-[esd] QES parse. Missing QES line used to
		 * look like "clear" while sticky esd returned on the next poll.
		 */
		if (last_send_qes_ok && !last_send_esd) {
			pr_info("qsee_fp: esd_clear ok try=%d (%s)\n", t, why);
			return saw_down;
		}
		pr_info("qsee_fp: esd_clear need try=%d/%d esd=%d qes_ok=%d itype=0x%x (%s)\n",
			t + 1, max_try, last_send_esd, last_send_qes_ok,
			last_send_itype, why);

		/*
		 * M4db3: soft-only must NOT call ESD_HANDLE — live holds show
		 * QEV [esd] then ESD_HANDLE SCM -22 kills TA before PRESS banner.
		 * Leave sticky esd; LATCH+softESD under finger may clear later.
		 */
		if (soft_only) {
			pr_info("qsee_fp: M4db soft-only leave sticky esd (no ESD_HANDLE) (%s)\n",
				why);
			return saw_down;
		}

		/* Soft TA ESD_HANDLE only first (no GPIO thrash) */
		if (send_ion(app_id, sess, SESS_BIG, FF_TA_ESD_HANDLE, 4, esd_pay,
			     "dev-ESD-HANDLE-loop") == -22) {
			pr_err("qsee_fp: esd_clear ESD_HANDLE SCM -22 (%s) — stop\n",
			       why);
			return saw_down;
		}
		msleep(30);

		if (t >= 1) {
			/* try1+: L2 reset IC (no VDD yet) — kills Base (M4cv) */
			l2_reset_ic(why);
			msleep(40);
			send_ion(app_id, sess, SESS_BIG, FF_TA_ESD_HANDLE, 4, esd_pay,
				 "dev-ESD-HANDLE-post-rst");
			msleep(30);
		}
		if (t >= 2) {
			/* try2+: VDD cycle last resort (can re-stick ESD on this unit) */
			l2_vdd_cycle(why);
			msleep(50);
			l2_reset_ic(why);
			msleep(40);
		}

		/*
		 * Re-arm FDT_DOWN only after handle/reset. Avoid SLEEP on try0 —
		 * WMODE thrash under sample wait induced [esd] (M4c1 AFR residual).
		 */
		if (t >= 1) {
			memset(wm_local, 0, sizeof(wm_local));
			*(__le32 *)wm_local = cpu_to_le32(FF_WMODE_SLEEP);
			send_ion(app_id, sess, SESS_BIG, FF_TA_CFG_WORK_MODE, 4,
				 wm_local, "dev-WMODE-SLEEP-esd");
			msleep(40);
		}
		memset(wm_local, 0, sizeof(wm_local));
		*(__le32 *)wm_local = cpu_to_le32(FF_WMODE_FDT_DOWN);
		send_ion(app_id, sess, SESS_BIG, FF_TA_CFG_WORK_MODE, 4, wm_local,
			 "dev-WMODE-FDT_DOWN-esd");
		msleep(30);
		if (t >= 1) {
			memset(usr, 0, 0x30);
			*(__le32 *)usr = cpu_to_le32(1);
			send_ion(app_id, sess, SESS_BIG, FF_TA_START_SCANNING, 0x30,
				 usr, "dev-SCAN-esd");
			msleep(20);
			/* Soft FDT CAP only (flag=1) — image flags not needed for clear */
			capture_ctx_fill_flag(sess, FF_IMG_COLS, FF_IMG_ROWS, 0x24,
					      FF_IMG_BYTES, false, 1);
			send_ion(app_id, sess, SESS_BIG, FF_TA_CAPTURE_IMAGE, 0x24, usr,
				 "dev-cap-esd-soft");
			msleep(15);
		}
	}
	memset(usr, 0, 8);
	send_ion(app_id, sess, SESS_BIG, FF_TA_QUERY_EVENT, 4, usr,
		 "dev-QEV-esd-final");
	if (last_send_finger_down || (last_send_itype & 0x2))
		saw_down = true;
	pr_info("qsee_fp: esd_clear done esd=%d qes_ok=%d itype=0x%x down=%d (%s)\n",
		last_send_esd, last_send_qes_ok, last_send_itype, saw_down, why);
	return saw_down;
}

/*
 * M4ai: force image acquisition path.
 * CHECK_DEAD_PIXEL 0x1020 uses same plen 0x24 capture_ctx as CAPTURE.
 * Dual img_off (0x24 / 0x100). Returns best ADC-range pixel count; sets
 * last_send_frame_raw / sensor_raw if TA logs them.
 */
static char f32_config_text[2048]; /* M4c0: expanded keys; used by SPI mode cycle */
static void f32_config_build(void);

/*
 * M4av: ft9362_config_spi_mode fails during CAP → no frame raw / img_scan.
 * Soft recover without FREE_SPI/INIT_SPI (those SCM-kill the TA on this unit).
 */
/*
 * M4az: image SPI fails when chip itype has RST(0x200)/ESD(0x400).
 * Poll QEV until idle/finger (not rst/esd), with ESD clear between tries.
 */
static int wait_chip_ready(u32 app_id, void *sess, const char *why, int max_try)
{
	u8 *usr = (u8 *)sess + 0x10;
	int t;

	if (max_try < 1)
		max_try = 1;
	if (max_try > 8)
		max_try = 8;
	for (t = 0; t < max_try; t++) {
		memset(usr, 0, 8);
		send_ion(app_id, sess, SESS_BIG, FF_TA_QUERY_EVENT, 4, usr,
			 "dev-QEV-ready");
		/*
		 * Ready: no ESD QES; itype idle(0/1) or finger-down(0x2).
		 * Reject inv(0x10), rst(0x200), esd(0x400) — image SPI fails
		 * with host-omit-interrupt when chip is in those states (M4az).
		 */
		if (!last_send_esd &&
		    !(last_send_itype & 0x610) &&
		    (last_send_itype == 0 || last_send_itype == 1 ||
		     (last_send_itype & 0x2))) {
			pr_info("qsee_fp: chip ready itype=0x%x try=%d (%s)\n",
				last_send_itype, t, why);
			return 0;
		}
		pr_info("qsee_fp: chip not ready itype=0x%x esd=%d try=%d (%s)\n",
			last_send_itype, last_send_esd, t, why);
		if (last_send_itype & 0x610 || last_send_esd) {
			if (t >= 1)
				l2_vdd_cycle(why);
			l2_reset_ic(why);
			msleep(60);
			esd_clear_loop(app_id, sess, why, 2);
		} else {
			msleep(40);
		}
	}
	pr_info("qsee_fp: chip ready timeout itype=0x%x esd=%d (%s)\n",
		last_send_itype, last_send_esd, why);
	return -ETIMEDOUT;
}

static void spi_image_recover(u32 app_id, void *sess, const char *why)
{
	u8 *usr = (u8 *)sess + 0x10;
	u8 wm[8];
	u8 cap[0x30];
	u32 spd = spi_default_bps > 0 ? (u32)spi_default_bps : 4000000;

	if (!spi_img_recover)
		return;
	pr_info("qsee_fp: spi_image_recover (%s) prior_fail=%d itype=0x%x\n", why,
		last_send_spi_mode_fail, last_send_itype);
	/*
	 * M4ay/M4az: no CAP-time INIT_SPI (Null). Clear RST/ESD then FDT re-arm.
	 */
	(void)wait_chip_ready(app_id, sess, why, 4);
	l2_reset_ic(why);
	msleep(80);
	*(__le32 *)usr = cpu_to_le32(spd);
	send_ion(app_id, sess, SESS_BIG, FF_TA_SET_SPI_SPEED, 4, usr,
		 "dev-SPI-SPEED-recover");
	msleep(30);
	memset(wm, 0, sizeof(wm));
	*(__le32 *)wm = cpu_to_le32(FF_WMODE_FDT_DOWN);
	send_ion(app_id, sess, SESS_BIG, FF_TA_CFG_WORK_MODE, 4, wm,
		 "dev-WMODE-FDT-spi-rec");
	msleep(30);
	memset(cap, 0, sizeof(cap));
	*(__le32 *)cap = cpu_to_le32(1);
	send_ion(app_id, sess, SESS_BIG, FF_TA_START_SCANNING, 0x30, cap,
		 "dev-SCAN-spi-rec");
	msleep(30);
	send_ion(app_id, sess, SESS_BIG, 0x1003, 0, NULL, "dev-LOGCAT-spi-rec");
	pr_info("qsee_fp: spi_image_recover done fail=%d vio=%d esd=%d itype=0x%x (%s)\n",
		last_send_spi_mode_fail, last_send_vio_ok, last_send_esd,
		last_send_itype, why);
}

/*
 * M4c0: scan full ion session (req + rsp). LOGCAT/residual is only ~810–1322 B
 * and truncates past serial Null / mid-try2; Base markers may land in either half.
 */
static void scan_ion_img_markers(void *sess, const char *why)
{
	const char *scan = (const char *)sess;
	size_t si;
	unsigned hits = 0;
	const size_t scan_n = SESS_BIG;

	for (si = 0; si + 20 < scan_n; si++) {
		if (!strncmp(scan + si, "Base update success", 19)) {
			last_send_base_update = true;
			g_img_base_ok = true;
			pr_info("qsee_fp: ION-SCAN %s: Base update success @0x%zx\n",
				why, si);
			hits++;
		} else if (!strncmp(scan + si, "get image dac [success]", 23) ||
			   !strncmp(scan + si, "newDAC:", 7) ||
			   !strncmp(scan + si, "Got Img Dac ok", 14)) {
			last_send_img_dac_ok = true;
			pr_info("qsee_fp: ION-SCAN %s: img dac '%.48s' @0x%zx\n",
				why, scan + si, si);
			hits++;
		} else if (!strncmp(scan + si, "******Step", 10) ||
			   !strncmp(scan + si, "Step1:", 6) ||
			   !strncmp(scan + si, "Step2:", 6) ||
			   !strncmp(scan + si, "Step3:", 6)) {
			last_send_img_step = true;
			pr_info("qsee_fp: ION-SCAN %s: step '%.56s' @0x%zx\n",
				why, scan + si, si);
			hits++;
		} else if (!strncmp(scan + si, "fdt calibration", 15) ||
			   !strncmp(scan + si, "got fdt dac success", 19) ||
			   !strncmp(scan + si, "fdt base check result", 21) ||
			   !strncmp(scan + si, "write fdt down base", 19) ||
			   !strncmp(scan + si, "get fdt dac [", 13)) {
			pr_info("qsee_fp: ION-SCAN %s: fdt '%.48s' @0x%zx\n",
				why, scan + si, si);
			hits++;
		} else if (!strncmp(scan + si, "successfully initialized", 24) ||
			   !strncmp(scan + si, "fingerprint device", 18)) {
			pr_info("qsee_fp: ION-SCAN %s: init '%.48s' @0x%zx\n",
				why, scan + si, si);
			hits++;
		} else if (!strncmp(scan + si, "failed to malloc memory for", 27) ||
			   !strncmp(scan + si, "image buffer = NULL", 19) ||
			   !strncmp(scan + si, "skip step2 and step3", 20) ||
			   !strncmp(scan + si, "fail to get img dac", 19) ||
			   !strncmp(scan + si, "host omit", 9) ||
			   !strncmp(scan + si, "ff_calibration_data_save", 24) ||
			   !strncmp(scan + si, "calib_data_save", 15)) {
			pr_info("qsee_fp: ION-SCAN %s: FAIL '%.56s' @0x%zx\n",
				why, scan + si, si);
			hits++;
		} else if (!strncmp(scan + si, "usMiddleTemp:", 13)) {
			pr_info("qsee_fp: ION-SCAN %s: middle '%.40s' @0x%zx\n",
				why, scan + si, si);
			hits++;
		} else if (!strncmp(scan + si, "g_builtin_buffer", 16) ||
			   !strncmp(scan + si, "IMemSpace_map", 13) ||
			   !strncmp(scan + si, "The response data should be placed in shared", 44) ||
			   !strncmp(scan + si, "ff_sensor_data_array_new", 24) ||
			   !strncmp(scan + si, "trans == (ff_transfer_t *)", 26) ||
			   !strncmp(scan + si, "TA shared memory size", 21)) {
			pr_info("qsee_fp: ION-SCAN %s: BIND '%.72s' @0x%zx\n",
				why, scan + si, si);
			hits++;
		}
	}
	pr_info("qsee_fp: ION-SCAN %s hits=%u base_ok=%d dac=%d step=%d\n",
		why, hits, g_img_base_ok, last_send_img_dac_ok, last_send_img_step);
}

/* Dump residual head + tail (tail catches circular buffer / late newDAC). */
static void dump_residual_budget(void *sess, const char *tag)
{
	void *rsp = (u8 *)sess + RSP_ION_OFF;
	u32 rlen = le32_to_cpu(*(__le32 *)rsp);
	u32 n = min_t(u32, rlen, 4096);
	u32 j, lj = 0;
	char ln[128];
	u32 tail_from;

	pr_info("qsee_fp: %s residual len=%u step=%d dac=%d base=%d\n",
		tag, rlen, last_send_img_step, last_send_img_dac_ok,
		last_send_base_update);
	/* M4ca: surface Android "cidri ... f: N/D/S/U" finger status for C3 evidence */
	{
		const char *hay = (const char *)rsp + 4;
		u32 hlen = n > 4 ? n - 4 : 0;
		const char *p = NULL;
		u32 i;

		for (i = 0; i + 8 < hlen; i++) {
			if (hay[i] == 'f' && hay[i + 1] == ':' &&
			    hay[i + 2] == ' ' && hay[i + 3] >= '0' &&
			    hay[i + 3] <= '3') {
				p = hay + i;
				break;
			}
		}
		if (p) {
			char st = p[3];
			const char *name = (st == '0') ? "U" :
					   (st == '1') ? "S" :
					   (st == '2') ? "D" :
					   (st == '3') ? "N" : "?";

			pr_info("qsee_fp: %s residual finger f:%c (%s) — 0=U 1=S 2=D 3=N\n",
				tag, st, name);
		}
	}
	for (j = 4; j < n; j++) {
		char c = ((char *)rsp)[j];

		if (c >= 0x20 && c < 0x7f && lj < 120)
			ln[lj++] = c;
		else if (lj > 5) {
			ln[lj] = 0;
			pr_info("qsee_fp: %s-RES: %s\n", tag, ln);
			lj = 0;
		} else {
			lj = 0;
		}
	}
	if (lj > 5) {
		ln[lj] = 0;
		pr_info("qsee_fp: %s-RES: %s\n", tag, ln);
	}
	/* Tail 280 B — if buffer is circular, newDAC/Base live here */
	if (n > 300) {
		tail_from = n - 280;
		lj = 0;
		pr_info("qsee_fp: %s residual TAIL from off=%u\n", tag, tail_from);
		for (j = tail_from; j < n; j++) {
			char c = ((char *)rsp)[j];

			if (c >= 0x20 && c < 0x7f && lj < 120)
				ln[lj++] = c;
			else if (lj > 5) {
				ln[lj] = 0;
				pr_info("qsee_fp: %s-TAIL: %s\n", tag, ln);
				lj = 0;
			} else {
				lj = 0;
			}
		}
		if (lj > 5) {
			ln[lj] = 0;
			pr_info("qsee_fp: %s-TAIL: %s\n", tag, ln);
		}
	}
	scan_ion_img_markers(sess, tag);
}

/*
 * M4bj: Android INIT = FDT calib then img Step1-3 Base update (allocates
 * nav base/raw). serial_sync Null (qsee_sfs hard path) is non-fatal but
 * LOGCAT truncates. After INIT: full ion scan; then FDT-then-img force:
 *   1) FDT CAP until got fdt dac (establishes FDT base)
 *   2) WMODE-5 + image CAP @8M for Step1-3 / Base update
 */
static bool try_force_img_base_calib(u32 app_id, void *sess)
{
	u8 *usr = (u8 *)sess + 0x10;
	u8 wm[8];
	u8 cap[0x30];
	unsigned int t;
	u32 spd = 8000000;
	bool ok = false;
	bool fdt_ready = false;

	pr_info("qsee_fp: force img base calib begin (FDT-then-Step1-3)\n");
	scan_ion_img_markers(sess, "pre-calib");
	if (g_img_base_ok) {
		pr_info("qsee_fp: Base update already in ion — skip force\n");
		return true;
	}

	/* M4c1: proactive SPI warm before FDT (cold boot often fails mode) */
	*(__le32 *)usr = cpu_to_le32(4000000);
	if (send_ion(app_id, sess, SESS_BIG, FF_TA_SET_SPI_SPEED, 4, usr,
		     "dev-SPI-SPEED-warm") == -22)
		return false;
	spi_image_recover(app_id, sess, "force-begin");
	msleep(50);

	*(__le32 *)usr = cpu_to_le32(spd);
	if (send_ion(app_id, sess, SESS_BIG, FF_TA_SET_SPI_SPEED, 4, usr,
		     "dev-SPI-SPEED-calib") == -22)
		return false;

	/*
	 * Phase A: full FDT base (Android: got fdt dac + fdt base check success
	 * + write fdt down base). Soft CAP may only print got fdt dac — repeat
	 * SCAN+CAP+LOGCAT until base check or budget.
	 */
	for (t = 0; t < 6 && !fdt_ready; t++) {
		memset(wm, 0, sizeof(wm));
		*(__le32 *)wm = cpu_to_le32(FF_WMODE_FDT_DOWN);
		send_ion(app_id, sess, SESS_BIG, FF_TA_CFG_WORK_MODE, 4, wm,
			 "dev-WMODE-FDT-calib");
		msleep(20);
		memset(cap, 0, sizeof(cap));
		*(__le32 *)cap = cpu_to_le32(1);
		send_ion(app_id, sess, SESS_BIG, FF_TA_START_SCANNING, 0x30, cap,
			 "dev-SCAN-FDT-calib");
		msleep(40);
		capture_ctx_fill_flag(sess, FF_IMG_COLS, FF_IMG_ROWS, 0x24,
				      FF_IMG_BYTES, false, 1);
		send_ion(app_id, sess, SESS_BIG, FF_TA_CAPTURE_IMAGE, 0x24, usr,
			 "dev-cap-FDT-calib");
		msleep(30);
		send_ion(app_id, sess, SESS_BIG, 0x1003, 0, NULL, "dev-LOGCAT-FDT-calib");
		/* Extra SCAN to push fdt base write */
		send_ion(app_id, sess, SESS_BIG, FF_TA_START_SCANNING, 0x30, cap,
			 "dev-SCAN-FDT-base");
		msleep(40);
		send_ion(app_id, sess, SESS_BIG, 0x1003, 0, NULL, "dev-LOGCAT-FDT-base");
		scan_ion_img_markers(sess, "fdt-phase");
		if (last_send_fdt_ok || last_send_vio_ok)
			fdt_ready = true;
		pr_info("qsee_fp: FDT-calib phase try=%u fdt_ready=%d fdt_ok=%d spi_fail=%d vio=%d\n",
			t, fdt_ready, last_send_fdt_ok, last_send_spi_mode_fail,
			last_send_vio_ok);
		if (last_send_spi_mode_fail && spi_img_recover)
			spi_image_recover(app_id, sess, "fdt-calib");
		msleep(30);
	}

	/*
	 * M4c1 residual budget:
	 * Residual hard-cap ~1322B truncates mid try2 (pre-newDAC).
	 * fw=0 skips Step1 entirely (measured). Keep fw=3 + residual head/tail.
	 * SPI warm above; mode-5 recovery is still the only force Step1 path.
	 */
	{
		static const u32 speeds[] = { 8000000, 4000000, 8000000 };
		unsigned int st;
		const u32 recovery_mode = 5;
		unsigned int phase;

		pr_info("qsee_fp: M4c1 Step1 mode-%u fw=3 residual head+tail (cap=%u)\n",
			recovery_mode, 1322);

		/* Phase B0: warm re-INIT after FDT (efc may be in chip) */
		for (t = 0; t < 2 && !ok; t++) {
			*(__le32 *)usr = cpu_to_le32(speeds[t % 3]);
			send_ion(app_id, sess, SESS_BIG, FF_TA_SET_SPI_SPEED, 4, usr,
				 "dev-SPI-SPEED-reinit");
			memset(cap, 0, sizeof(cap));
			*(__le32 *)cap = cpu_to_le32(1);
			send_ion(app_id, sess, SESS_BIG, FF_TA_START_SCANNING, 0x30, cap,
				 "dev-SCAN-reinit");
			msleep(40);
			send_ion(app_id, sess, SESS_BIG, FF_TA_INIT_DEVICE, 0, NULL,
				 "dev-INIT_DEV-warm");
			msleep(500);
			send_ion(app_id, sess, SESS_BIG, 0x1003, 0, NULL,
				 "dev-LOGCAT-warm-init");
			dump_residual_budget(sess, "WARM-INIT");
			if (last_send_base_update || last_send_img_dac_ok) {
				ok = true;
				g_img_base_ok = true;
				pr_info("qsee_fp: *** BASE via warm INIT t=%u ***\n", t);
			}
		}

		for (phase = 0; phase < 2 && !ok; phase++) {
			/*
			 * M4c0 measured: fw=0 skips Step1 entirely (residual ~298
			 * entry-only). fw=3 runs FDT+Step1 but truncates @1322 mid
			 * try2. Use fw=3 both phases; phase0 drains LOGCAT first.
			 */
			diag_fw_log_level = 3;
			diag_framework_log_level = phase ? 3 : 2;
			f32_config_build();
			{
				size_t clen = strlen(f32_config_text) + 1;

				send_ion(app_id, sess, SESS_BIG, FF_TA_SYNC_CFG,
					 (u32)clen, f32_config_text,
					 phase ? "dev-SYNC-verbose" : "dev-SYNC-silent");
			}
			/* Drain residual before Step1 */
			send_ion(app_id, sess, SESS_BIG, 0x1003, 0, NULL,
				 "dev-LOGCAT-pre-step1");

			pr_info("qsee_fp: recovery phase=%u fw=%d fr=%d\n",
				phase, diag_fw_log_level, diag_framework_log_level);

			for (t = 0; t < 2 && !ok; t++) {
				u32 sp = speeds[t % 3];

				*(__le32 *)usr = cpu_to_le32(sp);
				send_ion(app_id, sess, SESS_BIG, FF_TA_SET_SPI_SPEED,
					 4, usr, "dev-SPI-SPEED-imgc");
				memset(cap, 0, sizeof(cap));
				*(__le32 *)cap = cpu_to_le32(1);
				send_ion(app_id, sess, SESS_BIG, FF_TA_START_SCANNING,
					 0x30, cap, "dev-SCAN-pre-step1");
				msleep(50);
				/* Mode 5 → unknown → recovery FDT+Step1 AutoSDac */
				memset(wm, 0, sizeof(wm));
				*(__le32 *)wm = cpu_to_le32(recovery_mode);
				send_ion(app_id, sess, SESS_BIG, FF_TA_CFG_WORK_MODE,
					 4, wm, "dev-WMODE-recovery-step1");
				dump_residual_budget(sess,
						     phase ? "REC-V" : "REC-S");

				/* Poll LOGCAT — silent phase may still set sticky via CAP */
				for (st = 0; st < 10 && !ok; st++) {
					msleep(250);
					send_ion(app_id, sess, SESS_BIG, 0x1003, 0,
						 NULL, "dev-LOGCAT-dac-poll");
					if (last_send_base_update ||
					    last_send_img_dac_ok) {
						ok = true;
						g_img_base_ok = true;
						pr_info("qsee_fp: *** DAC/BASE poll st=%u ph=%u ***\n",
							st, phase);
						break;
					}
				}
				/* Step3-like CAP — primary success detector for silent */
				*(__le32 *)usr = cpu_to_le32(8000000);
				send_ion(app_id, sess, SESS_BIG, FF_TA_SET_SPI_SPEED,
					 4, usr, "dev-SPI-SPEED-step3");
				{
					unsigned int c;

					for (c = 0; c < 4 && !ok; c++) {
						memset(cap, 0, sizeof(cap));
						*(__le32 *)cap = cpu_to_le32(1);
						send_ion(app_id, sess, SESS_BIG,
							 FF_TA_START_SCANNING, 0x30,
							 cap, "dev-SCAN-step3");
						msleep(50);
						capture_ctx_fill_flag(sess,
								      FF_IMG_COLS,
								      FF_IMG_ROWS,
								      0x24,
								      FF_IMG_BYTES,
								      false, 0x240);
						send_ion(app_id, sess, SESS_BIG,
							 FF_TA_CAPTURE_IMAGE, 0x24,
							 usr, "dev-cap-step3");
						msleep(80);
						send_ion(app_id, sess, SESS_BIG,
							 0x1003, 0, NULL,
							 "dev-LOGCAT-step3");
						scan_ion_img_markers(sess, "step3");
						if (last_send_base_update ||
						    last_send_img_dac_ok ||
						    last_send_frame_raw ||
						    last_send_image_ready ||
						    last_send_sensor_raw) {
							ok = true;
							g_img_base_ok = true;
							pr_info("qsee_fp: *** BASE/DAC/RAW step3 c=%u ph=%u ***\n",
								c, phase);
							break;
						}
						/* Non-null CAP err? */
						if (!last_send_spi_mode_fail &&
						    last_send_vio_ok) {
							pr_info("qsee_fp: step3 vio_ok c=%u (possible Base)\n",
								c);
						}
					}
				}
				/* Warm INIT again after recovery (efc 144 sticky) */
				if (!ok) {
					send_ion(app_id, sess, SESS_BIG,
						 FF_TA_INIT_DEVICE, 0, NULL,
						 "dev-INIT_DEV-post-rec");
					msleep(400);
					dump_residual_budget(sess, "POST-REC-INIT");
					if (last_send_base_update ||
					    last_send_img_dac_ok) {
						ok = true;
						g_img_base_ok = true;
						pr_info("qsee_fp: *** BASE post-rec INIT ***\n");
					}
				}
				pr_info("qsee_fp: recovery ph=%u try=%u step=%d dac=%d base=%d frame=%d\n",
					phase, t, last_send_img_step,
					last_send_img_dac_ok, last_send_base_update,
					last_send_frame_raw);
			}
		}
	}

	send_ion(app_id, sess, SESS_BIG, FF_TA_SYNC_CALIB, 0, NULL, "dev-SYNC_CALIB-post");
	scan_ion_img_markers(sess, "post-sync");

	memset(wm, 0, sizeof(wm));
	*(__le32 *)wm = cpu_to_le32(FF_WMODE_FDT_DOWN);
	send_ion(app_id, sess, SESS_BIG, FF_TA_CFG_WORK_MODE, 4, wm,
		 "dev-WMODE-FDT-post-calib");
	msleep(20);
	memset(cap, 0, sizeof(cap));
	*(__le32 *)cap = cpu_to_le32(1);
	send_ion(app_id, sess, SESS_BIG, FF_TA_START_SCANNING, 0x30, cap,
		 "dev-SCAN-post-calib");
	pr_info("qsee_fp: force img base calib done ok=%d g_img_base_ok=%d step=%d dac=%d\n",
		ok, g_img_base_ok, last_send_img_step, last_send_img_dac_ok);
	return ok || g_img_base_ok;
}

/*
 * M4dk: best soft CAP form from matrix (used by AFR under finger).
 * Declared here so try_android_frame_raw can see them; matrix fills later.
 */
static int m4dk_soft_layout = -1;
static u32 m4dk_soft_flag;
static bool m4dk_have_soft;

/*
 * M4c4 / Android enroll transmit_image (~37ms) under finger:
 *   QES [finger touch] + itype 0x2 → CAPTURE×3 flag 0x240 → frame raw
 *
 * M4ct — sticky QES + CAP on latched REAL DOWN (post-M4cs):
 *  precap QUERY cleared touch; residual often MARK finger touch then
 *  QES_LAST unknow (last-line rule). Fix parse sticky; if sample already
 *  REAL DOWN, skip settle/precap and CAP immediately (no extra QUERY).
 *
 * M4cu — TA heap: fingerpr unloaded; under LATCH CAP with PA@+0x14 so
 *  image plane is builtin ion (Android g_builtin_buffer.usr bind) instead
 *  of pure array_new when TZ malloc is tight.
 *
 * M4cw — M4cv soft ESD left sticky esd=1 so LATCH (!esd && DOWN) never
 *  armed; settle lost touch. LATCH on REAL DOWN even with sticky ESD;
 *  one soft ESD_HANDLE (no QUERY) then CAP — do not drop latched DOWN.
 */
static void afr_settle_clean_touch(u32 app_id, void *sess)
{
	u8 pay1[4] = { 0 };
	u8 esd_pay[4] = { 0 };
	unsigned int i;

	send_ion(app_id, sess, SESS_BIG, FF_TA_QUERY_EVENT, 4, pay1,
		 "dev-QEV-afr-settle0");
	pr_info("qsee_fp: afr settle start itype=0x%x touch=%d esd=%d qes=%d\n",
		last_send_itype, last_send_finger_down, last_send_esd,
		last_send_qes_ok);

	for (i = 0; i < 6; i++) {
		if (!last_send_esd &&
		    (last_send_finger_down || (last_send_itype & 0x2))) {
			pr_info("qsee_fp: afr settle CLEAN i=%u itype=0x%x touch=%d\n",
				i, last_send_itype, last_send_finger_down);
			return;
		}
		if (last_send_esd) {
			send_ion(app_id, sess, SESS_BIG, FF_TA_ESD_HANDLE, 4,
				 esd_pay, "dev-ESD-afr-settle");
		}
		msleep(12);
		send_ion(app_id, sess, SESS_BIG, FF_TA_QUERY_EVENT, 4, pay1,
			 "dev-QEV-afr-settle");
	}
	pr_info("qsee_fp: afr settle END itype=0x%x touch=%d esd=%d (may still be dirty)\n",
		last_send_itype, last_send_finger_down, last_send_esd);
}

static u32 try_android_frame_raw(u32 app_id, void *sess, const char *why)
{
	u8 *usr = (u8 *)sess + 0x10;
	u8 pay1[4] = { 0 };
	u32 best = 0, nz = 0;
	u32 flag = 0x240;
	int er, saved_layout = cap_ctx_layout;
	const u32 hold_itype = last_send_itype;
	const bool hold_touch = last_send_finger_down;
	const bool hold_esd = last_send_esd;
	unsigned long t0, t1, t_burst0;
	u32 cap_ms;
	bool clean;
	bool latched;

	pr_info("qsee_fp: android-frame-raw M4dr (%s) itype=0x%x esd=%d touch=%d base=%d defer=%d\n",
		why, hold_itype, hold_esd, hold_touch, g_img_base_ok, defer_enroll);

	t_burst0 = jiffies;
	/* M4dr: no PA@+0x14 — HAL g_builtin usr is the ctx, TA must not array_new. */
	cap_ctx_suppress_pa = true;

	/*
	 * M4cw: REAL DOWN latches even with sticky ESD. Soft ESD_HANDLE only.
	 */
	latched = hold_touch || (hold_itype & 0x2);
	if (latched) {
		clean = true;
		if (hold_esd) {
			u8 esd_pay[4] = { 0 };

			send_ion(app_id, sess, SESS_BIG, FF_TA_ESD_HANDLE, 4,
				 esd_pay, "dev-ESD-afr-latch");
			msleep(15);
			last_send_finger_down = true;
			if (!(last_send_itype & 0x2))
				last_send_itype |= 0x2;
			last_send_esd = false;
			pr_info("qsee_fp: afr LATCH+softESD (REAL DOWN itype=0x%x had_esd=1)\n",
				hold_itype);
		} else {
			pr_info("qsee_fp: afr LATCH skip settle (sample REAL DOWN itype=0x%x)\n",
				hold_itype);
		}
	} else {
		afr_settle_clean_touch(app_id, sess);
		clean = !last_send_esd &&
			(last_send_finger_down || (last_send_itype & 0x2));
		if (!clean && (last_send_finger_down || (last_send_itype & 0x2))) {
			clean = true;
			last_send_esd = false;
			pr_info("qsee_fp: afr LATCH after settle despite residual esd\n");
		}
	}

	if (!clean) {
		pr_info("qsee_fp: afr skip image — not clean (esd=%d touch=%d itype=0x%x)\n",
			last_send_esd, last_send_finger_down, last_send_itype);
		goto afr_done;
	}

	/*
	 * M4dr: 16×16 L1 is exhausted (soft FDT, f:N, ~11s). One native
	 * 112×88 CAPTURE, plen 0x24, nbytes=0x4d00, no PA@+0x14.
	 * Pass: tens of ms + frame raw. Fail-fast: OOM ms~4 or FDT-slow.
	 */
	{
		const u32 cols = FF_IMG_COLS, rows = FF_IMG_ROWS;
		const u32 nb = FF_IMG_BYTES;

		cap_ctx_layout = 1;
		cap_ctx_suppress_pa = true;
		flag = 0x240;
		pr_info("qsee_fp: M4dr one CAP 112x88 L1 nopa nbytes=0x%x plen=0x24\n",
			nb);

		capture_ctx_fill_flag(sess, cols, rows, 0x24, nb, false, flag);
		t0 = jiffies;
		er = send_ion(app_id, sess, SESS_BIG, FF_TA_CAPTURE_IMAGE, 0x24,
			      usr, "dev-cap-afr-M4dr");
		t1 = jiffies;
		cap_ms = jiffies_to_msecs(t1 - t0);
		pr_info("qsee_fp: afr M4dr CAP flag=0x%x er=%d ms=%u app_err=0x%x frame=%d rows=%u vio=%d\n",
			flag, er, cap_ms, last_send_app_err, last_send_frame_raw,
			le32_to_cpu(*(__le32 *)(usr + 8)), last_send_vio_ok);

		if (er == -22) {
			cap_ctx_suppress_pa = false;
			cap_ctx_layout = saved_layout;
			return best;
		}
		if (last_send_frame_raw || last_send_sensor_raw ||
		    last_send_enrolling) {
			nz = capture_scan_all(sess, 0x24, nb, "afr-M4dr", er);
			send_ion(app_id, sess, SESS_BIG, 0x1003, 0, NULL,
				 "dev-LOGCAT-afr-ok");
			dump_residual_budget(sess, "AFR-OK");
			pr_info("qsee_fp: *** ANDROID FRAME RAW M4dr 112x88 (%s) nz=%u ms=%u ***\n",
				why, nz, cap_ms);
			best = max_t(u32, nz, 1);
		} else if (last_send_app_err == 0xffffff37u || er) {
			send_ion(app_id, sess, SESS_BIG, 0x1003, 0, NULL,
				 "dev-LOGCAT-afr-oom");
			dump_residual_budget(sess, "AFR-OOM");
			send_ion(app_id, sess, SESS_BIG, FF_TA_CHECK_HEALTH, 4,
				 pay1, "dev-HEALTH-afr-oom");
			pr_info("qsee_fp: M4dr OOM/err ms=%u app_err=0x%x — stop\n",
				cap_ms, last_send_app_err);
		} else if (cap_ms > 800) {
			send_ion(app_id, sess, SESS_BIG, 0x1003, 0, NULL,
				 "dev-LOGCAT-afr-fdtslow");
			dump_residual_budget(sess, "AFR-FDTSLOW");
			pr_info("qsee_fp: M4dr FDT-slow ms=%u — stop (native still not img_scan)\n",
				cap_ms);
		} else {
			nz = capture_scan_all(sess, 0x24, nb, "afr-M4dr", er);
			dump_residual_budget(sess, "AFR-FAST-EMPTY");
			pr_info("qsee_fp: M4dr fast empty nz=%u ms=%u frame=%d\n",
				nz, cap_ms, last_send_frame_raw);
			best = nz;
		}
	}

afr_done:
	/* M4c3: do not REPORT 0x2 after a failed AFR — that fakes DOWN. */
	cap_ctx_layout = saved_layout;
	cap_ctx_suppress_pa = false;
	pr_info("qsee_fp: android-frame-raw done wall_ms=%u best_nz=%u frame=%d clean=%d (%s)\n",
		jiffies_to_msecs(jiffies - t_burst0), best, last_send_frame_raw,
		clean, why);
	return best;
}

/*
 * M4ck idle smoke: layout=1 CAP after soft FDT.
 * Full 112x88 layout1 triggers ff_sensor_data_array_new OOM on this TA heap.
 * Strategy:
 *  1) layout1 soft with full dims — if OOM/Null, HEALTH recover
 *  2) layout1 soft with tiny dims (16x16) — prove layout1 field order works
 *     without TA-heap OOM (keeps rows)
 *  3) HEALTH + QUERY must not sticky-Null
 */
static void try_idle_layout1_cap(u32 app_id, void *sess)
{
	u8 *usr = (u8 *)sess + 0x10;
	u8 pay1[4] = { 0 };
	int saved = cap_ctx_layout;
	int er;
	u32 err_tiny, err_post;
	u32 rows_tiny;

	cap_ctx_layout = 1;
	pr_info("qsee_fp: M4ck idle layout1 CAP begin\n");

	/*
	 * Layout1 field form (nbytes@+0xc, img_off@+0x10) with dims that fit TA
	 * heap. 112x88 triggers ff_sensor_data_array_new OOM (see M4ck notes);
	 * 16x16 proves layout1 CAP without OOM/Null + HEALTH not sticky.
	 */
	capture_ctx_fill_flag(sess, 16, 16, 0x24, 16 * 16 * 2, false, 1);
	pr_info("qsee_fp: M4ck tiny ctx flag=0x1 cols=16 rows=16 w0c=0x%x w10=0x%x\n",
		le32_to_cpu(*(__le32 *)(usr + 0xc)),
		le32_to_cpu(*(__le32 *)(usr + 0x10)));
	er = send_ion(app_id, sess, SESS_BIG, FF_TA_CAPTURE_IMAGE, 0x24, usr,
		      "dev-cap-m4ck-tiny");
	err_tiny = last_send_app_err;
	rows_tiny = le32_to_cpu(*(__le32 *)(usr + 8));
	pr_info("qsee_fp: M4ck tiny CAP er=%d app_err=0x%x rows=%u (layout1 form)\n",
		er, err_tiny, rows_tiny);
	send_ion(app_id, sess, SESS_BIG, 0x1003, 0, NULL, "dev-LOGCAT-m4ck-tiny");
	dump_residual_budget(sess, "M4CK-TINY");

	send_ion(app_id, sess, SESS_BIG, FF_TA_CHECK_HEALTH, 4, pay1,
		 "dev-HEALTH-m4ck");
	err_post = last_send_app_err;
	pr_info("qsee_fp: M4ck HEALTH app_err=0x%x\n", err_post);
	send_ion(app_id, sess, SESS_BIG, FF_TA_QUERY_EVENT, 4, pay1,
		 "dev-QEV-m4ck");
	pr_info("qsee_fp: M4ck QUERY app_err=0x%x itype=0x%x\n",
		last_send_app_err, last_send_itype);

	if (err_tiny == 0 && last_send_app_err != 0xffffff37u && err_post != 0xffffff37u)
		pr_info("qsee_fp: *** M4ck idle layout1 CAP OK tiny_err=0 sticky=0 ***\n");
	else
		pr_info("qsee_fp: M4ck tiny_err=0x%x post_err=0x%x\n",
			err_tiny, last_send_app_err);
	cap_ctx_layout = saved;
}

/* M4dv: one native 112×88 CAPTURE, no finger. Log builtin-bind vs array_new. */
static bool try_idle_native_cap(u32 app_id, void *sess, u32 burst,
				bool keep_image)
{
	u8 *usr = (u8 *)sess + 0x10;
	u8 pay1[4] = { 0 };
	int saved = cap_ctx_layout;
	int er = 0;
	unsigned long t0, t1;
	u32 cap_ms;
	bool saved_pa = cap_ctx_suppress_pa;
	bool raw_ok = false;

	if (!burst)
		burst = 1;

	cap_ctx_layout = 1;
	cap_ctx_suppress_pa = true;
	/*
	 * M4dy: Android 0x24 capture_ctx is NOT cols/rows/nbytes (that made
	 * TA array_new 112×88). HAL writes burst/flag in usr; image header
	 * returns at usr+0x2c. send_cmd is still 0x78000/0x8040.
	 * Factory flag 0xE0000240 (bit6 skip wait-for-finger).
	 */
	{
		u32 nz = 0, cols, rows, avg = 0, mn = 0, mx = 0, fi;

		pr_info("qsee_fp: M4dy HAL capture_ctx burst=%u flag=0xE0000240 (no cols/rows) oneshot=%d\n",
			burst, keep_image);
		for (fi = 0; fi < (keep_image ? 1u : burst); fi++) {
			u32 plus10 = 1;

			if (skip_enroll && cap_hal_overlay && keep_image &&
			    overlay_idle_plus10)
				plus10 = overlay_idle_plus10;
			if (skip_enroll && cap_hal_overlay && keep_image)
				pr_info("qsee_fp: overlay idle CAP +0x10=%u burst=%u (HAL 1; this extra 3)\n",
					plus10, burst);
			if (overlay_idle_writereg && skip_enroll && cap_hal_overlay &&
			    keep_image) {
				u8 wr[8];

				memset(wr, 0, sizeof(wr));
				*(__le32 *)wr = cpu_to_le32(FF_HAL_WRREG_ADDR);
				*(__le32 *)(wr + 4) = cpu_to_le32(FF_HAL_WRREG_VAL);
				pr_info("qsee_fp: overlay idle WRITE_REG opcode=0x%x plen=0x8 addr=0x%x val=0x%x\n",
					FF_TA_WRITE_REG, FF_HAL_WRREG_ADDR,
					FF_HAL_WRREG_VAL);
				er = send_ion(app_id, sess, SESS_BIG,
					      FF_TA_WRITE_REG, 0x8, wr,
					      "dev-writereg-overlay-idle");
				pr_info("qsee_fp: overlay idle WRITE_REG er=%d app_err=0x%x\n",
					er, last_send_app_err);
				if (er == -22 || last_send_app_err == 0xffffff37u) {
					pr_info("qsee_fp: overlay idle WRITE_REG abort (no SCAN/CAP, no other addrs)\n");
					break;
				}
			}
			capture_ctx_fill_hal(sess, burst, keep_image ? 0 : fi,
					     FF_HAL_FLAG_FACTORY, 1, plus10);
			if (overlay_idle_scan && skip_enroll && cap_hal_overlay &&
			    keep_image) {
				pr_info("qsee_fp: overlay idle SCAN opcode=0x%x plen=0x24 (HAL capture_ctx before CAP)\n",
					FF_TA_START_SCANNING);
				er = send_ion(app_id, sess, SESS_BIG,
					      FF_TA_START_SCANNING, 0x24, usr,
					      "dev-scan-overlay-idle");
				pr_info("qsee_fp: overlay idle SCAN er=%d app_err=0x%x\n",
					er, last_send_app_err);
				if (er == -22 || last_send_app_err == 0xffffff37u) {
					pr_info("qsee_fp: overlay idle SCAN abort (no CAP, no plen 0x30)\n");
					break;
				}
				capture_ctx_fill_hal(sess, burst,
						     keep_image ? 0 : fi,
						     FF_HAL_FLAG_FACTORY, 1,
						     plus10);
			}
			t0 = jiffies;
			er = send_ion(app_id, sess, SESS_BIG, FF_TA_CAPTURE_IMAGE, 0x24,
				      usr, "dev-cap-m4dy-halctx");
			t1 = jiffies;
			cap_ms = jiffies_to_msecs(t1 - t0);
			cols = le32_to_cpu(*(__le32 *)(usr + 0x30));
			rows = le32_to_cpu(*(__le32 *)(usr + 0x34));
			nz = capture_scan_pixels(usr + 0x2c, FF_IMG_BYTES, NULL, &avg,
						 &mn, &mx);
			if (last_send_frame_raw)
				raw_ok = true;
			pr_info("qsee_fp: M4dy CAP[%u] er=%d ms=%u app_err=0x%x nz=%u avg=%u min=%u max=%u hdr_cols=%u hdr_rows=%u raw=%d\n",
				fi, er, cap_ms, last_send_app_err, nz, avg, mn, mx, cols,
				rows, last_send_frame_raw);
			if (er == -22)
				break;
			if (overlay_idle_scan && skip_enroll && cap_hal_overlay &&
			    keep_image && last_send_app_err == 0xffffff37u) {
				pr_info("qsee_fp: overlay idle CAP abort (no plen 0x30)\n");
				break;
			}
			if (fi + 1 < burst)
				msleep(12);
		}
		if (raw_ok)
			pr_info("qsee_fp: *** M4dy TA frame raw (HLOS usr+0x2c avg=%u min=%u max=%u) ***\n",
				avg, mn, mx);
		else if (nz)
			pr_info("qsee_fp: *** M4dy FRAME usr+0x2c nz=%u ***\n", nz);
		capture_dump_hal_image(sess, "M4DY-HDR");
		if (!keep_image && burst <= 1) {
			scan_ion_adc_plane(sess, "M4DY-ION");
			send_ion(app_id, sess, SESS_BIG, 0x1003, 0, NULL,
				 "dev-LOGCAT-m4dy");
			dump_residual_budget(sess, "M4DY-BIND");
			scan_ion_img_markers(sess, "M4DY-BIND");
		}
	}
	if (keep_image && !skip_enroll) {
		pr_info("qsee_fp: M4ee skip HEALTH/ION after overlay CAP (REPORT next)\n");
	} else {
		send_ion(app_id, sess, SESS_BIG, FF_TA_CHECK_HEALTH, 4, pay1,
			 "dev-HEALTH-m4dv");
		pr_info("qsee_fp: M4dv HEALTH er=%d app_err=0x%x burst=%u\n", er,
			last_send_app_err, burst);
	}
	cap_ctx_layout = saved;
	cap_ctx_suppress_pa = saved_pa;
	return raw_ok;
}

/*
 * Event-thread enroll: CAP flag 0xC0040002, +0x20=0, frames 0..2.
 * Returns 0 ok, 1 Null (skip REPORT), -22 TA dead.
 */
static int send_ion_seg(u32 app_id, void *sess, size_t sess_sz,
			u32 cmd, u32 plen, const void *pay, const char *tag);

/*
 * After enroll CAP the finger is down. Android switches FDT_UP_DETECT and
 * waits itype 0x4, then FDT_DOWN_DETECT — press/lift at user speed.
 * QEV-lift after enroll-flag CAP SCM -22 killed the TA, so wait HLOS
 * irq_count only. irq_down is irq_count at REAL DOWN: lift during CAP
 * already increments it — return immediately (last Register: 1s timeout
 * every press because wait started after that increment). Else poll
 * timeout_ms then proceed.
 */
static bool overlay_wait_lift_irq(unsigned int irq_down, unsigned int timeout_ms)
{
	struct file *f;
	char buf[32];
	unsigned int irq0 = 0, irq1 = 0;
	unsigned int tw, tw_max;
	loff_t pos;
	ssize_t n;

	f = filp_open("/sys/class/focaltech_fp/focaltech_fp/irq_count",
		      O_RDONLY, 0);
	if (IS_ERR_OR_NULL(f)) {
		msleep(80);
		return false;
	}
	pos = 0;
	n = kernel_read(f, buf, sizeof(buf) - 1, &pos);
	if (n > 0) {
		buf[n] = '\0';
		if (kstrtouint(buf, 10, &irq1) != 0)
			irq1 = irq_down;
	}
	if (irq1 > irq_down) {
		pr_info("qsee_fp: overlay lift already irq=%u > down=%u\n",
			irq1, irq_down);
		fput(f);
		return true;
	}
	irq0 = irq1;
	tw_max = timeout_ms / 20u;
	if (tw_max < 1u)
		tw_max = 1u;
	for (tw = 0; tw < tw_max; tw++) {
		msleep(20);
		pos = 0;
		n = kernel_read(f, buf, sizeof(buf) - 1, &pos);
		if (n > 0) {
			buf[n] = '\0';
			if (kstrtouint(buf, 10, &irq1) != 0)
				irq1 = irq0;
		}
		if (irq1 > irq0) {
			pr_info("qsee_fp: overlay lift IRQ %u->%u tw=%u\n",
				irq0, irq1, tw);
			fput(f);
			return true;
		}
	}
	pr_info("qsee_fp: overlay lift wait timeout irq=%u down=%u (proceed)\n",
		irq1, irq_down);
	fput(f);
	return false;
}

/*
 * Android: ~191 ms SFS write is inside last enroll. SET_GROUP then loads.
 * 0x1015 bit30 was ~5 ms quality, no RPMB WRITE. Load after that wait.
 */
static void overlay_persist_load(u32 app_id, void *sess)
{
	u8 gpay[128];
	const char *gpath = "/data/vendor_de/0/fpdata";

	memset(gpay, 0, sizeof(gpay));
	*(__le32 *)gpay = cpu_to_le32(0);
	memcpy(gpay + 4, gpath, strlen(gpath) + 1);
	send_ion(app_id, sess, SESS_BIG, FF_TA_SET_ACTIVE_GROUP,
		 4 + (u32)strlen(gpath) + 1, gpay, "dev-SET_GROUP-persist");
	pr_info("qsee_fp: overlay SET_GROUP persist load after rem=0 (gid=0)\n");
	send_ion(app_id, sess, SESS_BIG, FF_TA_ENUMERATE, 0, NULL,
		 "dev-ENUM-persist");
	pr_info("qsee_fp: overlay ENUM persist app_err=0x%x write=%d\n",
		last_send_app_err, last_persist_write);
	if (last_persist_write)
		pr_info("qsee_fp: overlay persist WRITE seen\n");
	else
		pr_info("qsee_fp: overlay persist no WRITE (SET_GROUP is load-only)\n");
}

static int overlay_enroll_flag_caps(u32 app_id, void *sess)
{
	u8 *eusr = (u8 *)sess + 0x10;
	int er;
	unsigned int fi, n_ok = 0;
	/*
	 * One burst (frame=0 only) returned slots=0x1 and was scored WEAK,
	 * so three presses stopped the app. HAL still wants frames 0..2.
	 * inte stays 0 on this path; do not treat one slot as three frames.
	 */
	last_send_frame_slots = 0;
	last_send_inte = 0;
	for (fi = 0; fi < 3; fi++) {
		capture_ctx_fill_hal(sess, 3, fi, FF_HAL_FLAG_ENROLL, 0, 1);
		er = send_ion(app_id, sess, SESS_BIG, FF_TA_CAPTURE_IMAGE, 0x24,
			      eusr, "dev-cap-enroll-flag");
		pr_info("qsee_fp: overlay enroll-flag CAP frame=%u er=%d app_err=0x%x raw=%d avgv=%u slots=0x%x inte=%u\n",
			fi, er, last_send_app_err, last_send_frame_raw,
			last_send_frame_avgv, last_send_frame_slots,
			last_send_inte);
		if (er == -22)
			return -22;
		if (last_send_app_err == 0xffffff37u) {
			pr_info("qsee_fp: enroll-flag CAP Null — skip REPORT\n");
			return 1;
		}
		if (last_send_frame_avgv < 600u && last_send_frame_avgv > 0)
			n_ok++;
		msleep(8);
	}
	pr_info("qsee_fp: overlay enroll-flag 3-CAP n_ok=%u slots=0x%x inte=%u\n",
		n_ok, last_send_frame_slots, last_send_inte);
	pr_info("qsee_fp: enroll-flag ctx %*ph\n", 0x24, eusr);
	capture_dump_hal_image(sess, "enroll-flag");
	scan_ion_adc_plane(sess, "enroll-flag");
	if (n_ok < 3) {
		pr_info("qsee_fp: overlay PRESS WEAK 3-CAP n_ok=%u\n", n_ok);
		return 1;
	}
	return 0;
}

static void overlay_auth_once(void)
{
	void *sess = g_keep_sess;
	u32 id_dev = g_keep_id;
	u8 *eusr;
	u8 hat[0x50];
	int er;

	if (!sess || !id_dev || !g_keep_enrolled) {
		pr_info("qsee_fp: overlay AUTH kick skipped (no kept enroll sess)\n");
		return;
	}
	eusr = (u8 *)sess + 0x10;
	last_send_tmpl_missing = false;
	last_send_saw_ff_tmpl = false;
	last_send_auth_id_ok = false;
	last_send_itype = 0;
	last_send_esd = false;
	/*
	 * Enroll's 0x2 comes only after FDT_DOWN_DETECT-pre-wait, which
	 * follows WMODE-FDT-next. Rem=0 already did the next. Match was
	 * not sending the pre-wait, so the press was silent or 0x12.
	 * Send that pre-wait. No SLEEP. No FDT_DOWN (mode 1). No 1Hz QEV.
	 */
	{
		u8 pay1[4] = { 0, 0, 0, 0 };
		unsigned int irq0 = 0, irq1 = 0, tw;
		u8 wm_sleep[4];
		u8 wm_det[4];
		bool have_down = false;
		struct file *firq;
		char irqbuf[32];
		loff_t pos;
		ssize_t nr;

		pr_info("qsee_fp: overlay AUTH skip FDT_DOWN-auth (mode 1 ESD)\n");
		memset(wm_det, 0, sizeof(wm_det));
		*(__le32 *)wm_det = cpu_to_le32(FF_WMODE_FDT_DOWN_DETECT);
		er = send_ion(id_dev, sess, SESS_BIG, FF_TA_CFG_WORK_MODE, 4,
			      wm_det, "dev-WMODE-FDT_DOWN_DETECT-auth");
		if (er == -22) {
			pr_info("qsee_fp: overlay AUTH kick TA dead\n");
			pr_info("qsee_fp: overlay AUTH FAIL err=0xfffffff0 tmpl_missing=0 id_ok=0 (not a match)\n");
			return;
		}
		pr_info("qsee_fp: overlay AUTH FDT_DOWN_DETECT-pre-wait (same as enroll)\n");
		pr_info("qsee_fp: overlay AUTH kick — wait lift then PRESS\n");
		pr_info("qsee_fp: *** PRESS then LIFT finger sample 0/1 (HOLD on power-button sensor) ***\n");
		firq = filp_open(
			"/sys/class/focaltech_fp/focaltech_fp/irq_count",
			O_RDONLY, 0);
		if (!IS_ERR_OR_NULL(firq)) {
			pos = 0;
			nr = kernel_read(firq, irqbuf, sizeof(irqbuf) - 1, &pos);
			if (nr > 0) {
				irqbuf[nr] = '\0';
				if (kstrtouint(irqbuf, 10, &irq0) != 0)
					irq0 = 0;
			}
		} else {
			firq = NULL;
		}
		for (tw = 0; tw < 400u; tw++) {
			irq1 = irq0;
			if (firq) {
				pos = 0;
				nr = kernel_read(firq, irqbuf, sizeof(irqbuf) - 1,
						 &pos);
				if (nr > 0) {
					irqbuf[nr] = '\0';
					if (kstrtouint(irqbuf, 10, &irq1) != 0)
						irq1 = irq0;
				}
			}
			if (irq1 > irq0) {
				er = send_ion(id_dev, sess, SESS_BIG,
					      FF_TA_QUERY_EVENT, 4, pay1,
					      "dev-QEV-auth-down");
				if (er == -22) {
					if (firq)
						fput(firq);
					pr_info("qsee_fp: overlay AUTH kick TA dead\n");
					pr_info("qsee_fp: overlay AUTH FAIL err=0xfffffff0 tmpl_missing=0 id_ok=0 (not a match)\n");
					return;
				}
				pr_info("qsee_fp: overlay AUTH QEV itype=0x%x esd=%d irq=%u->%u\n",
					last_send_itype, last_send_esd, irq0, irq1);
				irq0 = irq1;
				/* Exact 0x2 only. 0x212 (rst|inv|down) CAPed empty avgv=966. */
				if (last_send_itype == 0x2 && !last_send_esd) {
					have_down = true;
					break;
				}
				pr_info("qsee_fp: overlay AUTH skip leftover itype=0x%x esd=%d\n",
					last_send_itype, last_send_esd);
			}
			msleep(20);
		}
		if (firq)
			fput(firq);
		if (!have_down) {
			pr_info("qsee_fp: overlay AUTH kick no itype 0x%x — no CAP\n",
				last_send_itype);
			memset(wm_sleep, 0, sizeof(wm_sleep));
			*(__le32 *)wm_sleep = cpu_to_le32(FF_WMODE_SLEEP);
			send_ion(id_dev, sess, SESS_BIG, FF_TA_CFG_WORK_MODE, 4,
				 wm_sleep, "dev-WMODE-SLEEP-auth-fail");
			pr_info("qsee_fp: overlay AUTH kick no finger frame\n");
			pr_info("qsee_fp: overlay AUTH FAIL err=0xfffffff0 tmpl_missing=0 id_ok=0 (not a match)\n");
			return;
		}
		pr_info("qsee_fp: overlay AUTH REAL DOWN itype=0x%x — enroll-flag CAP\n",
			last_send_itype);
		er = overlay_enroll_flag_caps(id_dev, sess);
		if (er == -22) {
			pr_info("qsee_fp: overlay AUTH kick TA dead\n");
			pr_info("qsee_fp: overlay AUTH FAIL err=0xfffffff0 tmpl_missing=0 id_ok=0 (not a match)\n");
			return;
		}
		if (er == 0 && last_send_frame_avgv < 600u &&
		    last_send_frame_avgv > 0)
			goto auth_have_frame;
		pr_info("qsee_fp: overlay AUTH kick empty avgv=%u — no match CAP\n",
			last_send_frame_avgv);
		memset(wm_sleep, 0, sizeof(wm_sleep));
		*(__le32 *)wm_sleep = cpu_to_le32(FF_WMODE_SLEEP);
		send_ion(id_dev, sess, SESS_BIG, FF_TA_CFG_WORK_MODE, 4, wm_sleep,
			 "dev-WMODE-SLEEP-auth-fail");
		pr_info("qsee_fp: overlay AUTH kick no finger frame\n");
		pr_info("qsee_fp: overlay AUTH FAIL err=0xfffffff0 tmpl_missing=0 id_ok=0 (not a match)\n");
		return;
	}
auth_have_frame:
	event_ctx_fill(sess, 0x5, 4, 3, 0);
	er = send_ion(id_dev, sess, SESS_BIG, FF_TA_REPORT_EVENT, 0x2e0, eusr,
		      "dev-REPORT-auth");
	/* Kept enroll sess already has RAM templates. Do not SET_GROUP.
	 * QEV/CAP ION dumps reprint leftover "0 template loaded" and
	 * poison tmpl_missing/id_ok. Clear before AUTH; HIT is this
	 * send's err, not init SET_GROUP strings.
	 */
	last_send_tmpl_missing = false;
	last_send_saw_ff_tmpl = false;
	last_send_auth_id_ok = false;
	memset(hat, 0, sizeof(hat));
	hat[0x1c] = 2;
	*(__le32 *)(hat + 0x45) = cpu_to_le32(0);
	hat[0x49] = 1;
	er = send_ion_seg(id_dev, sess, SESS_BIG, FF_TA_AUTHENTICATE, 0x4a, hat,
			  "dev-AUTH");
	last_send_app_err = le32_to_cpu(*((__le32 *)sess + 2));
	pr_info("qsee_fp: overlay AUTH er=%d err=0x%x sample=%u\n",
		er, last_send_app_err, 0u);
	if (last_send_app_err)
		pr_info("qsee_fp: overlay AUTH FAIL err=0x%x tmpl_missing=%d id_ok=%d (not a match)\n",
			last_send_app_err, last_send_tmpl_missing,
			last_send_auth_id_ok);
	else
		pr_info("qsee_fp: overlay AUTH HIT err=0x0 sample=%u\n", 0u);
	{
		u8 wm_done[4];

		memset(wm_done, 0, sizeof(wm_done));
		*(__le32 *)wm_done = cpu_to_le32(FF_WMODE_SLEEP);
		send_ion(id_dev, sess, SESS_BIG, FF_TA_CFG_WORK_MODE, 4, wm_done,
			 "dev-WMODE-SLEEP-auth-done");
	}
}

static int overlay_auth_thread(void *unused)
{
	overlay_auth_once();
	return 0;
}

static int auth_kick_set(const char *val, const struct kernel_param *kp)
{
	int n = 0;

	if (kstrtoint(val, 0, &n) || n == 0)
		return 0;
	if (!g_keep_sess || !g_keep_enrolled)
		return -ENODEV;
	kthread_run(overlay_auth_thread, NULL, "fp5-auth");
	return 0;
}

static const struct kernel_param_ops auth_kick_ops = {
	.set = auth_kick_set,
	.get = param_get_int,
};

static int auth_kick;
module_param_cb(auth_kick, &auth_kick_ops, &auth_kick, 0644);
MODULE_PARM_DESC(auth_kick, "echo 1: AUTH one press on kept enroll sess");

/*
 * M4dd: single 28×24 layout1 CAP to test TEE heap at a named stage.
 * Returns true on array_new OOM (0xffffff37).
 */
static bool m4dd_probe_28(u32 app_id, void *sess, const char *stage)
{
	u8 *usr = (u8 *)sess + 0x10;
	u8 hpay[4] = { 0 };
	int saved = cap_ctx_layout;
	int er;
	u32 err;
	bool oom = false;

	cap_ctx_layout = 1;
	cap_ctx_suppress_pa = false;
	capture_ctx_fill_flag(sess, 28, 24, 0x24, 28 * 24 * 2, false, 0x240);
	er = send_ion(app_id, sess, SESS_BIG, FF_TA_CAPTURE_IMAGE, 0x24, usr,
		      "dev-cap-m4dd-probe");
	err = last_send_app_err;
	pr_info("qsee_fp: M4dd probe 28x24 @%s er=%d app_err=0x%x frame=%d rows=%u\n",
		stage, er, err, last_send_frame_raw,
		le32_to_cpu(*(__le32 *)(usr + 8)));
	if (last_send_frame_raw || last_send_sensor_raw) {
		pr_info("qsee_fp: *** M4dd FRAME RAW @%s ***\n", stage);
	} else if (err == 0xffffff37u) {
		oom = true;
		send_ion(app_id, sess, SESS_BIG, 0x1003, 0, NULL,
			 "dev-LOGCAT-m4dd-oom");
		dump_residual_budget(sess, "M4DD-OOM");
		send_ion(app_id, sess, SESS_BIG, FF_TA_CHECK_HEALTH, 4, hpay,
			 "dev-HEALTH-m4dd");
		pr_info("qsee_fp: M4dd OOM @%s — heap dead here\n", stage);
	} else if (err == 0) {
		pr_info("qsee_fp: M4dd soft/no-frame @%s (heap alive)\n", stage);
	}
	cap_ctx_layout = saved;
	cap_ctx_suppress_pa = false;
	return oom;
}

/*
 * M4di: one CAP at given layout/flag/dims/pa.
 * Returns: 1=frame raw, 0=soft err=0, -1=OOM/Null 0xffffff37, -2=other.
 */
static int m4di_probe_one(u32 app_id, void *sess, int layout, u32 cols, u32 rows,
			  u32 flag, bool use_pa, const char *tag)
{
	u8 *usr = (u8 *)sess + 0x10;
	u8 hpay[4] = { 0 };
	int saved = cap_ctx_layout;
	bool saved_pa = cap_ctx_suppress_pa;
	int er;
	u32 err, nb = cols * rows * 2;
	int rc;

	cap_ctx_layout = layout;
	cap_ctx_suppress_pa = !use_pa;
	capture_ctx_fill_flag(sess, cols, rows, 0x24, nb, false, flag);
	er = send_ion(app_id, sess, SESS_BIG, FF_TA_CAPTURE_IMAGE, 0x24, usr,
		      "dev-cap-m4di");
	err = last_send_app_err;
	pr_info("qsee_fp: M4di %s L%d %ux%u flag=0x%x pa=%d er=%d app_err=0x%x frame=%d rows=%u\n",
		tag, layout, cols, rows, flag, use_pa, er, err,
		last_send_frame_raw, le32_to_cpu(*(__le32 *)(usr + 8)));
	if (last_send_frame_raw || last_send_sensor_raw) {
		pr_info("qsee_fp: *** M4di FRAME RAW %s ***\n", tag);
		rc = 1;
	} else if (err == 0xffffff37u) {
		send_ion(app_id, sess, SESS_BIG, 0x1003, 0, NULL, "dev-LOGCAT-m4di-oom");
		dump_residual_budget(sess, "M4DI-OOM");
		rc = -1;
	} else if (err == 0) {
		rc = 0;
	} else {
		rc = -2;
	}
	/* HEALTH recover so next rung does not sticky-Null */
	send_ion(app_id, sess, SESS_BIG, FF_TA_CHECK_HEALTH, 4, hpay,
		 "dev-HEALTH-m4di");
	msleep(10);
	cap_ctx_layout = saved;
	cap_ctx_suppress_pa = saved_pa;
	return rc;
}

/*
 * M4di ladder on fresh TA (post-PROBE, pre-INIT_DEV): separate FDT soft path
 * (flag 0x1, M4ck OK) from image path (flag 0x240 OOM). Answers whether
 * "heap dead before Base" is image-flag-only or true empty heap.
 */
static void m4di_pre_init_ladder(u32 app_id, void *sess)
{
	int r_fdt16, r_fdt28, r_img16, r_img28, r_img28pa;

	pr_info("qsee_fp: M4di pre-INIT ladder begin (fresh TA, no Base)\n");

	/* FDT soft — should work if TA/heap not truly dead (M4ck) */
	r_fdt16 = m4di_probe_one(app_id, sess, 1, 16, 16, 0x1, false, "fdt16");
	r_fdt28 = m4di_probe_one(app_id, sess, 1, 28, 24, 0x1, false, "fdt28");
	/* Image flag tiny then 28×24; nopa then one PA */
	r_img16 = m4di_probe_one(app_id, sess, 1, 16, 16, 0x240, false, "img16");
	r_img28 = m4di_probe_one(app_id, sess, 1, 28, 24, 0x240, false, "img28");
	r_img28pa = m4di_probe_one(app_id, sess, 1, 28, 24, 0x240, true, "img28pa");

	pr_info("qsee_fp: M4di ladder summary fdt16=%d fdt28=%d img16=%d img28=%d img28pa=%d (1=frame 0=soft -1=OOM -2=other)\n",
		r_fdt16, r_fdt28, r_img16, r_img28, r_img28pa);

	if ((r_fdt16 == 0 || r_fdt16 == 1) && r_img16 == -1)
		pr_info("qsee_fp: M4di verdict: FDT soft OK, image flag OOM — not empty-heap, image path array_new\n");
	else if (r_fdt16 == -1 && r_img16 == -1)
		pr_info("qsee_fp: M4di verdict: FDT+image both OOM — heap/TA truly tight pre-Base\n");
	else if (r_img16 == 0 || r_img16 == 1 || r_img28 == 0 || r_img28 == 1)
		pr_info("qsee_fp: M4di verdict: image CAP soft/frame pre-Base ***\n");
	else if ((r_fdt16 == 0 || r_fdt16 == 1) && r_img28 == -1 && r_img16 != -1)
		pr_info("qsee_fp: M4di verdict: tiny image OK, 28x24 OOM — size limited\n");
	else
		pr_info("qsee_fp: M4di verdict: mixed/other (see ladder summary)\n");
}

/* M4dj: two rungs only — FDT soft 16×16 vs image 16×16 at a named stage */
static void m4dj_pair_at(u32 app_id, void *sess, const char *stage)
{
	int rf, ri;

	rf = m4di_probe_one(app_id, sess, 1, 16, 16, 0x1, false, stage);
	ri = m4di_probe_one(app_id, sess, 1, 16, 16, 0x240, false, stage);
	pr_info("qsee_fp: M4dj @%s fdt16=%d img16=%d (1=frame 0=soft -1=OOM)\n",
		stage, rf, ri);
	if ((rf == 0 || rf == 1) && ri == -1)
		pr_info("qsee_fp: M4dj @%s: FDT soft OK, image OOM\n", stage);
	else if (rf == -1 && ri == -1)
		pr_info("qsee_fp: M4dj @%s: both OOM\n", stage);
	else if (rf == 0 || rf == 1 || ri == 0 || ri == 1)
		pr_info("qsee_fp: M4dj @%s: CAP soft/frame ***\n", stage);
}

/*
 * M4dk: layout0 vs layout1 × FDT(0x1) vs image(0x240) at 16×16.
 * INIT Step3 can array_new; CAPTURE often cannot — form/flag may be the gate.
 * Sets m4dk_soft_* if any soft/frame (for AFR hint).
 */
static void m4dk_matrix(u32 app_id, void *sess, const char *stage)
{
	int r_l0f, r_l0i, r_l1f, r_l1i;

	pr_info("qsee_fp: M4dk matrix @%s begin (L0/L1 × fdt/img 16x16)\n", stage);
	r_l0f = m4di_probe_one(app_id, sess, 0, 16, 16, 0x1, false, "L0fdt");
	r_l0i = m4di_probe_one(app_id, sess, 0, 16, 16, 0x240, false, "L0img");
	r_l1f = m4di_probe_one(app_id, sess, 1, 16, 16, 0x1, false, "L1fdt");
	r_l1i = m4di_probe_one(app_id, sess, 1, 16, 16, 0x240, false, "L1img");
	pr_info("qsee_fp: M4dk @%s L0fdt=%d L0img=%d L1fdt=%d L1img=%d (1=frame 0=soft -1=OOM)\n",
		stage, r_l0f, r_l0i, r_l1f, r_l1i);

	/*
	 * M4dl: prefer **image** soft (0x240) over FDT (0x1) — M4dk picked
	 * L0+0x1 first and under-finger got FDT soft only (no frame).
	 * Order: L1img → L0img → L1fdt → L0fdt.
	 */
	if (!m4dk_have_soft) {
		if (r_l1i == 0 || r_l1i == 1) {
			m4dk_have_soft = true;
			m4dk_soft_layout = 1;
			m4dk_soft_flag = 0x240;
		} else if (r_l0i == 0 || r_l0i == 1) {
			m4dk_have_soft = true;
			m4dk_soft_layout = 0;
			m4dk_soft_flag = 0x240;
		} else if (r_l1f == 0 || r_l1f == 1) {
			m4dk_have_soft = true;
			m4dk_soft_layout = 1;
			m4dk_soft_flag = 0x1;
		} else if (r_l0f == 0 || r_l0f == 1) {
			m4dk_have_soft = true;
			m4dk_soft_layout = 0;
			m4dk_soft_flag = 0x1;
		}
		if (m4dk_have_soft)
			pr_info("qsee_fp: M4dl prefer L%d flag=0x%x for AFR\n",
				m4dk_soft_layout, m4dk_soft_flag);
	}

	if ((r_l0f == 0 || r_l0f == 1) && r_l1f == -1)
		pr_info("qsee_fp: M4dk @%s verdict: layout0 soft, layout1 OOM\n",
			stage);
	else if (r_l0f == -1 && r_l1f == -1 && r_l0i == -1 && r_l1i == -1)
		pr_info("qsee_fp: M4dk @%s verdict: all OOM (form/flag irrelevant)\n",
			stage);
	else if (r_l0i == 0 || r_l0i == 1 || r_l1i == 0 || r_l1i == 1)
		pr_info("qsee_fp: M4dk @%s verdict: image CAP soft/frame ***\n",
			stage);
	else if ((r_l0f == 0 || r_l0f == 1 || r_l1f == 0 || r_l1f == 1) &&
		 r_l0i == -1 && r_l1i == -1)
		pr_info("qsee_fp: M4dk @%s verdict: FDT soft only, image OOM\n",
			stage);
	else
		pr_info("qsee_fp: M4dk @%s verdict: mixed (see matrix)\n", stage);
}

static u32 try_force_image_path(u32 app_id, void *sess, bool ta_down, const char *why)
{
	u8 *usr = (u8 *)sess + 0x10;
	u8 wm[8];
	u8 cap[0x30];
	u32 best = 0, nz, flag;
	u32 offs[2] = { 0x24, 0x100 };
	unsigned int oi, fi;
	/*
	 * M4be: HAL __svc_capture_image gates on flag bit6 (0x40) and/or
	 * bit9 (0x200). Plain flag=1 is FDT-only soft-success. Prefer image
	 * bits first under finger so ft93xx_img_scan / frame raw can run.
	 */
	u32 flags_img[] = { 0x240, 0x41, 0x200, 0x40, 1 };
	u32 flags_short[] = { 0x41, 1 };
	u32 flags_full[] = { 0x240, 0x41, 0x200, 0x40, 1 };
	u32 *flags = dual_img_off ? flags_full : flags_short;
	unsigned int nflags = dual_img_off ? ARRAY_SIZE(flags_full) :
					     ARRAY_SIZE(flags_short);
	char tag[40];
	int er;
	u32 spd;
	bool did_spi_retry = false;

	pr_info("qsee_fp: force-image begin (%s) dead=%d dual=%d down=%d spi_fail=%d itype=0x%x\n",
		why, try_dead_pixel, dual_img_off, ta_down, last_send_spi_mode_fail,
		last_send_itype);

	/*
	 * M4bd: under finger, do NOT wait_chip_ready/esd_clear/l2_reset —
	 * Android images within ~50ms of IRQ while FDT_DOWN stays armed.
	 * Reset mid-press re-sticks [esd] and kills finger_touch (M4bc hold).
	 */
	if (!ta_down)
		(void)wait_chip_ready(app_id, sess, "force-pre", 4);

	/*
	 * M4c4: under finger — NO SPI speed / WMODE / SCAN prep (same as AFR).
	 * M4c1 residual: any WMODE under finger → QES [esd] → FDT-only CAP.
	 */
	if (ta_down) {
		pr_info("qsee_fp: force-img under finger MINIMAL (no WMODE/SCAN/SPI) spi_fail=%d\n",
			last_send_spi_mode_fail);
		goto force_cap_loop;
	}

	/* Soft SPI recover only (no INIT_SPI); idle path only */
	if (last_send_spi_mode_fail)
		spi_image_recover(app_id, sess, "force-pre");

	/* Capture SPI speed (Android: default 4M; capture_bps often unset) */
	spd = spi_capture_bps > 0 ? (u32)spi_capture_bps : 4000000;
	*(__le32 *)usr = cpu_to_le32(spd);
	if (send_ion(app_id, sess, SESS_BIG, FF_TA_SET_SPI_SPEED, 4, usr,
		     "dev-SPI-SPEED-img") == -22)
		return 0;

	/*
	 * Idle: optional mode-5 prime. Under finger never (re-sticks ESD).
	 * Only prime if force_img_mode5=1.
	 */
	if (force_img_mode5) {
		memset(wm, 0, sizeof(wm));
		*(__le32 *)wm = cpu_to_le32(FF_WMODE_IMG_CALIB);
		send_ion(app_id, sess, SESS_BIG, FF_TA_CFG_WORK_MODE, 4, wm,
			 "dev-WMODE-5-force-img");
		msleep(20);
	}
	memset(cap, 0, sizeof(cap));
	*(__le32 *)cap = cpu_to_le32(1);
	send_ion(app_id, sess, SESS_BIG, FF_TA_START_SCANNING, 0x30, cap,
		 "dev-SCAN-force-img");
	msleep(15);

	/* FDT_DOWN for nav-mode image scan (idle path arming only) */
	memset(wm, 0, sizeof(wm));
	*(__le32 *)wm = cpu_to_le32(FF_WMODE_FDT_DOWN);
	send_ion(app_id, sess, SESS_BIG, FF_TA_CFG_WORK_MODE, 4, wm,
		 "dev-WMODE-FDT-force-img");
	msleep(15);

force_cap_loop:

	/* M4be: under finger sweep image flags until frame raw; idle: short set */
	if (ta_down) {
		flags = flags_img;
		nflags = ARRAY_SIZE(flags_img);
	}

	for (oi = 0; oi < (dual_img_off && !ta_down ? 2u : 1u); oi++) {
		u32 img_off = offs[oi];

		for (fi = 0; fi < nflags; fi++) {
			flag = flags[fi];
			/* No a5 paint under finger; optional idle paint via paint_cap_a5 */
			capture_ctx_fill_flag(sess, FF_IMG_COLS, FF_IMG_ROWS, img_off,
					      FF_IMG_BYTES, paint_cap_a5 && !ta_down, flag);
			scnprintf(tag, sizeof(tag), "dev-cap-force-%s-f%02x-o%x", why,
				  flag, img_off);
			er = send_ion(app_id, sess, SESS_BIG, FF_TA_CAPTURE_IMAGE, 0x24,
				      usr, tag);
			if (er == -22)
				return best;
			/*
			 * M4ck: layout1 + image flags can Null (0xffffff37) / OOM.
			 * Do NOT thrash more CAPs — sticky Null until HEALTH. Stop.
			 */
			if (last_send_app_err == 0xffffff37u && !ta_down) {
				pr_info("qsee_fp: force CAP Null err=0x%x layout=%d — abort thrash (%s)\n",
					last_send_app_err, cap_ctx_layout, why);
				send_ion(app_id, sess, SESS_BIG, 0x1003, 0, NULL,
					 "dev-LOGCAT-force-null");
				dump_residual_budget(sess, "FORCE-NULL");
				send_ion(app_id, sess, SESS_BIG, FF_TA_CHECK_HEALTH, 4,
					 usr, "dev-HEALTH-force-null");
				pr_info("qsee_fp: after Null HEALTH app_err=0x%x\n",
					last_send_app_err);
				return best;
			}
			/*
			 * M4av: configure spi mode failed → img_scan never logs
			 * frame raw. One soft recover + single CAP retry.
			 */
			if (last_send_spi_mode_fail && spi_img_recover &&
			    !did_spi_retry && !ta_down) {
				/* M4c4: SPI recover only idle — under finger stay minimal */
				pr_info("qsee_fp: CAP spi mode fail — recover+retry (%s)\n",
					why);
				spi_image_recover(app_id, sess, "force-cap");
				did_spi_retry = true;
				*(__le32 *)usr = cpu_to_le32(spd);
				send_ion(app_id, sess, SESS_BIG, FF_TA_SET_SPI_SPEED, 4,
					 usr, "dev-SPI-SPEED-img2");
				memset(wm, 0, sizeof(wm));
				*(__le32 *)wm = cpu_to_le32(FF_WMODE_FDT_DOWN);
				send_ion(app_id, sess, SESS_BIG, FF_TA_CFG_WORK_MODE, 4,
					 wm, "dev-WMODE-FDT-force-img2");
				msleep(15);
				capture_ctx_fill_flag(sess, FF_IMG_COLS, FF_IMG_ROWS,
						      img_off, FF_IMG_BYTES, !ta_down,
						      flag);
				scnprintf(tag, sizeof(tag),
					  "dev-cap-force-%s-retry-f%02x", why, flag);
				er = send_ion(app_id, sess, SESS_BIG,
					      FF_TA_CAPTURE_IMAGE, 0x24, usr, tag);
				if (er == -22)
					return best;
			}
			/* Dump capture_ctx after CAP — layout-aware field print */
			{
				u32 w0c = le32_to_cpu(*(__le32 *)(usr + 0xc));
				u32 w10 = le32_to_cpu(*(__le32 *)(usr + 0x10));
				u32 d_off = (cap_ctx_layout == 1) ? w10 : w0c;
				u32 d_nb = (cap_ctx_layout == 1) ? w0c : w10;

				pr_info("qsee_fp: %s ctx after CAP layout=%d flag=0x%x cols=%u rows=%u img_off=0x%x nb=%u head=%*ph spi_fail=%d\n",
					tag, cap_ctx_layout,
					le32_to_cpu(*(__le32 *)(usr + 0)),
					le32_to_cpu(*(__le32 *)(usr + 4)),
					le32_to_cpu(*(__le32 *)(usr + 8)),
					d_off, d_nb, 16, usr + img_off,
					last_send_spi_mode_fail);
			}
			capture_ctx_fix_rows(sess, FF_IMG_COLS, FF_IMG_ROWS, img_off,
						 FF_IMG_BYTES);
			nz = capture_scan_all(sess, img_off, FF_IMG_BYTES, tag, er);
			if (nz > best)
				best = nz;
			/*
			 * Do NOT use large plen CAPTURE (0x24+img) here: live M4ap
			 * showed plen_big → sticky Null pointer on all later APP_SEND
			 * until HEALTH. Meta plen 0x24 only + SRAM harvest.
			 */
			if (last_send_frame_raw || last_send_sensor_raw ||
			    last_send_image_ready) {
				pr_info("qsee_fp: *** FORCE IMG MARK frame=%d sens=%d irdy=%d nz=%u ***\n",
					last_send_frame_raw, last_send_sensor_raw,
					last_send_image_ready, nz);
				return max_t(u32, best, nz);
			}
			/* Dense ion frame: keep as-is for SAVE/enroll feed */
			if (nz > 256) {
				pr_info("qsee_fp: *** DENSE IMAGE FRAME nz=%u off=0x%x (%s) ***\n",
					nz, img_off, why);
				best = nz;
				goto out_log;
			}

			/* M4bd: dead-pixel may paint a5 — never under finger */
			if (try_dead_pixel && !ta_down) {
				capture_ctx_fill_flag(sess, FF_IMG_COLS, FF_IMG_ROWS,
						      img_off, FF_IMG_BYTES, paint_cap_a5, flag);
				scnprintf(tag, sizeof(tag), "dev-dead-%s-f%02x-o%x", why,
					  flag, img_off);
				er = send_ion(app_id, sess, SESS_BIG,
					      FF_TA_CHECK_DEAD_PIXEL, 0x24, usr, tag);
				if (er == -22)
					return best;
				nz = capture_scan_all(sess, img_off, FF_IMG_BYTES, tag, er);
				if (nz > best)
					best = nz;
				if (last_send_frame_raw || last_send_sensor_raw) {
					pr_info("qsee_fp: *** DEAD-PIXEL IMG MARK nz=%u ***\n",
						nz);
					return max_t(u32, best, nz);
				}
			}
			/* Stop flag/off sweep if we already have strong pixels */
			if (best > 256)
				goto out_log;
		}
	}

out_log:
	/* Flush TA log window for last mode / img_scan / frame raw */
	send_ion(app_id, sess, SESS_BIG, 0x1003, 0, NULL, "dev-LOGCAT-force-img");
	if (last_send_frame_raw || last_send_sensor_raw || last_send_image_ready)
		pr_info("qsee_fp: *** LOGCAT force-img frame=%d sens=%d irdy=%d ***\n",
			last_send_frame_raw, last_send_sensor_raw, last_send_image_ready);
	/* Also pull via XFER/SRAM after forced CAP */
	{
		u32 pnz = try_pull_image_ree_ta(app_id, sess, why);

		if (pnz > best)
			best = pnz;
	}
	/*
	 * Second CAP pass at measured-best layout after SRAM harvest may copy
	 * a fuller frame into ion (Android transmit_image ~37ms window).
	 */
	if (best > 0 && best < 1000) {
		capture_ctx_fill_flag(sess, FF_IMG_COLS, FF_IMG_ROWS, 0x24,
				      FF_IMG_BYTES, false, 1);
		er = send_ion(app_id, sess, SESS_BIG, FF_TA_CAPTURE_IMAGE, 0x24,
			      usr, "dev-cap-force-reap");
		if (er != -22) {
			nz = capture_scan_all(sess, 0x24, FF_IMG_BYTES,
					      "dev-cap-force-reap", er);
			if (nz > best)
				best = nz;
			send_ion(app_id, sess, SESS_BIG, 0x1003, 0, NULL,
				 "dev-LOGCAT-force-reap");
		}
	}
	pr_info("qsee_fp: force-image done best_nz=%u frame=%d sens=%d (%s)\n",
		best, last_send_frame_raw, last_send_sensor_raw, why);
	return best;
}

static int ta_read_reg(u32 app_id, void *sess, u32 addr, u32 *out_val,
		       const char *tag)
{
	u8 *usr = (u8 *)sess + 0x10;
	int er;

	memset(usr, 0, 16);
	*(__le32 *)usr = cpu_to_le32(addr);
	er = send_ion(app_id, sess, SESS_BIG, FF_TA_READ_REG, 4, usr, tag);
	if (out_val)
		*out_val = le32_to_cpu(*(__le32 *)usr);
	return er;
}

static int ta_read_sram(u32 app_id, void *sess, u32 addr, u32 len,
			const char *tag)
{
	u8 *usr = (u8 *)sess + 0x10;
	u32 plen = 8 + len;
	int er;

	if (plen > 0x70000)
		return -EINVAL;
	memset(usr, 0, plen);
	*(__le32 *)(usr + 0) = cpu_to_le32(addr);
	*(__le32 *)(usr + 4) = cpu_to_le32(len);
	memset(usr + 8, 0xA5, len);
	/* HAL uses plen 0x8 meta only; also try large plen so ion holds payload */
	er = send_ion(app_id, sess, SESS_BIG, FF_TA_READ_SRAM, plen, usr, tag);
	if (er == -22)
		return er;
	/* fallback: meta-only 0x8 then scan ion mid for pixels */
	if (er) {
		memset(usr, 0, 8);
		*(__le32 *)(usr + 0) = cpu_to_le32(addr);
		*(__le32 *)(usr + 4) = cpu_to_le32(len);
		er = send_ion(app_id, sess, SESS_BIG, FF_TA_READ_SRAM, 8, usr,
			      tag);
	}
	return er;
}

/* After CAP: try XFER probe + SRAM dump; return best ADC-range pixel count */
static u32 try_pull_image_ree_ta(u32 app_id, void *sess, const char *why)
{
	static const u8 id_cmds[] = { 0x60, 0x90, 0xa0, 0xf0, 0x80, 0x00 };
	u32 best = 0, nz, regv = 0;
	unsigned int i;
	char tag[40];
	u8 *usr = (u8 *)sess + 0x10;
	u8 *rsp = (u8 *)sess + RSP_ION_OFF;
	u32 spd = 4000000;

	pr_info("qsee_fp: image-pull begin (%s)\n", why);

	/* Set SPI speed (HAL SET_SPI_SPEED plen 4) */
	*(__le32 *)usr = cpu_to_le32(spd);
	if (send_ion(app_id, sess, SESS_BIG, FF_TA_SET_SPI_SPEED, 4, usr,
		     "dev-SPI-SPEED") == -22)
		return 0;

	/* Register probes — value may land at usr[0] or usr[4] after send */
	for (i = 0; i < 8; i++) {
		u32 a = i;
		u32 v0, v4;

		scnprintf(tag, sizeof(tag), "dev-RREG-%02x", a);
		if (ta_read_reg(app_id, sess, a, &regv, tag) == -22)
			return 0;
		v0 = le32_to_cpu(*(__le32 *)usr);
		v4 = le32_to_cpu(*(__le32 *)(usr + 4));
		pr_info("qsee_fp: READ_REG 0x%02x => v0=0x%08x v4=0x%08x head=%*ph\n",
			a, v0, v4, 16, usr);
	}

	/* SPI xfer write1/read4 chip-id style */
	for (i = 0; i < ARRAY_SIZE(id_cmds); i++) {
		u8 cmd = id_cmds[i];

		scnprintf(tag, sizeof(tag), "dev-XFER-id-%02x", cmd);
		if (ta_xfer_spi(app_id, sess, &cmd, 1, 4, tag) == -22)
			return 0;
		pr_info("qsee_fp: XFER cmd=0x%02x ion8=%*ph rsp=%*ph\n", cmd, 8,
			usr + 8, 8, rsp + 4);
		nz = capture_scan_pixels(usr + 8, 32, NULL, NULL, NULL, NULL);
		if (nz > best)
			best = nz;
	}

	/*
	 * Full-duplex style: clock out zeros for image-sized read (some TAs
	 * treat wlen==rlen as duplex).
	 */
	{
		u32 chunk = 512;
		u8 *wbuf;

		wbuf = kzalloc(chunk, GFP_KERNEL);
		if (wbuf) {
			scnprintf(tag, sizeof(tag), "dev-XFER-duplex-%s", why);
			if (ta_xfer_spi(app_id, sess, wbuf, chunk, chunk, tag) !=
			    -22) {
				nz = capture_scan_all(sess, 8, chunk * 2, tag, 0);
				if (nz > best)
					best = nz;
				pr_info("qsee_fp: XFER-duplex head=%*ph nz=%u\n", 16,
					usr + 8, nz);
			}
			kfree(wbuf);
		}
	}

	/* Bulk XFER read after 1-byte cmd 0x00 */
	{
		u8 w0 = 0x00;

		scnprintf(tag, sizeof(tag), "dev-XFER-bulk-%s", why);
		if (ta_xfer_spi(app_id, sess, &w0, 1, min_t(u32, FF_IMG_BYTES, 4096),
				tag) != -22) {
			nz = capture_scan_all(sess, 8 + 1,
					      min_t(u32, FF_IMG_BYTES, 4096), tag,
					      0);
			if (nz > best)
				best = nz;
			pr_info("qsee_fp: XFER-bulk head=%*ph nz=%u\n", 16,
				usr + 9, nz);
		}
	}

	/*
	 * SRAM dumps — TA may stash last frame after CAPTURE/DEAD_PIXEL.
	 * Prefer 0x1000/0x2000 (M4ai: post-force dynamic ADC sparse there).
	 */
	{
		static const u32 sram_pref[] = {
			0x1000, 0x2000, 0x4000, 0, 0x100, 0x8000
		};

		for (i = 0; i < ARRAY_SIZE(sram_pref); i++) {
			u32 off = sram_pref[i];
			u32 len = min_t(u32, FF_IMG_BYTES, 0x5000);
			u32 avg = 0, mn = 0, mx = 0, na5 = 0;

			scnprintf(tag, sizeof(tag), "dev-RSRAM-0x%x", off);
			if (ta_read_sram(app_id, sess, off, len, tag) == -22)
				return best;
			nz = capture_scan_all(sess, 8, len, tag, 0);
			if (nz > best)
				best = nz;
			/* Dynamic sparse: copy into image slot for SAVE/enroll probe */
			capture_scan_pixels(usr + 8, min_t(u32, len, FF_IMG_BYTES), &na5,
					    &avg, &mn, &mx);
			if (nz > 16 && mx > mn + 32) {
				u32 copy = min_t(u32, len, FF_IMG_BYTES);

				if (0x10 + 0x24 + copy <= REQ_ION) {
					memcpy(usr + 0x24, usr + 8, copy);
					pr_info("qsee_fp: *** SRAM IMAGE DYNAMIC off=0x%x nz=%u avg=%u min=%u max=%u → ion+0x24 (%s) ***\n",
						off, nz, avg, mn, mx, why);
					best = max_t(u32, best, nz);
					break;
				}
			}
			pr_info("qsee_fp: RSRAM 0x%x nz_ret=%u\n", off, nz);
		}
	}

	/* Wide ion mid scan in case TA wrote elsewhere */
	nz = capture_scan_all(sess, 0x200, FF_IMG_BYTES, "dev-ion-mid", 0);
	if (nz > best)
		best = nz;

	pr_info("qsee_fp: image-pull done best_nz=%u (%s)\n", best, why);
	return best;
}

/*
 * Synthetic frame in shared ion (Android-like 112x88 u16). Used to test whether
 * REPORT/SAVE/enroll can consume HLOS-visible pixels when TA CAP leaves ion empty.
 * Pattern: ridge-ish gradient with spatial variation (not flat).
 */
static void fill_synthetic_frame(void *sess, u32 img_off)
{
	u8 *base = (u8 *)sess + 0x10 + img_off;
	u16 *pix = (u16 *)base;
	u32 x, y;

	for (y = 0; y < FF_IMG_ROWS; y++) {
		for (x = 0; x < FF_IMG_COLS; x++) {
			u32 v = 200 + ((x * 7 + y * 11) % 400) + ((x ^ y) & 0x3f);

			if (v > 900)
				v = 900;
			pix[y * FF_IMG_COLS + x] = cpu_to_le16((u16)v);
		}
	}
	pr_info("qsee_fp: synthetic frame filled off=0x%x head=%*ph\n", img_off, 16,
		base);
}

/*
 * Feed image in ion via SAVE_DATA + REPORT — probe enroll rem path.
 * If keep_existing, do not overwrite ion+0x24 (M4ai harvested / force CAP pixels).
 */
static int try_feed_image_enroll(u32 app_id, void *sess, bool ta_down,
				 bool keep_existing)
{
	u8 *usr = (u8 *)sess + 0x10;
	u32 nz;
	int er;
	u8 pix_backup[64];

	if (keep_existing)
		memcpy(pix_backup, usr + 0x24, sizeof(pix_backup));
	else
		fill_synthetic_frame(sess, 0x24);

	/* capture_ctx meta; re-fill pixels if we wiped them */
	capture_ctx_fill(sess, FF_IMG_COLS, FF_IMG_ROWS, 0x24, FF_IMG_BYTES, false);
	if (keep_existing)
		memcpy(usr + 0x24, pix_backup, sizeof(pix_backup));
	else
		fill_synthetic_frame(sess, 0x24);

	nz = capture_scan_all(sess, 0x24, FF_IMG_BYTES,
			      keep_existing ? "dev-real-frame" : "dev-synth-frame", 0);
	pr_info("qsee_fp: feed_image keep=%d nz=%u ta_down=%d\n", keep_existing, nz,
		ta_down);

	memset(usr, 0, 0x2e0);
	if (ta_down)
		*(__le32 *)usr = cpu_to_le32(0x2);
	*(__le32 *)(usr + 4) = cpu_to_le32(FF_IMG_COLS);
	*(__le32 *)(usr + 8) = cpu_to_le32(FF_IMG_ROWS);
	/* re-point image after event header wipe: restore pixels at 0x24 again */
	if (keep_existing)
		memcpy(usr + 0x24, pix_backup, sizeof(pix_backup));
	else
		fill_synthetic_frame(sess, 0x24);

	er = send_ion(app_id, sess, SESS_BIG, FF_TA_SAVE_DATA, 0x2e0, usr,
		      keep_existing ? "dev-SAVE-real" : "dev-SAVE-synth");
	if (er == -22)
		return er;
	er = send_ion(app_id, sess, SESS_BIG, FF_TA_REPORT_EVENT, 0x2e0, usr,
		      keep_existing ? "dev-REPORT-real" : "dev-REPORT-synth");
	if (er == -22)
		return er;
	send_ion(app_id, sess, SESS_BIG, 0x1003, 0, NULL,
		 keep_existing ? "dev-LOGCAT-real" : "dev-LOGCAT-synth");
	if (last_send_enrolling || last_send_frame_raw || last_send_sensor_raw)
		pr_info("qsee_fp: *** FEED ENROLL MARKERS keep=%d frame=%d enroll=%d sens=%d ***\n",
			keep_existing, last_send_frame_raw, last_send_enrolling,
			last_send_sensor_raw);
	return er;
}

static int try_feed_synthetic_enroll(u32 app_id, void *sess, bool ta_down)
{
	return try_feed_image_enroll(app_id, sess, ta_down, false);
}

/*
 * Android segmented transfer (FF_CMD_TA_SEGMENTED_TRANS = 0x1000).
 * From fingerprint.default.so ff_trustlet_client send path when payload is
 * not already at the ion base:
 *   ion+0x00: cmd=0x1000
 *   ion+0x04: plen=chunk+0x10
 *   ion+0x10: orig_cmd, seg_index, chunk_size, remaining_after
 *   ion+0x20: payload[chunk]
 * ENROLL uses this; plain direct framing SCM-kills the TA.
 */
static int send_ion_seg(u32 app_id, void *sess, size_t sess_sz,
			u32 cmd, u32 plen, const void *pay, const char *tag)
{
	void *req = sess;
	void *rsp = (u8 *)sess + RSP_ION_OFF;
	const u8 *src = pay;
	u32 remaining = plen;
	u32 seg_index = 0;
	/* leave room for 0x20 header; match Android sub-0x20 of buffer size */
	u32 max_chunk = 0x1000;
	int ret = 0;

	if (sess_sz < SESS_BIG || !pay || !plen)
		return -EINVAL;

	while (remaining) {
		u32 chunk = min(remaining, max_chunk);
		u32 rem_after = remaining - chunk;
		u8 *p = req;

		/* Only clear header + this chunk — do not wipe rest of ion (capture/image). */
		memset(p, 0, 0x20 + chunk);
		*(__le32 *)p = cpu_to_le32(FF_TA_SEGMENTED);
		*((__le32 *)p + 1) = cpu_to_le32(chunk + 0x10);
		*((__le32 *)(p + 0x10) + 0) = cpu_to_le32(cmd);
		*((__le32 *)(p + 0x10) + 1) = cpu_to_le32(seg_index);
		*((__le32 *)(p + 0x10) + 2) = cpu_to_le32(chunk);
		*((__le32 *)(p + 0x10) + 3) = cpu_to_le32(rem_after);
		memcpy(p + 0x20, src, chunk);
		*(__le32 *)rsp = 0;

		/* Must use app_send_ex (listener loop). Mainline app_send historically
		 * SCM-killed ENROLL when TZ blocked on FILE/RPMB.
		 */
		ret = app_send_ex(app_id, req, REQ_ION, rsp, RSP_ION, tag);
		pr_info("qsee_fp: SEG %s[%u] ret=%d cmd=0x%x chunk=%u rem_after=%u req0=0x%x rsp0=0x%x err=0x%x\n",
			tag, seg_index, ret, cmd, chunk, rem_after,
			le32_to_cpu(*(__le32 *)req),
			le32_to_cpu(*(__le32 *)rsp),
			le32_to_cpu(*((__le32 *)req + 2)));
		if (ret == 0 && (le32_to_cpu(*(__le32 *)req) & 0x80000000))
			dump_interesting(tag, rsp, min_t(size_t, RSP_ION, 512));
		if (ret)
			return ret;

		last_send_app_err = le32_to_cpu(*((__le32 *)req + 2));
		src += chunk;
		remaining = rem_after;
		seg_index++;
	}
	return ret;
}

/* Also try smaller ion-like splits in case full 0x78000 is rejected */
static int send_ion_custom(u32 app_id, void *sess, size_t sess_sz,
			   size_t req_len, size_t rsp_off, size_t rsp_len,
			   u32 cmd, u32 plen, const void *pay, const char *tag)
{
	void *req = sess;
	void *rsp = (u8 *)sess + rsp_off;
	int ret;
	u32 st_req, st_rsp;

	if (rsp_off + rsp_len > sess_sz || req_len > rsp_off)
		return -EINVAL;
	memset(sess, 0, sess_sz);
	*(__le32 *)req = cpu_to_le32(cmd);
	*((__le32 *)req + 1) = cpu_to_le32(plen);
	if (pay && plen)
		memcpy((u8 *)req + 0x10, pay, plen);
	memset(rsp, 0xee, min_t(size_t, rsp_len, 64));

	ret = qcom_scm_qseecom_app_send(app_id, req, req_len, rsp, rsp_len);
	st_req = le32_to_cpu(*(__le32 *)req);
	st_rsp = le32_to_cpu(*(__le32 *)rsp);
	if (ret == 0 && st_rsp != 0xffffffff) {
		pr_info("qsee_fp: CUS HIT %s ret=%d req0=0x%x rsp0=0x%x\n",
			tag, ret, st_req, st_rsp);
		dump_interesting(tag, rsp, 128);
	} else {
		pr_info("qsee_fp: CUS %s ret=%d req0=0x%x rsp0=0x%x\n",
			tag, ret, st_req, st_rsp);
	}
	return ret;
}

/*
 * Android oracle: trustlet_path is **focal32** with sb_length **0x80040**.
 * Device PROBE/INIT run on that large session, not on fingerpr.
 * fingerpr is the larger algorithm TA (match/enroll) — secondary.
 *
 * Config SYNC (0x100d) carries a NUL-terminated text/JSON blob (strlen+1)
 * at payload @0x10 (ff_trustlet_sync_config).
 */
/* Built at runtime so spi_bus_num / enable_spidev are module params.
 * Buffer declared near spi_image_recover (used for CAP-time mode cycle).
 */
static void f32_config_build(void)
{
	/*
	 * Config text for FF_CMD_TA_SYNC_CONFIG.
	 * Quoted on/off JSON → "unexpected char". Unquoted true/false JSON
	 * previously logged "configurations is synced" but keys still missing
	 * at INIT_SPI (Null pointer). Also try key=value lines (cfg_style=1).
	 */
	if (cfg_style == 1) {
		/* key = value */
		scnprintf(f32_config_text, sizeof(f32_config_text),
			  "driver.enable_spidev = %s\n"
			  "driver.spi_bus_num = %d\n"
			  "driver.spi_c_s_num = 0\n"
			  "driver.spi_on_demand = true\n"
			  "device.spi_default_bps = %d\n"
			  "device.spi_mode = %d\n"
			  "device.spi_capture_bps = %d\n"
			  "device.vio_is_1p8 = true\n"
			  "diagnosis.framework_log_level = 3\n",
			  enable_spidev ? "true" : "false",
			  spi_bus_num, spi_default_bps, spi_mode, spi_capture_bps);
	} else if (cfg_style == 2) {
		/* Android logcat-style "key   : value" lines */
		scnprintf(f32_config_text, sizeof(f32_config_text),
			  "driver.enable_spidev                    : %s\n"
			  "driver.spi_bus_num                      : %d\n"
			  "driver.spi_c_s_num                      : 0\n"
			  "driver.spi_on_demand                    : on\n"
			  "device.preferred_device_id               : 0x9391\n"
			  "device.spi_default_bps                  : 4000000\n"
			  "device.vio_is_1p8                       : on\n"
			  "device.enable_re_power_b4_probing       : on\n"
			  "diagnosis.enable_logcat_trustlet        : on\n"
			  "diagnosis.enable_trustlet_native_log    : on\n"
			  "diagnosis.framework_log_level           : 3\n",
			  enable_spidev ? "off" : "off",
			  spi_bus_num);
	} else if (cfg_style == 0) {
		/* Flat dotted JSON — TA says "synced" but SPI still defaults bus 0 */
		scnprintf(f32_config_text, sizeof(f32_config_text),
			  "{"
			  "\"driver.enable_spidev\":%s,"
			  "\"driver.spi_bus_num\":%d,"
			  "\"driver.spi_c_s_num\":0,"
			  "\"driver.spi_on_demand\":true,"
			  "\"device.preferred_device_id\":37841,"
			  "\"device.spi_default_bps\":%d,"
			  "\"device.spi_mode\":%d,"
			  "\"device.spi_capture_bps\":%d,"
			  "\"device.vio_is_1p8\":true,"
			  "\"device.enable_re_power_b4_probing\":true,"
			  "\"diagnosis.enable_logcat_trustlet\":true,"
			  "\"diagnosis.enable_trustlet_native_log\":true,"
			  "\"diagnosis.framework_log_level\":3"
			  "}",
			  enable_spidev ? "true" : "false",
			  spi_bus_num, spi_default_bps, spi_mode, spi_capture_bps);
	} else {
		/* Nested JSON (default cfg_style=3): SPI open + probe work.
		 * trusted_enrollment=false so ENROLL can proceed without KM HAT.
		 * Match Android oracle: image_processing_*=0 (auto), acquisition
		 * mode 0 (=polling-driven), burst_acquisition_mode=1, burst_num=3.
		 */
		/* M4bg: match Android device.* image path keys (prev/post hw process) */
		/*
		 * M4c0: do NOT inject extra keys as JSON bool when TA expects
		 * JSON_INTEGER (measured: expect JSON_INTEGER but got JSON_BOOL).
		 * Stick to Android-known key types only.
		 */
		scnprintf(f32_config_text, sizeof(f32_config_text),
			  "{\"driver\":{\"enable_spidev\":%s,\"spi_bus_num\":%d,"
			  "\"spi_c_s_num\":0,\"spi_on_demand\":true},"
			  "\"device\":{\"preferred_device_id\":37841,"
			  "\"spi_default_bps\":%d,\"spi_mode\":%d,"
			  "\"spi_capture_bps\":%d,"
			  "\"vio_is_1p8\":true,\"enable_re_power_b4_probing\":true,"
			  "\"enable_hw_reset_b4_scanning\":false,"
			  "\"enable_prev_hw_process\":true,"
			  "\"enable_post_hw_process\":true,"
			  /* M4bo: Android uses string "polling-driven" not integer 0 */
			  "\"image_acquisition_mode\":\"polling-driven\","
			  "\"burst_acquisition_num\":3,"
			  "\"burst_acquisition_mode\":1},"
			  /* HAL __svc_capture_image: factory.other_press_inte
			   * (coverage inte) and factory.dead_pixel_inte.
			   * Android enroll uses inte 128. other_press_inte=128
			   * alone did not log current inte times.
			   */
			  "\"factory\":{\"other_press_inte\":128,\"dead_pixel_inte\":128},"
			  "\"trustlet\":{\"enable_trusted_enrollment\":false,"
			  "\"enable_authenticate_token\":false},"
			  "\"common\":{\"max_enrolling_fingers\":5,"
			  "\"max_enrolling_samples\":20,"
			  "\"enrolling_break_after_n_samples\":0,"
			  "\"image_processing_cols\":0,"
			  "\"image_processing_rows\":0,"
			  /* Android persist_data_home is /vendor/focaltech.
			   * REE /mnt/persist/qti_fp in JSON split SFS from
			   * SET_GROUP /data/vendor_de/0/fpdata. Omit
			   * default_data_home (Android uses default).
			   */
			  "\"persist_data_home\":\"/vendor/focaltech\"},"
			  "\"algorithm\":{\"min_enrolling_quality_threshold\":40,"
			  "\"min_enrolling_coverage_threshold\":70,"
			  "\"enrolling_overlap_intervals\":\"0x54A0\","
			  "\"enable_duplicated_finger_checking\":true,"
			  "\"min_identify_quality_threshold\":40,"
			  "\"min_identify_coverage_threshold\":70},"
			  /* M4bw/M4c0: fw=0 skips Step1 entirely (measured); keep fw>=3 for recovery */
			  /*
			 * focal32 stores this integer as the logger threshold.
			 * A successful ff_file_open logs only level 1
			 * "%s:%u, '%s' leave." and that line is dropped while the
			 * threshold is above 1. Integer 1 on extra f4b85fde shrank
			 * the declared response to 0x12a; the wide scan reads the
			 * rest of the response ion. The failure format
			 * is the only qsee_sfs_open text. Do not synthesize one.
			 * The string "info" drowned ENROLL residual.
			 */
			  "\"diagnosis\":{\"framework_log_level\":1,"
			  "\"firmware_log_level\":3,"
			  "\"enable_logcat_trustlet\":true,"
			  "\"enable_trustlet_native_log\":%s,"
			  "\"enable_logcat_spi_data\":%s}}",
			  enable_spidev ? "true" : "false",
			  spi_bus_num, spi_default_bps, spi_mode, spi_capture_bps,
			  diag_native_log ? "true" : "false",
			  diag_spi_data ? "true" : "false");
	}
}

static void probe_device_ta(u32 id_dev, u32 id_aux)
{
	u8 buf[256];
	u8 pay1[4] = { 0, 0, 0, 0 };
	struct {
		u32 cmd;
		u32 plen;
		const char *name;
		const void *pay;
	} seq[16];
	unsigned int n = 0, i;
	struct qcom_tzmem_pool *pool;
	struct qcom_tzmem_pool_config cfg = {
		.initial_size = SESS_BIG + SZ_1M,
		.policy = QCOM_TZMEM_POLICY_ON_DEMAND,
		.increment = SZ_1M,
		.max_size = SESS_BIG + SZ_4M,
	};
	void *sess;
	size_t cfg_len;

	f32_config_build();
	cfg_len = strlen(f32_config_text) + 1; /* includes NUL */

	pr_info("qsee_fp: ===== DEVICE TA id=%u aux=%u ion 0x%x/0x%x cfg_len=%zu spi_bus=%d spidev=%d mode=%d bps=%d/%d =====\n",
		id_dev, id_aux, REQ_ION, RSP_ION, cfg_len, spi_bus_num, enable_spidev,
		spi_mode, spi_default_bps, spi_capture_bps);

	/* Optional small-buffer base/entry (Android uses a second handle) */
	if (id_aux) {
		memset(buf, 0, sizeof(buf));
		*(__le32 *)buf = cpu_to_le32(FF_SVC_GET_VERSION);
		*((__le32 *)buf + 1) = cpu_to_le32(0x2);
		send_sep(id_aux, "aux-base-205", buf, 8, REQ_SMALL, RSP_SMALL);
		msleep(20);
		memset(buf, 0, sizeof(buf));
		*(__le32 *)buf = cpu_to_le32(FF_TA_SET_KM_KEY);
		send_sep(id_aux, "aux-entry-1011", buf, 4, REQ_SMALL, RSP_SMALL);
		msleep(20);
	}

	pool = qcom_tzmem_pool_new(&cfg);
	if (IS_ERR(pool)) {
		pr_err("qsee_fp: sess pool %ld\n", PTR_ERR(pool));
		return;
	}
	sess = qcom_tzmem_alloc(pool, SESS_BIG, GFP_KERNEL);
	if (!sess) {
		qcom_tzmem_pool_free(pool);
		return;
	}
	pr_info("qsee_fp: big sess pa=0x%llx\n", (u64)qcom_tzmem_to_phys(sess));
	sess_shmbridge_register(sess);
	/*
	 * SFS opens ft_fp_serial_id.bin during INIT_DEV. Listeners only at
	 * SAVE is too late: SET_GROUP then -2 with no store I/O. Android
	 * qseecomd is up before the HAL starts the TA. skip_enroll (no
	 * finger) must register here too, or the open is unscored.
	 */
	pr_info("qsee_fp: overlay listeners before INIT_DEV (SFS serial) skip_enroll=%d\n",
		skip_enroll);
	listeners_register_enabled("before INIT_DEV");

	/* base/entry on device TA (ion); version wants subcmd=2 payload */
	{
		u8 ver_pay[4] = { 2, 0, 0, 0 };

		send_ion(id_dev, sess, SESS_BIG, FF_SVC_GET_VERSION, 4, ver_pay, "dev-base-205");
	}
	send_ion(id_dev, sess, SESS_BIG, FF_TA_SET_KM_KEY, 0, NULL, "dev-entry-1011");

	/*
	 * M4dk: layout0/1 × fdt/img matrix at entry (before SPI/PROBE/Base).
	 * M4dj: all layout1 OOM at entry; L0 may still soft (M4s path).
	 */
	if (early_cap_stages && skip_capture_path && !skip_enroll)
		m4dk_matrix(id_dev, sess, "post-entry");

	/*
	 * Android: attach → SYNC_CFG → PROBE (SPI open inside probe).
	 * CREATE (0x1001) returns err=0xffffff35 and breaks later APP_SEND (-22).
	 * Critical: if SYNC does not install driver.spi_bus_num, INIT_SPI defaults
	 * to QSEE_SPI_DEVICE_1 / qsee_spi_open(0) instead of bus 14.
	 */
	seq[n].cmd = 0x100d; seq[n].plen = (u32)cfg_len; seq[n].name = "SYNC_CFG";
	seq[n].pay = f32_config_text; n++;
	if (try_create) {
		/* May return soft err 0xffffff35; watch for SCM -22 after */
		seq[n].cmd = FF_TA_CREATE; seq[n].plen = 0; seq[n].name = "CREATE";
		seq[n].pay = NULL; n++;
	}
	seq[n].cmd = FF_TA_INIT_SPI; seq[n].plen = 0; seq[n].name = "INIT_SPI"; seq[n].pay = NULL; n++;
	seq[n].cmd = FF_TA_PROBE_DEVICE; seq[n].plen = 1; seq[n].name = "PROBE1"; seq[n].pay = pay1; n++;
	seq[n].cmd = FF_TA_PROBE_DEVICE; seq[n].plen = 1; seq[n].name = "PROBE2"; seq[n].pay = pay1; n++;
	/*
	 * M4dt: reattach (do_start=0) must not re-run INIT_DEV Step1–3.
	 * M4du: after SHUTDOWN+START the leftover INIT buffers are gone;
	 * skip INIT so under-finger CAPTURE is the first 112×88 array_new.
	 */
	if (skip_init_dev) {
		pr_info("qsee_fp: skip INIT_DEV (skip_init_dev=1 do_start=%d shutdown=%d)\n",
			do_start, shutdown_before_start);
		g_img_base_ok = !do_start && !shutdown_before_start;
	} else {
	seq[n].cmd = FF_TA_INIT_DEVICE; seq[n].plen = 0; seq[n].name = "INIT_DEV"; seq[n].pay = NULL; n++;
	/* Allow SPI FDT/img calibration time (Android does multi-try DAC search) */
	/* LOGCAT 0x1003 may flush more TA log after INIT_DEV */
	seq[n].cmd = 0x1003; seq[n].plen = 0; seq[n].name = "LOGCAT1"; seq[n].pay = NULL; n++;
	/* CHIP before full INIT — Android inits AFE then algorithm */
	seq[n].cmd = FF_TA_CHIP_PARAM_INIT; seq[n].plen = 0; seq[n].name = "CHIP_INIT"; seq[n].pay = NULL; n++;
	seq[n].cmd = FF_TA_INIT; seq[n].plen = 0; seq[n].name = "INIT"; seq[n].pay = NULL; n++;
	seq[n].cmd = 0x1003; seq[n].plen = 0; seq[n].name = "LOGCAT2"; seq[n].pay = NULL; n++;
	/* Calib/stats sync — Android FDT/img calibration + efc */
	seq[n].cmd = FF_TA_SYNC_CALIB; seq[n].plen = 0; seq[n].name = "SYNC_CALIB"; seq[n].pay = NULL; n++;
	/* M4db: skip SYNC_STATS on enroll/skip_capture — less post-Base thrash */
	if (!(skip_capture_path && !skip_enroll)) {
		seq[n].cmd = FF_TA_SYNC_STATS; seq[n].plen = 0;
		seq[n].name = "SYNC_STATS"; seq[n].pay = NULL; n++;
	}
	/*
	 * M4db: DEAD_PIXEL/FP_IS_TYPE after Base allocate image path and
	 * leave heap empty for CAP array_new (default off).
	 */
	if (init_dead_pixel) {
		seq[n].cmd = 0x1020; seq[n].plen = 0; seq[n].name = "DEAD_PIXEL";
		seq[n].pay = NULL; n++;
		seq[n].cmd = 0x1021; seq[n].plen = 4; seq[n].name = "FP_IS_TYPE";
		seq[n].pay = pay1; n++;
	} else {
		pr_info("qsee_fp: M4db skip INIT DEAD_PIXEL/FP_IS_TYPE (heap)\n");
	}
	}
	seq[n].cmd = FF_TA_CHECK_HEALTH; seq[n].plen = 4; seq[n].name = "HEALTH"; seq[n].pay = pay1; n++;

	serial_outcome_logged = false;
	for (i = 0; i < n; i++) {
		char tag[48];

		scnprintf(tag, sizeof(tag), "dev-%s-%x", seq[i].name, seq[i].cmd);
		{
			unsigned int fs_before = fs_listener_reqs;
			unsigned int rpmb_before = rpmb_listener_reqs;
			unsigned int time_before = time_listener_reqs;
			unsigned int ssd_before = ssd_listener_reqs;

			send_ion(id_dev, sess, SESS_BIG, seq[i].cmd, seq[i].plen,
				 seq[i].pay, tag);
			if (seq[i].cmd == FF_TA_INIT_DEVICE) {
				pr_info("qsee_fp: serial-open listeners fs=%u rpmb=%u time=%u ssd=%u\n",
					fs_listener_reqs - fs_before,
					rpmb_listener_reqs - rpmb_before,
					time_listener_reqs - time_before,
					ssd_listener_reqs - ssd_before);
				if (last_sfs_open_txt[0]) {
					pr_info("qsee_fp: serial-open outcome: %s\n",
						last_sfs_open_txt);
					serial_outcome_logged = true;
				} else {
					pr_info("qsee_fp: serial-open outcome: absent\n");
				}
			} else if (last_sfs_open_txt[0] && !serial_outcome_logged) {
				pr_info("qsee_fp: serial-open outcome: %s\n",
					last_sfs_open_txt);
				serial_outcome_logged = true;
			}
			if (seq[i].cmd == FF_TA_INIT_DEVICE &&
			    last_send_sfs_generic &&
			    fs_listener_reqs == fs_before)
				pr_info("qsee_fp: no filesystem request arrived before SFS_ERROR_GENERIC\n");
			if (seq[i].cmd == FF_TA_INIT_DEVICE &&
			    last_send_sfs_generic &&
			    rpmb_listener_reqs == rpmb_before)
				pr_info("qsee_fp: no RPMB request arrived before SFS_ERROR_GENERIC\n");
			if (seq[i].cmd == FF_TA_INIT_DEVICE &&
			    last_send_sfs_generic &&
			    rpmb_listener_reqs != rpmb_before)
				pr_info("qsee_fp: RPMB requests during serial open: %u\n",
					rpmb_listener_reqs - rpmb_before);
		}
		/*
		 * m4hq: Android INIT logs "support max image inte times at
		 * current spi speed(8.03M): 200". We SET_SPI 8.03M only after
		 * ENROLL, so INIT_DEV computed inte at 4M. Set capture speed
		 * after INIT_SPI so INIT_DEV sees 8.03M.
		 */
		if (cap_hal_overlay && seq[i].cmd == FF_TA_INIT_SPI) {
			u8 *susr = (u8 *)sess + 0x10;
			u32 spd = spi_capture_bps > 0 ? (u32)spi_capture_bps :
							  8030000u;

			*(__le32 *)susr = cpu_to_le32(spd);
			pr_info("qsee_fp: overlay SET_SPI after INIT_SPI spd=%u (inte at 8.03M)\n",
				spd);
			send_ion(id_dev, sess, SESS_BIG, FF_TA_SET_SPI_SPEED, 4, susr,
				 "dev-SPI-SPEED-post-initspi");
			msleep(15);
		}
		/*
		 * After PROBE: if SPI mode failed (chip_id shifted), VDD + speed
		 * then re-SYNC_CFG cycling device.spi_mode 0..3 (M4ax). Android
		 * default is 0; shifted chip_id means CPOL/CPHA mismatch for
		 * image-path SPI even when SN/FDT soft-succeed.
		 * No pinmux (hangs this unit).
		 */
		if (seq[i].cmd == FF_TA_PROBE_DEVICE && last_send_spi_mode_fail &&
		    !strcmp(seq[i].name, "PROBE2")) {
			u8 spd_pay[4];
			u8 *usr = (u8 *)sess + 0x10;
			int mode, saved_mode = spi_mode;
			size_t clen;

			pr_info("qsee_fp: SPI mode fail after PROBE — VDD+speed+mode cycle\n");
			l2_vdd_cycle("spi-mode-probe");
			*(__le32 *)spd_pay = cpu_to_le32(spi_default_bps);
			memcpy(usr, spd_pay, 4);
			send_ion(id_dev, sess, SESS_BIG, FF_TA_SET_SPI_SPEED, 4, usr,
				 "dev-SPI-SPEED-recover");
			msleep(20);
			for (mode = 0; mode < 4; mode++) {
				spi_mode = mode;
				f32_config_build();
				clen = strlen(f32_config_text) + 1;
				pr_info("qsee_fp: re-SYNC spi_mode=%d cfg_len=%zu\n",
					mode, clen);
				send_ion(id_dev, sess, SESS_BIG, FF_TA_SYNC_CFG, (u32)clen,
					 f32_config_text, "dev-SYNC-mode-cycle");
				msleep(20);
				send_ion(id_dev, sess, SESS_BIG, FF_TA_INIT_SPI, 0, NULL,
					 "dev-INIT_SPI-mode-cycle");
				msleep(20);
				send_ion(id_dev, sess, SESS_BIG, FF_TA_PROBE_DEVICE, 1,
					 pay1, "dev-PROBE-mode-cycle");
				if (!last_send_spi_mode_fail) {
					pr_info("qsee_fp: *** SPI mode cycle OK mode=%d ***\n",
						mode);
					break;
				}
			}
			if (last_send_spi_mode_fail) {
				spi_mode = saved_mode;
				pr_info("qsee_fp: SPI mode still failed after cycle (kept %d)\n",
					saved_mode);
			}
		}
		/* INIT_DEV may run FDT/img calib over SPI; give it wall time */
		if (seq[i].cmd == FF_TA_INIT_DEVICE) {
			/*
			 * M4cr: one LOGCAT drain + ion scan (was ×4; TA log thrash
			 * may leave heap tight before under-finger CAPTURE).
			 * Android INIT logs Base once then SLEEP — no multi-drain.
			 */
			msleep(500);
			send_ion(id_dev, sess, SESS_BIG, 0x1003, 0, NULL, "dev-LOGCAT-post-initdev");
			scan_ion_img_markers(sess, "post-INIT_DEV");
			if (last_send_base_update || g_img_base_ok) {
				g_img_base_ok = true;
				pr_info("qsee_fp: *** Base update after INIT_DEV ***\n");
				/*
				 * M4dk: CAP matrix immediately after Step3/Base —
				 * calib just malloc'd successfully; CAPTURE may still
				 * OOM (path paradox from M4dj).
				 */
				if (early_cap_stages && skip_capture_path &&
				    !skip_enroll) {
					u8 hpay[4] = { 0 };

					send_ion(id_dev, sess, SESS_BIG,
						 FF_TA_CHECK_HEALTH, 4, hpay,
						 "dev-HEALTH-pre-m4dk-base");
					m4dk_matrix(id_dev, sess, "post-Base");
				} else if (heap_probe && skip_capture_path &&
					   !skip_enroll) {
					m4dd_early_ran = true;
					m4dd_early_oom = m4dd_probe_28(
						id_dev, sess, "early-INIT_DEV");
				}
				if (slim_post_base && skip_capture_path &&
				    !skip_enroll) {
					u8 hpay[4] = { 0 };

					pr_info("qsee_fp: M4dd slim_post_base — skip CHIP/INIT/DEAD after Base\n");
					send_ion(id_dev, sess, SESS_BIG,
						 FF_TA_CHECK_HEALTH, 4, hpay,
						 "dev-HEALTH-slim-post-base");
					msleep(50);
					break;
				}
			}
			msleep(100);
		} else if (seq[i].cmd == FF_TA_INIT)
			msleep(200);
		else
			msleep(15);
		cond_resched();
	}

	/*
	 * Do NOT re-PROBE with alternate ids after INIT_DEV — wrong ids yield
	 * "got undesired device id: 0x0000" and later SCAN fails with
	 * "Device not found", breaking CAPTURE even when SN was read.
	 */

	/*
	 * M4bh/M4bi: force Android img Step1-3 / Base if INIT_DEV skipped it.
	 * M4cr: on enroll+skip_capture, NEVER force-calib (re-Step1 + LOGCAT
	 * thrash) — Android INIT Base once then under-finger CAP does not
	 * re-log Step1. Force only when base missing and not enroll path.
	 */
	if (g_img_base_ok || last_send_base_update) {
		g_img_base_ok = true;
		pr_info("qsee_fp: Base update already seen — skip force img calib\n");
		/*
		 * M4dd slim: extra SYNC_CALIB after Base re-touches calib path and
		 * may re-alloc. Skip on enroll+skip_capture when slim_post_base.
		 */
		if (slim_post_base && skip_capture_path && !skip_enroll)
			pr_info("qsee_fp: M4dd skip SYNC_CALIB-post-base (slim)\n");
		else
			send_ion(id_dev, sess, SESS_BIG, FF_TA_SYNC_CALIB, 0, NULL,
				 "dev-SYNC_CALIB-post-base");
	} else if (skip_init_dev) {
		pr_info("qsee_fp: M4du skip force calib / SYNC_CALIB (no INIT)\n");
	} else if (skip_capture_path && !skip_enroll) {
		pr_info("qsee_fp: M4cr enroll — skip force img calib (no base yet; INIT only)\n");
		send_ion(id_dev, sess, SESS_BIG, FF_TA_SYNC_CALIB, 0, NULL,
			 "dev-SYNC_CALIB-enroll-nobase");
	} else {
		(void)try_force_img_base_calib(id_dev, sess);
	}

	/*
	 * M4cs: Android after INIT goes SLEEP then later FDT_DOWN.
	 * M4df: if warm_base_nosleep, **skip SLEEP** so Step3 raw / image arrays
	 * may still be live for under-finger CAP reuse (no second array_new).
	 * M4dd proved heap dead after SLEEP at Base alone.
	 */
	if (!skip_enroll || skip_capture_path) {
		/* Overlay enroll attach: skip SLEEP (m4gq SLEEP+WMODE still
		 * no auto fdt; residual SLEEP). skip_enroll overlay=0 INIT
		 * still SLEEPs. Overlay idle skip_enroll keeps Base warm.
		 */
		bool nosleep = warm_base_nosleep && skip_capture_path &&
			       g_img_base_ok && cap_hal_overlay;

		if (nosleep) {
			pr_info("qsee_fp: M4df skip SLEEP after Base (keep image buffers warm)\n");
			msleep(20);
		} else {
			u8 wm_sleep[4];

			memset(wm_sleep, 0, sizeof(wm_sleep));
			*(__le32 *)wm_sleep = cpu_to_le32(FF_WMODE_SLEEP);
			send_ion(id_dev, sess, SESS_BIG, FF_TA_CFG_WORK_MODE, 4,
				 wm_sleep, "dev-WMODE-SLEEP-post-base");
			pr_info("qsee_fp: M4cs SLEEP after base (Android init end)\n");
			msleep(20);
		}
		/*
		 * M4da: HEALTH after Base — drop transient TA state. On M4df
		 * warm path still send HEALTH (does not SLEEP).
		 */
		if (g_img_base_ok) {
			u8 hpay[4] = { 0 };

			send_ion(id_dev, sess, SESS_BIG, FF_TA_CHECK_HEALTH, 4, hpay,
				 "dev-HEALTH-post-base-free");
			pr_info("qsee_fp: M4da HEALTH after Base (heap free try nosleep=%d)\n",
				nosleep);
			msleep(10);
		}
		/*
		 * M4dc/M4dd late probe: only if heap_probe=1 (M4df default off).
		 */
		if (g_img_base_ok && heap_probe) {
			bool late_oom;

			pr_info("qsee_fp: M4dd late probe begin (post-Base, slim=%d early_ran=%d early_oom=%d nosleep=%d)\n",
				slim_post_base, m4dd_early_ran, m4dd_early_oom,
				nosleep);
			late_oom = m4dd_probe_28(id_dev, sess,
						 nosleep ? "late-warm" : "late-post-SLEEP");
			if (m4dd_early_ran) {
				if (m4dd_early_oom && late_oom)
					pr_info("qsee_fp: M4dd verdict: heap dead at INIT_DEV Base alone\n");
				else if (!m4dd_early_oom && late_oom)
					pr_info("qsee_fp: M4dd verdict: heap died after Base (post-INIT thrash)\n");
				else if (!m4dd_early_oom && !late_oom)
					pr_info("qsee_fp: M4dd verdict: heap alive through post-Base\n");
				else
					pr_info("qsee_fp: M4dd verdict: early OOM late alive (unexpected)\n");
			}
			if (!nosleep) {
				u8 wm_s[4];

				memset(wm_s, 0, sizeof(wm_s));
				*(__le32 *)wm_s = cpu_to_le32(FF_WMODE_SLEEP);
				send_ion(id_dev, sess, SESS_BIG, FF_TA_CFG_WORK_MODE,
					 4, wm_s, "dev-WMODE-SLEEP-post-m4dd");
			}
			pr_info("qsee_fp: M4dd late probe done\n");
		} else if (g_img_base_ok) {
			pr_info("qsee_fp: M4df skip late heap probe (warm path)\n");
		}
	}

	/*
	 * M4dh: recycle TA after first Base, then **full** re-init (M4dg light
	 * attach died: vio fail → sticky Null → SCM -22). On the new instance:
	 *   pre-INIT_DEV 28×24  → heap state before Base
	 *   INIT_DEV Base
	 *   post-INIT_DEV 28×24 → heap state after Base
	 * Then SLEEP + continue sample path (must stay alive for PRESS).
	 */
	if (ta_reload_after_base && g_img_base_ok && skip_capture_path &&
	    !skip_enroll) {
		u32 old_id = id_dev, new_id = 0;
		int r;
		u8 hpay[4] = { 0 };
		u8 wm_sleep[4];
		u8 ver_pay[4] = { 2, 0, 0, 0 };
		size_t clen;
		bool post_oom = true;
		bool pre_ran = false, post_ran = false;

		pr_info("qsee_fp: M4dh reload begin (SHUTDOWN id=%u then full re-INIT)\n",
			old_id);
		send_ion(id_dev, sess, SESS_BIG, FF_TA_SYNC_CALIB, 0, NULL,
			 "dev-SYNC_CALIB-pre-reload");
		msleep(30);
		memset(wm_sleep, 0, sizeof(wm_sleep));
		*(__le32 *)wm_sleep = cpu_to_le32(FF_WMODE_SLEEP);
		send_ion(id_dev, sess, SESS_BIG, FF_TA_CFG_WORK_MODE, 4, wm_sleep,
			 "dev-WMODE-SLEEP-pre-reload");
		msleep(20);
		r = app_shutdown(old_id, "M4dh-reload");
		pr_info("qsee_fp: M4dh SHUTDOWN ret=%d\n", r);
		msleep(80);
		r = app_start("focal32", &new_id);
		if (r || !new_id) {
			pr_err("qsee_fp: M4dh START focal32 failed ret=%d\n", r);
		} else {
			id_dev = new_id;
			g_img_base_ok = false;
			last_send_base_update = false;
			memset(sess, 0, SESS_BIG);
			pr_info("qsee_fp: M4dh reloaded focal32 id=%u (was %u)\n",
				id_dev, old_id);

			/* L2 power — SHUTDOWN left chip cold; M4dg vio failed without this */
			l2_vdd_cycle("m4dh-reload");
			l2_reset_ic("m4dh-reload");
			msleep(30);

			send_ion(id_dev, sess, SESS_BIG, FF_SVC_GET_VERSION, 4,
				 ver_pay, "dev-base-205-reload");
			send_ion(id_dev, sess, SESS_BIG, FF_TA_SET_KM_KEY, 0, NULL,
				 "dev-entry-1011-reload");
			f32_config_build();
			clen = strlen(f32_config_text) + 1;
			send_ion(id_dev, sess, SESS_BIG, FF_TA_SYNC_CFG, (u32)clen,
				 f32_config_text, "dev-SYNC-reload");
			msleep(15);
			send_ion(id_dev, sess, SESS_BIG, FF_TA_INIT_SPI, 0, NULL,
				 "dev-INIT_SPI-reload");
			msleep(15);
			send_ion(id_dev, sess, SESS_BIG, FF_TA_PROBE_DEVICE, 1, pay1,
				 "dev-PROBE1-reload");
			msleep(15);
			send_ion(id_dev, sess, SESS_BIG, FF_TA_PROBE_DEVICE, 1, pay1,
				 "dev-PROBE2-reload");
			msleep(20);
			pr_info("qsee_fp: M4dh post-PROBE vio=%d spi_fail=%d\n",
				last_send_vio_ok, last_send_spi_mode_fail);

			/*
			 * M4di: FDT vs image ladder pre-INIT (only if vio OK).
			 * M4dh single 0x240 OOM was ambiguous — FDT soft may still work.
			 */
			if (last_send_vio_ok && !last_send_spi_mode_fail) {
				pre_ran = true;
				m4di_pre_init_ladder(id_dev, sess);
				send_ion(id_dev, sess, SESS_BIG, FF_TA_CHECK_HEALTH,
					 4, hpay, "dev-HEALTH-pre-initdev");
			} else {
				pr_info("qsee_fp: M4di skip pre-INIT ladder (vio/spi bad)\n");
			}

			/* Full INIT_DEV — restore Base + working device for samples */
			send_ion(id_dev, sess, SESS_BIG, FF_TA_INIT_DEVICE, 0, NULL,
				 "dev-INIT_DEV-reload");
			msleep(500);
			send_ion(id_dev, sess, SESS_BIG, 0x1003, 0, NULL,
				 "dev-LOGCAT-post-initdev-reload");
			scan_ion_img_markers(sess, "post-INIT_DEV-reload");
			if (last_send_base_update || g_img_base_ok) {
				g_img_base_ok = true;
				pr_info("qsee_fp: *** M4dh Base after INIT_DEV-reload ***\n");
			} else {
				pr_info("qsee_fp: M4dh no Base on reloaded INIT_DEV\n");
			}
			msleep(50);

			/*
			 * Post-Base: one image 28×24 only (ladder already ran pre).
			 * HEALTH first to clear any sticky from ladder.
			 */
			if (g_img_base_ok) {
				post_ran = true;
				send_ion(id_dev, sess, SESS_BIG, FF_TA_CHECK_HEALTH,
					 4, hpay, "dev-HEALTH-before-post-probe");
				post_oom = (m4di_probe_one(id_dev, sess, 1, 28, 24,
							   0x240, false,
							   "post-Base-img28") == -1);
				pr_info("qsee_fp: M4di post-Base img28 oom=%d base=1\n",
					post_oom);
			}

			if (pre_ran && post_ran)
				pr_info("qsee_fp: M4di done pre_ladder=1 post_oom=%d base=%d\n",
					post_oom, g_img_base_ok);
			else if (post_ran)
				pr_info("qsee_fp: M4di done post_oom=%d (no pre ladder)\n",
					post_oom);

			/* Android-style SLEEP after Base — samples re-arm FDT */
			memset(wm_sleep, 0, sizeof(wm_sleep));
			*(__le32 *)wm_sleep = cpu_to_le32(FF_WMODE_SLEEP);
			send_ion(id_dev, sess, SESS_BIG, FF_TA_CFG_WORK_MODE, 4,
				 wm_sleep, "dev-WMODE-SLEEP-post-m4dh");
			msleep(20);
			send_ion(id_dev, sess, SESS_BIG, FF_TA_CHECK_HEALTH, 4, hpay,
				 "dev-HEALTH-m4dh-ready");
			pr_info("qsee_fp: M4di reload done id=%u base=%d pre_ladder=%d post_oom=%d\n",
				id_dev, g_img_base_ok, pre_ran, post_oom);
		}
	}

	/*
	 * Auth-path experiment (after SPI open + device init).
	 * SET_ACTIVE_GROUP + work modes + AUTHENTICATE.
	 * Templates may still be missing without Android SFS data.
	 */
	{
		/* Room for path strings; HAT uses separate hat[0x50] below */
		u8 apay[128];

		/*
		 * Register skip_capture npaths=1 must be Android fpdata
		 * (SET_GROUP namespace key). /var/lib/fp5-fp first made
		 * post-SAVE persist SET_GROUP hit empty store -2.
		 */
		static const char *const gpaths[] = {
			"/data/vendor_de/0/fpdata",
			"/var/lib/fp5-fp",
			"/data/vendor/focaltech",
		};
		unsigned int pi;
		/*
		 * M4db: skip SET_GROUP entirely when defer_enroll+skip_capture —
		 * SFS path probes allocate before CAP for no benefit.
		 */
		unsigned int npaths = (skip_capture_path && !skip_enroll &&
				       defer_enroll) ?
					      0u :
					      ((skip_capture_path && !skip_enroll) ?
						       1u :
						       ARRAY_SIZE(gpaths));

		if (npaths == 0)
			pr_info("qsee_fp: M4db skip SET_GROUP (defer enroll heap)\n");

		for (pi = 0; pi < npaths; pi++) {
			const char *gpath = gpaths[pi];
			size_t glen = strlen(gpath) + 1;
			char tag[40];

			memset(apay, 0, sizeof(apay));
			*(__le32 *)apay = cpu_to_le32(0); /* gid */
			if (4 + glen > sizeof(apay))
				continue;
			memcpy(apay + 4, gpath, glen);
			scnprintf(tag, sizeof(tag), "dev-SET_GROUP-%u", pi);
			send_ion(id_dev, sess, SESS_BIG, FF_TA_SET_ACTIVE_GROUP,
				 4 + (u32)glen, apay, tag);
			msleep(30);
		}

		if (!(skip_capture_path && !skip_enroll)) {
			send_ion(id_dev, sess, SESS_BIG, FF_TA_ENUMERATE, 0, NULL,
				 "dev-ENUM");
			msleep(10);
			send_ion(id_dev, sess, SESS_BIG, FF_TA_GET_AUTH_ID, 0, NULL,
				 "dev-AUTH_ID");
			msleep(10);
		} else {
			pr_info("qsee_fp: overlay ENUM after SET_GROUP namespace (load/create SFS)\n");
			send_ion(id_dev, sess, SESS_BIG, FF_TA_ENUMERATE, 0, NULL,
				 "dev-ENUM-pre-enroll");
		}

		/* START_SCANNING err=0 previously; then AUTH on device TA */
		if (cap_hal_overlay) {
			pr_info("qsee_fp: M4dy skip START_SCANNING before overlay CAP\n");
		} else {
			memset(apay, 0, 0x30);
			send_ion(id_dev, sess, SESS_BIG, FF_TA_START_SCANNING, 0x30, apay,
				 "dev-SCAN");
			msleep(20);
		}
		if (cap_hal_overlay) {
			pr_info("qsee_fp: overlay skip QEVENT pre-enroll\n");
		} else {
			send_ion(id_dev, sess, SESS_BIG, FF_TA_QUERY_EVENT, 4, pay1,
				 "dev-QEVENT");
			msleep(10);
		}

		/*
		 * M4m capture path (default): keep TA alive; do NOT call ENROLL.
		 * CAPTURE_IMAGE 0x1013 plen 0x24 from HAL constant pool.
		 * Optional: PRE_ENROLL only (safe) to re-confirm challenge.
		 */
		{
			u8 cap[0x30];
			u8 wm[4];
			int er;
			u32 last_err;
			bool fdt_ok = false;
			bool finger_seen = false;
			u64 pre_enroll_challenge = 0;
			unsigned int pass;
			/* One FDT pass is enough for criterion 2; extra passes age SPI
			 * and correlate with intermittent "vio 1.8v fail" (ESD).
			 * Use 1 when enrolling; allow 2 for capture-only diagnostics.
			 */
			unsigned int max_pass = skip_enroll ? 2 : 1;

			/* PRE_ENROLL is safe (does not kill TA) */
			er = send_ion(id_dev, sess, SESS_BIG, FF_TA_PRE_ENROLL_HL, 0, NULL,
				      "dev-PRE_ENROLL");
			{
				u8 *reqp = sess;
				u64 ch = le64_to_cpu(*(__le64 *)(reqp + 0x10));

				pre_enroll_challenge = ch;
				pr_info("qsee_fp: PRE_ENROLL er=%d challenge=0x%llx skip_enroll=%d\n",
					er, ch, skip_enroll);
			}
			msleep(20);

			/*
			 * M4bb: enroll path may skip the multi-flag CAPTURE thrash that
			 * produces configure spi mode failed + itype RST before samples.
			 */
			if (skip_capture_path) {
				u8 *usr = (u8 *)sess + 0x10;
				u32 spd;

				/*
				 * M4ci: do NOT soft-CAP here. Live holds showed
				 * dev-cap-skipcap → vio 1.8v fail + configure spi
				 * mode failed, then sticky spi-fail MARKs on QEV.
				 * FDT arm = PWR + WMODE + SCAN + SPI speed only.
				 */
				pr_info("qsee_fp: skip_capture_path — FDT arm only (no skipcap CAP; M4ci)\n");
				/* L2 VIO/clocks before TA power */
				{
					struct file *fv, *fs;
					loff_t p = 0;

					fv = filp_open("/sys/class/focaltech_fp/focaltech_fp/vdd",
						       O_WRONLY, 0);
					if (!IS_ERR_OR_NULL(fv)) {
						kernel_write(fv, "1\n", 2, &p);
						fput(fv);
					}
					p = 0;
					fs = filp_open("/sys/class/focaltech_fp/focaltech_fp/spiclk",
						       O_WRONLY, 0);
					if (!IS_ERR_OR_NULL(fs)) {
						kernel_write(fs, "1\n", 2, &p);
						fput(fs);
					}
					msleep(30);
				}
				if (cap_hal_overlay) {
					pr_info("qsee_fp: overlay skip PWR-skipcap\n");
				} else {
					memset(wm, 0, sizeof(wm));
					wm[0] = 1;
					send_ion(id_dev, sess, SESS_BIG, FF_TA_CFG_POWER_MODE, 4, wm,
						 "dev-PWR-skipcap");
					msleep(20);
				}
				if (cap_hal_overlay || skip_enroll) {
					pr_info("qsee_fp: skip WMODE/SCAN/SET_SPI-skipcap (no FDT 112/96/80)\n");
				} else {
					spd = spi_capture_bps > 0 ? (u32)spi_capture_bps :
								    4000000u;
					*(__le32 *)usr = cpu_to_le32(spd);
					send_ion(id_dev, sess, SESS_BIG, FF_TA_SET_SPI_SPEED, 4,
						 usr, "dev-SPI-SPEED-skipcap");
					msleep(15);
					(void)wait_chip_ready(id_dev, sess, "skipcap", 4);
				}
				/* Arm success without CAP — Base already from INIT */
				fdt_ok = true;
				pr_info("qsee_fp: skip_capture_path fdt_ok=%d spi_fail=%d vio=%d (no CAP)\n",
					fdt_ok, last_send_spi_mode_fail, last_send_vio_ok);
				/*
				 * M4cr: do not force_img_base here — already handled
				 * post-INIT. Extra force under enroll fragments TA heap
				 * then CAPTURE array_new OOMs (Android never re-forces).
				 */
				if (fdt_ok && g_img_base_ok)
					send_ion(id_dev, sess, SESS_BIG, FF_TA_SYNC_CALIB, 0,
						 NULL, "dev-SYNC_CALIB-skipcap");
				/*
				 * skip_capture jumps over pre-ENROLL listener
				 * register. SAVE rem=0 then had no FILE_SERVICE
				 * (post-probe ran after enroll). Android has FS
				 * up for the whole HAL session.
				 */
				pr_info("qsee_fp: overlay listeners skipcap-pre-enroll (SFS before SAVE)\n");
				listeners_register_enabled("skipcap-pre-enroll");
				goto cap_done;
			}

			/*
			 * Power + work-mode scan (compact): Android uses FDT_DOWN_DETECT.
			 * Modes 0-2 historically map to SLEEP; scan 0..15 for named switch.
			 */
			memset(wm, 0, sizeof(wm));
			wm[0] = 1;
			send_ion(id_dev, sess, SESS_BIG, FF_TA_CFG_POWER_MODE, 4, wm, "dev-PWR1");
			msleep(20);
			{
				/* Compact mode probe: full 0..15 is slow/noisy; for enroll
				 * path prefer 0,1,5 (FDT_DOWN) then SCAN.
				 */
				static const unsigned int wmodes[] = { 0, 1, 5, 9, 10 };
				unsigned int wi, wn = wait_finger_s > 0 ? 3 : ARRAY_SIZE(wmodes);

				for (wi = 0; wi < wn; wi++) {
					char wtag[28];
					unsigned int mode = wait_finger_s > 0 ? wmodes[wi] : wi;

					memset(wm, 0, sizeof(wm));
					*(__le32 *)wm = cpu_to_le32(mode);
					scnprintf(wtag, sizeof(wtag), "dev-WMODE-%u", mode);
					er = send_ion(id_dev, sess, SESS_BIG,
						      FF_TA_CFG_WORK_MODE, 4, wm, wtag);
					pr_info("qsee_fp: WMODE %u er=%d err=0x%x\n", mode, er,
						le32_to_cpu(*((__le32 *)sess + 2)));
					msleep(15);
				}
			}

			memset(cap, 0, sizeof(cap));
			er = send_ion(id_dev, sess, SESS_BIG, FF_TA_START_SCANNING, 0x30, cap,
				      "dev-SCAN2");
			pr_info("qsee_fp: SCAN2 er=%d\n", er);
			msleep(30);

			/*
			 * CAPTURE path — FDT runs during later CAPTURE when early
			 * attempts hit n>0/Null. After first err=0 + FDT log, re-CAP
			 * with large in-band image room and scan ion for non-a5 pixels.
			 * LOGCAT flushes the 810-byte TA window past FDT into img.
			 * M4ck: layout1 idle smoke skips multi-dim thrash → one soft CAP.
			 */
			if (cap_ctx_layout == 1 && idle_layout1_cap) {
				/*
				 * M4ck: skip multi-dim thrash; tiny layout1 CAP in
				 * try_idle_layout1_cap. Soft FDT via SCAN/WMODE already.
				 */
				fdt_ok = true;
				pr_info("qsee_fp: layout1 idle path — defer CAP to M4ck tiny\n");
				goto cap_done;
			}
			{
				const u32 img_off = 0x100;
				u8 *usr = (u8 *)sess + 0x10;
				u32 dims[][2] = { { 112, 88 }, { 96, 96 }, { 80, 80 } };

				/* Pass 0: zero-payload (HAL-style) — often triggers FDT */
				memset(cap, 0, sizeof(cap));
				er = send_ion(id_dev, sess, SESS_BIG, FF_TA_CAPTURE_IMAGE, 0x24,
					      cap, "dev-cap-zero");
				last_err = le32_to_cpu(*((__le32 *)sess + 2));
				pr_info("qsee_fp: cap-zero er=%d err=0x%x\n", er, last_err);
				msleep(50);
				send_ion(id_dev, sess, SESS_BIG, 0x1003, 0, NULL, "dev-LOGCAT-after-cap0");
				msleep(30);

				for (pass = 0; pass < max_pass; pass++) {
					unsigned int di;

					for (di = 0; di < ARRAY_SIZE(dims); di++) {
						char ctag[40];
						u32 cols = dims[di][0], rows = dims[di][1];
						u32 npix = cols * rows;
						u32 plen;
						u8 *p;
						u32 j, nz = 0, na5 = 0;

						/* meta-only CAPTURE */
						memset(sess, 0, min_t(size_t, SESS_BIG, 0x20000));
						memset(usr, 0, 0x100);
						*(__le32 *)(usr + 0) = cpu_to_le32(1);
						*(__le32 *)(usr + 4) = cpu_to_le32(cols);
						*(__le32 *)(usr + 8) = cpu_to_le32(rows);
						*(__le32 *)(usr + 0xc) = cpu_to_le32(img_off);
						*(__le32 *)(usr + 0x10) = cpu_to_le32(npix * 2);
						memset((u8 *)sess + 0x10 + img_off, 0xA5, 64);
						scnprintf(ctag, sizeof(ctag), "dev-cap-meta-p%u-%ux%u",
							  pass, cols, rows);
						er = send_ion(id_dev, sess, SESS_BIG,
							      FF_TA_CAPTURE_IMAGE, 0x24, usr, ctag);
						last_err = le32_to_cpu(*((__le32 *)sess + 2));
						pr_info("qsee_fp: %s er=%d err=0x%x img=%*ph\n", ctag,
							er, last_err, 16,
							(u8 *)sess + 0x10 + img_off);
						if (er == 0 && last_err == 0) {
							fdt_ok = true;
							pr_info("qsee_fp: CAPTURE soft-success (no Null) %s\n",
								ctag);
						}
						if (er == -22)
							goto cap_done;
						msleep(40);
						/* Drain TA log — look for img calib past FDT */
						send_ion(id_dev, sess, SESS_BIG, 0x1003, 0, NULL,
							 "dev-LOGCAT-post-fdt");
						msleep(20);

						/*
						 * M4ck: large plen CAP (0x24+img) sticky-Nulls
						 * later APP_SEND (M4ap). Default off.
						 */
						if (!cap_big_plen) {
							pr_info("qsee_fp: skip cap-big plen (cap_big_plen=0 M4ck)\n");
							continue;
						}
						plen = 0x24 + npix * 2;
						if (plen > 0x70000)
							continue;
						memset(usr, 0, plen);
						*(__le32 *)(usr + 0) = cpu_to_le32(1);
						*(__le32 *)(usr + 4) = cpu_to_le32(cols);
						*(__le32 *)(usr + 8) = cpu_to_le32(rows);
						*(__le32 *)(usr + 0xc) = cpu_to_le32(0x24);
						*(__le32 *)(usr + 0x10) = cpu_to_le32(npix * 2);
						p = usr + 0x24;
						memset(p, 0xA5, min_t(u32, npix * 2, 256));
						scnprintf(ctag, sizeof(ctag), "dev-cap-big-p%u-%ux%u",
							  pass, cols, rows);
						er = send_ion(id_dev, sess, SESS_BIG,
							      FF_TA_CAPTURE_IMAGE, plen, usr, ctag);
						last_err = le32_to_cpu(*((__le32 *)sess + 2));
						/* scan image area for real pixels */
						for (j = 0; j < min_t(u32, npix * 2, 4096); j++) {
							if (p[j] != 0xa5)
								na5++;
							if (p[j] && p[j] != 0xa5)
								nz++;
						}
						pr_info("qsee_fp: %s er=%d err=0x%x na5=%u nz=%u head=%*ph\n",
							ctag, er, last_err, na5, nz, 16, p);
						if (nz > 8) {
							pr_info("qsee_fp: CAPTURE IMAGE DATA nonzero %s\n",
								ctag);
							fdt_ok = true;
							goto cap_done;
						}
						if (er == 0 && last_err == 0)
							fdt_ok = true;
						if (er == -22)
							goto cap_done;
						msleep(40);
					}
					/* After a pass of FDT success, re-INIT algorithm path */
					if (fdt_ok) {
						send_ion(id_dev, sess, SESS_BIG, FF_TA_INIT, 0, NULL,
							 "dev-INIT-after-fdt");
						msleep(100);
						send_ion(id_dev, sess, SESS_BIG, FF_TA_SYNC_CALIB, 0,
							 NULL, "dev-SYNC_CALIB-after-fdt");
						msleep(50);
						send_ion(id_dev, sess, SESS_BIG, 0x1003, 0, NULL,
							 "dev-LOGCAT-after-reinit");
						msleep(30);
					}
				}
			}
cap_done:
			pr_info("qsee_fp: capture path done fdt_ok=%d skip_cap=%d layout=%d\n",
				fdt_ok, skip_capture_path, cap_ctx_layout);
			/*
			 * M4ck: controlled layout1 CAP smoke (idle) before any force thrash.
			 * Prefer this over multi-flag force when layout=1.
			 */
			if (idle_layout1_cap && fdt_ok &&
			    !(skip_capture_path && !skip_enroll) &&
			    !(skip_enroll && cap_hal_overlay))
				try_idle_layout1_cap(id_dev, sess);
			if (skip_enroll && cap_hal_overlay && !do_auth) {
				pr_info("qsee_fp: skip_enroll overlay idle CAP burst=3 oneshot (no ENROLL, no REPORT)\n");
				try_idle_native_cap(id_dev, sess, 3, true);
			} else if (idle_native_cap && skip_enroll)
				try_idle_native_cap(id_dev, sess, 1, false);
			/*
			 * M4bb: skip post-fdt force-image thrash when enrolling with
			 * skip_capture_path — reserve image SPI for finger samples.
			 * M4ck: skip multi-flag force when layout=1 (Null thrash).
			 */
			if (fdt_ok && !(skip_capture_path && !skip_enroll) &&
			    cap_ctx_layout != 1 && !idle_native_cap &&
			    !cap_hal_overlay) {
				u32 pnz = try_pull_image_ree_ta(id_dev, sess, "post-fdt");
				u32 fnz;

				pr_info("qsee_fp: post-fdt image-pull nz=%u\n", pnz);
				/* M4ai: forced CAPTURE/DEAD_PIXEL dual-layout image path */
				fnz = try_force_image_path(id_dev, sess, false, "post-fdt");
				if (fnz > pnz)
					pnz = fnz;
				pr_info("qsee_fp: post-fdt force-image nz=%u frame=%d sens=%d\n",
					pnz, last_send_frame_raw, last_send_sensor_raw);
				/* Feed harvested/force pixels before pure synthetic */
				if (pnz > 16)
					try_feed_image_enroll(id_dev, sess, false, true);
				else
					try_feed_synthetic_enroll(id_dev, sess, false);
			} else if (cap_ctx_layout == 1) {
				pr_info("qsee_fp: skip force-image thrash (layout=1 M4ck)\n");
			}
			msleep(20);

			/*
			 * Finger wait: poll L2 irq_count sysfs + QUERY_EVENT for down (0x2).
			 * Android enroll only proceeds on IRQ finger-down.
			 */
			if (wait_finger_s > 0 && fdt_ok) {
				unsigned int t;
				unsigned int irq0 = 0, irq_now = 0;
				struct file *firq = NULL;
				char irqbuf[32];
				loff_t pos;
				ssize_t nr;

				/* Put sensor in FDT_DOWN-ish scan: power + modes 0..8 then SCAN */
				memset(wm, 0, sizeof(wm));
				wm[0] = 1;
				send_ion(id_dev, sess, SESS_BIG, FF_TA_CFG_POWER_MODE, 4, wm,
					 "dev-PWR-fingerwait");
				for (t = 0; t <= 8; t++) {
					memset(wm, 0, sizeof(wm));
					*(__le32 *)wm = cpu_to_le32(t);
					send_ion(id_dev, sess, SESS_BIG, FF_TA_CFG_WORK_MODE, 4, wm,
						 "dev-WMODE-fw");
					msleep(10);
				}
				memset(cap, 0, sizeof(cap));
				send_ion(id_dev, sess, SESS_BIG, FF_TA_START_SCANNING, 0x30, cap,
					 "dev-SCAN-fingerwait");

				firq = filp_open("/sys/class/focaltech_fp/focaltech_fp/irq_count",
						 O_RDONLY, 0);
				if (!IS_ERR_OR_NULL(firq)) {
					pos = 0;
					nr = kernel_read(firq, irqbuf, sizeof(irqbuf) - 1, &pos);
					if (nr > 0) {
						irqbuf[nr] = 0;
						if (kstrtouint(irqbuf, 10, &irq0) == 0)
							pr_info("qsee_fp: fingerwait irq0=%u\n", irq0);
					}
				} else {
					firq = NULL;
				}
				pr_info("qsee_fp: waiting up to %ds for finger (IRQ/TA) FDT ok\n",
					wait_finger_s);
				/*
				 * Poll IRQ quietly. Require either:
				 *  - TA event 0x2 (finger DOWN), or
				 *  - IRQ delta >= 15 over the wait (real press; residual
				 *    FDT noise is ~0–3 total once TA scanning is settled).
				 * Do not exit on a few residual edges alone.
				 */
				for (t = 0; t < (unsigned int)wait_finger_s * 5; t++) {
					msleep(200);
					if (firq) {
						pos = 0;
						nr = kernel_read(firq, irqbuf, sizeof(irqbuf) - 1,
								 &pos);
						if (nr > 0) {
							irqbuf[nr] = 0;
							if (kstrtouint(irqbuf, 10, &irq_now) == 0 &&
							    irq_now >= irq0 + 15) {
								pr_info("qsee_fp: L2 IRQ sustained %u->%u finger\n",
									irq0, irq_now);
								finger_seen = true;
								irq0 = irq_now;
								break;
							}
						}
					}
					if ((t % 5) == 4) {
						send_ion(id_dev, sess, SESS_BIG, FF_TA_QUERY_EVENT,
							 4, pay1, "dev-QEVENT-fw");
						{
							u32 ev = le32_to_cpu(
								*(__le32 *)((u8 *)sess + 0x10));

							if (ev & 0x2) {
								pr_info("qsee_fp: FINGER DOWN ev=0x%x\n",
									ev);
								finger_seen = true;
								break;
							}
						}
						send_ion(id_dev, sess, SESS_BIG, FF_TA_QUERY_FINGER,
							 4, pay1, "dev-QFINGER-fw");
					}
					cond_resched();
				}
				if (firq)
					fput(firq);
				pr_info("qsee_fp: fingerwait done seen=%d\n", finger_seen);
			}

			if (cap_hal_overlay) {
				pr_info("qsee_fp: overlay skip QEVENT2 pre-enroll\n");
			} else {
				send_ion(id_dev, sess, SESS_BIG, FF_TA_QUERY_EVENT, 4, pay1,
					 "dev-QEVENT2");
				msleep(15);
			}
			if (cap_hal_overlay) {
				pr_info("qsee_fp: overlay skip QFINGER pre-enroll\n");
			} else {
				send_ion(id_dev, sess, SESS_BIG, FF_TA_QUERY_FINGER, 4, pay1,
					 "dev-QFINGER");
				msleep(15);
			}

			/* Liveness after capture path */
			if (cap_hal_overlay) {
				pr_info("qsee_fp: overlay skip HEALTH-post-cap\n");
				er = 0;
			} else {
				er = send_ion(id_dev, sess, SESS_BIG, FF_TA_CHECK_HEALTH, 4, pay1,
					      "dev-HEALTH-post-cap");
				pr_info("qsee_fp: HEALTH after capture path er=%d\n", er);
			}

			/*
			 * Android enroll protocol (from logcat oracle):
			 *   PRE_ENROLL → ENROLL once (new fid) → FDT_DOWN loop:
			 *     IRQ type 0x2 → CAPTURE (frame raw / transmit_image ~37ms)
			 *     → "enrolling... rem--" → finger UP → next DOWN.
			 * Never re-call ENROLL per sample (that mints a new fid each time
			 * and leaves AUTH as "enrollment is unexpected broken off").
			 */
			/*
			 * When enroll_force_cap, start ENROLL even if fingerwait missed
			 * — samples still require IRQ/ta_down for image (M4bc).
			 * With skip_capture_path, also proceed if fdt_ok so we do not
			 * need a pre-ENROLL finger hold window.
			 */
			if ((!skip_enroll || do_auth) &&
			    (!enroll_needs_finger || finger_seen || enroll_force_cap ||
			     (skip_capture_path && fdt_ok))) {
				u8 hat[0x50];
				u64 challenge;
				unsigned int sample;
				u8 *usr = (u8 *)sess + 0x10;
				bool rem_seen = false;
				bool ta_enrolling = false;
				u32 overlay_rem = 0xffffffffu;
				u32 overlay_rem_was = 20;
				unsigned int overlay_stall = 0;
				bool prev_overlay_empty = false;

				/*
				 * M4cv/M4db: never L2-reset pre-ENROLL when preserve_img_base.
				 */
				if (preserve_img_base) {
					pr_info("qsee_fp: M4db skip L2 reset pre-ENROLL (preserve base sticky=%d)\n",
						g_img_base_ok);
				} else {
					l2_reset_ic("pre-ENROLL-esd");
					msleep(40);
				}
				/*
				 * Soft ESD only if TA still answers QEV. M4db hold: after HEALTH
				 * ESD_HANDLE returned SCM -22 and killed sample banners.
				 * Overlay: skip this QEV (m4gz still 1-frame).
				 */
				if (cap_hal_overlay) {
					pr_info("qsee_fp: overlay skip QEV-pre-enroll-alive\n");
				} else {
					u8 esd_pay[4] = { 0, 0, 0, 0 };
					int qer;

					qer = send_ion(id_dev, sess, SESS_BIG, FF_TA_QUERY_EVENT,
						       4, pay1, "dev-QEV-pre-enroll-alive");
					if (qer == -22) {
						pr_err("qsee_fp: M4db TA dead before pre-ENROLL ESD — abort\n");
						goto enroll_done;
					}
					if (last_send_esd) {
						if (send_ion(id_dev, sess, SESS_BIG,
							     FF_TA_ESD_HANDLE, 4, esd_pay,
							     "dev-ESD-HANDLE-pre") == -22) {
							pr_err("qsee_fp: M4db ESD-pre -22 — abort\n");
							goto enroll_done;
						}
						msleep(30);
					} else {
						pr_info("qsee_fp: M4db skip ESD-pre (no sticky esd)\n");
					}
				}

				/*
				 * Android: one PRE_ENROLL → ENROLL (that challenge
				 * in the HAT). A second PRE_ENROLL mints a new
				 * challenge after capture-path ION clobber.
				 */
				if (cap_hal_overlay) {
					pr_info("qsee_fp: overlay skip PRE_ENROLL2\n");
					challenge = pre_enroll_challenge;
					/*
					 * m4hr: SET_SPI after INIT_SPI HIT, still
					 * img_scan_start raw=0. Listeners were only
					 * post-probe (after ENROLL). Android has FS/RPMB
					 * up for transmit_image. Register before ENROLL.
					 */
					pr_info("qsee_fp: overlay listeners pre-ENROLL (img_scan/SFS)\n");
					listeners_register_enabled("pre-ENROLL");
				} else {
					send_ion(id_dev, sess, SESS_BIG, FF_TA_PRE_ENROLL_HL, 0,
						 NULL, "dev-PRE_ENROLL2");
					challenge = le64_to_cpu(*(__le64 *)((u8 *)sess + 0x10));
				}
				pr_info("qsee_fp: ENROLL attempt finger=%d force_cap=%d challenge=0x%llx\n",
					finger_seen, enroll_force_cap, challenge);

				/*
				 * Do NOT re-INIT_SPI/PROBE here — SCM -22 TA death.
				 * Default: skip SYNC/PWR reassert (enroll_resync=0). Keep SPI
				 * session from last successful CAP; only light L2 clocks +
				 * WMODE 5. enroll_resync=1 restores old SYNC+PWR path.
				 */
				{
					struct file *fv, *fs;
					loff_t p = 0;

					fv = filp_open("/sys/class/focaltech_fp/focaltech_fp/vdd",
						       O_WRONLY, 0);
					if (!IS_ERR_OR_NULL(fv)) {
						kernel_write(fv, "1\n", 2, &p);
						fput(fv);
					}
					p = 0;
					fs = filp_open("/sys/class/focaltech_fp/focaltech_fp/spiclk",
						       O_WRONLY, 0);
					if (!IS_ERR_OR_NULL(fs)) {
						kernel_write(fs, "1\n", 2, &p);
						fput(fs);
					}
					pr_info("qsee_fp: L2 vdd+spiclk reassert before enroll resync=%d\n",
						enroll_resync);
					msleep(20);
					if (cap_hal_overlay) {
						size_t clen;

						/*
						 * Deliver firmware_log_level "info" (Android
						 * string). Leftover attach skip SYNC would
						 * keep this boot's INIT integer-3 config.
						 * m4hn: native log on here only — INIT kept
						 * it off so Base sticky still lands.
						 */
						diag_native_log = true;
						f32_config_build();
						clen = strlen(f32_config_text) + 1;
						pr_info("qsee_fp: overlay SYNC-pre-enroll clen=%zu (firmware_log_level 3)\n",
							clen);
						send_ion(id_dev, sess, SESS_BIG, FF_TA_SYNC_CONFIG,
							 (u32)clen, f32_config_text,
							 "dev-SYNC-pre-enroll");
						msleep(20);
					} else if (enroll_resync) {
						size_t clen;

						f32_config_build();
						clen = strlen(f32_config_text) + 1;
						send_ion(id_dev, sess, SESS_BIG, FF_TA_SYNC_CONFIG,
							 (u32)clen, f32_config_text,
							 "dev-SYNC-pre-enroll");
						msleep(20);
						memset(wm, 0, sizeof(wm));
						wm[0] = 1;
						send_ion(id_dev, sess, SESS_BIG,
							 FF_TA_CFG_POWER_MODE, 4, wm,
							 "dev-PWR-enroll");
						msleep(20);
					}
				}
				/*
				 * M4c2: stay FDT_DOWN before ENROLL (not mode 5).
				 * m4gh: WMODE-pre HIT then ENROLL logged chips mode 1
				 * + start auto fdt. m4h7 skip WMODE-pre still no auto
				 * fdt; send it again with WMODE-FDT-armed after ENROLL.
				 */
				memset(wm, 0, sizeof(wm));
				*(__le32 *)wm = cpu_to_le32(FF_WMODE_FDT_DOWN);
				if (cap_hal_overlay)
					pr_info("qsee_fp: overlay WMODE-FDT-pre-enroll after SLEEP\n");
				send_ion(id_dev, sess, SESS_BIG, FF_TA_CFG_WORK_MODE, 4, wm,
					 "dev-WMODE-FDT-pre-enroll");
				msleep(30);

				/*
				 * M4cy: defer ENROLL_HL until after sample CAPs so enroll
				 * workspace does not starve array_new. Still PRE_ENROLL +
				 * FDT arm, then samples; ENROLL after loop.
				 */
				if (do_auth) {
					enroll_samples = 3;
					pr_info("qsee_fp: overlay AUTH skip ENROLL (Match one press)\n");
				}
				if (!defer_enroll && skip_init_dev) {
					pr_info("qsee_fp: M4du skip ENROLL_HL (no INIT — would SCM-kill TA before PRESS)\n");
					defer_enroll = true;
				}
				if (!do_auth && !defer_enroll) {
					/* ONE ENROLL start — expect single "new fid" then rem */
					memset(hat, 0, sizeof(hat));
					hat[0] = 0;
					memcpy(hat + 1, &challenge, 8);
					hat[0x1c] = 2; /* authenticator_type fingerprint */
					hat[0x49] = 1;
					er = send_ion_seg(id_dev, sess, SESS_BIG,
							  FF_TA_ENROLL_HL, 0x4a, hat,
							  "dev-ENROLL-start");
					pr_info("qsee_fp: ENROLL start er=%d err=0x%x (expect one new fid)\n",
						er, le32_to_cpu(*((__le32 *)sess + 2)));
					if (er == -22) {
						pr_err("qsee_fp: ENROLL start killed TA\n");
						goto enroll_done;
					}
					{
						u8 esd_pay[4] = { 0, 0, 0, 0 };

						if (preserve_img_base && g_img_base_ok) {
							/* M4du: ESD_HANDLE after ENROLL SCM -22
							 * kills TA before PRESS. Android does not
							 * reset/esd between ENROLL and frame raw.
							 */
							pr_info("qsee_fp: M4cv skip L2 reset + ESD_HANDLE post-ENROLL (Base warm)\n");
						} else {
							l2_reset_ic("post-ENROLL-esd");
							if (send_ion(id_dev, sess, SESS_BIG,
								     FF_TA_ESD_HANDLE, 4, esd_pay,
								     "dev-ESD-HANDLE-post") == -22) {
								pr_err("qsee_fp: post-ENROLL ESD killed TA\n");
								goto enroll_done;
							}
							msleep(30);
						}
					}
				} else if (!do_auth) {
					pr_info("qsee_fp: M4cy defer ENROLL — CAP samples first (heap)\n");
				}
				/*
				 * Arm FDT_DOWN + SCAN (post-ENROLL or pre-sample when deferred).
				 * M4cv: soft esd_clear only (no L2) when Base warm.
				 */
				if (cap_hal_overlay) {
					pr_info("qsee_fp: M4dy skip post-ENROLL WMODE/SCAN (keep INIT image ptr)\n");
				} else {
					memset(wm, 0, sizeof(wm));
					*(__le32 *)wm = cpu_to_le32(FF_WMODE_FDT_DOWN);
					send_ion(id_dev, sess, SESS_BIG, FF_TA_CFG_WORK_MODE, 4,
						 wm, "dev-WMODE-FDT_DOWN-post-enroll");
					msleep(20);
					memset(cap, 0, sizeof(cap));
					*(__le32 *)cap = cpu_to_le32(1);
					send_ion(id_dev, sess, SESS_BIG, FF_TA_START_SCANNING,
						 0x30, cap, "dev-SCAN-post-enroll");
					msleep(20);
					pr_info("qsee_fp: post-ENROLL FDT_DOWN (no mode5) base=%d vio=%d defer=%d\n",
						g_img_base_ok, last_send_vio_ok, defer_enroll);
					esd_clear_loop(id_dev, sess, "post-enroll",
						       (preserve_img_base && g_img_base_ok) ? 2 : 5);
				}

				/*
				 * Sample loop (Android): wait IRQ/type 0x2 → CAPTURE only →
				 * REPORT_EVENT (0x1017 plen 0x2e0) feeds enroll stats.
				 *
				 * Measured on pmOS: once TA FDT/SCAN is active, L2 irq_count
				 * is nearly flat without a press (residual +0..3). Requiring
				 * an IRQ edge before CAP caused 8× "skip CAP" and never fed
				 * the TA enroll pipeline. Default enroll_force_cap=1: wait
				 * sample_wait_s for optional edge/DOWN, then always CAP.
				 */
				if (enroll_samples < 1)
					enroll_samples = 1;
				/* Missed LED windows still consume a slot. 60 covers
				 * 20 accepts plus missed flashes. Was 30, which
				 * stopped a Register at rem=2.
				 */
				if (enroll_samples > 60)
					enroll_samples = 60;
				if (sample_wait_s < 1)
					sample_wait_s = 1;
				if (sample_wait_s > 60)
					sample_wait_s = 60;
				/*
				 * Timing (user-facing): init ~15s already done; samples are
				 * press/lift edges. Defaults lean short — long waits felt like
				 * >>90s wall clock with CAP + REPORT overhead.
				 */
				pr_info("qsee_fp: *** HOLD FINGER NOW — %d samples, up to ~%ds waits + CAP (press/lift each) force_cap=%d ***\n",
					enroll_samples, enroll_samples * sample_wait_s,
					enroll_force_cap);
				pr_info("qsee_fp: *** tip: re-press each sample (edge); continuous hold may only fire once ***\n");
				/*
				 * M4cd: enable L2 IRQ once (no per-sample power cycle).
				 * M4cx: one FDT+SCAN arm only. Soft ESD if needed without
				 * SCAN-armed2 (M4cw hold: second SCAN killed TA SCM -22).
				 */
				l2_listen_on("pre-sample-loop");
				if (cap_hal_overlay)
					pr_info("qsee_fp: overlay skip esd_clear pre-sample-loop\n");
				else
					esd_clear_loop(id_dev, sess, "pre-sample-loop", 3);
				{
					u8 *vusr = (u8 *)sess + 0x10;
					int arm_er;

					memset(wm, 0, sizeof(wm));
					*(__le32 *)wm = cpu_to_le32(FF_WMODE_FDT_DOWN);
					/*
					 * m4hg: auto fdt ENROLL residual ends at
					 * ft93xx_set_scan_r; img_scan stays at start.
					 * 0x101f after ENROLL may abort that setup.
					 */
					if (cap_hal_overlay) {
						pr_info("qsee_fp: overlay skip 0x101f after ENROLL (keep auto fdt img_scan)\n");
						arm_er = 0;
					} else {
						arm_er = send_ion(id_dev, sess, SESS_BIG,
								  FF_TA_CFG_WORK_MODE, 4, wm,
								  "dev-WMODE-FDT-armed");
					}
					msleep(20);
					memset(vusr, 0, 0x30);
					*(__le32 *)vusr = cpu_to_le32(1);
					/*
					 * m4hj: SCAN-armed 0x24 with auto fdt started
					 * img_scan before PRESS; irq_count stuck,
					 * no REAL DOWN. Skip it.
					 */
					if (cap_hal_overlay) {
						u32 spd;

						pr_info("qsee_fp: overlay skip SCAN-armed (keep ENROLL auto fdt)\n");
						/*
						 * m4hk: img_scan stuck at start.
						 * Android logs max inte at 8.03M.
						 * SET_SPI here, not next to CAP
						 * (SET_SPI-before-CAP was REPORT -22).
						 */
						spd = spi_capture_bps > 0 ?
							      (u32)spi_capture_bps :
							      8030000u;
						*(__le32 *)vusr = cpu_to_le32(spd);
						pr_info("qsee_fp: overlay SET_SPI after ENROLL spd=%u\n",
							spd);
						if (arm_er != -22)
							arm_er = send_ion(id_dev, sess, SESS_BIG,
									  FF_TA_SET_SPI_SPEED, 4,
									  vusr,
									  "dev-SPI-SPEED-post-enroll");
						msleep(15);
					} else if (arm_er != -22) {
						arm_er = send_ion(id_dev, sess, SESS_BIG,
								  FF_TA_START_SCANNING, 0x30,
								  vusr, "dev-SCAN-armed");
					}
					msleep(30);
					if (cap_hal_overlay) {
						pr_info("qsee_fp: overlay skip QEV-pre-hold\n");
					} else if (arm_er != -22) {
						arm_er = send_ion(id_dev, sess, SESS_BIG,
								  FF_TA_QUERY_EVENT, 4, pay1,
								  "dev-QEV-pre-hold");
					}
					pr_info("qsee_fp: pre-hold arm esd=%d qes_ok=%d itype=0x%x touch=%d er=%d\n",
						last_send_esd, last_send_qes_ok,
						last_send_itype, last_send_finger_down,
						arm_er);
					if (arm_er == -22) {
						pr_err("qsee_fp: M4cx TA dead at pre-hold arm — abort samples\n");
						goto enroll_done;
					}
					if (last_send_esd) {
						/*
						 * M4db4: never ESD_HANDLE here — SCM -22 kills
						 * TA before PRESS banner. Proceed with sticky esd;
						 * LATCH under finger handles soft ESD if needed.
						 */
						pr_info("qsee_fp: M4db pre-hold sticky esd — proceed no ESD_HANDLE\n");
					}
				}
				if (cap_hal_overlay)
					pr_info("qsee_fp: M4ee skip wait_chip_ready (L2 reset poisons overlay SPI)\n");
				else
					(void)wait_chip_ready(id_dev, sess, "pre-sample", 3);
				/* Detect TA death: one QEV probe.
				 * Overlay: that probe logs [esd] before PRESS
				 * (m4gw) and is not how Android waits.
				 */
				if (cap_hal_overlay) {
					pr_info("qsee_fp: overlay skip QEV-pre-sample-alive\n");
				} else if (send_ion(id_dev, sess, SESS_BIG, FF_TA_QUERY_EVENT, 4,
						  pay1, "dev-QEV-pre-sample-alive") == -22) {
					pr_err("qsee_fp: M4cx TA dead pre-sample — abort\n");
					goto enroll_done;
				}
				if (!(skip_capture_path && !skip_enroll) &&
				    !cap_hal_overlay) {
					unsigned int wu;
					u8 *wusr = (u8 *)sess + 0x10;

					for (wu = 0; wu < 3; wu++) {
						char wtag[28];

						*(__le32 *)wusr =
							cpu_to_le32(spi_capture_bps > 0 ?
								    (u32)spi_capture_bps :
								    4000000u);
						send_ion(id_dev, sess, SESS_BIG,
							 FF_TA_SET_SPI_SPEED, 4, wusr,
							 "dev-SPI-SPEED-warm");
						memset(wm, 0, sizeof(wm));
						*(__le32 *)wm =
							cpu_to_le32(FF_WMODE_FDT_DOWN);
						send_ion(id_dev, sess, SESS_BIG,
							 FF_TA_CFG_WORK_MODE, 4, wm,
							 "dev-WMODE-warm");
						capture_ctx_fill_flag(sess, FF_IMG_COLS,
								      FF_IMG_ROWS, 0x24,
								      FF_IMG_BYTES, false, 1);
						scnprintf(wtag, sizeof(wtag),
							  "dev-cap-warm-%u", wu);
						send_ion(id_dev, sess, SESS_BIG,
							 FF_TA_CAPTURE_IMAGE, 0x24, wusr,
							 wtag);
						capture_ctx_fix_rows(sess, FF_IMG_COLS,
									 FF_IMG_ROWS, 0x24,
									 FF_IMG_BYTES);
						pr_info("qsee_fp: SPI warm CAP %u spi_fail=%d vio=%d itype=0x%x\n",
							wu, last_send_spi_mode_fail,
							last_send_vio_ok, last_send_itype);
						if (last_send_vio_ok &&
						    !last_send_spi_mode_fail &&
						    !(last_send_itype & 0x600))
							break;
						if ((last_send_spi_mode_fail ||
						     (last_send_itype & 0x600)) &&
						    spi_img_recover)
							spi_image_recover(id_dev, sess,
									  "warm");
						msleep(80);
					}
				} else {
					pr_info("qsee_fp: skip warm CAP thrash (skip_capture_path) — finger-only force\n");
				}
				for (sample = 0; sample < (unsigned int)enroll_samples; sample++) {
					char ctag[32];
					unsigned int irq_a = 0, irq_b = 0, tw;
					struct file *firq2 = NULL;
					char irqbuf2[32];
					loff_t pos2;
					ssize_t nr2;
					bool edge = false;
					bool ta_down = false;
					bool real_down = false;
					bool esd_rec_done = false;
					bool esd_late_done = false;
					bool overlay_empty = false;
					u8 *evt;
					overlay_rem = 0xffffffffu;
					/*
					 * M4c9: loop is msleep(40) per tw. Old tw_max=s*5 was
					 * only ~0.2s per "second" (12s when s=60). Use
					 * 25 iters/sec so sample_wait_s is real seconds.
					 */
					unsigned int tw_max =
						(unsigned int)sample_wait_s * 25;
					unsigned int touch_polls =
						touch_wait_ms > 0 ?
							(unsigned int)((touch_wait_ms + 19) / 20) :
							25u;

					if (rem_seen) {
						pr_info("qsee_fp: rem already seen — skip remaining samples\n");
						break;
					}

					/*
					 * M4de: require finger LIFT between samples. Without this,
					 * sticky touch/IRQ after sample N auto-fires sample N+1
					 * (~1s) before the user can re-press (M4dd hold: s1
					 * itype=0x0 touch=1 with no intentional press).
					 */
					if (sample > 0) {
						unsigned int lift_tw, clear = 0;

						pr_info("qsee_fp: *** LIFT finger — wait clear before sample %u ***\n",
							sample);
						if (cap_hal_overlay) {
							/*
							 * QEV-lift after enroll-flag CAP
							 * SCM -22 killed the TA. Lift IRQ
							 * already waited after ev=5.
							 */
							pr_info("qsee_fp: overlay skip QEV-lift before sample %u (lift IRQ already)\n",
								sample);
							goto after_lift_wait;
						}
						for (lift_tw = 0; lift_tw < tw_max; lift_tw++) {
							msleep(40);
							if (send_ion(id_dev, sess, SESS_BIG,
								     FF_TA_QUERY_EVENT, 4, pay1,
								     "dev-QEV-lift") == -22) {
								pr_err("qsee_fp: M4de TA dead lift-wait sample %u\n",
								       sample);
								goto enroll_done;
							}
							if (!last_send_finger_down &&
							    !(last_send_itype & 0x2)) {
								clear++;
								/* ~320ms stable clear */
								if (clear >= 8) {
									pr_info("qsee_fp: sample %u lift clear (stable)\n",
										sample);
									break;
								}
							} else {
								clear = 0;
								if ((lift_tw % 25) == 0)
									pr_info("qsee_fp: sample %u still DOWN — LIFT then re-press (touch=%d itype=0x%x)\n",
										sample,
										last_send_finger_down,
										last_send_itype);
							}
						}
						if (clear < 8)
							pr_info("qsee_fp: sample %u lift wait timeout — arm next anyway\n",
								sample);
						msleep(150);
					}
after_lift_wait:

					/*
					 * M4cd: do NOT l2_listen_on / VDD sysfs each sample —
					 * that power-cycled the IC and re-stuck ESD. Keep SPI
					 * clock vote only; IRQ already enabled once.
					 */
					{
						struct file *fs2;
						loff_t p2 = 0;

						fs2 = filp_open(
							"/sys/class/focaltech_fp/focaltech_fp/spiclk",
							O_WRONLY, 0);
						if (!IS_ERR_OR_NULL(fs2)) {
							kernel_write(fs2, "1\n", 2, &p2);
							fput(fs2);
						}
					}
					/* Soft re-arm FDT only (no power cycle).
					 * Overlay: skip 0x101f after ENROLL so TA
					 * FDT_DOWN_DETECT is not overwritten (Android
					 * INIT 0x101f=9 was vio/esd, still raw[0]).
					 * Skip SCAN plen 0x30. Non-overlay keeps mode 1.
					 */
					{
						int re;
						u8 *as = (u8 *)sess + 0x10;

						memset(wm, 0, sizeof(wm));
						*(__le32 *)wm = cpu_to_le32(FF_WMODE_FDT_DOWN);
						if (cap_hal_overlay) {
							*(__le32 *)wm = cpu_to_le32(FF_WMODE_FDT_DOWN_DETECT);
							re = send_ion(id_dev, sess, SESS_BIG,
								      FF_TA_CFG_WORK_MODE, 4, wm,
								      "dev-WMODE-FDT_DOWN_DETECT-enroll");
							pr_info("qsee_fp: overlay WMODE-FDT_DOWN_DETECT-pre-wait sample %u er=%d\n",
								sample, re);
						} else {
							re = send_ion(id_dev, sess, SESS_BIG,
								      FF_TA_CFG_WORK_MODE, 4, wm,
								      "dev-WMODE-FDT-pre-wait");
							if (re == -22) {
								pr_err("qsee_fp: M4cx TA dead sample %u pre-wait\n",
								       sample);
								goto enroll_done;
							}
						}
						if (cap_hal_overlay) {
							/* SCAN plen 0x30 first-word 1 is spent. */
							pr_info("qsee_fp: overlay skip SCAN-pre-wait sample %u\n",
								sample);
						} else {
							memset(as, 0, 0x30);
							*(__le32 *)as = cpu_to_le32(1);
							re = send_ion(id_dev, sess, SESS_BIG,
								      FF_TA_START_SCANNING, 0x30,
								      as, "dev-SCAN-pre-wait");
							if (re == -22) {
								pr_err("qsee_fp: M4cx TA dead sample %u SCAN-pre-wait\n",
								       sample);
								goto enroll_done;
							}
							msleep(15);
						}
					}

					firq2 = filp_open(
						"/sys/class/focaltech_fp/focaltech_fp/irq_count",
						O_RDONLY, 0);
					if (!IS_ERR_OR_NULL(firq2)) {
						unsigned long last_edge_j = 0;
						bool mid_esd_done = false;
						unsigned int deb_ms =
							irq_debounce_ms > 0 ?
								(unsigned int)irq_debounce_ms :
								250u;

						pos2 = 0;
						nr2 = kernel_read(firq2, irqbuf2, sizeof(irqbuf2) - 1,
								  &pos2);
						if (nr2 > 0) {
							irqbuf2[nr2] = 0;
							kstrtouint(irqbuf2, 10, &irq_a);
						}
						if (prev_overlay_empty) {
							unsigned int qi, stable = 0, irq_q = irq_a;

							pr_info("qsee_fp: overlay quiet after empty (wait new press)\n");
							for (qi = 0; qi < 8; qi++) {
								unsigned int irq_n = irq_q;

								msleep(40);
								pos2 = 0;
								nr2 = kernel_read(firq2, irqbuf2,
										  sizeof(irqbuf2) - 1,
										  &pos2);
								if (nr2 > 0) {
									irqbuf2[nr2] = 0;
									kstrtouint(irqbuf2, 10, &irq_n);
								}
								if (irq_n == irq_q)
									stable++;
								else {
									stable = 0;
									irq_q = irq_n;
								}
								if (stable >= 8)
									break;
							}
							irq_a = irq_q;
							prev_overlay_empty = false;
						}
						/* LED after quiet so the cue is not during settle. */
						pr_info("qsee_fp: *** PRESS then LIFT finger sample %u/%d (HOLD on power-button sensor) ***\n",
							sample, enroll_samples);
						/*
						 * M4cc: poll irq; debounce edge handling so ESD
						 * storms cannot burn the whole sample window on
						 * 500ms QUERY loops. Light QES poll ~1/s.
						 */
						for (tw = 0; tw < tw_max; tw++) {
							unsigned int polls;
							unsigned int pq;

							msleep(40);
							pos2 = 0;
							nr2 = kernel_read(firq2, irqbuf2,
									  sizeof(irqbuf2) - 1, &pos2);
							if (nr2 > 0) {
								irqbuf2[nr2] = 0;
								if (kstrtouint(irqbuf2, 10,
									       &irq_b) != 0)
									irq_b = irq_a;
							}
							if ((tw % 25) == 0)
								pr_info("qsee_fp: sample %u wait irq=%u (base=%u) tw=%u\n",
									sample, irq_b, irq_a, tw);

							/* Light QES poll ~1/s. After overlay
							 * enroll CAP, QEV-poll SCM -22 (sample 1).
							 */
							if ((tw % 25) == 12 && !cap_hal_overlay) {
								if (send_ion(id_dev, sess,
									     SESS_BIG,
									     FF_TA_QUERY_EVENT, 4,
									     pay1,
									     "dev-QEV-poll") == -22) {
									pr_err("qsee_fp: M4cx TA dead sample %u QEV-poll\n",
									       sample);
									fput(firq2);
									goto enroll_done;
								}
								/*
								 * M4de: require itype DOWN bit — sticky
								 * touch alone auto-fired sample 1.
								 */
								if (last_send_itype & 0x2) {
									pr_info("qsee_fp: sample %u QES poll DOWN touch=%d itype=0x%x\n",
										sample,
										last_send_finger_down,
										last_send_itype);
									real_down = true;
									ta_down = true;
									edge = true;
									break;
								}
							}

							if (irq_b < irq_a +
							    (unsigned int)max(irq_edge_delta, 1))
								continue;

							/* Absorb IRQ storm without QUERY spam */
							if (last_edge_j &&
							    !time_after(jiffies,
									last_edge_j +
									msecs_to_jiffies(deb_ms))) {
								irq_a = irq_b;
								continue;
							}
							last_edge_j = jiffies;
							edge = true;
							pr_info("qsee_fp: sample %u IRQ %u->%u (debounced)\n",
								sample, irq_a, irq_b);
							irq_a = irq_b;

							/*
							 * m4ib: sample>0 IRQ-as-down skipped
							 * QEV-s → itype=0x0 empty CAP avgv~863
							 * quality 5, rem stuck at 19. WMODE
							 * after ev=6 also edges IRQ. Wait
							 * itype 0x2 like sample 0.
							 */

							if (send_ion(id_dev, sess, SESS_BIG,
								     FF_TA_QUERY_EVENT, 4, pay1,
								     "dev-QEV-s") == -22) {
								pr_err("qsee_fp: M4cx TA dead sample %u QEV-s\n",
								       sample);
								fput(firq2);
								goto enroll_done;
							}
							/*
							 * M4dq: QFINGER zeroes last_send_itype
							 * (send_ion resets it). CAP then sees
							 * AFE f:N. If QEV already itype 0x2,
							 * CAPTURE immediately — no QFINGER.
							 */
							if (last_send_itype == 0x2) {
								pr_info("qsee_fp: sample %u REAL DOWN QEV-s (touch=%d itype=0x%x) — skip QFINGER\n",
									sample,
									last_send_finger_down,
									last_send_itype);
								real_down = true;
								ta_down = true;
								break;
							}
							if (cap_hal_overlay) {
								pr_info("qsee_fp: overlay skip QFINGER-s sample %u (keep itype, wait IRQ)\n",
									sample);
								continue;
							}
							send_ion(id_dev, sess, SESS_BIG,
								 FF_TA_QUERY_FINGER, 4, pay1,
								 "dev-QFINGER-s");
							pr_info("qsee_fp: sample %u post-IRQ itype=0x%x touch=%d esd=%d qes_ok=%d\n",
								sample, last_send_itype,
								last_send_finger_down,
								last_send_esd, last_send_qes_ok);

							/* M4de: itype DOWN only (not sticky touch) */
							if (last_send_itype & 0x2) {
								pr_info("qsee_fp: sample %u REAL DOWN (touch=%d itype=0x%x)\n",
									sample,
									last_send_finger_down,
									last_send_itype);
								real_down = true;
								ta_down = true;
								break;
							}
							if (last_send_finger_down &&
							    !(last_send_itype & 0x2))
								pr_info("qsee_fp: sample %u touch sticky no itype — ignore (wait itype)\n",
									sample);

							/*
							 * M4cc: sticky ESD → one clear + FDT re-arm,
							 * then short touchwait. Full touch_wait_ms
							 * only when not esd.
							 */
							if (last_send_esd && !mid_esd_done) {
								/*
								 * Overlay: QEV-s [esd] before the real
								 * press then WMODE-FDT-esdclr (m4gs).
								 * Keep wait-touch; do not re-arm.
								 */
								if (cap_hal_overlay) {
									pr_info("qsee_fp: overlay skip esdclr sample %u (sticky esd, keep FDT)\n",
										sample);
									mid_esd_done = true;
									continue;
								}
								/*
								 * M4cv: soft ESD only when Base warm —
								 * L2 reset here wiped Base → Step1 OOM.
								 */
								pr_info("qsee_fp: sample %u sticky ESD — %s re-arm FDT\n",
									sample,
									(preserve_img_base &&
									 g_img_base_ok) ?
										"soft-only" :
										"clear→L2rst +");
								if (esd_clear_loop(id_dev, sess,
										   "mid-wait-esd", 2)) {
									pr_info("qsee_fp: sample %u REAL DOWN during esdclr\n",
										sample);
									real_down = true;
									ta_down = true;
									mid_esd_done = true;
									break;
								}
								memset(wm, 0, sizeof(wm));
								*(__le32 *)wm = cpu_to_le32(
									FF_WMODE_FDT_DOWN);
								send_ion(id_dev, sess, SESS_BIG,
									 FF_TA_CFG_WORK_MODE, 4,
									 wm, "dev-WMODE-FDT-esdclr");
								if (cap_hal_overlay) {
									pr_info("qsee_fp: skip SCAN-esdclr (overlay Nulls image ptr)\n");
								} else {
									u8 *as = (u8 *)sess + 0x10;

									memset(as, 0, 0x30);
									*(__le32 *)as = cpu_to_le32(1);
									send_ion(id_dev, sess,
										 SESS_BIG,
										 FF_TA_START_SCANNING,
										 0x30, as,
										 "dev-SCAN-esdclr");
								}
								msleep(30);
								mid_esd_done = true;
								l2_listen_on("post-esdclr");
								send_ion(id_dev, sess, SESS_BIG,
									 FF_TA_QUERY_EVENT, 4,
									 pay1, "dev-QEV-post-esdclr");
								send_ion(id_dev, sess, SESS_BIG,
									 FF_TA_QUERY_FINGER, 4,
									 pay1, "dev-QFINGER-post-esdclr");
								pr_info("qsee_fp: sample %u post-esdclr itype=0x%x touch=%d esd=%d qes_ok=%d\n",
									sample, last_send_itype,
									last_send_finger_down,
									last_send_esd,
									last_send_qes_ok);
								if (last_send_itype & 0x2) {
									pr_info("qsee_fp: sample %u REAL DOWN after esdclr (touch=%d itype=0x%x)\n",
										sample,
										last_send_finger_down,
										last_send_itype);
									real_down = true;
									ta_down = true;
									break;
								}
							}

							if (last_send_esd)
								polls = esd_touch_wait_ms > 0 ?
									(unsigned int)((esd_touch_wait_ms +
											19) /
										       20) :
									3u;
							else
								polls = touch_polls;
							if (polls > 25)
								polls = 25;
							for (pq = 0; pq < polls; pq++) {
								msleep(20);
								send_ion(id_dev, sess, SESS_BIG,
									 FF_TA_QUERY_EVENT, 4,
									 pay1, "dev-QEV-touchwait");
								if (last_send_itype & 0x2)
									break;
							}
							if (last_send_itype & 0x2) {
								pr_info("qsee_fp: sample %u REAL DOWN (touch=%d itype=0x%x)\n",
									sample,
									last_send_finger_down,
									last_send_itype);
								real_down = true;
								ta_down = true;
								break;
							}
							if (irq_means_down) {
								pr_info("qsee_fp: sample %u TA DOWN (irq_means_down touch=%d esd=%d itype=0x%x)\n",
									sample, last_send_finger_down,
									last_send_esd,
									last_send_itype);
								ta_down = true;
								break;
							}
							/* M4cc: no late-window fake DOWN on ESD */
							pr_info("qsee_fp: sample %u IRQ without touch — keep waiting (esd=%d mid_esd=%d)\n",
								sample, last_send_esd, mid_esd_done);
						}
						fput(firq2);
					} else {
						pr_info("qsee_fp: *** PRESS then LIFT finger sample %u/%d (HOLD on power-button sensor) ***\n",
							sample, enroll_samples);
					}
					/*
					 * M4cb: FORCE CAP without real_down is FDT-only (no rem).
					 * Still optional for path exercise; never claim real_down.
					 */
					if (!ta_down && !real_down) {
						if (enroll_force_cap) {
							pr_info("qsee_fp: sample %u no REAL DOWN — FORCE idle CAP (enroll_force_cap; no rem expected)\n",
								sample);
							/* ta_down stays false → idle CAP / force-image */
							edge = true;
						} else {
							pr_info("qsee_fp: sample %u no IRQ/DOWN — skip CAP (finger-only)\n",
								sample);
							continue;
						}
					}

					/*
					 * Image CAP (M4ar): under finger, run force-image FIRST
					 * (Android transmit_image ~37ms wraps CAP/img_scan), then
					 * flag-sweep meta CAP. img_off 0x24 (not 0x100). No a5
					 * paint when ta_down (can confuse TA). No large-plen CAP.
					 */
					{
						unsigned int bi, ntry, fi;
						bool got_enroll = false;
						const u32 cols = 112, rows = 88;
						const u32 npix = cols * rows;
						const u32 img_off = 0x24;
						u32 nz = 0;
						/*
						 * M4be: image-path flags first (bit6/bit9).
						 * flag=1 alone → FDT soft-success only.
						 */
						static const u32 flag_cands[] = {
							0x240, 0x41, 0x200, 0x40, 0x241, 1
						};
						u32 flags_try[8];
						unsigned int nflags = 0;

						/*
						 * M4be: under finger, image flags first — do NOT
						 * prepend plain capture_flag=1 (FDT-only).
						 */
						if (ta_down) {
							if (capture_flag & 0x240)
								flags_try[nflags++] = capture_flag;
							for (fi = 0; fi < ARRAY_SIZE(flag_cands);
							     fi++) {
								u32 f = flag_cands[fi];
								unsigned int k;
								bool dup = false;

								for (k = 0; k < nflags; k++) {
									if (flags_try[k] == f) {
										dup = true;
										break;
									}
								}
								if (!dup && nflags < ARRAY_SIZE(flags_try))
									flags_try[nflags++] = f;
							}
						} else {
							flags_try[nflags++] = capture_flag ?
								capture_flag : 1;
							if (capture_flag_sweep) {
								for (fi = 0; fi < ARRAY_SIZE(flag_cands);
								     fi++) {
									u32 f = flag_cands[fi];
									unsigned int k;
									bool dup = false;

									for (k = 0; k < nflags; k++) {
										if (flags_try[k] == f) {
											dup = true;
											break;
										}
									}
									if (!dup &&
									    nflags < ARRAY_SIZE(flags_try))
										flags_try[nflags++] = f;
								}
							}
						}
						/*
						 * M4au/M4be: under finger, try image flags until
						 * frame raw, then triple-CAP that winning flag.
						 * Start with 0x240 (bits 6+9) not plain 1.
						 */
						if (ta_down) {
							unsigned int want =
								sample_triple_cap > 0 ?
									(unsigned int)sample_triple_cap :
									3u;

							if (want > 6)
								want = 6;
							/* First pass: one CAP per image flag */
							ntry = nflags;
							/* After loop may re-run winner — see below */
							if (ntry < want)
								ntry = want;
						} else {
							ntry = 1;
							if (!nflags)
								flags_try[nflags++] = 1;
						}
						if (!nflags)
							nflags = 1;
						if (cap_hal_overlay) {
							/*
							 * M4dy idle: factory 0xE0000240 → frame raw.
							 * Enroll-thread 0xC0040002 after ENROLL_HL
							 * was Null 0xffffff37 then SCM -22.
							 */
							flags_try[0] = FF_HAL_FLAG_FACTORY;
							nflags = 1;
							ntry = 1;
							pr_info("qsee_fp: M4dy sample HAL overlay flag=0x%x ntry=1 ta_down=%d\n",
								flags_try[0], ta_down);
						}

						if (real_down && !cap_hal_overlay) {
							char fwhy[24];
							u32 fnz;

							/*
							 * M4cg: REAL DOWN → FAST AFR only (0x240×3).
							 * No multi-flag thrash / force-image under
							 * finger (M4cf: 6s of FDT CAP, never img).
							 */
							pr_info("qsee_fp: sample %u finger CAP FAST REAL touch=%d esd=%d itype=0x%x\n",
								sample, last_send_finger_down,
								last_send_esd, last_send_itype);
							scnprintf(fwhy, sizeof(fwhy), "pre-s%u",
								  sample);
							fnz = try_android_frame_raw(id_dev, sess,
										    fwhy);
							if (fnz > 0)
								nz = fnz;
							if (last_send_frame_raw ||
							    last_send_sensor_raw ||
							    last_send_enrolling) {
								pr_info("qsee_fp: *** AFR rem markers sample=%u frame=%d sens=%d enroll=%d nz=%u ***\n",
									sample, last_send_frame_raw,
									last_send_sensor_raw,
									last_send_enrolling, fnz);
								rem_seen = true;
								got_enroll = true;
							} else {
								pr_info("qsee_fp: sample %u FAST AFR no rem (nz=%u frame=%d) — skip force thrash\n",
									sample, fnz,
									last_send_frame_raw);
							}
							/* Under real finger: only 0x240×3 already done */
							nflags = 1;
							flags_try[0] = 0x240;
							ntry = 0; /* skip secondary CAP loop */
						} else if (enroll_force_cap && !cap_hal_overlay) {
							char fwhy[24];
							u32 fnz;

							/* Idle force-image — diagnostics only */
							scnprintf(fwhy, sizeof(fwhy), "force-s%u",
								  sample);
							fnz = try_force_image_path(id_dev, sess,
										   false, fwhy);
							if (fnz > nz)
								nz = fnz;
							pr_info("qsee_fp: sample %u idle force-image nz=%u frame=%d\n",
								sample, fnz, last_send_frame_raw);
						} else if (!cap_hal_overlay) {
							memset(wm, 0, sizeof(wm));
							*(__le32 *)wm =
								cpu_to_le32(FF_WMODE_FDT_DOWN);
							send_ion(id_dev, sess, SESS_BIG,
								 FF_TA_CFG_WORK_MODE, 4, wm,
								 "dev-WMODE-FDT_DOWN-sample");
							msleep(10);
						}

						if (cap_hal_overlay) {
							u8 *eusr = (u8 *)sess + 0x10;
							int eflag;

							last_send_qual_err = false;
							/*
							 * m4el..m4fc: REAL DOWN + enroll-flag
							 * then REPORT ev=2/3/7 → quality or
							 * "got image ready", never rem.
							 * HAL device_event_thread after a
							 * successful enroll CAP (ret 0):
							 * mov w12,#5; REPORT +0x4=5,
							 * +0x2c8=burst, +0x2d0=0, +0x2d8=4.
							 * ev=7 is unlock IMAGE_READY.
							 */
							if ((ta_down || real_down) &&
							    (!cap_hal_overlay ||
							     (last_send_itype & 0x2))) {
								pr_info("qsee_fp: overlay REAL DOWN — enroll-flag CAP itype=0x%x\n",
									last_send_itype);
								eflag = overlay_enroll_flag_caps(id_dev,
												 sess);
								if (eflag == -22) {
									pr_err("qsee_fp: enroll-flag CAP killed TA\n");
									goto enroll_done;
								}
								if (eflag == 1) {
									overlay_empty = true;
									prev_overlay_empty = true;
									overlay_stall++;
									pr_info("qsee_fp: overlay PRESS WEAK re-arm stall=%u rem=%u sample=%u\n",
										overlay_stall,
										overlay_rem <= 20 ? overlay_rem :
										overlay_rem_was,
										sample);
									if (overlay_stall >= 3u) {
										pr_info("qsee_fp: overlay rem stall %u empty/weak rem=%u — stop samples\n",
											overlay_stall,
											overlay_rem <= 20 ? overlay_rem :
											overlay_rem_was);
										goto enroll_done;
									}
									goto overlay_empty_rearm;
								}
								if (eflag == 0) {
									/*
									 * m4id: empty CAP avgv~863 quality 5 does
									 * not rem-- and burns the attempt budget.
									 */
									if (last_send_frame_avgv >= 600u) {
										pr_info("qsee_fp: overlay skip REPORT empty avgv=%u\n",
											last_send_frame_avgv);
										pr_info("qsee_fp: overlay PRESS EMPTY avgv=%u sample=%u\n",
											last_send_frame_avgv, sample);
										overlay_empty = true;
										prev_overlay_empty = true;
										overlay_stall++;
										if (overlay_stall >= 3u) {
											pr_info("qsee_fp: overlay rem stall %u empty/weak rem=%u — stop samples\n",
												overlay_stall,
												overlay_rem <= 20 ? overlay_rem :
												overlay_rem_was);
											goto enroll_done;
										}
										/* m4ig: still ev=6 + WMODE after empty. */
										goto overlay_empty_rearm;
									}
									/*
									 * Android REPORT is immediate after 3-CAP
									 * (enroll ~10 ms). 80ms + 16× LOGCAT never
									 * filled slots=0x7 and added ~400ms/press.
									 */
									pr_info("qsee_fp: overlay skip LOGCAT drain (REPORT after 3-CAP)\n");
									/* 3-CAP is the burst (TA logs each as raw[0]). Do not WEAK-skip REPORT. */
									/* Do not HEALTH between CAP and REPORT —
									 * that overwrites the CAP ion image.
									 * m4fi: CAP then REPORT ev=5 immediately.
									 */
									pr_info("qsee_fp: overlay REPORT rem-probe dirty pay+0x10=0x%x pay+0x24=0x%x\n",
										le32_to_cpu(*(__le32 *)(eusr + 0x10)),
										le32_to_cpu(*(__le32 *)(eusr + 0x24)));
									event_ctx_fill(sess, 0x5, 4, 3, 0);
									/*
									 * m4hz: pay+0x24=0x13 may be leftover
									 * (CAP memset only 0x24 bytes). Poison the
									 * rem/fid slots only — do not memset 0x2e0
									 * (HEALTH-class CAP header wipe).
									 */
									*(__le32 *)(eusr + 0x10) = cpu_to_le32(0xaaaaaaaau);
									*(__le32 *)(eusr + 0x24) = cpu_to_le32(0xa5a5a5a5u);
									pr_info("qsee_fp: overlay REPORT rem-probe before pay+0x10=0x%x pay+0x24=0x%x\n",
										le32_to_cpu(*(__le32 *)(eusr + 0x10)),
										le32_to_cpu(*(__le32 *)(eusr + 0x24)));
									pr_info("qsee_fp: event_ctx before REPORT +0x04=0x%x +0x2c8=0x%x +0x2d0=0x%x +0x2d8=0x%x\n",
										le32_to_cpu(*(__le32 *)(eusr + 4)),
										le32_to_cpu(*(__le32 *)(eusr + 0x2c8)),
										le32_to_cpu(*(__le32 *)(eusr + 0x2d0)),
										le32_to_cpu(*(__le32 *)(eusr + 0x2d8)));
									er = send_ion(id_dev, sess,
										      SESS_BIG,
										      FF_TA_REPORT_EVENT,
										      0x2e0, eusr,
										      "dev-REPORT-ev5");
									if (er == -22) {
										pr_err("qsee_fp: overlay REPORT ev=5 killed TA — stop samples\n");
										goto enroll_done;
									}
									scan_enroll_markers(sess,
											    "dev-REPORT-ev5");
									/*
									 * FP6 fingerprintd: do_enroll copies rem into
									 * the REPORT response payload at +36; the TA
									 * log starves exactly when a sample is
									 * accepted. Dump that word (QSEECom req
									 * envelope +0x10 is payload).
									 */
									pr_info("qsee_fp: overlay REPORT rem-probe req+0x08=0x%x +0x0c=0x%x pay+0x0c=0x%x pay+0x10=0x%x pay+0x24=0x%x slots=0x%x\n",
										le32_to_cpu(*(__le32 *)((u8 *)sess + 8)),
										le32_to_cpu(*(__le32 *)((u8 *)sess + 0xc)),
										le32_to_cpu(*(__le32 *)(eusr + 0x0c)),
										le32_to_cpu(*(__le32 *)(eusr + 0x10)),
										le32_to_cpu(*(__le32 *)(eusr + 0x24)),
										last_send_frame_slots);
									overlay_rem = le32_to_cpu(*(__le32 *)(eusr + 0x24));
									if (overlay_rem != 0xa5a5a5a5u &&
									    overlay_rem <= 20) {
										pr_info("qsee_fp: overlay rem=%u fid=0x%x sample=%u\n",
											overlay_rem,
											le32_to_cpu(*(__le32 *)(eusr + 0x10)),
											sample);
										if (overlay_rem < overlay_rem_was) {
											pr_info("qsee_fp: overlay PRESS OK rem=%u sample=%u\n",
												overlay_rem, sample);
											overlay_stall = 0;
										} else
											pr_info("qsee_fp: overlay PRESS REJECT rem=%u avgv=%u sample=%u\n",
												overlay_rem,
												last_send_frame_avgv,
												sample);
										overlay_rem_was = overlay_rem;
									} else {
										overlay_rem = 0xffffffffu;
									}
									pr_info("qsee_fp: overlay REPORT ev=5 +0x2d0=0 flag=4 enroll=%d qerr=%d rem=%u\n",
										last_send_enrolling,
										last_send_qual_err,
										overlay_rem);
									if (do_auth) {
										if (!g_keep_enrolled) {
											u8 gpay[128];
											const char *gpath = "/var/lib/fp5-fp";

											memset(gpay, 0, sizeof(gpay));
											*(__le32 *)gpay = cpu_to_le32(0);
											memcpy(gpay + 4, gpath, strlen(gpath) + 1);
											send_ion(id_dev, sess, SESS_BIG,
												 FF_TA_SET_ACTIVE_GROUP,
												 4 + (u32)strlen(gpath) + 1, gpay,
												 "dev-SET_GROUP-auth");
										}
										memset(hat, 0, sizeof(hat));
										hat[0x1c] = 2;
										*(__le32 *)(hat + 0x45) = cpu_to_le32(0);
										hat[0x49] = 1;
										er = send_ion_seg(id_dev, sess, SESS_BIG,
												  FF_TA_AUTHENTICATE, 0x4a, hat,
												  "dev-AUTH");
										last_send_app_err = le32_to_cpu(*((__le32 *)sess + 2));
										pr_info("qsee_fp: overlay AUTH er=%d err=0x%x sample=%u\n",
											er, last_send_app_err, sample);
										scan_enroll_markers(sess, "dev-AUTH");
										if (last_send_app_err || last_send_tmpl_missing ||
										    !last_send_auth_id_ok)
											pr_info("qsee_fp: overlay AUTH FAIL err=0x%x tmpl_missing=%d id_ok=%d (not a match)\n",
												last_send_app_err,
												last_send_tmpl_missing,
												last_send_auth_id_ok);
										else
											pr_info("qsee_fp: overlay AUTH HIT err=0x0 sample=%u\n",
												sample);
										goto enroll_done;
									}
									pr_info("qsee_fp: event_ctx +0x04=0x%x +0x08=0x%x +0x0c=0x%x +0x10=0x%x +0x2c8=0x%x +0x2d0=0x%x +0x2d8=0x%x\n",
										le32_to_cpu(*(__le32 *)(eusr + 4)),
										le32_to_cpu(*(__le32 *)(eusr + 8)),
										le32_to_cpu(*(__le32 *)(eusr + 0xc)),
										le32_to_cpu(*(__le32 *)(eusr + 0x10)),
										le32_to_cpu(*(__le32 *)(eusr + 0x2c8)),
										le32_to_cpu(*(__le32 *)(eusr + 0x2d0)),
										le32_to_cpu(*(__le32 *)(eusr + 0x2d8)));
									/*
									 * Android device_event_thread calls
									 * ff_trustlet_save_data with opcode 0x1014,
									 * plen 4, and payload word 0x40000000.
									 * That is the ~200 ms template write.
									 * Opcode 0x1015 with bit30 is the 5 ms
									 * quality path and does not write.
									 * SET_GROUP then loads the stored finger.
									 * FDT rearm stays so the next press is 0x2.
									 */
									if (overlay_rem == 0) {
										__le32 save_flag;

										memset(eusr, 0, 0x230);
										save_flag = cpu_to_le32(0x40000000);
										*(__le32 *)eusr = save_flag;
										pr_info("qsee_fp: overlay SAVE template rem=0 opcode=0x1014 flag=0x40000000 (rearm FDT for Match)\n");
										er = send_ion(id_dev, sess, SESS_BIG,
											      0x1014, 4, &save_flag,
											      "dev-SAVE-template");
										if (er == -22) {
											pr_err("qsee_fp: overlay SAVE template killed TA\n");
											goto enroll_done;
										}
										overlay_persist_load(id_dev, sess);
									} else {
										pr_info("qsee_fp: overlay skip persist wait rem=%u (until rem=0)\n",
											overlay_rem);
									}
overlay_empty_rearm:
									/*
									 * Android enroll: ev=5 on touch, FDT_UP_DETECT,
									 * itype 0x4 lift, ev=6, FDT_DOWN_DETECT. No 2s
									 * gate — press/lift at user speed. QEV-lift
									 * SCM -22; wait HLOS IRQ only.
									 */
									memset(wm, 0, sizeof(wm));
									*(__le32 *)wm = cpu_to_le32(FF_WMODE_FDT_UP);
									send_ion(id_dev, sess, SESS_BIG,
										 FF_TA_CFG_WORK_MODE, 4, wm,
										 "dev-WMODE-FDT_UP-lift");
									pr_info("qsee_fp: overlay wait lift IRQ (no QEV, no 2s) sample=%u down=%u\n",
										sample, irq_b);
									overlay_wait_lift_irq(irq_b, 80u);
									event_ctx_fill(sess, 0x6, 4, 3, 0);
									*(__le32 *)(eusr + 0x10) = cpu_to_le32(0xaaaaaaaau);
									*(__le32 *)(eusr + 0x24) = cpu_to_le32(0xa5a5a5a5u);
									er = send_ion(id_dev, sess,
										      SESS_BIG,
										      FF_TA_REPORT_EVENT,
										      0x2e0, eusr,
										      "dev-REPORT-ev6");
									if (er == -22) {
										pr_err("qsee_fp: overlay REPORT ev=6 killed TA — stop samples\n");
										goto enroll_done;
									}
									pr_info("qsee_fp: overlay REPORT ev=6 sample=%u (lift; ignore rem)\n",
										sample);
									memset(wm, 0, sizeof(wm));
									*(__le32 *)wm = cpu_to_le32(cap_hal_overlay ?
										FF_WMODE_FDT_DOWN_DETECT :
										FF_WMODE_FDT_DOWN);
									send_ion(id_dev, sess, SESS_BIG,
										 FF_TA_CFG_WORK_MODE, 4, wm,
										 cap_hal_overlay ?
										 "dev-WMODE-FDT_DOWN_DETECT-next" :
										 "dev-WMODE-FDT_DOWN-next");
									pr_info("qsee_fp: overlay WMODE-FDT-next after ev=6 (no SCAN)\n");
									pr_info("qsee_fp: overlay ARM wait itype sample=%u rem=%u\n",
										sample,
										overlay_rem <= 20 ? overlay_rem :
										overlay_rem_was);
								}
							} else {
								/*
								 * No itype 0x2: do not idle CAP +
								 * REPORT ev=7. m4ib: sample>0 IRQ
								 * without QEV-s CAPed empty avgv~863.
								 */
								pr_info("qsee_fp: overlay skip enroll-flag CAP itype=0x%x (need FDT 0x2; no ev=7)\n",
									last_send_itype);
							}
overlay_after_report:
							send_ion(id_dev, sess, SESS_BIG, 0x1003, 0,
								 NULL, "dev-LOGCAT-post-report");
							scan_enroll_markers(sess,
									    "dev-LOGCAT-post-report");
							if (overlay_rem <= 20) {
								if (overlay_rem == 0) {
									rem_seen = true;
									got_enroll = true;
									ta_enrolling = true;
									pr_info("qsee_fp: overlay rem=0 template — stop samples\n");
								} else {
									pr_info("qsee_fp: overlay rem=%u sample=%u — more presses\n",
										overlay_rem, sample);
								}
							} else if (last_send_enrolling) {
								rem_seen = true;
								got_enroll = true;
								ta_enrolling = true;
							} else {
								pr_info("qsee_fp: overlay frame raw but TA did not rem-- qerr=%d\n",
									last_send_qual_err);
							}
							ntry = 0;
						}

						for (bi = 0; bi < ntry && !got_enroll; bi++) {
							u32 this_flag = flags_try[bi % nflags];

							/*
							 * M4c4: under finger, do NOT START_SCANNING
							 * / WMODE before each CAP (induces ESD).
							 * M4dy overlay: SCAN-pre-cap Nulls the INIT image
							 * pointer (idle frame raw was overlay CAP
							 * with no extra SCAN).
							 */
							if (!ta_down && !cap_hal_overlay) {
								memset(cap, 0, sizeof(cap));
								*(__le32 *)cap = cpu_to_le32(1);
								send_ion(id_dev, sess, SESS_BIG,
									 FF_TA_START_SCANNING, 0x30,
									 cap, "dev-SCAN-pre-cap");
								msleep(10);
							}

							/*
							 * M4c3 / M4bg: do NOT REPORT_EVENT 0x2 before
							 * CAP under finger. Fake-down skips img_scan.
							 */
							/*
							 * Optional mode-5 (default off). Never under
							 * finger — Android frame raw is FDT_DOWN only.
							 */
							if (cap_img_mode_pulse && !ta_down) {
								memset(wm, 0, sizeof(wm));
								*(__le32 *)wm = cpu_to_le32(
									FF_WMODE_IMG_CALIB);
								send_ion(id_dev, sess, SESS_BIG,
									 FF_TA_CFG_WORK_MODE, 4,
									 wm,
									 "dev-WMODE-img-pre-cap");
								msleep(15);
							}

							/* No a5 paint under finger — can poison scan */
							if (cap_hal_overlay) {
								capture_ctx_fill_hal(sess, 1, bi,
										     FF_HAL_FLAG_FACTORY,
										     1, 1);
								this_flag = FF_HAL_FLAG_FACTORY;
							} else {
								capture_ctx_fill_flag(sess, cols,
										      rows, img_off,
										      npix * 2,
										      !ta_down,
										      this_flag);
							}
							scnprintf(ctag, sizeof(ctag),
								  "dev-cap-enroll-%u", sample);
							er = send_ion(id_dev, sess, SESS_BIG,
								      FF_TA_CAPTURE_IMAGE, 0x24,
								      usr, ctag);
							last_err = le32_to_cpu(
								*((__le32 *)sess + 2));
							if (cap_hal_overlay) {
								capture_dump_hal_image(sess, ctag);
								nz = capture_scan_all(sess, 0x44,
										      npix * 2,
										      ctag, er);
							} else {
								capture_ctx_fix_rows(sess, cols,
										     rows, img_off,
										     npix * 2);
								nz = capture_scan_all(sess, img_off,
										      npix * 2,
										      ctag, er);
							}
							pr_info("qsee_fp: %s er=%d err=0x%x flag=0x%x ta_down=%d edge=%d vio=%d raw=%d irdy=%d sens=%d enroll=%d pix=%u try=%u\n",
								ctag, er, last_err, this_flag,
								ta_down, edge, last_send_vio_ok,
								last_send_frame_raw,
								last_send_image_ready,
								last_send_sensor_raw,
								last_send_enrolling, nz, bi);
							if (er == -22)
								break;

							/* TA-SPI/SRAM image pull — finger only (M4bc) */
							if (ta_down) {
								u32 pnz;

								scnprintf(ctag, sizeof(ctag),
									  "s%u-t%u", sample, bi);
								pnz = try_pull_image_ree_ta(
									id_dev, sess, ctag);
								if (pnz > nz)
									nz = pnz;
								/* Pixels alone ≠ enroll rem (need TA enrolling...) */
								if (pnz > 64)
									pr_info("qsee_fp: *** IMAGE PIXELS sample=%u nz=%u (pull) ***\n",
										sample, pnz);
							}
							/*
							 * M4bc: no post-CAP force-image when !ta_down.
							 * Idle force thrashes SPI (configure spi mode
							 * fail / ESD) and burns the window before the
							 * next finger press. Force only in PRE-FORCE
							 * under finger above.
							 */
							/*
							 * M4bd: no synthetic feed under finger —
							 * Android never injects fake frames;
							 * synth confuses enroll and never yields rem.
							 */

							send_ion(id_dev, sess, SESS_BIG, 0x1003, 0,
								 NULL, "dev-LOGCAT-post-cap");
							if (last_send_frame_raw) {
								pr_info("qsee_fp: SAMPLE_MARK sample=%u frame=%d sens=%d irdy=%d enroll=%d\n",
									sample, last_send_frame_raw,
									last_send_sensor_raw,
									last_send_image_ready,
									last_send_enrolling);
								rem_seen = true;
								got_enroll = true;
							}

							evt = (u8 *)sess + 0x10;
							memset(evt, 0, 0x2e0);
							if (ta_down)
								*(__le32 *)evt = cpu_to_le32(0x2);
							send_ion(id_dev, sess, SESS_BIG,
								 FF_TA_REPORT_EVENT, 0x2e0, evt,
								 "dev-REPORT-enroll");
							if (ta_down) {
								memset(evt, 0, 0x2e0);
								*(__le32 *)evt = cpu_to_le32(0x2);
								send_ion(id_dev, sess, SESS_BIG,
									 FF_TA_SAVE_DATA, 0x2e0,
									 evt, "dev-SAVE-enroll");
							}

							send_ion(id_dev, sess, SESS_BIG, 0x1003, 0,
								 NULL, "dev-LOGCAT-enroll-sample");
							{
								const char *scan =
									(const char *)sess;
								size_t si;

								for (si = 0; si + 18 < 0x8000; si++) {
									if (!strncmp(scan + si,
										     "frame raw", 9) ||
									    !strncmp(scan + si,
										     "enrolling...", 12) ||
									    !strncmp(scan + si,
										     "rem = ", 6) ||
									    !strncmp(scan + si,
										     "enrolled fid", 12) ||
									    !strncmp(scan + si,
										     "got finger touched",
										     18) ||
									    !strncmp(scan + si,
										     "got image ready",
										     15) ||
									    !strncmp(scan + si,
										     "sensor raw data",
										     15) ||
									    !strncmp(scan + si,
										     "avgv =", 6)) {
										pr_info("qsee_fp: SAMPLE_MARK sample=%u '%.56s'\n",
											sample,
											scan + si);
										if (!strncmp(scan + si,
											     "enrolling...",
											     12) ||
										    !strncmp(scan + si,
											     "rem = ", 6) ||
										    !strncmp(scan + si,
											     "enrolled fid",
											     12) ||
										    !strncmp(scan + si,
											     "frame raw",
											     9) ||
										    !strncmp(scan + si,
											     "sensor raw data",
											     15) ||
										    !strncmp(scan + si,
											     "got image ready",
											     15)) {
											rem_seen = true;
											got_enroll = true;
										}
									}
								}
							}
							if (last_send_frame_raw ||
							    last_send_enrolling ||
							    last_send_sensor_raw || nz > 64) {
								pr_info("qsee_fp: SAMPLE_MARK sample=%u frame=%d sens=%d enroll=%d down=%d pix=%u try=%u\n",
									sample, last_send_frame_raw,
									last_send_sensor_raw,
									last_send_enrolling, ta_down,
									nz, bi);
								if (last_send_frame_raw ||
								    last_send_enrolling ||
								    last_send_sensor_raw)
									rem_seen = true;
								if (last_send_enrolling ||
								    last_send_frame_raw)
									got_enroll = true;
							}
							if (er == -22)
								break;
							if (ta_down && bi + 1 < ntry)
								msleep(40);
						}
					}

					if (er == -22) {
						pr_err("qsee_fp: CAP killed path at sample %u\n",
						       sample);
						break;
					}

					/*
					 * M4c7: Android FDT_UP only after successful finger sample
					 * (got rem). Force-CAP without finger must NOT FDT_UP —
					 * that disarms wait-touch and kills next IRQ (M4c6).
					 */
					if (overlay_empty || cap_hal_overlay) {
						pr_info("qsee_fp: overlay skip FDT_UP-post-sample (ev=6 already re-armed)\n");
					} else if (last_send_enrolling || last_send_finger_down) {
						memset(wm, 0, sizeof(wm));
						*(__le32 *)wm = cpu_to_le32(FF_WMODE_FDT_UP);
						send_ion(id_dev, sess, SESS_BIG,
							 FF_TA_CFG_WORK_MODE, 4, wm,
							 "dev-WMODE-FDT_UP-post-sample");
						msleep(20);
						if (ta_down && cap_hal_overlay) {
							pr_info("qsee_fp: overlay skip QEV-lift post-sample %u (keep TA)\n",
								sample);
						} else if (ta_down) {
							unsigned int lift_tw;

							pr_info("qsee_fp: sample %u — LIFT finger, then re-press for next\n",
								sample);
							for (lift_tw = 0; lift_tw < 20; lift_tw++) {
								msleep(100);
								send_ion(id_dev, sess, SESS_BIG,
									 FF_TA_QUERY_EVENT, 4, pay1,
									 "dev-QEV-lift");
								if (!last_send_finger_down)
									break;
							}
						}
					} else {
						pr_info("qsee_fp: sample %u stay FDT_DOWN (no rem — keep wait-touch armed)\n",
							sample);
						msleep(50);
					}
					/* Overlay idle SCAN-next Nulled the INIT image ptr.
					 * ev=6 already sent WMODE-FDT_DOWN-next.
					 */
					if (cap_hal_overlay) {
						pr_info("qsee_fp: skip SCAN-next (overlay, ev=6 re-armed)\n");
					} else {
						memset(wm, 0, sizeof(wm));
						*(__le32 *)wm = cpu_to_le32(FF_WMODE_FDT_DOWN);
						send_ion(id_dev, sess, SESS_BIG,
							 FF_TA_CFG_WORK_MODE, 4, wm,
							 "dev-WMODE-FDT_DOWN-next");
						msleep(15);
						memset(cap, 0, sizeof(cap));
						*(__le32 *)cap = cpu_to_le32(1);
						send_ion(id_dev, sess, SESS_BIG,
							 FF_TA_START_SCANNING, 0x30, cap,
							 "dev-SCAN-next");
						msleep(20);
					}
					if (sample_esd_clear && last_send_esd)
						esd_clear_loop(id_dev, sess, "post-sample", 1);
				}
				pr_info("qsee_fp: enroll sample loop done rem_seen=%d ta_enrolling=%d\n",
					rem_seen, ta_enrolling);
				if (rem_seen)
					g_keep_enrolled = true;

				/*
				 * M4cy: ENROLL after CAP samples if deferred — heap test.
				 * Rem for past frames may not apply; frame raw is the win.
				 */
				if (defer_enroll) {
					u8 hat_late[0x50];
					u64 ch2;

					send_ion(id_dev, sess, SESS_BIG, FF_TA_PRE_ENROLL_HL,
						 0, NULL, "dev-PRE_ENROLL-late");
					ch2 = le64_to_cpu(*(__le64 *)((u8 *)sess + 0x10));
					memset(hat_late, 0, sizeof(hat_late));
					hat_late[0] = 0;
					memcpy(hat_late + 1, &ch2, 8);
					hat_late[0x1c] = 2;
					hat_late[0x49] = 1;
					er = send_ion_seg(id_dev, sess, SESS_BIG,
							  FF_TA_ENROLL_HL, 0x4a, hat_late,
							  "dev-ENROLL-late");
					pr_info("qsee_fp: M4cy ENROLL-late er=%d err=0x%x rem_seen=%d\n",
						er, le32_to_cpu(*((__le32 *)sess + 2)),
						rem_seen);
				}

				/* Android POST only after rem=0. CANCEL after a
				 * quality-pass finger SCM-killed the TA (m4fs).
				 * Leave the TA; more samples need it alive.
				 */
				if (rem_seen) {
					/*
					 * SLEEP then Match FDT_DOWN never got itype 0x2
					 * (8s leftover 0x210/0x1/ESD). Keep enroll FDT.
					 */
					pr_info("qsee_fp: overlay skip POST after rem=0 (keep FDT for Match, no SLEEP)\n");
				} else if (ta_enrolling)
					pr_info("qsee_fp: skip POST+CANCEL (TA enrolling, rem not 0)\n");
				else
					pr_info("qsee_fp: skip POST+CANCEL (no rem — keep TA)\n");
				pr_info("qsee_fp: skip AUTH (Match is a separate press)\n");
enroll_done:
				;
			} else if (!skip_enroll) {
				pr_info("qsee_fp: skip_enroll=0 but no finger — not ENROLLing\n");
			} else {
				pr_info("qsee_fp: skip_enroll=1 — not sending ENROLL (keeps TA alive)\n");
			}

			er = send_ion(id_dev, sess, SESS_BIG, FF_TA_CHECK_HEALTH, 4, pay1,
				      "dev-HEALTH-final");
			pr_info("qsee_fp: HEALTH final er=%d (TA alive if 0)\n", er);
		}

	}

	if (g_keep_enrolled && sess && pool) {
		g_keep_sess = sess;
		g_keep_pool = pool;
		g_keep_id = id_dev;
		pr_info("qsee_fp: keep sess for Match AUTH kick id=%u\n", id_dev);
	} else {
		sess_shmbridge_unregister();
		qcom_tzmem_free(sess);
		qcom_tzmem_pool_free(pool);
	}
	pr_info("qsee_fp: device-TA probe+auth-path complete\n");
}

static void probe_fingerpr(u32 id_f32, u32 id_fp)
{
	/* Primary: focal32 = device trustlet; pass fingerpr as aux for AUTH tries. */
	if (id_f32)
		probe_device_ta(id_f32, id_fp);
	else if (id_fp)
		probe_device_ta(id_fp, 0);
}

static int __init qsee_fp_init(void)
{
	u32 id_f32 = 0, id_fp = 0;
	int ret;

	pr_info("qsee_fp: init\n");
	if (resolve())
		return 0;

	/*
	 * M4c0: START TAs *before* listeners. Registering FS/RPMB/SSD first
	 * was correlated with build_img/START -EIO after clean boot (tzmem
	 * pressure or ordering). Android qseecomd loads apps after listeners
	 * but with a full qseecom stack; mainline is happier START-first.
	 */
	if (force_f32_id)
		id_f32 = force_f32_id;
	if (force_fp_id)
		id_fp = force_fp_id;

	/*
	 * M4cu: reclaim fingerpr TZ RAM from prior holds when not using it.
	 * force_fp_id alone used to keep a ~10MB TA resident forever (rmmod
	 * does not SHUTDOWN). Android device path is focal32-only.
	 */
	if (force_fp_id && !start_fingerpr) {
		(void)app_shutdown((u32)force_fp_id, "M4cu-no-fingerpr");
		id_fp = 0;
	}

	/*
	 * M4du: rmmod does not free TEE mallocs. SHUTDOWN ids 1..20 (live
	 * START has been 0x4 and 0x10) then APP_START so CAPTURE is not a
	 * second 112×88 on top of this boot's INIT_DEV.
	 * If SHUTDOWN misses and START returns -22, attach leftover — do not
	 * drop the id and exit with "no TA ids" (that skipped PRESS/flash).
	 */
	/*
	 * Android qseecomd registers FS, RPMB, TIME, and SSD before any
	 * APP_START. A listener that is not registered makes qseecom return
	 * an error to the TA with no HLOS request, which is how the serial
	 * open became SFS_ERROR_GENERIC. GPFILE stays off this path.
	 */
	if (listeners_before_start &&
	    (enable_fs_listener || enable_rpmb_listener || enable_ssd_listener) &&
	    !enable_gpfile_listener)
		listeners_register_enabled("before APP_START");

	if (shutdown_before_start) {
		u32 id, saved = force_f32_id ? (u32)force_f32_id : 0;

		pr_info("qsee_fp: M4du SHUTDOWN ids 1..20 then APP_START\n");
		for (id = 1; id <= 20; id++)
			(void)app_shutdown(id, "M4du-heap");
		force_f32_id = 0;
		id_f32 = 0;
		do_start = true;
		/* SHUTDOWN leaves the IC cold (M4dg vio fail without this). */
		l2_vdd_cycle("M4du-heap");
		l2_reset_ic("M4du-heap");
		ret = app_start("focal32", &id_f32);
		if (ret || !id_f32) {
			id_f32 = saved ? saved : 4;
			do_start = false;
			force_f32_id = (int)id_f32;
			pr_info("qsee_fp: M4du START failed ret=%d — attach f32=%u\n",
				ret, id_f32);
		} else {
			pr_info("qsee_fp: M4du fresh START id=%u\n", id_f32);
		}
	}

	if (do_start) {
		/* Device trustlet is focal32 (Android trustlet_path); load first */
		if (!id_f32) {
			ret = app_start("focal32", &id_f32);
			if (ret)
				pr_err("qsee_fp: START focal32 %d\n", ret);
		}
		if (start_fingerpr && !id_fp) {
			ret = app_start("fingerpr", &id_fp);
			if (ret)
				pr_warn("qsee_fp: START fingerpr %d (device path may still work)\n", ret);
		} else if (!start_fingerpr) {
			pr_info("qsee_fp: M4cu skip START fingerpr (focal32-only heap)\n");
		}
	}
	if (probe_level == 1) {
		pr_info("qsee_fp: probe_level=1 START only f32=%u fp=%u\n", id_f32, id_fp);
		return 0;
	}

	/*
	 * M4c0: START already ran. Default defer_listeners registers after
	 * INIT Base (inside probe_device_ta) so ENROLL/REPORT can SFS.
	 * defer_listeners=0 registers now, before INIT (heap risk).
	 */
	if (defer_listeners)
		pr_info("qsee_fp: M4dj defer listeners until after device probe\n");
	else
		listeners_register_enabled("after START, before INIT");

	if (probe_level == 2) {
		pr_info("qsee_fp: probe_level=2 listener-only done\n");
		return 0;
	}
	if (!id_f32 && !id_fp) {
		pr_err("qsee_fp: no TA ids\n");
		return 0;
	}
	if (probe_level == 3) {
		/* Minimal ion send to test blocked-listener path without full probe */
		struct qcom_tzmem_pool *pool;
		struct qcom_tzmem_pool_config cfg = {
			.initial_size = SESS_BIG + SZ_1M,
			.policy = QCOM_TZMEM_POLICY_ON_DEMAND,
			.increment = SZ_1M,
			.max_size = SESS_BIG + SZ_4M,
		};
		void *sess;
		u8 ver_pay[4] = { 2, 0, 0, 0 };
		u32 id = id_f32 ? id_f32 : id_fp;

		pool = qcom_tzmem_pool_new(&cfg);
		if (IS_ERR(pool)) {
			pr_err("qsee_fp: level3 pool %ld\n", PTR_ERR(pool));
			return 0;
		}
		sess = qcom_tzmem_alloc(pool, SESS_BIG, GFP_KERNEL);
		if (!sess) {
			qcom_tzmem_pool_free(pool);
			return 0;
		}
		pr_info("qsee_fp: probe_level=3 single GET_VERSION id=%u\n", id);
		ret = send_ion(id, sess, SESS_BIG, FF_SVC_GET_VERSION, 4, ver_pay, "lvl3-205");
		pr_info("qsee_fp: level3 send_ion ret=%d\n", ret);
		sess_shmbridge_unregister();
		qcom_tzmem_free(sess);
		qcom_tzmem_pool_free(pool);
		return 0;
	}
	probe_fingerpr(id_f32, id_fp);

	/* Safety net if Base path skipped the after-Base register. */
	if (defer_listeners)
		listeners_register_enabled("post-probe");

	pr_info("qsee_fp: done\n");
	return 0;
}

static void __exit qsee_fp_exit(void)
{
	sess_shmbridge_unregister();
	listener_cleanup();
	pr_info("qsee_fp: exit\n");
}

module_init(qsee_fp_init);
module_exit(qsee_fp_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Fingerpr TA open attempts on FP5 pmOS");
