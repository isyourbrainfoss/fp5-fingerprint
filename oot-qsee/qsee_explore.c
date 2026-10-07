// SPDX-License-Identifier: GPL-2.0-only
/*
 * Explore focal32/fingerpr after APP_START:
 *  - full version dump (0x205/0x2)
 *  - scan third dword as subcommand
 *  - fingerpr probes
 * Optional: power L2 FP (external — load focaltech_fp_life separately)
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/kprobes.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/firmware/qcom/qcom_tzmem.h>
#include <linux/string.h>
#include <linux/sizes.h>
#include <linux/delay.h>

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

#define QSEECOM_RESULT_SUCCESS 0
#define QSEECOM_TZ_OWNER_QSEE_OS 50
#define QSEECOM_TZ_SVC_APP_MGR 1
#define QSEECOM_TZ_CMD_APP_START 1
#define REQ_LEN 0x40
#define RSP_LEN 0x800

static char *ta_dir = "/lib/firmware/qsee";
module_param(ta_dir, charp, 0644);
static bool start_tas = true;
module_param(start_tas, bool, 0644);
static int force_f32_id = -1;
module_param(force_f32_id, int, 0644);
static int force_fp_id = -1;
module_param(force_fp_id, int, 0644);

typedef unsigned long (*kln_t)(const char *);
typedef int (*qcall_t)(const struct qcom_scm_desc *, struct qcom_scm_qseecom_resp *);
static kln_t kln;
static qcall_t qcall;

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

	if (IS_ERR(f))
		return PTR_ERR(f);
	sz = i_size_read(file_inode(f));
	if (sz <= 0 || sz > 32 << 20) {
		filp_close(f, NULL);
		return -EINVAL;
	}
	b = kvmalloc(sz, GFP_KERNEL);
	if (!b) {
		filp_close(f, NULL);
		return -ENOMEM;
	}
	n = kernel_read(f, b, sz, &f->f_pos);
	filp_close(f, NULL);
	if (n != sz) {
		kvfree(b);
		return -EIO;
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
	if (ret)
		return ret;
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
	pr_info("qsee_ex: START %s ret=%d res=0x%llx id=0x%llx\n",
		name, ret, r.result, r.data);
	qcom_tzmem_free(tz);
	qcom_tzmem_pool_free(pool);
	if (ret || r.result != QSEECOM_RESULT_SUCCESS)
		return ret ? ret : -EIO;
	*id = (u32)r.data;
	return 0;
}

static void dump_rsp(const char *tag, const u8 *p, size_t n)
{
	char line[200];
	char ascii[128];
	size_t i, j = 0, k;
	char *o;

	/* hex chunks of 32 */
	for (i = 0; i < n; i += 32) {
		size_t chunk = min_t(size_t, 32, n - i);

		o = line;
		for (k = 0; k < chunk; k++)
			o += scnprintf(o, 4, "%02x ", p[i + k]);
		pr_info("qsee_ex: %s[%03zu]: %s\n", tag, i, line);
	}
	/* extract C strings */
	for (i = 0; i < n && j < sizeof(ascii) - 1; ) {
		if (p[i] >= 0x20 && p[i] < 0x7f) {
			size_t s = i;

			while (i < n && p[i] >= 0x20 && p[i] < 0x7f)
				i++;
			if (i - s >= 3) {
				size_t len = min_t(size_t, i - s, sizeof(ascii) - 1 - j);

				if (j)
					ascii[j++] = '|';
				memcpy(ascii + j, p + s, len);
				j += len;
				ascii[j] = 0;
			}
		} else {
			i++;
		}
	}
	if (j)
		pr_info("qsee_ex: %s strings: %s\n", tag, ascii);
}

static int send4(u32 app_id, const char *tag, u32 w0, u32 w1, u32 w2, u32 w3,
		 bool dump_full)
{
	struct qcom_tzmem_pool_config cfg = {
		.initial_size = SZ_64K,
		.policy = QCOM_TZMEM_POLICY_STATIC,
		.max_size = SZ_64K,
	};
	struct qcom_tzmem_pool *pool;
	void *req, *rsp;
	int ret;
	u32 st;

	pool = qcom_tzmem_pool_new(&cfg);
	if (IS_ERR(pool))
		return PTR_ERR(pool);
	req = qcom_tzmem_alloc(pool, REQ_LEN, GFP_KERNEL);
	rsp = qcom_tzmem_alloc(pool, RSP_LEN, GFP_KERNEL);
	if (!req || !rsp) {
		if (req)
			qcom_tzmem_free(req);
		if (rsp)
			qcom_tzmem_free(rsp);
		qcom_tzmem_pool_free(pool);
		return -ENOMEM;
	}
	memset(req, 0, REQ_LEN);
	memset(rsp, 0, RSP_LEN);
	*(__le32 *)req = cpu_to_le32(w0);
	*((__le32 *)req + 1) = cpu_to_le32(w1);
	*((__le32 *)req + 2) = cpu_to_le32(w2);
	*((__le32 *)req + 3) = cpu_to_le32(w3);

	ret = qcom_scm_qseecom_app_send(app_id, req, REQ_LEN, rsp, RSP_LEN);
	st = le32_to_cpu(*(__le32 *)rsp);
	pr_info("qsee_ex: SEND %s app=%u ret=%d st=0x%x (%u) hdr=%08x %08x %08x %08x\n",
		tag, app_id, ret, st, st, w0, w1, w2, w3);

	if (dump_full || (ret == 0 && st != 0xffffffff && st != 0xde))
		dump_rsp(tag, rsp, dump_full ? 256 : 96);

	qcom_tzmem_free(req);
	qcom_tzmem_free(rsp);
	qcom_tzmem_pool_free(pool);
	return ret;
}

static void explore_app(u32 app_id, const char *name)
{
	u32 sub;
	static const u32 interesting[] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
		17, 18, 19, 20, 21, 22, 32, 33, 34, 35, 36, 38, 40, 43, 44,
		0x100, 0x200, 0x205, 0x1000, 0x1011,
	};
	unsigned int i;

	pr_info("qsee_ex: ===== explore %s id=%u =====\n", name, app_id);

	/* Full version banner */
	send4(app_id, "ver", 0x205, 0x2, 0, 0, true);

	/* Scan subcommand in w2 with fixed SO header */
	for (i = 0; i < ARRAY_SIZE(interesting); i++) {
		char t[32];

		sub = interesting[i];
		scnprintf(t, sizeof(t), "%s-w2=%u", name, sub);
		send4(app_id, t, 0x205, 0x2, sub, 0, false);
		cond_resched();
	}

	/* Scan w1 with w0=0x205 */
	for (sub = 0; sub <= 8; sub++) {
		char t[32];

		scnprintf(t, sizeof(t), "%s-w1=%u", name, sub);
		send4(app_id, t, 0x205, sub, 0, 0, false);
	}

	/* Scan w0 alone */
	{
		u32 tops[] = { 0, 1, 2, 0x100, 0x200, 0x205, 0x1011, 0x10000 };

		for (i = 0; i < ARRAY_SIZE(tops); i++) {
			char t[32];

			scnprintf(t, sizeof(t), "%s-w0=%x", name, tops[i]);
			send4(app_id, t, tops[i], 0, 0, 0, false);
		}
	}
}

static int __init qsee_ex_init(void)
{
	u32 id_f32 = 0, id_fp = 0;
	int ret;

	pr_info("qsee_ex: init start_tas=%d force_f32=%d force_fp=%d\n",
		start_tas, force_f32_id, force_fp_id);
	if (resolve()) {
		pr_err("qsee_ex: resolve failed\n");
		return 0;
	}

	if (force_f32_id >= 0)
		id_f32 = force_f32_id;
	if (force_fp_id >= 0)
		id_fp = force_fp_id;

	if (start_tas) {
		ret = app_start("focal32", &id_f32);
		if (ret)
			pr_err("qsee_ex: focal32 START %d — try force_*_id or reboot\n", ret);
		ret = app_start("fingerpr", &id_fp);
		if (ret)
			pr_err("qsee_ex: fingerpr START %d\n", ret);
	}

	if (id_f32)
		explore_app(id_f32, "f32");
	if (id_fp)
		explore_app(id_fp, "fp");

	/* Also try id 4/5 if forces/start missed */
	if (!id_f32 && !id_fp) {
		pr_info("qsee_ex: probing ids 4 and 5 blindly\n");
		explore_app(4, "id4");
		explore_app(5, "id5");
	}

	pr_info("qsee_ex: done f32=%u fp=%u\n", id_f32, id_fp);
	return 0;
}

static void __exit qsee_ex_exit(void)
{
	pr_info("qsee_ex: exit\n");
}

module_init(qsee_ex_init);
module_exit(qsee_ex_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Explore FocalTech QSEE TA command surface on FP5");
