// SPDX-License-Identifier: GPL-2.0-only
/* Load focal32 first, then fingerpr; SEND SO header; dump non-error responses */
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
enum { QCOM_SCM_VAL=0,QCOM_SCM_RO=1,QCOM_SCM_RW=2,QCOM_SCM_BUFVAL=3};
#define QCOM_SCM_ARGS_IMPL(num,a,b,c,d,e,f,g,h,i,j,...) ((((a)&3)<<4)|(((b)&3)<<6)|(((c)&3)<<8)|(((d)&3)<<10)|(((e)&3)<<12)|(((f)&3)<<14)|(((g)&3)<<16)|(((h)&3)<<18)|(((i)&3)<<20)|(((j)&3)<<22)|((num)&0xf))
#define QCOM_SCM_ARGS(...) QCOM_SCM_ARGS_IMPL(__VA_ARGS__,0,0,0,0,0,0,0,0,0,0)
struct qcom_scm_desc{u32 svc,cmd,arginfo;u64 args[MAX_QCOM_SCM_ARGS];u32 owner;};
struct qcom_scm_qseecom_resp{u64 result,resp_type,data;};
#define QSEECOM_RESULT_SUCCESS 0
#define QSEECOM_TZ_OWNER_QSEE_OS 50
#define QSEECOM_TZ_SVC_APP_MGR 1
#define QSEECOM_TZ_CMD_APP_START 1
#define REQ_LEN 0x40
#define RSP_LEN 0x800

static char *ta_dir="/lib/firmware/qsee";
module_param(ta_dir,charp,0644);
typedef unsigned long (*kln_t)(const char*);
typedef int (*qcall_t)(const struct qcom_scm_desc*,struct qcom_scm_qseecom_resp*);
static kln_t kln; static qcall_t qcall;
static noinline __attribute__((no_sanitize("kcfi"))) int inv(const struct qcom_scm_desc*d,struct qcom_scm_qseecom_resp*r){return qcall(d,r);}
static int resolve(void){struct kprobe kp={.symbol_name="kallsyms_lookup_name"};if(register_kprobe(&kp))return -ENOENT;kln=(kln_t)kp.addr;unregister_kprobe(&kp);qcall=(qcall_t)kln("qcom_scm_qseecom_call");return qcall?0:-ENOENT;}
static int read_file(const char*path,u8**out,size_t*olen){struct file*f=filp_open(path,O_RDONLY,0);loff_t sz;u8*b;ssize_t n;if(IS_ERR(f))return PTR_ERR(f);sz=i_size_read(file_inode(f));if(sz<=0||sz>32<<20){filp_close(f,NULL);return -EINVAL;}b=kvmalloc(sz,GFP_KERNEL);if(!b){filp_close(f,NULL);return -ENOMEM;}n=kernel_read(f,b,sz,&f->f_pos);filp_close(f,NULL);if(n!=sz){kvfree(b);return -EIO;}*out=b;*olen=sz;return 0;}
static int build_img(const char*dir,const char*name,u8**img,size_t*ilen,size_t*mlen){char path[256];u8*p[9]={};size_t l[9]={};size_t tot=0,off=0,mdt;u8*o;int i,r;snprintf(path,sizeof(path),"%s/%s.mdt",dir,name);r=read_file(path,&p[0],&l[0]);if(r)return r;mdt=l[0];tot=mdt;for(i=0;i<8;i++){snprintf(path,sizeof(path),"%s/%s.b0%d",dir,name,i);r=read_file(path,&p[i+1],&l[i+1]);if(r)goto e;tot+=l[i+1];}o=kvmalloc(tot,GFP_KERNEL);if(!o){r=-ENOMEM;goto e;}for(i=0;i<9;i++){memcpy(o+off,p[i],l[i]);off+=l[i];kvfree(p[i]);}*img=o;*ilen=tot;*mlen=mdt;return 0;e:for(i=0;i<9;i++)kvfree(p[i]);return r;}
static int app_start(const char*name,u32*id){u8*img;size_t il,ml;struct qcom_tzmem_pool*pool;struct qcom_tzmem_pool_config cfg;void*tz;struct qcom_scm_desc d={};struct qcom_scm_qseecom_resp r={};int ret;ret=build_img(ta_dir,name,&img,&il,&ml);if(ret)return ret;cfg.initial_size=ALIGN(il+SZ_1M,SZ_2M);cfg.policy=QCOM_TZMEM_POLICY_ON_DEMAND;cfg.increment=SZ_1M;cfg.max_size=ALIGN(il+SZ_4M,SZ_2M);pool=qcom_tzmem_pool_new(&cfg);if(IS_ERR(pool)){kvfree(img);return PTR_ERR(pool);}tz=qcom_tzmem_alloc(pool,il,GFP_KERNEL);if(!tz){qcom_tzmem_pool_free(pool);kvfree(img);return -ENOMEM;}memcpy(tz,img,il);kvfree(img);d.owner=QSEECOM_TZ_OWNER_QSEE_OS;d.svc=QSEECOM_TZ_SVC_APP_MGR;d.cmd=QSEECOM_TZ_CMD_APP_START;d.arginfo=QCOM_SCM_ARGS(3,QCOM_SCM_VAL,QCOM_SCM_VAL,QCOM_SCM_VAL);d.args[0]=ml;d.args[1]=il;d.args[2]=qcom_tzmem_to_phys(tz);ret=inv(&d,&r);pr_info("qsee_ok: START %s ret=%d res=0x%llx id=0x%llx\n",name,ret,r.result,r.data);qcom_tzmem_free(tz);qcom_tzmem_pool_free(pool);if(ret||r.result!=QSEECOM_RESULT_SUCCESS)return ret?ret:-EIO;*id=(u32)r.data;return 0;}

static void dump(const char*t,const u8*p,size_t n){char line[200];size_t i;char*o=line;n=min_t(size_t,n,48);for(i=0;i<n;i++)o+=scnprintf(o,4,"%02x ",p[i]);pr_info("qsee_ok: %s: %s\n",t,line);if(n>4){/* printable tail */char s[64];size_t j=0;for(i=0;i<n&&j<63;i++)if(p[i]>=0x20&&p[i]<0x7f)s[j++]=p[i];s[j]=0;if(j)pr_info("qsee_ok: %s ascii: '%s'\n",t,s);}}

static int send_hdr(u32 app_id,const char*tag,u32 w0,u32 w1,u32 w2,u32 w3){
	struct qcom_tzmem_pool_config cfg={.initial_size=SZ_64K,.policy=QCOM_TZMEM_POLICY_STATIC,.max_size=SZ_64K};
	struct qcom_tzmem_pool*pool=qcom_tzmem_pool_new(&cfg);
	void*req,*rsp;int ret;u32 st;
	if(IS_ERR(pool))return PTR_ERR(pool);
	req=qcom_tzmem_alloc(pool,REQ_LEN,GFP_KERNEL);
	rsp=qcom_tzmem_alloc(pool,RSP_LEN,GFP_KERNEL);
	if(!req||!rsp){if(req)qcom_tzmem_free(req);if(rsp)qcom_tzmem_free(rsp);qcom_tzmem_pool_free(pool);return -ENOMEM;}
	memset(req,0,REQ_LEN);memset(rsp,0,RSP_LEN);
	*(__le32*)req=cpu_to_le32(w0);
	*((__le32*)req+1)=cpu_to_le32(w1);
	*((__le32*)req+2)=cpu_to_le32(w2);
	*((__le32*)req+3)=cpu_to_le32(w3);
	ret=qcom_scm_qseecom_app_send(app_id,req,REQ_LEN,rsp,RSP_LEN);
	st=le32_to_cpu(*(__le32*)rsp);
	pr_info("qsee_ok: SEND %s app=%u ret=%d st=0x%x hdr=%08x %08x %08x %08x\n",
		tag,app_id,ret,st,w0,w1,w2,w3);
	dump(tag,rsp,64);
	qcom_tzmem_free(req);qcom_tzmem_free(rsp);qcom_tzmem_pool_free(pool);
	return ret;
}

static int __init init_mod(void){
	u32 id_f32=0,id_fp=0;int ret;
	pr_info("qsee_ok: init (focal32 first)\n");
	if(resolve())return 0;
	ret=app_start("focal32",&id_f32);
	if(ret){pr_err("qsee_ok: focal32 START %d\n",ret);return 0;}
	ret=app_start("fingerpr",&id_fp);
	if(ret) pr_warn("qsee_ok: fingerpr START %d (continue)\n",ret);

	/* Baseline SO header */
	send_hdr(id_f32,"f32-base",0x205,0x2,0,0);
	send_hdr(id_f32,"f32-cmd1",0x205,0x2,1,0);
	send_hdr(id_f32,"f32-cmd4",0x205,0x2,4,0);
	send_hdr(id_f32,"f32-cmd10",0x205,0x2,10,0);
	/* try second word variants */
	send_hdr(id_f32,"f32-w1=1",0x205,1,0,0);
	send_hdr(id_f32,"f32-w1=0",0x205,0,0,0);
	send_hdr(id_f32,"f32-w0=1",1,0,0,0);
	send_hdr(id_f32,"f32-w0=0",0,0,0,0);

	if(id_fp){
		send_hdr(id_fp,"fp-base",0x205,0x2,0,0);
		send_hdr(id_fp,"fp-cmd1",0x205,0x2,1,0);
		send_hdr(id_fp,"fp-cmd4",0x205,0x2,4,0);
	}
	pr_info("qsee_ok: done f32=%u fp=%u\n",id_f32,id_fp);
	return 0;
}
static void __exit exit_mod(void){}
module_init(init_mod);module_exit(exit_mod);MODULE_LICENSE("GPL");
