/*
 * The day/night filter: a piece of infrared-blocking glass on a small
 * actuator in front of the sensor. Daylight needs it in the light path, or
 * infrared comes through the colour dyes and the picture goes violet; in the
 * dark it is moved out, so the sensor can use what infrared there is, and
 * the picture is switched to black and white because the colours would be
 * wrong anyway.
 *
 * The actuator hangs on an H-bridge across two GPIO pads. One pad high with
 * the other low moves it one way, the reverse moves it back, and both low
 * holds it without drawing current. So a move is a pulse, and the pads are
 * left low afterwards. Seen on an SSC338Q board with pads 24 and 23,
 * 2026-10-05: nothing had ever driven them, the filter sat in the night
 * position and every daylight picture was violet until the first pulse.
 *
 * The filter is put where the settings say on every start, not only when
 * they change: where it was left at power-off is not something to rely on.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "fpvcam.h"

#define PULSE_MS 300
/* Elsewhere only when the logic is exercised off the board. */
#ifndef GPIO_ROOT
#define GPIO_ROOT "/sys/class/gpio"
#endif

static int put(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = write(fd, text, strlen(text));
	close(fd);
	return n < 0 ? -1 : 0;
}

static int gpio_set(int pin, int value)
{
	char path[64], num[16];

	snprintf(path, sizeof(path), GPIO_ROOT "/gpio%d/value", pin);
	if (access(path, W_OK)) {
		snprintf(num, sizeof(num), "%d", pin);
		put(GPIO_ROOT "/export", num);
	}
	/* "low" and "high" set the direction and the level in one step, so
	 * the pad never drives the wrong way in between. */
	snprintf(path, sizeof(path), GPIO_ROOT "/gpio%d/direction", pin);
	return put(path, value ? "high" : "low");
}

void night_apply(const struct config *c)
{
	static int day_pin = -2, night_pin = -2, mode = -1;
	int high, low;

	if (c->ircut_pin_day == day_pin && c->ircut_pin_night == night_pin && c->night_mode == mode)
		return;
	day_pin = c->ircut_pin_day;
	night_pin = c->ircut_pin_night;
	mode = c->night_mode;
	if (day_pin < 0 || night_pin < 0 || day_pin == night_pin)
		return;

	high = mode ? night_pin : day_pin;
	low = mode ? day_pin : night_pin;
	if (gpio_set(low, 0) || gpio_set(high, 1)) {
		gpio_set(high, 0);
		LOGW("night: cannot drive GPIO %d and %d; the filter was not moved", high, low);
		return;
	}
	usleep(PULSE_MS * 1000);
	gpio_set(high, 0);
	LOGI("night: filter moved to the %s position (GPIO %d high for %d ms, %d low)",
	     mode ? "night" : "day", high, PULSE_MS, low);
}
