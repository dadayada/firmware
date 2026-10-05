/*
 * Start-up, shutdown, logging and the hardware watchdog. See fpvcam.h for
 * which thread owns what.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <linux/watchdog.h>

#include "fpvcam.h"

volatile int g_quit;
static int to_stderr;
static int main_pending;

/*
 * Everything goes to syslog, because the init script starts this with
 * start-stop-daemon -b and that sends stderr nowhere: Divinus' own messages
 * could not be read on the camera for exactly that reason. `logread | grep
 * fpvcam` shows them. -v adds stderr for a run from a shell.
 */
void log_msg(int prio, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsyslog(prio, fmt, ap);
	va_end(ap);
	if (to_stderr) {
		va_start(ap, fmt);
		vfprintf(stderr, fmt, ap);
		va_end(ap);
		fputc('\n', stderr);
	}
}

uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

void main_request(int flags)
{
	__sync_fetch_and_or(&main_pending, flags);
}

static void on_signal(int sig)
{
	(void)sig;
	g_quit = 1;
}

/* ---- watchdog ---- */

static int wdt = -1;

/*
 * Fed once a second from the main thread for as long as the process lives.
 * A crash or a kill -9 closes the device without the magic 'V', the kernel
 * keeps counting, and the board resets: on a camera nobody can reach, that
 * is the recovery. A clean stop writes the 'V' so that `S95fpvcam stop`
 * does not reboot the camera.
 */
static void watchdog_set(int seconds)
{
	static const char *const paths[] = { "/dev/watchdog0", "/dev/watchdog" };
	size_t i;

	if (!seconds) {
		if (wdt >= 0) {
			if (write(wdt, "V", 1) != 1)
				LOGW("watchdog: could not disarm it: %s", strerror(errno));
			close(wdt);
			wdt = -1;
			LOGI("watchdog: off");
		}
		return;
	}
	/* A timeout of a few seconds would reset the board during an ordinary
	 * pipeline restart, which holds the page that could raise it again. */
	if (seconds < 10)
		seconds = 10;
	for (i = 0; wdt < 0 && i < sizeof(paths) / sizeof(paths[0]); i++)
		wdt = open(paths[i], O_WRONLY | O_CLOEXEC);
	if (wdt < 0) {
		LOGW("watchdog: no watchdog device, running without one");
		return;
	}
	if (ioctl(wdt, WDIOC_SETTIMEOUT, &seconds))
		LOGW("watchdog: cannot set the timeout: %s", strerror(errno));
	else
		LOGI("watchdog: resets the board after %d s without fpvcam", seconds);
}

static void watchdog_feed(void)
{
	if (wdt >= 0 && write(wdt, "", 1) != 1)
		LOGW("watchdog: feeding it failed: %s", strerror(errno));
}

int main(int argc, char **argv)
{
	const char *config = CONFIG_PATH;
	pthread_t sensor, usb;
	struct sigaction sa;
	struct config c;
	int opt;

	while ((opt = getopt(argc, argv, "c:v")) != -1) {
		switch (opt) {
		case 'c':
			config = optarg;
			break;
		case 'v':
			to_stderr = 1;
			break;
		default:
			fprintf(stderr, "usage: fpvcam [-c config] [-v]\n"
				"  -c  settings file (default " CONFIG_PATH ")\n"
				"  -v  log to stderr as well as syslog\n");
			return 2;
		}
	}
	openlog("fpvcam", LOG_PID, LOG_DAEMON);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, NULL);

	config_load(config);
	config_get(&c);
	LOGI("starting: sensor %dx%d@%d %s, usb %s", c.width, c.height, c.fps,
	     codec_name(c.codec), c.usb_enable ? codec_name(c.usb_codec) : "off");

	if (sdk_init()) {
		LOGE("the SigmaStar SDK did not initialise; are the mi_* kernel modules loaded?");
		return 1;
	}
	watchdog_set(c.watchdog);
	night_apply(&c);
	/* The servers come up before the pipelines: if the sensor cannot
	 * start, the page that says why and lets it be fixed is reachable. */
	if (rtsp_start() || http_start() ||
	    pthread_create(&sensor, NULL, sensor_thread, NULL) ||
	    pthread_create(&usb, NULL, usb_thread, NULL)) {
		LOGE("cannot start a thread: %s", strerror(errno));
		watchdog_set(0);
		return 1;
	}

	while (!g_quit) {
		int flags = __sync_fetch_and_and(&main_pending, 0), i;

		if (flags) {
			config_get(&c);
			if (flags & A_WDT)
				watchdog_set(c.watchdog);
			if (flags & A_NIGHT)
				night_apply(&c);
		}
		watchdog_feed();
		/* In tenths, so a click on the page moves the filter at once. */
		for (i = 0; i < 10 && !g_quit && !main_pending; i++)
			usleep(100000);
	}

	LOGI("stopping");
	/* The encoder channels and the sensor are torn down by their own
	 * threads; leaving them up would make the next start fail on
	 * "channel exists". The watchdog stays armed until they are done. */
	pthread_join(usb, NULL);
	pthread_join(sensor, NULL);
	sdk_exit();
	watchdog_set(0);
	return 0;
}
