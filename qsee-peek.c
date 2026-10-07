/* Magisk: attach to fps_hal, dump QSEECom SEND_CMD ion head on ioctl. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <asm/ptrace.h>
#include <linux/elf.h>
#include <linux/ptrace.h>
#include <dirent.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define IOC_TYPE(nr) (((nr) >> 8) & 0xffu)
#define IOC_NR(nr) ((nr) & 0xffu)
#define SYS_IOCTL 29
#define MAX_SENDS 40

static FILE *g_log;
static int g_sends;
static int g_ioctls;
static volatile int g_stop;
static int g_in_sys[65536];

static void on_sig(int s)
{
	(void)s;
	g_stop = 1;
}

static void ts(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	fprintf(g_log, "[%ld.%06ld] ", (long)t.tv_sec, t.tv_nsec / 1000);
}

static pid_t g_pid;
static int g_memfd = -1;

static int read_mem(uint64_t addr, void *buf, size_t n)
{
	struct iovec local = { .iov_base = buf, .iov_len = n };
	struct iovec remote = { .iov_base = (void *)addr, .iov_len = n };
	ssize_t r = process_vm_readv(g_pid, &local, 1, &remote, 1, 0);
	if (r == (ssize_t)n)
		return 0;
	if (g_memfd < 0) {
		char path[64];
		snprintf(path, sizeof path, "/proc/%d/mem", g_pid);
		g_memfd = open(path, O_RDONLY);
	}
	if (g_memfd >= 0 && pread(g_memfd, buf, n, (off_t)addr) == (ssize_t)n)
		return 0;
	return -1;
}

static uint64_t peek64(pid_t tid, uint64_t addr)
{
	uint64_t v = 0;
	if (read_mem(addr, &v, 8) == 0)
		return v;
	errno = 0;
	v = (uint64_t)ptrace(PTRACE_PEEKDATA, tid, (void *)addr, 0);
	if (errno)
		return 0;
	return v;
}

static void dump_mem(pid_t tid, uint64_t addr, unsigned n, const char *tag)
{
	unsigned char buf[128];
	unsigned i;
	int rc;
	if (n > sizeof buf)
		n = sizeof buf;
	memset(buf, 0, n);
	rc = read_mem(addr, buf, n);
	if (rc) {
		for (i = 0; i + 8 <= n; i += 8) {
			uint64_t w = peek64(tid, addr + i);
			memcpy(buf + i, &w, 8);
		}
	}
	fprintf(g_log, "  %s @0x%llx rc=%d:", tag, (unsigned long long)addr, rc);
	for (i = 0; i < n; i += 4) {
		uint32_t lo;
		memcpy(&lo, buf + i, 4);
		if ((i & 15) == 0)
			fprintf(g_log, "\n   ");
		fprintf(g_log, " %08x", lo);
	}
	fprintf(g_log, "\n");
}

static int getregs(pid_t tid, struct user_pt_regs *r)
{
	struct iovec iov = { .iov_base = r, .iov_len = sizeof(*r) };
	return ptrace(PTRACE_GETREGSET, tid, (void *)(long)NT_PRSTATUS, &iov);
}

static int attach_all(pid_t pid)
{
	char path[64];
	DIR *d;
	struct dirent *e;
	snprintf(path, sizeof path, "/proc/%d/task", pid);
	d = opendir(path);
	if (!d)
		return -1;
	while ((e = readdir(d))) {
		pid_t tid;
		if (e->d_name[0] == '.')
			continue;
		tid = (pid_t)atoi(e->d_name);
		if (tid <= 0)
			continue;
		if (ptrace(PTRACE_SEIZE, tid, 0, PTRACE_O_TRACESYSGOOD))
			fprintf(g_log, "seize %d errno=%d\n", tid, errno);
		else
			fprintf(g_log, "seize %d ok\n", tid);
		ptrace(PTRACE_INTERRUPT, tid, 0, 0);
	}
	closedir(d);
	return 0;
}

int main(int argc, char **argv)
{
	pid_t pid;
	int status;
	if (argc < 2) {
		fprintf(stderr, "usage: %s <pid>\n", argv[0]);
		return 2;
	}
	pid = (pid_t)atoi(argv[1]);
	g_pid = pid;
	g_log = fopen("/data/local/tmp/qsee-peek.log", "w");
	if (!g_log)
		g_log = stderr;
	setvbuf(g_log, NULL, _IONBF, 0);
	fprintf(g_log, "peek pid=%d\n", pid);
	if (attach_all(pid)) {
		fprintf(g_log, "attach fail errno=%d\n", errno);
		return 1;
	}
	signal(SIGTERM, on_sig);
	signal(SIGINT, on_sig);
	for (;;) {
		pid_t tid;
		if (g_stop)
			break;
		tid = waitpid(-1, &status, __WALL);
		if (tid < 0) {
			fprintf(g_log, "waitpid errno=%d\n", errno);
			if (errno == EINTR && !g_stop)
				continue;
			break;
		}
		if (WIFEXITED(status) || WIFSIGNALED(status))
			continue;
		if (WIFSTOPPED(status) && (status >> 8) == (SIGTRAP | 0x80)) {
			struct user_pt_regs r;
			if (getregs(tid, &r) == 0) {
				int entering = !g_in_sys[tid & 65535];
				g_in_sys[tid & 65535] = entering;
				if (entering && r.regs[8] == SYS_IOCTL) {
					uint64_t fd = r.regs[0];
					uint64_t req = r.regs[1];
					uint64_t arg = r.regs[2];
					g_ioctls++;
					if (g_ioctls <= 8)
						fprintf(g_log, "ioctl tid=%d fd=%llu req=0x%llx\n",
							tid, (unsigned long long)fd,
							(unsigned long long)req);
					if ((fd == 8 || fd == 7 || fd == 9) &&
					    IOC_TYPE((uint32_t)req) == 0x97 && arg) {
						uint64_t cmd_buf = peek64(tid, arg);
						uint32_t cmd_len = (uint32_t)peek64(tid, arg + 8);
						uint32_t rsp_len = (uint32_t)peek64(tid, arg + 24);
						uint32_t cmd = (uint32_t)peek64(tid, cmd_buf);
						uint32_t plen = (uint32_t)peek64(tid, cmd_buf + 4);
						ts();
						fprintf(g_log,
							"SEND tid=%d slen=0x%x rlen=0x%x cmd=0x%08x plen=0x%08x cmd_buf=0x%llx arg=0x%llx\n",
							tid, cmd_len, rsp_len, cmd, plen,
							(unsigned long long)cmd_buf,
							(unsigned long long)arg);
						dump_mem(tid, arg, 32, "send_cmd_req");
						dump_mem(tid, cmd_buf, 64, "ion+0");
						dump_mem(tid, cmd_buf + 0x10, 36, "usr+0x10 capture_ctx?");
						g_sends++;
						if (g_sends >= MAX_SENDS)
							break;
					}
				}
			}
		}
		ptrace(PTRACE_SYSCALL, tid, 0, 0);
	}
	fprintf(g_log, "done sends=%d\n", g_sends);
	{
		char path[64];
		DIR *d;
		struct dirent *e;
		snprintf(path, sizeof path, "/proc/%d/task", pid);
		d = opendir(path);
		if (d) {
			while ((e = readdir(d))) {
				pid_t t;
				if (e->d_name[0] == '.')
					continue;
				t = (pid_t)atoi(e->d_name);
				if (t > 0)
					ptrace(PTRACE_DETACH, t, 0, 0);
			}
			closedir(d);
		}
	}
	fclose(g_log);
	return 0;
}
