// SPDX-License-Identifier: GPL-2.0-only
/*
 * FP5: Android-like QSEE session for fingerpr —
 * APP_START + REGION_NOTIFICATION + contiguous req/rsp SEND.
 *
 * Layout from fingerprint.default.so:
 *   session size ~0x80040 (or at least 0x840)
 *   SEND: req_len=0x40 @ base, rsp_len=0x800 @ base+0x40
 *   req prefix: le32 0x205, le32 0x2
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
enum qcom_scm_arg_types {
	QCOM_SCM_VAL = 0, QCOM_SCM_RO = 1, QCOM_SCM_RW = 2, QCOM_SCM_BUFVAL = 3,
};
#define QCOM_SCM_ARGS_IMPL(num, a, b, c, d, e, f, g, h, i, j, ...) (\
	(((a) & 0x3) << 4) | (((b) & 0x3) << 6) | (((c) & 0x3) << 8) | \
	(((d) & 0x3) << 10) | (((e) & 0x3) << 12) | (((f) & 0x3) << 14) | \
	(((g) & 0x3) << 16) | (((h) & 0x3) << 18) | (((i) & 0x3) << 20) | \
	(((j) & 0x3) << 22) | ((num) & 0xf))
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
#define QSEECOM_RESULT_FAILURE		0xFFFFFFFFULL

#define QSEECOM_TZ_OWNER_QSEE_OS	50
#define QSEECOM_TZ_OWNER_TZ_APPS	48
#define QSEECOM_TZ_SVC_APP_MGR		1
#define QSEECOM_TZ_SVC_APP_ID_PLACEHOLDER 0
#define QSEECOM_TZ_CMD_APP_START	1
#define QSEECOM_TZ_CMD_APP_LOOKUP	3
#define QSEECOM_TZ_CMD_APP_REGION_NOTIFICATION 5
#define QSEECOM_TZ_CMD_APP_SEND		1

/* Android SO sizes */
#define SESS_SIZE	0x80040
#define REQ_OFF		0
#define REQ_LEN		0x40
#define RSP_OFF		0x40
#define RSP_LEN		0x800

static char *ta_dir = "/lib/firmware/qsee";
module_param(ta_dir, charp, 0644);
static char *ta_name = "fingerpr";
module_param(ta_name, charp, 0644);
static bool do_region_notify = true;
module_param(do_region_notify, bool, 0644);

typedef unsigned long (*kln_t)(const char *n);
typedef int (*qcall_t)(const struct qcom_scm_desc *d, struct qcom_scm_qseecom_resp *r);
static kln_t kln;
static qcall_t qcall_fn;

static noinline __attribute__((no_sanitize("kcfi")))
int inv(const struct qcom_scm_desc *d, struct qcom_scm_qseecom_resp *r)
{
	return qcall_fn(d, r);
}

static int resolve(void)
{
	struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };
	int ret = register_kprobe(&kp);

	if (ret)
		return ret;
	kln = (kln_t)kp.addr;
	unregister_kprobe(&kp);
	qcall_fn = (qcall_t)kln("qcom_scm_qseecom_call");
	return qcall_fn ? 0 : -ENOENT;
}

static int read_file(const char *path, u8 **out, size_t *olen)
{
	struct file *f;
	loff_t sz;
	u8 *b;
	ssize_t n;

	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);
	sz = i_size_read(file_inode(f));
	if (sz <= 0 || sz > 32 * 1024 * 1024) {
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

static int app_start(const char *name, u32 *app_id)
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
	pr_info("qsee_sess: START %s ret=%d res=0x%llx type=0x%llx data=0x%llx\n",
		name, ret, r.result, r.resp_type, r.data);

	qcom_tzmem_free(tz);
	qcom_tzmem_pool_free(pool);

	if (ret || r.result != QSEECOM_RESULT_SUCCESS)
		return ret ? ret : -EIO;
	*app_id = (u32)r.data;
	return 0;
}

static int region_notify(phys_addr_t addr, size_t size)
{
	struct qcom_scm_desc d = {};
	struct qcom_scm_qseecom_resp r = {};
	int ret;

	d.owner = QSEECOM_TZ_OWNER_QSEE_OS;
	d.svc = QSEECOM_TZ_SVC_APP_MGR;
	d.cmd = QSEECOM_TZ_CMD_APP_REGION_NOTIFICATION;
	/* Android: VAL, VAL for addr, size */
	d.arginfo = QCOM_SCM_ARGS(2, QCOM_SCM_VAL, QCOM_SCM_VAL);
	d.args[0] = addr;
	d.args[1] = size;

	ret = inv(&d, &r);
	pr_info("qsee_sess: REGION_NOTIFY pa=0x%llx sz=0x%zx ret=%d res=0x%llx type=0x%llx data=0x%llx\n",
		(u64)addr, size, ret, r.result, r.resp_type, r.data);
	return ret;
}

static void dump_hex(const char *tag, const void *buf, size_t len)
{
	const u8 *p = buf;
	char line[128];
	size_t n = min_t(size_t, len, 32);
	size_t i;
	char *o = line;

	for (i = 0; i < n; i++)
		o += scnprintf(o, 4, "%02x ", p[i]);
	pr_info("qsee_sess: %s: %s\n", tag, line);
}

/* Contiguous session SEND: req at 0, rsp at 0x40 within one TZ alloc */
static int session_send(u32 app_id, void *session, const void *req_data, size_t req_fill)
{
	void *req = session + REQ_OFF;
	void *rsp = session + RSP_OFF;
	phys_addr_t req_pa, rsp_pa;
	struct qcom_scm_desc d = {};
	struct qcom_scm_qseecom_resp r = {};
	int ret;
	u32 st;

	memset(session, 0, SESS_SIZE);
	if (req_fill > REQ_LEN)
		req_fill = REQ_LEN;
	memcpy(req, req_data, req_fill);
	memset(rsp, 0xee, RSP_LEN);

	req_pa = qcom_tzmem_to_phys(req);
	rsp_pa = qcom_tzmem_to_phys(rsp);
	if (!req_pa || !rsp_pa) {
		/* Interior pointer might fail on some tzmem — try base+offset */
		phys_addr_t base_pa = qcom_tzmem_to_phys(session);

		if (!base_pa) {
			pr_err("qsee_sess: tzmem_to_phys failed req=%pa rsp=%pa base=%pa\n",
			       &req_pa, &rsp_pa, &base_pa);
			return -EFAULT;
		}
		req_pa = base_pa + REQ_OFF;
		rsp_pa = base_pa + RSP_OFF;
		pr_info("qsee_sess: using base_pa+off req=0x%llx rsp=0x%llx\n",
			(u64)req_pa, (u64)rsp_pa);
	}

	d.owner = QSEECOM_TZ_OWNER_TZ_APPS;
	d.svc = QSEECOM_TZ_SVC_APP_ID_PLACEHOLDER;
	d.cmd = QSEECOM_TZ_CMD_APP_SEND;
	d.arginfo = QCOM_SCM_ARGS(5, QCOM_SCM_VAL, QCOM_SCM_RW, QCOM_SCM_VAL,
				  QCOM_SCM_RW, QCOM_SCM_VAL);
	d.args[0] = app_id;
	d.args[1] = req_pa;
	d.args[2] = REQ_LEN;
	d.args[3] = rsp_pa;
	d.args[4] = RSP_LEN;

	ret = inv(&d, &r);
	st = le32_to_cpu(*(__le32 *)rsp);
	pr_info("qsee_sess: SEND ret=%d scm_res=0x%llx type=0x%llx data=0x%llx app_st=0x%x\n",
		ret, r.result, r.resp_type, r.data, st);
	dump_hex("req", req, 16);
	dump_hex("rsp", rsp, 32);
	return ret;
}

static int run_session(u32 app_id)
{
	struct qcom_tzmem_pool *pool;
	struct qcom_tzmem_pool_config cfg = {
		.initial_size = SESS_SIZE + SZ_1M,
		.policy = QCOM_TZMEM_POLICY_ON_DEMAND,
		.increment = SZ_1M,
		.max_size = SESS_SIZE + SZ_2M,
	};
	void *session;
	phys_addr_t spa;
	u8 hdr[REQ_LEN];
	int ret;

	pool = qcom_tzmem_pool_new(&cfg);
	if (IS_ERR(pool))
		return PTR_ERR(pool);

	session = qcom_tzmem_alloc(pool, SESS_SIZE, GFP_KERNEL);
	if (!session) {
		pr_err("qsee_sess: alloc session 0x%x failed\n", SESS_SIZE);
		qcom_tzmem_pool_free(pool);
		return -ENOMEM;
	}
	memset(session, 0, SESS_SIZE);
	spa = qcom_tzmem_to_phys(session);
	pr_info("qsee_sess: session v=%p pa=0x%llx size=0x%x\n",
		session, (u64)spa, SESS_SIZE);

	if (do_region_notify && spa)
		region_notify(spa, SESS_SIZE);

	/* Probe 1: Android SO header only */
	memset(hdr, 0, sizeof(hdr));
	*(__le32 *)hdr = cpu_to_le32(0x205);
	*((__le32 *)hdr + 1) = cpu_to_le32(0x2);
	pr_info("qsee_sess: --- probe SO header 0x205/0x2 ---\n");
	session_send(app_id, session, hdr, 8);

	/* Probe 2: header + cmd word at offset 8 */
	{
		u32 cmds[] = { 0, 1, 4, 10, 22, 43, 44 };
		unsigned int i;

		for (i = 0; i < ARRAY_SIZE(cmds); i++) {
			memset(hdr, 0, sizeof(hdr));
			*(__le32 *)hdr = cpu_to_le32(0x205);
			*((__le32 *)hdr + 1) = cpu_to_le32(0x2);
			*((__le32 *)hdr + 2) = cpu_to_le32(cmds[i]);
			pr_info("qsee_sess: --- header+cmd=%u ---\n", cmds[i]);
			session_send(app_id, session, hdr, 12);
		}
	}

	/* Probe 3: shared-buffer style using exported send API with interior ptrs */
	{
		void *req = session + REQ_OFF;
		void *rsp = session + RSP_OFF;
		int r2;

		memset(session, 0, SESS_SIZE);
		*(__le32 *)req = cpu_to_le32(0x205);
		*((__le32 *)req + 1) = cpu_to_le32(0x2);
		r2 = qcom_scm_qseecom_app_send(app_id, req, REQ_LEN, rsp, RSP_LEN);
		pr_info("qsee_sess: exported send interior ret=%d st=0x%x (pa_req=%llx pa_rsp=%llx)\n",
			r2, le32_to_cpu(*(__le32 *)rsp),
			(u64)qcom_tzmem_to_phys(req), (u64)qcom_tzmem_to_phys(rsp));
		dump_hex("rsp2", rsp, 32);
	}

	/* Probe 4: also try separate allocs with same header (control) */
	{
		void *req, *rsp;
		int r2;

		req = qcom_tzmem_alloc(pool, REQ_LEN, GFP_KERNEL);
		rsp = qcom_tzmem_alloc(pool, RSP_LEN, GFP_KERNEL);
		if (req && rsp) {
			memset(req, 0, REQ_LEN);
			memset(rsp, 0xee, RSP_LEN);
			*(__le32 *)req = cpu_to_le32(0x205);
			*((__le32 *)req + 1) = cpu_to_le32(0x2);
			r2 = qcom_scm_qseecom_app_send(app_id, req, REQ_LEN, rsp, RSP_LEN);
			pr_info("qsee_sess: separate bufs send ret=%d st=0x%x\n",
				r2, le32_to_cpu(*(__le32 *)rsp));
			dump_hex("rsp3", rsp, 32);
		}
		if (req)
			qcom_tzmem_free(req);
		if (rsp)
			qcom_tzmem_free(rsp);
	}

	/* Probe 5: load focal32 too then resend (some stacks need both) */
	{
		u32 id2 = 0;
		int r2 = app_start("focal32", &id2);

		if (!r2) {
			memset(hdr, 0, sizeof(hdr));
			*(__le32 *)hdr = cpu_to_le32(0x205);
			*((__le32 *)hdr + 1) = cpu_to_le32(0x2);
			pr_info("qsee_sess: --- after focal32 id=%u, resend to fingerpr ---\n", id2);
			session_send(app_id, session, hdr, 8);
			pr_info("qsee_sess: --- send same header to focal32 ---\n");
			session_send(id2, session, hdr, 8);
		}
	}

	qcom_tzmem_free(session);
	qcom_tzmem_pool_free(pool);
	return 0;
}

static int __init qsee_sess_init(void)
{
	u32 app_id = 0;
	int ret;

	pr_info("qsee_sess: init ta=%s region_notify=%d\n", ta_name, do_region_notify);
	ret = resolve();
	if (ret) {
		pr_err("qsee_sess: resolve %d\n", ret);
		return 0;
	}
	ret = app_start(ta_name, &app_id);
	if (ret) {
		pr_err("qsee_sess: START failed %d (reboot if TA already loaded)\n", ret);
		return 0;
	}
	run_session(app_id);
	pr_info("qsee_sess: done\n");
	return 0;
}

static void __exit qsee_sess_exit(void)
{
	pr_info("qsee_sess: exit\n");
}

module_init(qsee_sess_init);
module_exit(qsee_sess_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("QSEE fingerpr session buffer + region notify + SEND");
MODULE_VERSION("0.2");
