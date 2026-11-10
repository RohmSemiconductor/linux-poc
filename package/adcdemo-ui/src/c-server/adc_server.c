// SPDX-License-Identifier: GPL-2.0
/*
 * bvf_adc_server - BeagleV Fire ADC web server in C.
 *
 * Replacement of web-ui/unified-server/adc_server.py for the BeagleV Fire:
 *
 *   GET /     serves the React front-end (static files)
 *   WS /ws   binary ADC stream, JSON commands {"command":"start"|"stop"}
 *
 * On "start" the ADC streamer (bvf_adc_stream) is launched. Its ring buffer
 * items (struct adc_data, 36860 plain 16-bit samples) are converted to the
 * common wire format (chunks of 1024 samples, see below) and sent to the
 * browser, one WebSocket frame per ring item.
 *
 * Wire format, per chunk, little endian:
 *   u32 usecs, u32 samples[1024], u32 gpio_lev0[1024]
 * The sample words have their bytes swapped, because the UI applies the RPi
 * SPI byte swap to them.
 *
 * One thread per HTTP connection; a WebSocket session is served by a single
 * poll() loop (socket, streamer stdout, ring buffer), no Python, no per
 * sample work other than a 16 bit byte swap.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "adc_common.h"
#include "shmem.h"
#include "ws.h"

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "The wire format is little endian, big endian hosts are not supported"
#endif

#define DEFAULT_PORT		80
#define DEFAULT_DIST_DIR	"/ui/react-frontend-webgl2/dist"
#define DEFAULT_STREAMER	"bvf_adc_stream"

#define WIRE_SAMPS		1024
#define WIRE_CHUNK_BYTES	(4 + WIRE_SAMPS * 4 * 2)
#define STAGE_SAMPS		(MAX_SAMPS + WIRE_SAMPS)
/* Whole chunks that can come out of one ring item plus the carried samples */
#define MAX_CHUNKS		(STAGE_SAMPS / WIRE_SAMPS)

/* Room before the payload for the WS header; keeps the payload aligned */
#define FRAME_HDR_ROOM		16
#define RING_OPEN_TIMEOUT_MS	3000
#define STREAMER_STOP_TIMEOUT_MS 5000
#define ITEMS_PER_PASS		8
#define POLL_IDLE_MS		2
#define STATS_INTERVAL_S	5
#define RX_BUF_SIZE		8192

static const char *g_dist_dir = DEFAULT_DIST_DIR;
static const char *g_streamer = DEFAULT_STREAMER;
static volatile sig_atomic_t g_quit;
static pthread_mutex_t g_stream_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int g_sessions;

static void logmsg(const char *fmt, ...)
{
	struct timespec ts;
	struct tm tm;
	va_list ap;

	clock_gettime(CLOCK_REALTIME, &ts);
	localtime_r(&ts.tv_sec, &tm);
	fprintf(stderr, "%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int send_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	while (len) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		p += n;
		len -= n;
	}
	return 0;
}

/* ---- HTTP: static files ------------------------------------------------ */

static const char *mime_type(const char *path)
{
	static const struct {
		const char *ext;
		const char *mime;
	} tbl[] = {
		{ ".html", "text/html; charset=utf-8" },
		{ ".js", "text/javascript" },
		{ ".mjs", "text/javascript" },
		{ ".css", "text/css" },
		{ ".json", "application/json" },
		{ ".map", "application/json" },
		{ ".svg", "image/svg+xml" },
		{ ".png", "image/png" },
		{ ".ico", "image/x-icon" },
		{ ".woff2", "font/woff2" },
		{ ".woff", "font/woff" },
		{ ".txt", "text/plain" },
	};
	const char *ext = strrchr(path, '.');
	size_t i;

	if (ext)
		for (i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++)
			if (!strcasecmp(ext, tbl[i].ext))
				return tbl[i].mime;
	return "application/octet-stream";
}

static int http_error(int fd, int code, const char *text)
{
	char buf[256];
	int n = snprintf(buf, sizeof(buf),
			 "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\n"
			 "Connection: close\r\n\r\n%s", code, text,
			 strlen(text), text);

	return send_all(fd, buf, n);
}

static int open_static(const char *url, struct stat *st, char *path,
		       size_t pathsz)
{
	char rel[512];
	size_t len;
	int fd;

	/* Strip the query string; no percent decoding, no parent directories */
	len = strcspn(url, "?#");
	if (len >= sizeof(rel) || strstr(url, ".."))
		return -EINVAL;
	memcpy(rel, url, len);
	rel[len] = '\0';

	snprintf(path, pathsz, "%s/%s", g_dist_dir, rel);
	fd = open(path, O_RDONLY);
	if (fd >= 0 && !fstat(fd, st) && S_ISDIR(st->st_mode)) {
		close(fd);
		fd = -1;
		strncat(path, "/index.html", pathsz - strlen(path) - 1);
		fd = open(path, O_RDONLY);
	}
	if (fd >= 0 && !fstat(fd, st) && S_ISREG(st->st_mode))
		return fd;
	if (fd >= 0)
		close(fd);

	/* SPA fallback */
	snprintf(path, pathsz, "%s/index.html", g_dist_dir);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -ENOENT;
	if (fstat(fd, st) || !S_ISREG(st->st_mode)) {
		close(fd);
		return -ENOENT;
	}
	return fd;
}

static void serve_static(int fd, const char *url)
{
	struct stat st;
	char path[1024], hdr[384];
	off_t off = 0;
	int file, n;

	file = open_static(url, &st, path, sizeof(path));
	if (file < 0) {
		http_error(fd, 404, "dist/ not found. Did you run `npm run build`?");
		return;
	}

	n = snprintf(hdr, sizeof(hdr),
		     "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
		     "Content-Length: %lld\r\nConnection: close\r\n\r\n",
		     mime_type(path), (long long)st.st_size);
	if (send_all(fd, hdr, n))
		goto out;

	while (off < st.st_size) {
		ssize_t sent = sendfile(fd, file, &off, st.st_size - off);

		if (sent < 0 && errno == EINTR)
			continue;
		if (sent <= 0)
			break;
	}
out:
	close(file);
}

/* ---- ADC data conversion ----------------------------------------------- */

/*
 * Ring items hold MAX_SAMPS samples, which is not a multiple of the wire
 * chunk size, so the remainder is carried over to the next item. The fabric
 * clocks the samples at a constant rate: the timestamp of every wire chunk
 * is derived from the item timestamp and the sample rate.
 */
struct stage {
	uint16_t samples[STAGE_SAMPS];
	uint32_t gpio[STAGE_SAMPS];
	unsigned int n;		/* samples carried */
	double t0;		/* timestamp (us) of the first carried sample */
};

static void put_chunk(uint8_t *out, uint32_t usecs, const uint16_t *s,
		      const uint32_t *g)
{
	uint32_t *o = (uint32_t *)(out + 4);
	unsigned int i;

	memcpy(out, &usecs, sizeof(usecs));
	for (i = 0; i < WIRE_SAMPS; i++)
		o[i] = __builtin_bswap16(s[i]);
	memcpy(o + WIRE_SAMPS, g, WIRE_SAMPS * sizeof(*g));
}

/*
 * Convert @item to wire chunks into @out (room for MAX_CHUNKS chunks).
 * Returns the number of chunks written.
 */
static unsigned int convert_item(struct stage *st, const struct adc_data *item,
				 uint8_t *out, unsigned int rate_hz)
{
	double us_per_samp = 1e6 / rate_hz;
	unsigned int pos = 0, chunks = 0;

	if (st->n) {
		double expected = st->t0 + st->n * us_per_samp;
		int32_t diff = (int32_t)(item->usecs - (uint32_t)expected);

		/* Samples were dropped in between: resync on this item */
		if (abs(diff) >= 2 * WIRE_SAMPS * us_per_samp)
			st->n = 0;
	}
	if (!st->n)
		st->t0 = item->usecs;

	memcpy(st->samples + st->n, item->samples, sizeof(item->samples));
	memcpy(st->gpio + st->n, item->gpio_lev0, sizeof(item->gpio_lev0));
	st->n += MAX_SAMPS;

	while (st->n - pos >= WIRE_SAMPS) {
		uint32_t ts = (uint64_t)(st->t0 + pos * us_per_samp);

		put_chunk(out + chunks * WIRE_CHUNK_BYTES, ts,
			  st->samples + pos, st->gpio + pos);
		chunks++;
		pos += WIRE_SAMPS;
	}

	st->n -= pos;
	st->t0 += pos * us_per_samp;
	memmove(st->samples, st->samples + pos, st->n * sizeof(*st->samples));
	memmove(st->gpio, st->gpio + pos, st->n * sizeof(*st->gpio));

	return chunks;
}

/* ---- WebSocket session ------------------------------------------------- */

struct session {
	int fd;
	/* streaming state */
	bool streaming;
	bool have_lock;
	pid_t pid;
	int out_fd;		/* streamer stdout */
	char line[256];
	size_t line_len;
	unsigned int rate_hz;
	double t_start;
	double duration_ms;	/* 0: until stopped */
	/* ring */
	struct shmem_info shi;
	struct mvaring *mr;
	double open_deadline;
	uint16_t last_dropped;
	/* buffers */
	struct adc_data *item;
	struct stage *stage;
	uint8_t *frame;		/* FRAME_HDR_ROOM + MAX_CHUNKS wire chunks */
	/* statistics for diagnosing the data path */
	uint64_t items;
	unsigned int peak_backlog;
	double max_send_ms;
	double next_stats;
};

static int ws_send(struct session *s, uint8_t opcode, const void *data,
		   size_t len)
{
	uint8_t hdr[WS_MAX_HDR];
	size_t hlen = ws_frame_header(hdr, opcode, len);
	int ret = send_all(s->fd, hdr, hlen);

	return ret ? ret : send_all(s->fd, data, len);
}

static int ws_send_json(struct session *s, const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return ws_send(s, WS_OP_TEXT, buf, n);
}

static void parse_streamer_line(struct session *s, const char *line)
{
	const char *key = "Actual sample-rate:";

	logmsg("[streamer] %s", line);
	if (strncmp(line, key, strlen(key)))
		return;

	unsigned int rate = strtoul(line + strlen(key), NULL, 10);

	if (rate) {
		s->rate_hz = rate;
		ws_send_json(s, "{\"type\":\"actual_sample_rate\",\"value\":%u}",
			     rate);
	}
}

static void read_streamer_output(struct session *s)
{
	char buf[256];
	ssize_t n = read(s->out_fd, buf, sizeof(buf));
	ssize_t i;

	if (n <= 0) {
		if (n < 0 && errno == EINTR)
			return;
		close(s->out_fd);
		s->out_fd = -1;
		return;
	}
	for (i = 0; i < n; i++) {
		if (buf[i] == '\n') {
			s->line[s->line_len] = '\0';
			if (s->line_len)
				parse_streamer_line(s, s->line);
			s->line_len = 0;
		} else if (s->line_len < sizeof(s->line) - 1) {
			s->line[s->line_len++] = buf[i];
		}
	}
}

static int spawn_streamer(struct session *s, unsigned int rate_ksps)
{
	char rate[16];
	int pfd[2];

	if (pipe2(pfd, O_CLOEXEC))
		return -errno;

	/* Do not attach to a stale ring of a previous run */
	shm_unlink(SHM_NAME);

	s->pid = fork();
	if (s->pid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		return -errno;
	}
	if (!s->pid) {
		dup2(pfd[1], STDOUT_FILENO);
		snprintf(rate, sizeof(rate), "%u", rate_ksps);
		if (rate_ksps)
			execlp(g_streamer, g_streamer, "-c", "-r", rate,
			       (char *)NULL);
		else
			execlp(g_streamer, g_streamer, "-c", (char *)NULL);
		perror(g_streamer);
		_exit(127);
	}
	close(pfd[1]);
	s->out_fd = pfd[0];
	s->line_len = 0;
	return 0;
}

static void close_ring(struct session *s)
{
	if (s->mr) {
		shmem_close(&s->shi);
		s->mr = NULL;
	}
}

static void stop_streamer(struct session *s)
{
	double end = now_ms() + STREAMER_STOP_TIMEOUT_MS;
	int status;

	if (s->pid > 0) {
		/* SIGINT lets the streamer stop the sampler gracefully */
		kill(s->pid, SIGINT);
		while (waitpid(s->pid, &status, WNOHANG) == 0) {
			if (now_ms() > end) {
				kill(s->pid, SIGKILL);
				waitpid(s->pid, &status, 0);
				break;
			}
			usleep(10000);
		}
		logmsg("streamer stopped (pid %d)", s->pid);
		s->pid = -1;
	}
	if (s->out_fd >= 0) {
		close(s->out_fd);
		s->out_fd = -1;
	}
	close_ring(s);
	s->stage->n = 0;
	s->streaming = false;
	if (s->have_lock) {
		s->have_lock = false;
		pthread_mutex_unlock(&g_stream_lock);
	}
}

static void start_stream(struct session *s, unsigned int rate_hz,
			 double duration_ms)
{
	int ret;

	if (pthread_mutex_trylock(&g_stream_lock)) {
		ws_send_json(s, "{\"type\":\"error\",\"message\":"
				"\"Another client is streaming\"}");
		return;
	}
	s->have_lock = true;

	ret = spawn_streamer(s, rate_hz / 1000);
	if (ret) {
		logmsg("cannot start streamer: %s", strerror(-ret));
		ws_send_json(s, "{\"type\":\"error\",\"message\":"
				"\"Cannot start streamer\"}");
		stop_streamer(s);
		return;
	}

	s->streaming = true;
	s->rate_hz = rate_hz ? rate_hz : 1000000;
	s->t_start = now_ms();
	s->duration_ms = duration_ms;
	s->open_deadline = s->t_start + RING_OPEN_TIMEOUT_MS;
	s->last_dropped = 0;
	s->items = 0;
	s->peak_backlog = 0;
	s->max_send_ms = 0;
	s->next_stats = s->t_start + STATS_INTERVAL_S * 1000;
	logmsg("streamer started (pid %d), %u Hz", s->pid, s->rate_hz);
	ws_send_json(s, "{\"type\":\"started\"}");
}

static void stop_stream(struct session *s)
{
	double elapsed = now_ms() - s->t_start;

	stop_streamer(s);
	ws_send_json(s, "{\"type\":\"stopped\",\"actualDurationMs\":%.2f}",
		     elapsed);
	logmsg("streaming stopped, %.1f ms", elapsed);
}

static bool try_open_ring(struct session *s)
{
	if (shmem_open(SHM_NAME, SHM_SIZE, &s->shi, true))
		return false;
	s->mr = s->shi.buff;
	if (!ring_is_ok(s->mr)) {
		shmem_close(&s->shi);
		s->mr = NULL;
		return false;
	}
	logmsg("opened ring buffer '%s'", SHM_NAME);
	return true;
}

/* Returns true if data was sent, so the caller does not need to sleep */
static bool service_ring(struct session *s)
{
	bool sent = false;
	int i;

	for (i = 0; i < ITEMS_PER_PASS; i++) {
		unsigned int backlog = ring_available(s->mr);
		unsigned int chunks;
		uint8_t hdr[WS_MAX_HDR], *start;
		size_t len, hlen;
		double t;
		int ret;

		if (backlog > s->peak_backlog)
			s->peak_backlog = backlog;
		ret = ring_read(s->mr, s->item, 1);
		if (ret <= 0)
			break;

		s->items++;
		chunks = convert_item(s->stage, s->item,
				      s->frame + FRAME_HDR_ROOM, s->rate_hz);
		if (!chunks)
			continue;

		/* Header is built right below the payload: one send() */
		len = chunks * WIRE_CHUNK_BYTES;
		hlen = ws_frame_header(hdr, WS_OP_BINARY, len);
		start = s->frame + FRAME_HDR_ROOM - hlen;
		memcpy(start, hdr, hlen);
		t = now_ms();
		ret = send_all(s->fd, start, hlen + len);
		t = now_ms() - t;
		if (t > s->max_send_ms)
			s->max_send_ms = t;
		if (ret) {
			s->streaming = false;
			return false;
		}
		sent = true;
	}

	if (s->mr->dropped != s->last_dropped) {
		s->last_dropped = s->mr->dropped;
		logmsg("WARNING: ring dropped counter %u (reader too slow)",
		       s->last_dropped);
	}
	return sent;
}

static void report_stats(struct session *s)
{
	logmsg("stats: %llu items, peak ring backlog %u/%u, max send %.1f ms",
	       (unsigned long long)s->items, s->peak_backlog,
	       s->mr ? s->mr->num_slots - 1 : 0, s->max_send_ms);
	s->peak_backlog = 0;
	s->max_send_ms = 0;
}

static void service_streaming(struct session *s, int *timeout_ms)
{
	double t = now_ms();
	int status;

	if (s->pid > 0 && waitpid(s->pid, &status, WNOHANG) == s->pid) {
		logmsg("streamer exited unexpectedly (status %d)", status);
		s->pid = -1;
		ws_send_json(s, "{\"type\":\"error\",\"message\":"
				"\"ADC streamer exited\"}");
		stop_stream(s);
		return;
	}

	if (s->duration_ms && t - s->t_start >= s->duration_ms) {
		stop_stream(s);
		return;
	}

	if (!s->mr) {
		if (!try_open_ring(s) && t > s->open_deadline) {
			logmsg("ring buffer did not appear");
			ws_send_json(s, "{\"type\":\"error\",\"message\":"
					"\"Cannot open ring buffer\"}");
			stop_stream(s);
		}
		return;
	}

	if (service_ring(s))
		*timeout_ms = 0;
	if (!s->streaming) {
		/* Send failed: the client is gone */
		stop_streamer(s);
		return;
	}
	if (t >= s->next_stats) {
		report_stats(s);
		s->next_stats = t + STATS_INTERVAL_S * 1000;
	}
}

/* Value of a numeric JSON member, 0 if missing. No general JSON parser. */
static double json_number(const char *json, const char *key)
{
	const char *p = strstr(json, key);

	if (!p)
		return 0;
	p = strchr(p + strlen(key), ':');
	return p ? strtod(p + 1, NULL) : 0;
}

static void handle_command(struct session *s, char *msg)
{
	if (strstr(msg, "\"start\"")) {
		if (!s->streaming)
			start_stream(s, json_number(msg, "\"sampleRate\""),
				     json_number(msg, "\"durationMs\""));
	} else if (strstr(msg, "\"stop\"")) {
		if (s->streaming)
			stop_stream(s);
	} else {
		ws_send_json(s, "{\"type\":\"error\",\"message\":"
				"\"Invalid command\"}");
	}
}

/*
 * Parse client frames in @rx. Returns the number of bytes consumed, or a
 * negative value if the connection should be closed.
 */
static int handle_frames(struct session *s, uint8_t *rx, size_t len)
{
	size_t used = 0;

	while (len - used >= 2) {
		uint8_t *p = rx + used;
		uint8_t opcode = p[0] & 0x0f;
		bool masked = p[1] & 0x80;
		uint64_t plen = p[1] & 0x7f;
		size_t hlen = 2, i;
		char msg[512];

		if (plen == 126) {
			if (len - used < 4)
				break;
			plen = p[2] << 8 | p[3];
			hlen = 4;
		} else if (plen == 127) {
			return -EMSGSIZE;
		}
		if (masked)
			hlen += 4;
		if (plen > RX_BUF_SIZE - 14)
			return -EMSGSIZE;
		if (len - used < hlen + plen)
			break;

		if (masked)
			for (i = 0; i < plen; i++)
				p[hlen + i] ^= p[hlen - 4 + (i & 3)];

		switch (opcode) {
		case WS_OP_TEXT:
			if (plen < sizeof(msg)) {
				memcpy(msg, p + hlen, plen);
				msg[plen] = '\0';
				handle_command(s, msg);
			}
			break;
		case WS_OP_PING:
			ws_send(s, WS_OP_PONG, p + hlen, plen);
			break;
		case WS_OP_CLOSE:
			ws_send(s, WS_OP_CLOSE, p + hlen, plen);
			return -ECONNRESET;
		default:
			break;
		}
		used += hlen + plen;
	}
	return used;
}

static void ws_session(int fd)
{
	struct session s = { .fd = fd, .pid = -1, .out_fd = -1 };
	uint8_t rx[RX_BUF_SIZE];
	size_t rxlen = 0;
	int timeout = 200;

	s.item = malloc(sizeof(*s.item));
	s.stage = calloc(1, sizeof(*s.stage));
	s.frame = aligned_alloc(16, FRAME_HDR_ROOM +
				MAX_CHUNKS * WIRE_CHUNK_BYTES);
	if (!s.item || !s.stage || !s.frame)
		goto out;

	logmsg("client connected");
	while (!g_quit) {
		struct pollfd pfd[2] = {
			{ .fd = fd, .events = POLLIN },
			{ .fd = s.out_fd, .events = POLLIN },
		};
		int n;

		n = poll(pfd, s.out_fd >= 0 ? 2 : 1, timeout);
		if (n < 0 && errno != EINTR)
			break;

		if (n > 0 && (pfd[0].revents & (POLLIN | POLLHUP | POLLERR))) {
			ssize_t r = recv(fd, rx + rxlen, sizeof(rx) - rxlen, 0);
			int used;

			if (r <= 0)
				break;
			rxlen += r;
			used = handle_frames(&s, rx, rxlen);
			if (used < 0)
				break;
			memmove(rx, rx + used, rxlen - used);
			rxlen -= used;
		}
		if (n > 0 && s.out_fd >= 0 && (pfd[1].revents & (POLLIN | POLLHUP)))
			read_streamer_output(&s);

		timeout = s.streaming ? POLL_IDLE_MS : 200;
		if (s.streaming)
			service_streaming(&s, &timeout);
	}

	if (s.streaming || s.pid > 0)
		stop_streamer(&s);
	logmsg("client disconnected");
out:
	free(s.item);
	free(s.stage);
	free(s.frame);
}

/* ---- HTTP connection --------------------------------------------------- */

static bool header_value(const char *req, const char *name, char *out,
			 size_t outsz)
{
	const char *p = strcasestr(req, name);
	size_t len;

	if (!p)
		return false;
	p += strlen(name);
	while (*p == ' ')
		p++;
	len = strcspn(p, "\r\n");
	if (len >= outsz)
		return false;
	memcpy(out, p, len);
	out[len] = '\0';
	return true;
}

static void handle_connection(int fd)
{
	char req[RX_BUF_SIZE], url[512], key[128], upgrade[32];
	size_t len = 0;

	while (len < sizeof(req) - 1) {
		ssize_t n = recv(fd, req + len, sizeof(req) - 1 - len, 0);

		if (n <= 0)
			return;
		len += n;
		req[len] = '\0';
		if (strstr(req, "\r\n\r\n"))
			break;
	}

	if (sscanf(req, "GET %511s ", url) != 1) {
		http_error(fd, 400, "Bad request");
		return;
	}

	if (!strcmp(url, "/ws") &&
	    header_value(req, "Upgrade:", upgrade, sizeof(upgrade)) &&
	    !strcasecmp(upgrade, "websocket") &&
	    header_value(req, "Sec-WebSocket-Key:", key, sizeof(key))) {
		char accept[29], resp[256];
		int n, one = 1;

		ws_accept_key(key, accept);
		n = snprintf(resp, sizeof(resp),
			     "HTTP/1.1 101 Switching Protocols\r\n"
			     "Upgrade: websocket\r\nConnection: Upgrade\r\n"
			     "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
		if (send_all(fd, resp, n))
			return;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		ws_session(fd);
		return;
	}

	serve_static(fd, url);
}

static void *connection_thread(void *arg)
{
	int fd = (int)(intptr_t)arg;

	handle_connection(fd);
	close(fd);
	atomic_fetch_sub(&g_sessions, 1);
	return NULL;
}

static void on_signal(int sig)
{
	g_quit = 1;
}

static void usage(const char *prog)
{
	printf("Usage: %s [-p port] [-d dist-dir] [-s streamer]\n", prog);
	printf("  -p  TCP port (default %d)\n", DEFAULT_PORT);
	printf("  -d  front-end directory (default %s)\n", DEFAULT_DIST_DIR);
	printf("  -s  ADC streamer program (default %s)\n", DEFAULT_STREAMER);
}

int main(int argc, char *argv[])
{
	struct sigaction sa = { .sa_handler = on_signal };
	struct sockaddr_in addr = { .sin_family = AF_INET };
	int port = DEFAULT_PORT, lfd, opt, one = 1;
	double end;

	while ((opt = getopt(argc, argv, "p:d:s:h")) != -1) {
		switch (opt) {
		case 'p':
			port = atoi(optarg);
			break;
		case 'd':
			g_dist_dir = optarg;
			break;
		case 's':
			g_streamer = optarg;
			break;
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : 1;
		}
	}

	signal(SIGPIPE, SIG_IGN);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	lfd = socket(AF_INET, SOCK_STREAM, 0);
	if (lfd < 0) {
		perror("socket");
		return 1;
	}
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);
	if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) ||
	    listen(lfd, 8)) {
		perror("bind/listen");
		return 1;
	}
	logmsg("ADC server on port %d, front-end '%s', streamer '%s'", port,
	       g_dist_dir, g_streamer);

	while (!g_quit) {
		struct pollfd pfd = { .fd = lfd, .events = POLLIN };
		pthread_attr_t attr;
		pthread_t th;
		int fd;

		if (poll(&pfd, 1, 200) <= 0)
			continue;
		fd = accept(lfd, NULL, NULL);
		if (fd < 0)
			continue;

		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		atomic_fetch_add(&g_sessions, 1);
		if (pthread_create(&th, &attr, connection_thread,
				   (void *)(intptr_t)fd)) {
			atomic_fetch_sub(&g_sessions, 1);
			close(fd);
		}
		pthread_attr_destroy(&attr);
	}

	/* Let sessions stop their streamers */
	end = now_ms() + STREAMER_STOP_TIMEOUT_MS + 1000;
	while (atomic_load(&g_sessions) && now_ms() < end)
		usleep(20000);
	close(lfd);
	logmsg("server stopped");
	return 0;
}
