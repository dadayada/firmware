/*
 * fpvcam: one streamer for an FPV board with a MIPI sensor on a SigmaStar
 * infinity6e SoC and an optional UVC camera. Two RTSP paths on one port,
 * every setting reachable from a web page, saved and applied when changed.
 *
 * Threads, and what each one owns:
 *   main.c   main thread    start-up, signals, the hardware watchdog
 *   sdk.c    sensor thread  sensor -> VIF -> VPE -> VENC channel 0, the ISP
 *   usb.c    usb thread     the UVC device, VENC channel 1 or libjpeg
 *   rtsp.c   rtsp thread    RTSP requests; frames are sent by their producer
 *   http.c   http thread    the page and its API; only sets config and flags
 *
 * A setting never reaches the SDK from the http thread. It changes the
 * config under its lock, saves the file, and raises a flag; the thread that
 * owns the hardware picks the flag up between two frames. Every pipeline
 * call therefore comes from one thread, which is what the vendor libraries
 * were written for.
 */
#ifndef FPVCAM_H
#define FPVCAM_H

#include <stddef.h>
#include <stdint.h>
#include <syslog.h>

#define CONFIG_PATH "/etc/fpvcam.conf"

enum { STREAM_MAIN, STREAM_USB, STREAM_COUNT };
enum { CODEC_H264, CODEC_H265, CODEC_MJPEG };
enum { RC_CBR, RC_VBR, RC_AVBR };

/* What a changed setting has to restart. Several can be raised at once. */
#define A_ISP   0x01	/* an ISP call, no interruption */
#define A_ENC   0x02	/* the sensor's encoder channel is recreated */
#define A_PIPE  0x04	/* the whole sensor pipeline is rebuilt */
#define A_USB   0x08	/* the USB capture and its encoder restart */
#define A_RTSP  0x10	/* the RTSP listener is reopened */
#define A_HTTP  0x20	/* the web listener is reopened */
#define A_WDT   0x40	/* the watchdog timeout is reprogrammed */

struct config {
	/* sensor pipeline */
	int sensor_mode, width, height, fps, mirror, flip, nr3d;
	/* picture */
	int brightness, contrast, lightness, saturation, sharpness, grayscale;
	/* exposure */
	int exposure_mode, max_shutter_us, max_sensor_gain, max_isp_gain;
	int shutter_us, sensor_gain, ev_comp, antiflicker;
	/* white balance */
	int awb_mode, wb_r_gain, wb_g_gain, wb_b_gain;
	/* sensor encoder */
	int codec, rc_mode, bitrate, gop, profile, min_qp, max_qp;
	/* usb camera */
	int usb_enable, usb_width, usb_height, usb_fps, usb_codec;
	int usb_bitrate, usb_gop, usb_jpeg_quality, usb_cpu;
	char usb_device[64];
	/* server */
	int rtsp_port, rtp_payload, web_port, watchdog;
};

/* log.c is too small to exist: these live in main.c. */
void log_msg(int prio, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define LOGE(...) log_msg(LOG_ERR, __VA_ARGS__)
#define LOGW(...) log_msg(LOG_WARNING, __VA_ARGS__)
#define LOGI(...) log_msg(LOG_INFO, __VA_ARGS__)
uint64_t now_ms(void);
extern volatile int g_quit;
void main_request(int flags);

/* config.c */
void config_defaults(struct config *c);
void config_load(const char *path);
int config_save(void);
void config_get(struct config *out);
/* Sets one key. Returns the A_* flags the change needs, or -1 with a reason. */
int config_set(const char *key, const char *value, char *err, size_t errlen);
int config_reset(void);
size_t config_json(char *buf, size_t len);
const char *codec_name(int codec);

/* json.c is also too small to exist: a bounded appender, in config.c. */
struct sbuf { char *p; size_t len, cap; };
void sb_printf(struct sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_json_str(struct sbuf *b, const char *s);

/* sdk.c */
struct enc_params {
	int codec, width, height, fps, rc_mode, bitrate, gop, profile;
	int min_qp, max_qp;
};
int sdk_init(void);
void sdk_exit(void);
void *sensor_thread(void *arg);
void sensor_request(int flags);
void sensor_status_json(struct sbuf *b);
int venc_create(int chn, const struct enc_params *p);
void venc_destroy(int chn);
int venc_fd(int chn);
/* Drains one encoded frame of channel chn into the RTSP stream id. */
int venc_forward(int chn, int stream);
void venc_request_idr(int chn);
int venc_device(int chn, unsigned int *dev);
/* Hands one YUYV frame to an encoder channel that has no pipeline feeding it. */
int venc_inject_yuyv(int chn, unsigned int dev, int width, int height,
		     const uint8_t *yuyv, uint64_t pts_us);

/* usb.c */
void *usb_thread(void *arg);
void usb_request(void);
void usb_status_json(struct sbuf *b);

/* rtsp.c */
int rtsp_start(void);
void rtsp_request_restart(void);
/* Declares what a stream carries; a codec or size change drops its clients. */
void rtsp_set_stream(int id, int codec, int width, int height, int fps);
void rtsp_clear_stream(int id);
/* data is Annex-B, one or more NAL units; last marks the end of the frame. */
void rtsp_send_h26x(int id, const uint8_t *data, size_t len, uint64_t pts_us, int last);
void rtsp_send_jpeg(int id, const uint8_t *data, size_t len, uint64_t pts_us);
int rtsp_clients(int id);
/* True once after a client started playing: the producer owes it a keyframe. */
int rtsp_take_idr_request(int id);
void rtsp_stats(int id, unsigned int *fps_x10, unsigned int *kbps);

/* http.c */
int http_start(void);
void http_request_restart(void);

#endif
