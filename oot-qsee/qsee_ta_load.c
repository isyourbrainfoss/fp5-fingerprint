// SPDX-License-Identifier: GPL-2.0-only
/*
 * Load QSEE TAs (fingerpr / focal32) on FP5 mainline via APP_START SCM.
 *
 * Mainline only exports LOOKUP+SEND; APP_START is invoked by resolving the
 * static qcom_scm_qseecom_call() through kallsyms (lab technique).
 *
 * Image layout (Android QSEECom convention):
 *   buf = mdt || b00 || b01 || ... || b07
 *   mdt_len = sizeof(mdt), img_len = sizeof(buf)
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/uaccess.h>
#include <linux/kprobes.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/firmware/qcom/qcom_tzmem.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/sizes.h>

/* Mirror drivers/firmware/qcom/qcom_scm.h (not UAPI) */
#define MAX_QCOM_SCM_ARGS 10
enum qcom_scm_arg_types {
	QCOM_SCM_VAL = 0,
	QCOM_SCM_RO = 1,
	QCOM_SCM_RW = 2,
	QCOM_SCM_BUFVAL = 3,
};
#define QCOM_SCM_ARGS_IMPL(num, a, b, c, d, e, f, g, h, i, j, ...) (\
	(((a) & 0x3) << 4) | (((b) & 0x3) << 6) | (((c) & 0x3) << 8) | \
	(((d) & 0x3) << 10) | (((e) & 0x3) << 12) | (((f) & 0x3) << 14) | \
	(((g) & 0x3) << 16) | (((h) & 0x3) << 18) | (((i) & 0x3) << 20) | \
	(((j) & 0x3) << 22) | ((num) & 0xf))
#define QCOM_SCM_ARGS(...) QCOM_SCM_ARGS_IMPL(__VA_ARGS__, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)

struct qcom_scm_desc {
	u32 svc;
	u32 cmd;
	u32 arginfo;
	u64 args[MAX_QCOM_SCM_ARGS];
	u32 owner;
};

struct qcom_scm_qseecom_resp {
	u64 result;
	u64 resp_type;
	u64 data;
};

#define QSEECOM_RESULT_SUCCESS		0
#define QSEECOM_RESULT_INCOMPLETE	1
#define QSEECOM_RESULT_BLOCKED_ON_LISTENER 2
#define QSEECOM_RESULT_FAILURE		0xFFFFFFFFULL

#define QSEECOM_TZ_OWNER_QSEE_OS	50
#define QSEECOM_TZ_SVC_APP_MGR		1
#define QSEECOM_TZ_CMD_APP_START	1
#define QSEECOM_TZ_CMD_APP_LOOKUP	3

static char *ta_dir = "/lib/firmware/qsee";
module_param(ta_dir, charp, 0644);
MODULE_PARM_DESC(ta_dir, "Directory with <name>.mdt and .b00-.b07");

static char *ta_name = "fingerpr";
module_param(ta_name, charp, 0644);
MODULE_PARM_DESC(ta_name, "TA base name (fingerpr or focal32)");

typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);
typedef int (*qseecom_call_t)(const struct qcom_scm_desc *desc,
			      struct qcom_scm_qseecom_resp *res);
typedef struct qcom_tzmem_pool *(*get_tzmem_pool_t)(void);

static kallsyms_lookup_name_t kln;
static qseecom_call_t qseecom_call;
static get_tzmem_pool_t get_tzmem_pool;

static int resolve_kallsyms(void)
{
	struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };
	int ret;

	ret = register_kprobe(&kp);
	if (ret) {
		pr_err("qsee_ta_load: register_kprobe kallsyms_lookup_name: %d\n", ret);
		return ret;
	}
	kln = (kallsyms_lookup_name_t)kp.addr;
	unregister_kprobe(&kp);
	if (!kln)
		return -ENOENT;

	qseecom_call = (qseecom_call_t)kln("qcom_scm_qseecom_call");
	get_tzmem_pool = (get_tzmem_pool_t)kln("qcom_scm_get_tzmem_pool");

	pr_info("qsee_ta_load: kln=%ps qseecom_call=%ps get_pool=%ps\n",
		kln, qseecom_call, get_tzmem_pool);

	if (!qseecom_call)
		return -ENOENT;
	return 0;
}

static int read_file_to_buf(const char *path, u8 **out, size_t *out_len)
{
	struct file *f;
	loff_t size;
	ssize_t n;
	u8 *buf;

	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);

	size = i_size_read(file_inode(f));
	if (size <= 0 || size > 32 * 1024 * 1024) {
		filp_close(f, NULL);
		return -EINVAL;
	}
	buf = kvmalloc(size, GFP_KERNEL);
	if (!buf) {
		filp_close(f, NULL);
		return -ENOMEM;
	}
	n = kernel_read(f, buf, size, &f->f_pos);
	filp_close(f, NULL);
	if (n != size) {
		kvfree(buf);
		return -EIO;
	}
	*out = buf;
	*out_len = size;
	return 0;
}

/* Concatenate mdt + b00..b07 into one buffer */
static int build_ta_image(const char *dir, const char *name,
			  u8 **img_out, size_t *img_len_out, size_t *mdt_len_out)
{
	char path[256];
	u8 *parts[9] = {};
	size_t plens[9] = {};
	size_t total = 0, mdt_len = 0;
	u8 *img;
	size_t off;
	int i, ret;

	/* mdt */
	snprintf(path, sizeof(path), "%s/%s.mdt", dir, name);
	ret = read_file_to_buf(path, &parts[0], &plens[0]);
	if (ret) {
		pr_err("qsee_ta_load: read %s failed: %d\n", path, ret);
		return ret;
	}
	mdt_len = plens[0];
	total = mdt_len;

	for (i = 0; i < 8; i++) {
		snprintf(path, sizeof(path), "%s/%s.b0%d", dir, name, i);
		ret = read_file_to_buf(path, &parts[i + 1], &plens[i + 1]);
		if (ret) {
			pr_err("qsee_ta_load: read %s failed: %d\n", path, ret);
			goto err_parts;
		}
		total += plens[i + 1];
	}

	img = kvmalloc(total, GFP_KERNEL);
	if (!img) {
		ret = -ENOMEM;
		goto err_parts;
	}
	off = 0;
	for (i = 0; i < 9; i++) {
		memcpy(img + off, parts[i], plens[i]);
		off += plens[i];
		kvfree(parts[i]);
		parts[i] = NULL;
	}

	pr_info("qsee_ta_load: built %s image total=%zu mdt_len=%zu\n",
		name, total, mdt_len);
	*img_out = img;
	*img_len_out = total;
	*mdt_len_out = mdt_len;
	return 0;

err_parts:
	for (i = 0; i < 9; i++)
		kvfree(parts[i]);
	return ret;
}

/* Indirect call into static SCM (kallsyms) — bypass kCFI for this only. */
static noinline __attribute__((no_sanitize("kcfi")))
int invoke_qseecom_call(const struct qcom_scm_desc *desc,
			struct qcom_scm_qseecom_resp *res)
{
	return qseecom_call(desc, res);
}

static int do_app_start(const u8 *img, size_t img_len, size_t mdt_len,
			const char *name)
{
	struct qcom_tzmem_pool *pool;
	/* Private pool sized for full TA (global SCM pool is too small). */
	struct qcom_tzmem_pool_config pcfg = {
		.initial_size = ALIGN(img_len + SZ_1M, SZ_2M),
		.policy = QCOM_TZMEM_POLICY_ON_DEMAND,
		.increment = SZ_1M,
		.max_size = ALIGN(img_len + SZ_4M, SZ_2M),
	};
	void *tzbuf;
	phys_addr_t pa;
	struct qcom_scm_desc desc = {};
	struct qcom_scm_qseecom_resp res = {};
	u32 app_id = 0;
	int ret;
	bool own_pool = true;

	pool = qcom_tzmem_pool_new(&pcfg);
	if (IS_ERR(pool)) {
		pr_err("qsee_ta_load: tzmem_pool_new(%zu): %ld\n",
		       pcfg.initial_size, PTR_ERR(pool));
		/* fallback: try global pool (may only work for tiny images) */
		if (get_tzmem_pool) {
			pool = get_tzmem_pool();
			own_pool = false;
			pr_info("qsee_ta_load: falling back to scm global pool\n");
		}
		if (!pool || IS_ERR(pool))
			return IS_ERR(pool) ? PTR_ERR(pool) : -ENOMEM;
	} else {
		pr_info("qsee_ta_load: private tzmem pool initial=%zu max=%zu\n",
			pcfg.initial_size, pcfg.max_size);
	}

	tzbuf = qcom_tzmem_alloc(pool, img_len, GFP_KERNEL);
	if (!tzbuf) {
		pr_err("qsee_ta_load: tzmem_alloc %zu failed\n", img_len);
		if (own_pool)
			qcom_tzmem_pool_free(pool);
		return -ENOMEM;
	}
	memcpy(tzbuf, img, img_len);
	pa = qcom_tzmem_to_phys(tzbuf);

	/* Optional: LOOKUP first */
	ret = qcom_scm_qseecom_app_get_id(name, &app_id);
	if (!ret) {
		pr_info("qsee_ta_load: %s already loaded app_id=0x%x — skip START\n",
			name, app_id);
		goto out_free;
	}
	pr_info("qsee_ta_load: LOOKUP %s pre-start: %d (expected -ENOENT)\n", name, ret);

	/* APP_START: owner QSEE_OS, svc APP_MGR, cmd 1; args mdt_len, img_len, pa */
	desc.owner = QSEECOM_TZ_OWNER_QSEE_OS;
	desc.svc = QSEECOM_TZ_SVC_APP_MGR;
	desc.cmd = QSEECOM_TZ_CMD_APP_START;
	desc.arginfo = QCOM_SCM_ARGS(3, QCOM_SCM_VAL, QCOM_SCM_VAL, QCOM_SCM_VAL);
	desc.args[0] = mdt_len;
	desc.args[1] = img_len;
	desc.args[2] = pa;

	pr_info("qsee_ta_load: APP_START %s mdt=%zu img=%zu pa=0x%llx\n",
		name, mdt_len, img_len, (u64)pa);

	ret = invoke_qseecom_call(&desc, &res);
	pr_info("qsee_ta_load: APP_START scm ret=%d result=0x%llx type=0x%llx data=0x%llx\n",
		ret, res.result, res.resp_type, res.data);

	if (ret)
		goto out_free;

	if (res.result == QSEECOM_RESULT_INCOMPLETE ||
	    res.result == QSEECOM_RESULT_BLOCKED_ON_LISTENER) {
		pr_warn("qsee_ta_load: incomplete/blocked — listener support needed\n");
		ret = -EAGAIN;
		goto out_free;
	}
	if (res.result == QSEECOM_RESULT_FAILURE) {
		pr_err("qsee_ta_load: TZ RESULT_FAILURE\n");
		ret = -EIO;
		goto out_free;
	}
	if (res.result != QSEECOM_RESULT_SUCCESS) {
		pr_err("qsee_ta_load: unexpected result 0x%llx\n", res.result);
		ret = -EIO;
		goto out_free;
	}

	/* data often holds app_id on success */
	pr_info("qsee_ta_load: APP_START success, data/app_id=0x%llx\n", res.data);

	msleep(50);
	ret = qcom_scm_qseecom_app_get_id(name, &app_id);
	if (!ret)
		pr_info("qsee_ta_load: POST LOOKUP %s -> app_id=0x%x YES\n", name, app_id);
	else
		pr_warn("qsee_ta_load: POST LOOKUP %s -> %d (may still need listeners)\n",
			name, ret);

out_free:
	qcom_tzmem_free(tzbuf);
	if (own_pool)
		qcom_tzmem_pool_free(pool);
	return ret;
}

static int __init qsee_ta_load_init(void)
{
	u8 *img = NULL;
	size_t img_len = 0, mdt_len = 0;
	int ret;

	pr_info("qsee_ta_load: init ta=%s dir=%s\n", ta_name, ta_dir);

	ret = resolve_kallsyms();
	if (ret)
		return ret;

	ret = build_ta_image(ta_dir, ta_name, &img, &img_len, &mdt_len);
	if (ret)
		return ret;

	ret = do_app_start(img, img_len, mdt_len, ta_name);
	kvfree(img);
	return ret; /* keep module loaded even on failure so dmesg is inspectable? 
		     * return 0 so module stays for re-lookup via other tools */
}

/* Keep module loaded for inspection regardless of load result */
static int __init qsee_ta_load_init_wrap(void)
{
	int ret = qsee_ta_load_init();

	if (ret)
		pr_err("qsee_ta_load: load attempt finished with %d (module stays)\n", ret);
	return 0;
}

static void __exit qsee_ta_load_exit(void)
{
	pr_info("qsee_ta_load: exit\n");
}

module_init(qsee_ta_load_init_wrap);
module_exit(qsee_ta_load_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("FP5 lab");
MODULE_DESCRIPTION("QSEE TA APP_START loader for fingerpr/focal32 on mainline");
MODULE_VERSION("0.1");
