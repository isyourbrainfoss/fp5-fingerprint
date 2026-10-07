// SPDX-License-Identifier: GPL-2.0-only
/*
 * FP5 lab: force-enable mainline QSEECOM client path and probe TZ apps.
 *
 * Stock boot skips qseecom on untested machines (allowlist). Version query
 * already succeeds on FP5 — we only need the platform device + lookups.
 *
 * Does NOT load fingerpr/focal32 yet (mainline has LOOKUP+SEND, not Android
 * start_app). Positive step toward session unlock.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/firmware/qcom/qcom_scm.h>

static bool force_pdev = true;
module_param(force_pdev, bool, 0644);
MODULE_PARM_DESC(force_pdev, "Register qcom_qseecom platform device (default true)");

static struct platform_device *g_qsee_pdev;

/* Names used by Android FP stack and known mainline clients */
static const char *const probe_apps[] = {
	"qcom.tz.uefisecapp",
	"uefisecapp",
	"fingerpr",
	"focal32",
	"fingerprint",
	"keymaster64",
	"keymaster",
	"widevine",
	"rtic",
	"smplap64",
	"qseecom_sample",
	NULL,
};

static void probe_one(const char *name)
{
	u32 app_id = 0;
	int ret;

	ret = qcom_scm_qseecom_app_get_id(name, &app_id);
	if (ret == -ENOENT)
		pr_info("qsee_probe_fp5: LOOKUP %-20s -> not loaded (-ENOENT)\n", name);
	else if (ret)
		pr_info("qsee_probe_fp5: LOOKUP %-20s -> err %d\n", name, ret);
	else
		pr_info("qsee_probe_fp5: LOOKUP %-20s -> app_id=0x%x (%u) OK\n",
			name, app_id, app_id);
}

static int __init qsee_probe_init(void)
{
	int i;

	pr_info("qsee_probe_fp5: starting (force_pdev=%d)\n", force_pdev);

	if (force_pdev) {
		/*
		 * Mirror qcom_scm_qseecom_init() after allowlist: create the
		 * software platform device so qcom_qseecom.ko can probe.
		 */
		g_qsee_pdev = platform_device_alloc("qcom_qseecom", PLATFORM_DEVID_NONE);
		if (!g_qsee_pdev) {
			pr_err("qsee_probe_fp5: platform_device_alloc failed\n");
			return -ENOMEM;
		}
		/* Parent stays NULL; SCM is global. Mainline uses scm as parent. */
		if (platform_device_add(g_qsee_pdev)) {
			pr_err("qsee_probe_fp5: platform_device_add failed\n");
			platform_device_put(g_qsee_pdev);
			g_qsee_pdev = NULL;
			return -ENODEV;
		}
		pr_info("qsee_probe_fp5: registered platform device qcom_qseecom\n");
	}

	pr_info("qsee_probe_fp5: --- TZ app lookup (already-resident apps only) ---\n");
	for (i = 0; probe_apps[i]; i++)
		probe_one(probe_apps[i]);
	pr_info("qsee_probe_fp5: --- lookup done ---\n");
	pr_info("qsee_probe_fp5: next: modprobe qcom_qseecom; check dmesg for uefisecapp client\n");

	return 0;
}

static void __exit qsee_probe_exit(void)
{
	if (g_qsee_pdev) {
		platform_device_unregister(g_qsee_pdev);
		g_qsee_pdev = NULL;
		pr_info("qsee_probe_fp5: unregistered qcom_qseecom pdev\n");
	}
}

module_init(qsee_probe_init);
module_exit(qsee_probe_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("FP5 lab");
MODULE_DESCRIPTION("Force QSEECOM pdev + TZ app lookup on Fairphone 5 pmOS");
MODULE_VERSION("0.1");
