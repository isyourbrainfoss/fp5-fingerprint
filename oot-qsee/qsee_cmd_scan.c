// SPDX-License-Identifier: GPL-2.0-only
/* After APP_START, scan cmd IDs 0..64 with status+cmd header; log non-error. */
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

#define MAX_QCOM_SCM_ARGS 10
enum { QCOM_SCM_VAL = 0, QCOM_SCM_RO = 1, QCOM_SCM_RW = 2, QCOM_SCM_BUFVAL = 3 };
#define QCOM_SCM_ARGS_IMPL(num, a, b, c, d, e, f, g, h, i, j, ...) (\
	(((a)&3)<<4)|(((b)&3)<<6)|(((c)&3)<<8)|(((d)&3)<<10)|(((e)&3)<<12)|\
	(((f)&3)<<14)|(((g)&3)<<16)|(((h)&3)<<18)|(((i)&3)<<20)|(((j)&3)<<22)|((num)&0xf))
#define QCOM_SCM_ARGS(...) QCOM_SCM_ARGS_IMPL(__VA_ARGS__,0,0,0,0,0,0,0,0,0,0)
struct qcom_scm_desc {
	u32 svc, cmd, arginfo; u64 args[MAX_QCOM_SCM_ARGS]; u32 owner;
};
struct qcom_scm_qseecom_resp { u64 result, resp_type, data; };
#define QSEECOM_RESULT_SUCCESS 0
#define QSEECOM_TZ_OWNER_QSEE_OS 50
#define QSEECOM_TZ_SVC_APP_MGR 1
#define QSEECOM_TZ_CMD_APP_START 1

static char *ta_dir = "/lib/firmware/qsee";
module_param(ta_dir, charp, 0644);
static char *ta_name = "fingerpr";
module_param(ta_name, charp, 0644);

typedef unsigned long (*kln_t)(const char *);
typedef int (*qcall_t)(const struct qcom_scm_desc *, struct qcom_scm_qseecom_resp *);
static kln_t kln; static qcall_t qcall;

static noinline __attribute__((no_sanitize("kcfi")))
int inv(const struct qcom_scm_desc *d, struct qcom_scm_qseecom_resp *r)
{ return qcall(d, r); }

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
	loff_t sz; u8 *b; ssize_t n;
	if (IS_ERR(f)) return PTR_ERR(f);
	sz = i_size_read(file_inode(f));
	if (sz <= 0 || sz > 32<<20) { filp_close(f, NULL); return -EINVAL; }
	b = kvmalloc(sz, GFP_KERNEL);
	if (!b) { filp_close(f, NULL); return -ENOMEM; }
	n = kernel_read(f, b, sz, &f->f_pos);
	filp_close(f, NULL);
	if (n != sz) { kvfree(b); return -EIO; }
	*out = b; *olen = sz; return 0;
}

static int build_img(const char *dir, const char *name, u8 **img, size_t *ilen, size_t *mlen)
{
	char path[256]; u8 *p[9]={}; size_t l[9]={}; size_t tot=0, off=0, mdt; u8 *o; int i, r;
	snprintf(path, sizeof(path), "%s/%s.mdt", dir, name);
	r = read_file(path, &p[0], &l[0]); if (r) return r;
	mdt = l[0]; tot = mdt;
	for (i = 0; i < 8; i++) {
		snprintf(path, sizeof(path), "%s/%s.b0%d", dir, name, i);
		r = read_file(path, &p[i+1], &l[i+1]);
		if (r) goto e;
		tot += l[i+1];
	}
	o = kvmalloc(tot, GFP_KERNEL); if (!o) { r=-ENOMEM; goto e; }
	for (i = 0; i < 9; i++) { memcpy(o+off, p[i], l[i]); off+=l[i]; kvfree(p[i]); }
	*img=o; *ilen=tot; *mlen=mdt; return 0;
e:	for (i=0;i<9;i++) kvfree(p[i]); return r;
}

static int start_ta(const char *name, u32 *id)
{
	u8 *img; size_t il, ml; struct qcom_tzmem_pool *pool;
	struct qcom_tzmem_pool_config cfg;
	void *tz; struct qcom_scm_desc d={}; struct qcom_scm_qseecom_resp r={}; int ret;
	ret = build_img(ta_dir, name, &img, &il, &ml); if (ret) return ret;
	cfg.initial_size = ALIGN(il+SZ_1M, SZ_2M);
	cfg.policy = QCOM_TZMEM_POLICY_ON_DEMAND;
	cfg.increment = SZ_1M; cfg.max_size = ALIGN(il+SZ_4M, SZ_2M);
	pool = qcom_tzmem_pool_new(&cfg);
	if (IS_ERR(pool)) { kvfree(img); return PTR_ERR(pool); }
	tz = qcom_tzmem_alloc(pool, il, GFP_KERNEL);
	if (!tz) { qcom_tzmem_pool_free(pool); kvfree(img); return -ENOMEM; }
	memcpy(tz, img, il); kvfree(img);
	d.owner = QSEECOM_TZ_OWNER_QSEE_OS; d.svc = QSEECOM_TZ_SVC_APP_MGR;
	d.cmd = QSEECOM_TZ_CMD_APP_START;
	d.arginfo = QCOM_SCM_ARGS(3, QCOM_SCM_VAL, QCOM_SCM_VAL, QCOM_SCM_VAL);
	d.args[0]=ml; d.args[1]=il; d.args[2]=qcom_tzmem_to_phys(tz);
	ret = inv(&d, &r);
	pr_info("qsee_cmd_scan: START %s ret=%d res=0x%llx type=0x%llx id=0x%llx\n",
		name, ret, r.result, r.resp_type, r.data);
	qcom_tzmem_free(tz); qcom_tzmem_pool_free(pool);
	if (ret || r.result != QSEECOM_RESULT_SUCCESS) return ret ? ret : -EIO;
	*id = (u32)r.data; return 0;
}

/* Layout: le32 status, le32 cmd, le32 a0, le32 a1, pad */
static int scan_cmds(u32 app_id)
{
	struct qcom_tzmem_pool_config cfg = {
		.initial_size = SZ_256K, .policy = QCOM_TZMEM_POLICY_STATIC,
		.increment = 0, .max_size = SZ_256K,
	};
	struct qcom_tzmem_pool *pool = qcom_tzmem_pool_new(&cfg);
	u32 cmd; int hits = 0;
	if (IS_ERR(pool)) return PTR_ERR(pool);

	pr_info("qsee_cmd_scan: scanning app_id=%u cmds 0..80\n", app_id);
	for (cmd = 0; cmd <= 80; cmd++) {
		void *req, *rsp; int ret; u32 st, c0, a0, a1;
		req = qcom_tzmem_alloc(pool, 256, GFP_KERNEL);
		rsp = qcom_tzmem_alloc(pool, 512, GFP_KERNEL);
		if (!req || !rsp) {
			if (req) qcom_tzmem_free(req);
			if (rsp) qcom_tzmem_free(rsp);
			break;
		}
		memset(req, 0, 256); memset(rsp, 0xee, 512);
		/* layout A: status@0 cmd@4 */
		*(__le32 *)req = 0;
		*((__le32 *)req + 1) = cpu_to_le32(cmd);
		ret = qcom_scm_qseecom_app_send(app_id, req, 256, rsp, 512);
		st = le32_to_cpu(*(__le32 *)rsp);
		c0 = le32_to_cpu(*((__le32 *)rsp + 1));
		a0 = le32_to_cpu(*((__le32 *)rsp + 2));
		a1 = le32_to_cpu(*((__le32 *)rsp + 3));
		if (ret == 0 && st != 0xffffffff && st != 0xeeeeeeee) {
			pr_info("qsee_cmd_scan: A hit cmd=%u ret=%d st=0x%x c0=0x%x a0=0x%x a1=0x%x\n",
				cmd, ret, st, c0, a0, a1);
			hits++;
		}
		/* layout B: cmd@0 status filled by TA in same buf */
		memset(req, 0, 256); memset(rsp, 0xee, 512);
		*(__le32 *)req = cpu_to_le32(cmd);
		ret = qcom_scm_qseecom_app_send(app_id, req, 256, rsp, 512);
		st = le32_to_cpu(*(__le32 *)rsp);
		if (ret == 0 && st != 0xffffffff && st != 0 && st != 0xeeeeeeee) {
			pr_info("qsee_cmd_scan: B hit cmd=%u st=0x%x next=0x%x 0x%x\n",
				cmd, st,
				le32_to_cpu(*((__le32 *)rsp + 1)),
				le32_to_cpu(*((__le32 *)rsp + 2)));
			hits++;
		}
		/* layout C: in-place same buffer send (req==rsp pattern via copy) */
		memset(req, 0, 256);
		*(__le32 *)req = cpu_to_le32(cmd);
		memcpy(rsp, req, 256);
		ret = qcom_scm_qseecom_app_send(app_id, rsp, 256, rsp, 256);
		st = le32_to_cpu(*(__le32 *)rsp);
		if (ret == 0 && st != 0xffffffff && st != 0 && st != 0xeeeeeeee) {
			pr_info("qsee_cmd_scan: C hit cmd=%u st=0x%x\n", cmd, st);
			hits++;
		}
		qcom_tzmem_free(req); qcom_tzmem_free(rsp);
		cond_resched();
	}
	pr_info("qsee_cmd_scan: done hits=%d\n", hits);
	qcom_tzmem_pool_free(pool);
	return 0;
}

static int __init init_scan(void)
{
	u32 id = 0; int ret;
	pr_info("qsee_cmd_scan: init ta=%s\n", ta_name);
	if (resolve()) { pr_err("resolve fail\n"); return 0; }
	ret = start_ta(ta_name, &id);
	if (ret) { pr_err("START fail %d\n", ret); return 0; }
	scan_cmds(id);
	return 0;
}
static void __exit exit_scan(void) { pr_info("qsee_cmd_scan: exit\n"); }
module_init(init_scan); module_exit(exit_scan);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Scan FocalTech TA cmd IDs after APP_START");
