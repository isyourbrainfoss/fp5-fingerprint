// SPDX-License-Identifier: GPL-2.0-only
/*
 * Load fingerpr/focal32 via APP_START, then probe APP_SEND with numeric app_id.
 * Lab step toward auth (M4). Safe probes first (version/health/init headers).
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
#include <linux/delay.h>
#include <linux/sizes.h>
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
	u32 svc; u32 cmd; u32 arginfo;
	u64 args[MAX_QCOM_SCM_ARGS];
	u32 owner;
};
struct qcom_scm_qseecom_resp {
	u64 result; u64 resp_type; u64 data;
};

#define QSEECOM_RESULT_SUCCESS 0
#define QSEECOM_RESULT_INCOMPLETE 1
#define QSEECOM_RESULT_BLOCKED_ON_LISTENER 2
#define QSEECOM_RESULT_FAILURE 0xFFFFFFFFULL
#define QSEECOM_TZ_OWNER_QSEE_OS 50
#define QSEECOM_TZ_SVC_APP_MGR 1
#define QSEECOM_TZ_CMD_APP_START 1

/* Guessed sequential IDs from HAL string order (ff_cmd_name table). */
#define FF_CMD_TA_CREATE		1
#define FF_CMD_TA_INIT			4
#define FF_CMD_TA_PROBE_DEVICE		10
#define FF_CMD_TA_CHECK_HEALTH		22
#define FF_CMD_TA_GET_AUTHENTICATOR_ID	38
#define FF_CMD_SVC_GET_VERSION_INFO	44
#define FF_CMD_SVC_GET_DEVICE_INFO	47

static char *ta_dir = "/lib/firmware/qsee";
module_param(ta_dir, charp, 0644);

typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);
typedef int (*qseecom_call_t)(const struct qcom_scm_desc *desc,
			      struct qcom_scm_qseecom_resp *res);

static kallsyms_lookup_name_t kln;
static qseecom_call_t qseecom_call_fn;

static noinline __attribute__((no_sanitize("kcfi")))
int invoke_qseecom_call(const struct qcom_scm_desc *desc,
			struct qcom_scm_qseecom_resp *res)
{
	return qseecom_call_fn(desc, res);
}

static int resolve_syms(void)
{
	struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };
	int ret = register_kprobe(&kp);

	if (ret)
		return ret;
	kln = (kallsyms_lookup_name_t)kp.addr;
	unregister_kprobe(&kp);
	if (!kln)
		return -ENOENT;
	qseecom_call_fn = (qseecom_call_t)kln("qcom_scm_qseecom_call");
	if (!qseecom_call_fn)
		return -ENOENT;
	return 0;
}

static int read_file(const char *path, u8 **out, size_t *out_len)
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

static int build_image_fixed(const char *dir, const char *name,
			     u8 **img_out, size_t *img_len, size_t *mdt_len_out)
{
	char path[256];
	u8 *parts[9] = {};
	size_t plens[9] = {};
	size_t total = 0, off = 0, mdt_len;
	u8 *img;
	int i, ret;

	snprintf(path, sizeof(path), "%s/%s.mdt", dir, name);
	ret = read_file(path, &parts[0], &plens[0]);
	if (ret)
		return ret;
	mdt_len = plens[0];
	total = mdt_len;
	for (i = 0; i < 8; i++) {
		snprintf(path, sizeof(path), "%s/%s.b0%d", dir, name, i);
		ret = read_file(path, &parts[i + 1], &plens[i + 1]);
		if (ret)
			goto err;
		total += plens[i + 1];
	}
	img = kvmalloc(total, GFP_KERNEL);
	if (!img) {
		ret = -ENOMEM;
		goto err;
	}
	for (i = 0; i < 9; i++) {
		memcpy(img + off, parts[i], plens[i]);
		off += plens[i];
		kvfree(parts[i]);
	}
	*img_out = img;
	*img_len = total;
	*mdt_len_out = mdt_len;
	return 0;
err:
	for (i = 0; i < 9; i++)
		kvfree(parts[i]);
	return ret;
}

static int app_start(const char *name, u32 *app_id_out)
{
	u8 *img = NULL;
	size_t img_len = 0, mdt_len = 0;
	struct qcom_tzmem_pool *pool;
	struct qcom_tzmem_pool_config pcfg;
	void *tzbuf;
	struct qcom_scm_desc desc = {};
	struct qcom_scm_qseecom_resp res = {};
	int ret;

	ret = build_image_fixed(ta_dir, name, &img, &img_len, &mdt_len);
	if (ret) {
		pr_err("qsee_ta_talk: build %s: %d\n", name, ret);
		return ret;
	}

	pcfg.initial_size = ALIGN(img_len + SZ_1M, SZ_2M);
	pcfg.policy = QCOM_TZMEM_POLICY_ON_DEMAND;
	pcfg.increment = SZ_1M;
	pcfg.max_size = ALIGN(img_len + SZ_4M, SZ_2M);
	pool = qcom_tzmem_pool_new(&pcfg);
	if (IS_ERR(pool)) {
		kvfree(img);
		return PTR_ERR(pool);
	}
	tzbuf = qcom_tzmem_alloc(pool, img_len, GFP_KERNEL);
	if (!tzbuf) {
		qcom_tzmem_pool_free(pool);
		kvfree(img);
		return -ENOMEM;
	}
	memcpy(tzbuf, img, img_len);
	kvfree(img);

	desc.owner = QSEECOM_TZ_OWNER_QSEE_OS;
	desc.svc = QSEECOM_TZ_SVC_APP_MGR;
	desc.cmd = QSEECOM_TZ_CMD_APP_START;
	desc.arginfo = QCOM_SCM_ARGS(3, QCOM_SCM_VAL, QCOM_SCM_VAL, QCOM_SCM_VAL);
	desc.args[0] = mdt_len;
	desc.args[1] = img_len;
	desc.args[2] = qcom_tzmem_to_phys(tzbuf);

	ret = invoke_qseecom_call(&desc, &res);
	pr_info("qsee_ta_talk: START %s ret=%d result=0x%llx type=0x%llx data=0x%llx\n",
		name, ret, res.result, res.resp_type, res.data);

	qcom_tzmem_free(tzbuf);
	qcom_tzmem_pool_free(pool);

	if (ret || res.result != QSEECOM_RESULT_SUCCESS)
		return ret ? ret : -EIO;
	*app_id_out = (u32)res.data;
	return 0;
}

/* Request layouts to try (cmd u32 at various places). */
struct probe_fmt {
	const char *name;
	size_t req_size;
	size_t rsp_size;
	void (*fill)(void *req, size_t req_size, u32 cmd);
};

static void fill_cmd0(void *req, size_t req_size, u32 cmd)
{
	memset(req, 0, req_size);
	*(__le32 *)req = cpu_to_le32(cmd);
}

static void fill_cmd0_len(void *req, size_t req_size, u32 cmd)
{
	memset(req, 0, req_size);
	*(__le32 *)req = cpu_to_le32(cmd);
	if (req_size >= 8)
		*((__le32 *)req + 1) = cpu_to_le32(req_size);
}

static void fill_magic_cmd(void *req, size_t req_size, u32 cmd)
{
	memset(req, 0, req_size);
	/* common vendor magic-ish patterns */
	*(__le32 *)req = cpu_to_le32(0x46505300); /* 'FPS\0' */
	if (req_size >= 8)
		*((__le32 *)req + 1) = cpu_to_le32(cmd);
}

static void fill_cmd_at4(void *req, size_t req_size, u32 cmd)
{
	memset(req, 0, req_size);
	if (req_size >= 8)
		*((__le32 *)req + 1) = cpu_to_le32(cmd);
	else
		*(__le32 *)req = cpu_to_le32(cmd);
}

static void hexdump_prefix(const char *tag, const void *buf, size_t len)
{
	char line[3 * 32 + 1];
	size_t n = min_t(size_t, len, 32);
	size_t i;
	char *p = line;

	for (i = 0; i < n; i++)
		p += scnprintf(p, 4, "%02x ", ((const u8 *)buf)[i]);
	pr_info("qsee_ta_talk: %s [%zu]: %s\n", tag, len, line);
}

static int app_send_probe(u32 app_id, const char *app_name)
{
	static const u32 cmds[] = {
		0,
		FF_CMD_TA_CREATE,
		FF_CMD_TA_INIT,
		FF_CMD_TA_PROBE_DEVICE,
		FF_CMD_TA_CHECK_HEALTH,
		FF_CMD_TA_GET_AUTHENTICATOR_ID,
		FF_CMD_SVC_GET_VERSION_INFO,
		FF_CMD_SVC_GET_DEVICE_INFO,
		0x100, 0x1000, 0xffffffff,
	};
	static const struct {
		const char *name;
		size_t req_sz, rsp_sz;
		void (*fill)(void *, size_t, u32);
	} fmts[] = {
		{ "cmd@0_64", 64, 256, fill_cmd0 },
		{ "cmd@0_256", 256, 512, fill_cmd0 },
		{ "cmd+len_256", 256, 512, fill_cmd0_len },
		{ "magic+cmd_64", 64, 256, fill_magic_cmd },
		{ "cmd@4_64", 64, 256, fill_cmd_at4 },
		{ "cmd@0_4k", 4096, 4096, fill_cmd0 },
	};
	struct qcom_tzmem_pool *pool;
	struct qcom_tzmem_pool_config pcfg = {
		.initial_size = SZ_1M,
		.policy = QCOM_TZMEM_POLICY_ON_DEMAND,
		.increment = SZ_256K,
		.max_size = SZ_2M,
	};
	unsigned int fi, ci;
	int interesting = 0;

	pool = qcom_tzmem_pool_new(&pcfg);
	if (IS_ERR(pool))
		return PTR_ERR(pool);

	pr_info("qsee_ta_talk: === SEND probes for %s app_id=%u ===\n",
		app_name, app_id);

	for (fi = 0; fi < ARRAY_SIZE(fmts); fi++) {
		for (ci = 0; ci < ARRAY_SIZE(cmds); ci++) {
			void *req, *rsp;
			int ret;
			u32 cmd = cmds[ci];

			req = qcom_tzmem_alloc(pool, fmts[fi].req_sz, GFP_KERNEL);
			rsp = qcom_tzmem_alloc(pool, fmts[fi].rsp_sz, GFP_KERNEL);
			if (!req || !rsp) {
				if (req)
					qcom_tzmem_free(req);
				if (rsp)
					qcom_tzmem_free(rsp);
				continue;
			}
			memset(rsp, 0, fmts[fi].rsp_sz);
			fmts[fi].fill(req, fmts[fi].req_sz, cmd);

			ret = qcom_scm_qseecom_app_send(app_id, req, fmts[fi].req_sz,
							rsp, fmts[fi].rsp_sz);

			/* Log non-default responses or successful-looking data */
			if (ret == 0) {
				bool nonzero = false;
				size_t i;

				for (i = 0; i < min_t(size_t, fmts[fi].rsp_sz, 64); i++) {
					if (((u8 *)rsp)[i]) {
						nonzero = true;
						break;
					}
				}
				if (nonzero || cmd <= 5) {
					pr_info("qsee_ta_talk: SEND ok app=%u fmt=%s cmd=0x%x\n",
						app_id, fmts[fi].name, cmd);
					hexdump_prefix("rsp", rsp, fmts[fi].rsp_sz);
					interesting++;
				}
			} else if (ret != -EIO && ret != -EINVAL && ret != -ENOENT) {
				pr_info("qsee_ta_talk: SEND ret=%d app=%u fmt=%s cmd=0x%x\n",
					ret, app_id, fmts[fi].name, cmd);
			}

			qcom_tzmem_free(req);
			qcom_tzmem_free(rsp);
			/* avoid hammering TZ */
			cond_resched();
		}
	}

	/* Also try zero-length-ish minimal buffers with cmd 0 only once more at 16/32 */
	{
		void *req, *rsp;
		int ret;

		req = qcom_tzmem_alloc(pool, 16, GFP_KERNEL);
		rsp = qcom_tzmem_alloc(pool, 64, GFP_KERNEL);
		if (req && rsp) {
			memset(req, 0, 16);
			memset(rsp, 0, 64);
			ret = qcom_scm_qseecom_app_send(app_id, req, 16, rsp, 64);
			pr_info("qsee_ta_talk: SEND min16 ret=%d app=%u\n", ret, app_id);
			if (ret == 0)
				hexdump_prefix("rsp16", rsp, 64);
		}
		if (req)
			qcom_tzmem_free(req);
		if (rsp)
			qcom_tzmem_free(rsp);
	}

	pr_info("qsee_ta_talk: === done %s interesting=%d ===\n",
		app_name, interesting);
	qcom_tzmem_pool_free(pool);
	return 0;
}

static int __init qsee_ta_talk_init(void)
{
	u32 id_fp = 0, id_f32 = 0;
	int ret;
	u32 probe_ids[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	unsigned int i;

	pr_info("qsee_ta_talk: start dir=%s\n", ta_dir);
	ret = resolve_syms();
	if (ret) {
		pr_err("qsee_ta_talk: resolve: %d\n", ret);
		return 0;
	}

	ret = app_start("fingerpr", &id_fp);
	if (ret)
		pr_err("qsee_ta_talk: fingerpr START failed %d\n", ret);
	else
		app_send_probe(id_fp, "fingerpr");

	ret = app_start("focal32", &id_f32);
	if (ret)
		pr_err("qsee_ta_talk: focal32 START failed %d\n", ret);
	else
		app_send_probe(id_f32, "focal32");

	/* If START fails (TA still resident / EINVAL), probe likely app_ids. */
	if (!id_fp && !id_f32) {
		pr_info("qsee_ta_talk: START failed — probing app_ids 1..8\n");
		for (i = 0; i < ARRAY_SIZE(probe_ids); i++) {
			char tag[16];

			scnprintf(tag, sizeof(tag), "id%u", probe_ids[i]);
			app_send_probe(probe_ids[i], tag);
		}
	}

	pr_info("qsee_ta_talk: finished fingerpr_id=%u focal32_id=%u\n",
		id_fp, id_f32);
	return 0;
}

static void __exit qsee_ta_talk_exit(void)
{
	pr_info("qsee_ta_talk: exit\n");
}

module_init(qsee_ta_talk_init);
module_exit(qsee_ta_talk_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("QSEE TA load+SEND probe for FP5 fingerprint unlock");
MODULE_VERSION("0.1");
