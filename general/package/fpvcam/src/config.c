/*
 * The settings table. One row per setting carries everything the rest of the
 * program needs to know about it: the key in /etc/fpvcam.conf, the label and
 * hint the web page shows, the range it is validated against, and which part
 * of the pipeline a change restarts. The page builds its form from this
 * table (GET /api/config), so adding a setting is one row here plus the code
 * that reads the field.
 */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fpvcam.h"

enum { T_INT, T_BOOL, T_ENUM, T_STR };

struct setting {
	const char *key, *group, *label, *hint;
	int type, min, max;
	const char *opts;	/* T_ENUM: names, comma separated, index = value */
	const char *def;
	int apply;
	size_t off;
};

#define OFF(f) offsetof(struct config, f)

static const struct setting settings[] = {
	{ "sensor_mode", "Sensor", "Sensor mode",
	  "auto picks the smallest mode that covers the resolution and frame rate",
	  T_INT, -1, 15, NULL, "-1", A_PIPE, OFF(sensor_mode) },
	{ "width", "Sensor", "Width", "output width in pixels, scaled down from the sensor mode",
	  T_INT, 160, 3840, NULL, "1920", A_PIPE, OFF(width) },
	{ "height", "Sensor", "Height", "output height in pixels",
	  T_INT, 120, 2160, NULL, "1080", A_PIPE, OFF(height) },
	{ "fps", "Sensor", "Frame rate", "frames per second; limited by the sensor mode",
	  T_INT, 1, 240, NULL, "60", A_PIPE, OFF(fps) },
	{ "mirror", "Sensor", "Mirror", NULL, T_BOOL, 0, 1, NULL, "0", A_PIPE, OFF(mirror) },
	{ "flip", "Sensor", "Flip", NULL, T_BOOL, 0, 1, NULL, "0", A_PIPE, OFF(flip) },
	{ "nr3d", "Sensor", "3D noise reduction", "0 is off, 7 is strongest; higher smears motion",
	  T_INT, 0, 7, NULL, "1", A_PIPE, OFF(nr3d) },

	{ "brightness", "Picture", "Brightness", "0-100, -1 keeps the sensor tuning file's value",
	  T_INT, -1, 100, NULL, "-1", A_ISP, OFF(brightness) },
	{ "contrast", "Picture", "Contrast", "0-100, -1 keeps the tuning file's value",
	  T_INT, -1, 100, NULL, "-1", A_ISP, OFF(contrast) },
	{ "lightness", "Picture", "Lightness", "0-100, -1 keeps the tuning file's value",
	  T_INT, -1, 100, NULL, "-1", A_ISP, OFF(lightness) },
	{ "saturation", "Picture", "Saturation",
	  "multiplies the tuning file's strength: 32 is 1x, 64 is 2x, 0 is none; -1 leaves it alone",
	  T_INT, -1, 127, NULL, "-1", A_ISP, OFF(saturation) },
	{ "sharpness", "Picture", "Sharpness",
	  "percent of the tuning file's strength, 0-400; -1 leaves it alone",
	  T_INT, -1, 400, NULL, "-1", A_ISP, OFF(sharpness) },
	{ "grayscale", "Picture", "Grayscale", NULL, T_BOOL, 0, 1, NULL, "0", A_ISP, OFF(grayscale) },

	{ "exposure_mode", "Exposure", "Exposure", NULL,
	  T_ENUM, 0, 1, "auto,manual", "auto", A_ISP, OFF(exposure_mode) },
	{ "max_shutter_us", "Exposure", "Auto: longest shutter (us)",
	  "0 limits it to just under one frame period, so dim light cannot lower the frame rate",
	  T_INT, 0, 1000000, NULL, "0", A_ISP, OFF(max_shutter_us) },
	{ "max_sensor_gain", "Exposure", "Auto: highest sensor gain (x)",
	  "0 keeps the tuning file's limit", T_INT, 0, 4096, NULL, "0", A_ISP, OFF(max_sensor_gain) },
	{ "max_isp_gain", "Exposure", "Auto: highest digital gain (x)",
	  "0 keeps the tuning file's limit", T_INT, 0, 64, NULL, "0", A_ISP, OFF(max_isp_gain) },
	{ "ev_comp", "Exposure", "Auto: compensation (EV x10)", "-30 to 30, i.e. -3.0 to +3.0 EV",
	  T_INT, -30, 30, NULL, "0", A_ISP, OFF(ev_comp) },
	{ "shutter_us", "Exposure", "Manual: shutter (us)", NULL,
	  T_INT, 10, 1000000, NULL, "8000", A_ISP, OFF(shutter_us) },
	{ "sensor_gain", "Exposure", "Manual: sensor gain (x)", NULL,
	  T_INT, 1, 4096, NULL, "1", A_ISP, OFF(sensor_gain) },
	{ "antiflicker", "Exposure", "Anti-flicker", "mains frequency of the lighting",
	  T_ENUM, 0, 3, "off,60hz,50hz,auto", "off", A_ISP, OFF(antiflicker) },

	{ "awb_mode", "White balance", "White balance", NULL,
	  T_ENUM, 0, 1, "auto,manual", "auto", A_ISP, OFF(awb_mode) },
	{ "wb_r_gain", "White balance", "Manual: red gain", "1024 is 1x; the status box shows what auto chose",
	  T_INT, 0, 8192, NULL, "1024", A_ISP, OFF(wb_r_gain) },
	{ "wb_g_gain", "White balance", "Manual: green gain", NULL,
	  T_INT, 0, 8192, NULL, "1024", A_ISP, OFF(wb_g_gain) },
	{ "wb_b_gain", "White balance", "Manual: blue gain", NULL,
	  T_INT, 0, 8192, NULL, "1024", A_ISP, OFF(wb_b_gain) },

	{ "codec", "Sensor stream", "Codec", NULL,
	  T_ENUM, 0, 1, "h264,h265", "h264", A_ENC, OFF(codec) },
	{ "rc_mode", "Sensor stream", "Rate control", "cbr holds the bitrate, vbr and avbr treat it as a ceiling",
	  T_ENUM, 0, 2, "cbr,vbr,avbr", "cbr", A_ENC, OFF(rc_mode) },
	{ "bitrate", "Sensor stream", "Bitrate (kbit/s)", NULL,
	  T_INT, 128, 40000, NULL, "4096", A_ENC, OFF(bitrate) },
	{ "gop", "Sensor stream", "Keyframe interval (frames)", NULL,
	  T_INT, 1, 600, NULL, "30", A_ENC, OFF(gop) },
	{ "profile", "Sensor stream", "H.264 profile", NULL,
	  T_ENUM, 0, 2, "baseline,main,high", "high", A_ENC, OFF(profile) },
	{ "min_qp", "Sensor stream", "Lowest QP (vbr, avbr)", NULL,
	  T_INT, 0, 51, NULL, "12", A_ENC, OFF(min_qp) },
	{ "max_qp", "Sensor stream", "Highest QP (vbr, avbr)", NULL,
	  T_INT, 0, 51, NULL, "48", A_ENC, OFF(max_qp) },

	{ "usb_enable", "USB camera", "Enabled", NULL, T_BOOL, 0, 1, NULL, "1", A_USB, OFF(usb_enable) },
	{ "usb_device", "USB camera", "Device", NULL,
	  T_STR, 0, 0, NULL, "/dev/video0", A_USB, OFF(usb_device) },
	{ "usb_width", "USB camera", "Width", "what to ask the camera for; it answers with the nearest mode it has",
	  T_INT, 16, 1920, NULL, "384", A_USB, OFF(usb_width) },
	{ "usb_height", "USB camera", "Height", NULL, T_INT, 16, 1080, NULL, "288", A_USB, OFF(usb_height) },
	{ "usb_fps", "USB camera", "Frame rate", NULL, T_INT, 1, 120, NULL, "25", A_USB, OFF(usb_fps) },
	/* mjpeg until the hardware path has been seen working on a board: it is
	 * the one that is known to. */
	{ "usb_codec", "USB camera", "Codec", "h264 and h265 use the hardware encoder, mjpeg encodes on the CPU",
	  T_ENUM, 0, 2, "h264,h265,mjpeg", "mjpeg", A_USB, OFF(usb_codec) },
	{ "usb_bitrate", "USB camera", "Bitrate (kbit/s)", "h264 and h265 only",
	  T_INT, 64, 20000, NULL, "1024", A_USB, OFF(usb_bitrate) },
	{ "usb_gop", "USB camera", "Keyframe interval (frames)", "h264 and h265 only",
	  T_INT, 1, 600, NULL, "25", A_USB, OFF(usb_gop) },
	{ "usb_jpeg_quality", "USB camera", "JPEG quality", "mjpeg only, 1-100",
	  T_INT, 1, 100, NULL, "80", A_USB, OFF(usb_jpeg_quality) },
	{ "usb_cpu", "USB camera", "CPU core", "core the USB thread is pinned to, -1 lets the scheduler choose",
	  T_INT, -1, 3, NULL, "1", A_USB, OFF(usb_cpu) },

	{ "rtsp_port", "Server", "RTSP port", NULL, T_INT, 1, 65535, NULL, "554", A_RTSP, OFF(rtsp_port) },
	{ "rtp_payload", "Server", "RTP payload size (bytes)",
	  "1200 fits inside a WireGuard tunnel without fragmenting; up to 1400 on plain Ethernet",
	  T_INT, 500, 1440, NULL, "1200", 0, OFF(rtp_payload) },
	{ "web_port", "Server", "Web port", NULL, T_INT, 1, 65535, NULL, "80", A_HTTP, OFF(web_port) },
	{ "watchdog", "Server", "Watchdog (s)", "the board resets if fpvcam hangs this long; 0 turns it off",
	  T_INT, 0, 300, NULL, "30", A_WDT, OFF(watchdog) },
};

#define NSETTINGS (sizeof(settings) / sizeof(settings[0]))

static struct config cfg;
static char cfg_path[128] = CONFIG_PATH;
static pthread_mutex_t cfg_lock = PTHREAD_MUTEX_INITIALIZER;

const char *codec_name(int codec)
{
	return codec == CODEC_H264 ? "h264" : codec == CODEC_H265 ? "h265" : "mjpeg";
}

static int *int_field(struct config *c, const struct setting *s)
{
	return (int *)((char *)c + s->off);
}

static int enum_index(const char *opts, const char *name)
{
	size_t n = strlen(name);
	int i = 0;

	while (*opts) {
		const char *end = strchr(opts, ',');
		size_t len = end ? (size_t)(end - opts) : strlen(opts);

		if (len == n && !memcmp(opts, name, n))
			return i;
		if (!end)
			break;
		opts = end + 1;
		i++;
	}
	return -1;
}

static void enum_name(const char *opts, int index, char *out, size_t len)
{
	while (index-- > 0 && (opts = strchr(opts, ',')))
		opts++;
	snprintf(out, len, "%.*s", opts ? (int)strcspn(opts, ",") : 0, opts ? opts : "");
}

/* Validates value for s and stores it in c. 0, or -1 with a reason. */
static int store(struct config *c, const struct setting *s, const char *value,
		 char *err, size_t errlen)
{
	char *end;
	long v;

	switch (s->type) {
	case T_STR:
		if (!*value || strlen(value) >= sizeof(c->usb_device) || strpbrk(value, "\"\\\n")) {
			snprintf(err, errlen, "not a usable value");
			return -1;
		}
		strcpy((char *)c + s->off, value);
		return 0;
	case T_ENUM:
		v = enum_index(s->opts, value);
		if (v < 0) {
			snprintf(err, errlen, "must be one of %s", s->opts);
			return -1;
		}
		break;
	case T_BOOL:
		if (!strcmp(value, "1") || !strcmp(value, "true") || !strcmp(value, "on"))
			v = 1;
		else if (!strcmp(value, "0") || !strcmp(value, "false") || !strcmp(value, "off"))
			v = 0;
		else {
			snprintf(err, errlen, "must be 0 or 1");
			return -1;
		}
		break;
	default:
		errno = 0;
		v = strtol(value, &end, 10);
		if (errno || end == value || *end || v < s->min || v > s->max) {
			snprintf(err, errlen, "must be a number from %d to %d", s->min, s->max);
			return -1;
		}
	}
	*int_field(c, s) = (int)v;
	return 0;
}

static void format(const struct config *c, const struct setting *s, char *out, size_t len)
{
	if (s->type == T_STR)
		snprintf(out, len, "%s", (const char *)c + s->off);
	else if (s->type == T_ENUM)
		enum_name(s->opts, *int_field((struct config *)c, s), out, len);
	else
		snprintf(out, len, "%d", *int_field((struct config *)c, s));
}

void config_defaults(struct config *c)
{
	char err[64];
	size_t i;

	memset(c, 0, sizeof(*c));
	for (i = 0; i < NSETTINGS; i++)
		store(c, &settings[i], settings[i].def, err, sizeof(err));
}

static const struct setting *find(const char *key)
{
	size_t i;

	for (i = 0; i < NSETTINGS; i++)
		if (!strcmp(settings[i].key, key))
			return &settings[i];
	return NULL;
}

/*
 * A missing file is the normal first boot: the defaults stand and the file
 * appears the first time a setting is changed. A bad line never stops the
 * program, because the camera has to come up with a picture so that the
 * setting can be corrected from the page.
 */
void config_load(const char *path)
{
	char line[256], err[96];
	FILE *f;

	pthread_mutex_lock(&cfg_lock);
	/* Saves go back to the file the settings were read from. */
	snprintf(cfg_path, sizeof(cfg_path), "%s", path);
	config_defaults(&cfg);
	f = fopen(path, "r");
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			const struct setting *s;
			char *eq, *key = line + strspn(line, " \t");

			key[strcspn(key, "\r\n")] = '\0';
			if (!*key || *key == '#')
				continue;
			eq = strchr(key, '=');
			if (!eq)
				continue;
			*eq++ = '\0';
			s = find(key);
			if (!s)
				LOGW("config: unknown setting '%s' ignored", key);
			else if (store(&cfg, s, eq, err, sizeof(err)))
				LOGW("config: %s=%s rejected (%s), using %s", key, eq, err, s->def);
		}
		fclose(f);
	} else if (errno != ENOENT) {
		LOGW("config: cannot read %s: %s", path, strerror(errno));
	}
	pthread_mutex_unlock(&cfg_lock);
}

/*
 * Written beside the target and renamed over it, so a power cut during a
 * save leaves either the old file or the new one, never half of one. /etc is
 * jffs2 on this board and rename is atomic there.
 */
static int save_locked(const char *path)
{
	char tmp[136], val[96];
	size_t i;
	FILE *f;
	int ok;

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	f = fopen(tmp, "w");
	if (!f) {
		LOGE("config: cannot write %s: %s", tmp, strerror(errno));
		return -1;
	}
	fprintf(f, "# fpvcam settings. Written by the web page; edit by hand only while\n"
		   "# fpvcam is stopped, or it will overwrite the file on the next change.\n");
	for (i = 0; i < NSETTINGS; i++) {
		format(&cfg, &settings[i], val, sizeof(val));
		fprintf(f, "%s=%s\n", settings[i].key, val);
	}
	ok = !ferror(f) && !fflush(f) && !fsync(fileno(f));
	ok = !fclose(f) && ok;
	if (!ok || rename(tmp, path)) {
		LOGE("config: saving %s failed: %s", path, strerror(errno));
		unlink(tmp);
		return -1;
	}
	return 0;
}

int config_save(void)
{
	int r;

	pthread_mutex_lock(&cfg_lock);
	r = save_locked(cfg_path);
	pthread_mutex_unlock(&cfg_lock);
	return r;
}

void config_get(struct config *out)
{
	pthread_mutex_lock(&cfg_lock);
	*out = cfg;
	pthread_mutex_unlock(&cfg_lock);
}

int config_set(const char *key, const char *value, char *err, size_t errlen)
{
	const struct setting *s = find(key);
	char before[96], after[96];
	int flags = 0;

	if (!s) {
		snprintf(err, errlen, "no such setting");
		return -1;
	}
	pthread_mutex_lock(&cfg_lock);
	format(&cfg, s, before, sizeof(before));
	if (store(&cfg, s, value, err, errlen)) {
		pthread_mutex_unlock(&cfg_lock);
		return -1;
	}
	format(&cfg, s, after, sizeof(after));
	if (strcmp(before, after)) {
		flags = s->apply;
		LOGI("config: %s %s -> %s", key, before, after);
	}
	pthread_mutex_unlock(&cfg_lock);
	return flags;
}

int config_reset(void)
{
	pthread_mutex_lock(&cfg_lock);
	config_defaults(&cfg);
	pthread_mutex_unlock(&cfg_lock);
	LOGI("config: every setting back to its default");
	return A_ISP | A_PIPE | A_USB | A_RTSP | A_HTTP | A_WDT;
}

void sb_printf(struct sbuf *b, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (b->len >= b->cap)
		return;
	va_start(ap, fmt);
	n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	b->len += (size_t)n;
	if (b->len >= b->cap)
		b->len = b->cap - 1;
}

void sb_json_str(struct sbuf *b, const char *s)
{
	sb_printf(b, "\"");
	for (; s && *s; s++) {
		if (*s == '"' || *s == '\\')
			sb_printf(b, "\\%c", *s);
		else if ((unsigned char)*s < 0x20)
			sb_printf(b, " ");
		else
			sb_printf(b, "%c", *s);
	}
	sb_printf(b, "\"");
}

static const char *apply_name(int apply)
{
	switch (apply) {
	case A_ISP:  return "live";
	case A_ENC:  return "encoder";
	case A_PIPE: return "pipeline";
	case A_USB:  return "usb";
	default:     return "server";
	}
}

size_t config_json(char *buf, size_t len)
{
	static const char *const types[] = { "int", "bool", "enum", "str" };
	struct sbuf b = { buf, 0, len };
	struct config c;
	char val[96];
	size_t i;

	config_get(&c);
	sb_printf(&b, "{\"settings\":[");
	for (i = 0; i < NSETTINGS; i++) {
		const struct setting *s = &settings[i];

		format(&c, s, val, sizeof(val));
		sb_printf(&b, "%s{\"key\":\"%s\",\"group\":\"%s\",\"label\":", i ? "," : "",
			  s->key, s->group);
		sb_json_str(&b, s->label);
		sb_printf(&b, ",\"type\":\"%s\",\"min\":%d,\"max\":%d,\"apply\":\"%s\",\"value\":",
			  types[s->type], s->min, s->max, apply_name(s->apply));
		sb_json_str(&b, val);
		sb_printf(&b, ",\"def\":");
		sb_json_str(&b, s->def);
		if (s->opts) {
			sb_printf(&b, ",\"opts\":");
			sb_json_str(&b, s->opts);
		}
		if (s->hint) {
			sb_printf(&b, ",\"hint\":");
			sb_json_str(&b, s->hint);
		}
		sb_printf(&b, "}");
	}
	sb_printf(&b, "]}");
	return b.len;
}
