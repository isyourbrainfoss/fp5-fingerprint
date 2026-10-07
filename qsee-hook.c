/* Drop-in libQSEEComAPI.so for fps_hal. HAL only imports start/send/shutdown.
 * Real impl is bind-mounted at REALPATH. No ifunc (reloc crash). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define REALPATH "/vendor/lib64/libQSEEComAPI.so"
#define LOGPATH "/data/local/tmp/qsee-hook.log"
#define MAX_SENDS 64

static FILE *g_log;
static int g_sends;
static void *g_real;
static int (*g_start)(void **, const char *, const char *, uint32_t);
static int (*g_shut)(void **);
static int (*g_send)(void *, void *, uint32_t, void *, uint32_t);

static void log_open(void)
{
	if (g_log)
		return;
	g_log = fopen(LOGPATH, "a");
	if (!g_log)
		return;
	setvbuf(g_log, NULL, _IONBF, 0);
}

static void ts(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	if (g_log)
		fprintf(g_log, "[%ld.%06ld] ", (long)t.tv_sec, (long)(t.tv_nsec / 1000));
}

static void dump_u32s(const char *tag, const void *p, unsigned nbytes)
{
	const uint32_t *w;
	unsigned i, n;
	if (!g_log || !p)
		return;
	w = p;
	n = nbytes / 4;
	fprintf(g_log, "  %s u32:", tag);
	for (i = 0; i < n; i++) {
		if ((i & 3) == 0)
			fprintf(g_log, "\n   ");
		fprintf(g_log, " %08x", w[i]);
	}
	fprintf(g_log, "\n");
}

static void dump_bytes(const char *tag, const void *p, unsigned n)
{
	const unsigned char *b = p;
	unsigned i;
	if (!g_log)
		return;
	fprintf(g_log, "  %s @%p:", tag, p);
	if (!p) {
		fprintf(g_log, " null\n");
		return;
	}
	for (i = 0; i < n; i++) {
		if ((i & 15) == 0)
			fprintf(g_log, "\n   ");
		fprintf(g_log, " %02x", b[i]);
	}
	fprintf(g_log, "\n");
}

static int load_real(void)
{
	if (g_send)
		return 0;
	g_real = dlopen(REALPATH, RTLD_NOW);
	if (!g_real)
		return -1;
	g_start = dlsym(g_real, "QSEECom_start_app");
	g_shut = dlsym(g_real, "QSEECom_shutdown_app");
	g_send = dlsym(g_real, "QSEECom_send_cmd");
	return g_send ? 0 : -1;
}

__attribute__((constructor))
static void init_hook(void)
{
	log_open();
	load_real();
	if (g_log)
		fprintf(g_log, "hook pid=%d real=%p start=%p send=%p shut=%p\n",
			getpid(), g_real, (void *)g_start, (void *)g_send, (void *)g_shut);
}

int QSEECom_start_app(void **handle, const char *path, const char *fname, uint32_t sb)
{
	if (load_real() || !g_start) {
		errno = ENOSYS;
		return -1;
	}
	log_open();
	if (g_log) {
		ts();
		fprintf(g_log, "start_app path=%s fname=%s sb=0x%x\n",
			path ? path : "", fname ? fname : "", sb);
	}
	return g_start(handle, path, fname, sb);
}

int QSEECom_shutdown_app(void **handle)
{
	if (load_real() || !g_shut) {
		errno = ENOSYS;
		return -1;
	}
	log_open();
	if (g_log) {
		ts();
		fprintf(g_log, "shutdown_app\n");
	}
	return g_shut(handle);
}

int QSEECom_send_cmd(void *handle, void *send_buf, uint32_t sbuf_len,
		     void *resp_buf, uint32_t rbuf_len)
{
	uint32_t cmd = 0, plen = 0;
	int rc;

	if (load_real() || !g_send) {
		errno = ENOSYS;
		return -1;
	}
	log_open();
	if (g_log && g_sends < MAX_SENDS) {
		if (send_buf && sbuf_len >= 8) {
			memcpy(&cmd, send_buf, 4);
			memcpy(&plen, (char *)send_buf + 4, 4);
		}
		ts();
		fprintf(g_log,
			"SEND send=%p slen=0x%x resp=%p rlen=0x%x cmd=0x%08x plen=0x%08x\n",
			send_buf, sbuf_len, resp_buf, rbuf_len, cmd, plen);
		if (send_buf) {
			dump_u32s("ion+0", send_buf, 80);
			dump_u32s("usr+0x10 capture_ctx", (char *)send_buf + 0x10, 48);
			dump_bytes("usr+0x10 raw", (char *)send_buf + 0x10, 48);
		}
		g_sends++;
	}
	rc = g_send(handle, send_buf, sbuf_len, resp_buf, rbuf_len);
	if (g_log && g_sends <= MAX_SENDS) {
		ts();
		fprintf(g_log, "SEND leave rc=%d errno=%d\n", rc, errno);
		if (send_buf)
			dump_u32s("ion+0 after", send_buf, 64);
	}
	return rc;
}
