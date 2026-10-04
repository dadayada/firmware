/*
 * The web page and the four requests behind it:
 *
 *   GET  /              the page (page.html, linked in by page.S)
 *   GET  /api/config    every setting with its label, range and value
 *   GET  /api/status    what the hardware is doing right now
 *   POST /api/set       key=value&key=value; validated, saved, applied
 *                       (save=0 among them applies without saving: a way to
 *                       try a setting that a reboot will undo)
 *   POST /api/defaults  every setting back to its default
 *
 * One connection at a time, closed after the reply. The only clients are a
 * settings page and the occasional curl, and a camera that serves them one
 * by one has nothing here that can be made to wait on anything else.
 *
 * There is no login, as there was none on the Divinus page this replaces:
 * the camera sits on a point-to-point link or inside a WireGuard tunnel.
 * Put it on a shared network and anyone there can change its settings.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "fpvcam.h"

extern const char page_html[], page_html_end[];

static volatile int restart;
static uint64_t started;

void http_request_restart(void)
{
	restart = 1;
}

static void send_all(int fd, const char *p, size_t len)
{
	while (len) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);

		if (n <= 0)
			return;
		p += n;
		len -= (size_t)n;
	}
}

static void respond(int fd, const char *status, const char *type, const char *body, size_t len)
{
	char head[256];
	int n = snprintf(head, sizeof(head),
			 "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
			 "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
			 status, type, (unsigned int)len);

	send_all(fd, head, (size_t)n);
	send_all(fd, body, len);
}

static void url_decode(char *s)
{
	char *o = s;

	for (; *s; s++) {
		if (*s == '+') {
			*o++ = ' ';
		} else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
			char hex[3] = { s[1], s[2], 0 };

			*o++ = (char)strtol(hex, NULL, 16);
			s += 2;
		} else {
			*o++ = *s;
		}
	}
	*o = '\0';
}

static void dispatch(int flags)
{
	if (flags & (A_ISP | A_ENC | A_PIPE))
		sensor_request(flags & (A_ISP | A_ENC | A_PIPE));
	if (flags & A_USB)
		usb_request();
	if (flags & A_RTSP)
		rtsp_request_restart();
	if (flags & A_HTTP)
		http_request_restart();
	if (flags & A_WDT)
		main_request(A_WDT);
}

/*
 * Each pair is validated and stored on its own, so one bad value does not
 * hold back the others in the same request; the reply names every key that
 * was refused and why. The file is written before anything is restarted: a
 * setting that takes the pipeline down must already be on flash if the
 * board is power-cycled to get out of it.
 */
static void api_set(int fd, char *args)
{
	char out[2048], err[96];
	struct sbuf b = { out, 0, sizeof(out) };
	int flags = 0, errors = 0, saved = 1, keep = 1;
	char *pair, *save = NULL;

	sb_printf(&b, "{\"errors\":{");
	for (pair = strtok_r(args, "&", &save); pair; pair = strtok_r(NULL, "&", &save)) {
		char *value = strchr(pair, '=');
		int r;

		if (!value)
			continue;
		*value++ = '\0';
		url_decode(pair);
		url_decode(value);
		if (!strcmp(pair, "save")) {
			keep = strcmp(value, "0") != 0;
			continue;
		}
		r = config_set(pair, value, err, sizeof(err));
		if (r < 0) {
			sb_printf(&b, "%s", errors++ ? "," : "");
			sb_json_str(&b, pair);
			sb_printf(&b, ":");
			sb_json_str(&b, err);
		} else {
			flags |= r | 0x10000;
		}
	}
	if (flags && keep)
		saved = !config_save();
	else if (flags) {
		saved = 0;
		LOGI("web: applied without saving, as asked");
	}
	dispatch(flags);
	sb_printf(&b, "},\"ok\":%s,\"saved\":%s,\"restarts\":\"%s\"}",
		  errors ? "false" : "true", saved ? "true" : "false",
		  flags & A_PIPE ? "pipeline" : flags & A_ENC ? "encoder" :
		  flags & A_USB ? "usb" : flags & (A_RTSP | A_HTTP) ? "server" : "");
	respond(fd, "200 OK", "application/json", out, b.len);
}

static void api_status(int fd)
{
	char out[4096];
	struct sbuf b = { out, 0, sizeof(out) };
	struct config c;

	config_get(&c);
	sb_printf(&b, "{\"uptime\":%u,\"rtsp_port\":%d,", (unsigned int)((now_ms() - started) / 1000),
		  c.rtsp_port);
	sensor_status_json(&b);
	sb_printf(&b, ",");
	usb_status_json(&b);
	sb_printf(&b, "}");
	respond(fd, "200 OK", "application/json", out, b.len);
}

static void serve(int fd)
{
	static char req[16384], cfg_json[20480];
	size_t len = 0, need = 0;
	char *body = NULL, *path, *query;

	for (;;) {
		ssize_t n = recv(fd, req + len, sizeof(req) - 1 - len, 0);

		if (n <= 0)
			return;
		len += (size_t)n;
		req[len] = '\0';
		if (!body && (body = strstr(req, "\r\n\r\n"))) {
			const char *cl = strcasestr(req, "\r\nContent-Length:");

			body += 4;
			need = (size_t)(body - req) + (cl && cl < body ? (size_t)atoi(cl + 17) : 0);
		}
		if (body && len >= need)
			break;
		if (len >= sizeof(req) - 1) {
			respond(fd, "413 Payload Too Large", "text/plain", "too large\n", 10);
			return;
		}
	}

	path = strchr(req, ' ');
	if (!path)
		return;
	*path++ = '\0';
	path[strcspn(path, " \r\n")] = '\0';
	query = strchr(path, '?');
	if (query)
		*query++ = '\0';

	if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
		respond(fd, "200 OK", "text/html; charset=utf-8", page_html,
			(size_t)(page_html_end - page_html));
	} else if (!strcmp(path, "/api/config")) {
		respond(fd, "200 OK", "application/json", cfg_json, config_json(cfg_json, sizeof(cfg_json)));
	} else if (!strcmp(path, "/api/status")) {
		api_status(fd);
	} else if (!strcmp(path, "/api/set")) {
		api_set(fd, !strcmp(req, "POST") ? body : query ? query : body);
	} else if (!strcmp(path, "/api/defaults") && !strcmp(req, "POST")) {
		int flags = config_reset();

		config_save();
		dispatch(flags);
		respond(fd, "200 OK", "application/json", "{\"ok\":true}", 11);
	} else {
		respond(fd, "404 Not Found", "text/plain", "not found\n", 10);
	}
}

static int open_listener(int port)
{
	struct sockaddr_in a;
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1;

	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons((uint16_t)port);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) || listen(fd, 8)) {
		LOGE("web: cannot listen on port %d: %s", port, strerror(errno));
		close(fd);
		return -1;
	}
	LOGI("web: listening on port %d", port);
	return fd;
}

static void *http_thread(void *arg)
{
	struct timeval tv = { 3, 0 };
	uint64_t retry = 0;
	int lfd = -1;

	(void)arg;
	while (!g_quit) {
		struct pollfd pfd;
		int fd;

		if (restart || (lfd < 0 && now_ms() >= retry)) {
			struct config c;

			config_get(&c);
			restart = 0;
			if (lfd >= 0)
				close(lfd);
			lfd = open_listener(c.web_port);
			retry = now_ms() + 3000;
		}
		if (lfd < 0) {
			usleep(200000);
			continue;
		}
		pfd.fd = lfd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, 300) <= 0)
			continue;
		fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
		if (fd < 0)
			continue;
		/* A browser that opens a connection and says nothing must not
		 * keep the page from everyone else. */
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		serve(fd);
		close(fd);
	}
	return NULL;
}

int http_start(void)
{
	pthread_t t;

	started = now_ms();
	restart = 1;
	return pthread_create(&t, NULL, http_thread, NULL);
}
