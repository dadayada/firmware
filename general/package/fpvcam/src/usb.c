/*
 * The USB camera: a UVC device read through V4L2 and served as the /usb
 * stream. The camera this was written for, an InfiRay 384x288 thermal core,
 * offers raw YUYV and nothing else, so its frames have to be compressed
 * here. Two ways, chosen by the usb_codec setting:
 *
 *   h264, h265  the frame is converted to NV12 and handed to a second
 *               channel of the SoC's encoder. Almost no CPU, and a
 *               fraction of the bitrate.
 *   mjpeg       libjpeg (libjpeg-turbo, NEON) on the CPU, what usbmjpeg
 *               did. Kept because it is known to work on this board.
 *
 * A camera that delivers MJPEG itself is passed through untouched when
 * usb_codec is mjpeg.
 *
 * The camera can be unplugged and plugged back at any time; the thread just
 * keeps trying to open it. Nothing here may take the sensor stream down.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <jpeglib.h>

#include "fpvcam.h"

#define NBUF	4
#define VENC_CH	1

struct capture {
	int fd, width, height, fps, mjpeg;
	void *maps[NBUF];
	size_t lens[NBUF];
};

struct jpeg {
	struct jpeg_compress_struct cinfo;
	struct jpeg_error_mgr err;
	jmp_buf fail;
	int ready;
	uint8_t *line;		/* one scanline of packed Y Cb Cr */
	unsigned char *out;
	unsigned long cap;
};

static struct {
	char state[48], error[96], encoder[24];
	int width, height, fps;
} U;
static pthread_mutex_t status_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int pending;

void usb_request(void)
{
	pending = 1;
}

static void set_state(const char *state, const char *error)
{
	pthread_mutex_lock(&status_lock);
	/* The same failure is retried every few seconds; say it once. */
	if (*error && strcmp(U.error, error))
		LOGW("usb: %s", error);
	snprintf(U.state, sizeof(U.state), "%s", state);
	snprintf(U.error, sizeof(U.error), "%s", error);
	pthread_mutex_unlock(&status_lock);
}

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do {
		r = ioctl(fd, req, arg);
	} while (r == -1 && errno == EINTR);
	return r;
}

static uint64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

/* ---- V4L2 ---- */

static void close_capture(struct capture *c)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	int i;

	if (c->fd < 0)
		return;
	xioctl(c->fd, VIDIOC_STREAMOFF, &type);
	for (i = 0; i < NBUF; i++)
		if (c->maps[i])
			munmap(c->maps[i], c->lens[i]);
	close(c->fd);
	memset(c, 0, sizeof(*c));
	c->fd = -1;
}

static int open_capture(struct capture *c, const struct config *cfg)
{
	struct v4l2_format fmt;
	struct v4l2_streamparm parm;
	struct v4l2_requestbuffers req;
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	char msg[96];
	unsigned int i;

	memset(c, 0, sizeof(*c));
	c->fd = open(cfg->usb_device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (c->fd < 0) {
		snprintf(msg, sizeof(msg), "no camera at %.40s", cfg->usb_device);
		set_state(msg, "");
		return -1;
	}

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = type;
	fmt.fmt.pix.width = (unsigned int)cfg->usb_width;
	fmt.fmt.pix.height = (unsigned int)cfg->usb_height;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	if (xioctl(c->fd, VIDIOC_S_FMT, &fmt) < 0) {
		snprintf(msg, sizeof(msg), "%.40s: cannot set a format: %s", cfg->usb_device, strerror(errno));
		goto fail;
	}
	/* The driver answers with what the camera really has. The size is
	 * taken as given; the pixel format decides what can be done with it. */
	c->width = (int)fmt.fmt.pix.width;
	c->height = (int)fmt.fmt.pix.height;
	if (fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_MJPEG) {
		if (cfg->usb_codec != CODEC_MJPEG) {
			snprintf(msg, sizeof(msg), "this camera delivers MJPEG only; set the USB codec to mjpeg");
			goto fail;
		}
		c->mjpeg = 1;
	} else if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
		snprintf(msg, sizeof(msg), "the camera offers neither YUYV nor MJPEG");
		goto fail;
	} else if ((c->width | c->height) & 1) {
		snprintf(msg, sizeof(msg), "odd frame size %dx%d cannot be encoded", c->width, c->height);
		goto fail;
	}

	/* Frame rate is a request; a camera with one rate ignores it. */
	memset(&parm, 0, sizeof(parm));
	parm.type = type;
	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = (unsigned int)cfg->usb_fps;
	xioctl(c->fd, VIDIOC_S_PARM, &parm);
	c->fps = cfg->usb_fps;
	if (!xioctl(c->fd, VIDIOC_G_PARM, &parm) && parm.parm.capture.timeperframe.numerator)
		c->fps = (int)((parm.parm.capture.timeperframe.denominator +
				parm.parm.capture.timeperframe.numerator / 2) /
			       parm.parm.capture.timeperframe.numerator);
	if (c->fps < 1)
		c->fps = cfg->usb_fps;

	memset(&req, 0, sizeof(req));
	req.count = NBUF;
	req.type = type;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(c->fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
		snprintf(msg, sizeof(msg), "%.40s: cannot get capture buffers", cfg->usb_device);
		goto fail;
	}
	for (i = 0; i < req.count && i < NBUF; i++) {
		struct v4l2_buffer b;
		void *p;

		memset(&b, 0, sizeof(b));
		b.type = type;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		if (xioctl(c->fd, VIDIOC_QUERYBUF, &b) < 0) {
			snprintf(msg, sizeof(msg), "%.40s: cannot query a capture buffer", cfg->usb_device);
			goto fail;
		}
		/* The raw system call, in pages: independent of which mmap
		 * wrapper and which off_t width this file was built with. */
		p = (void *)syscall(SYS_mmap2, NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED,
				    c->fd, b.m.offset >> 12);
		if (p == MAP_FAILED) {
			snprintf(msg, sizeof(msg), "%.40s: cannot map a capture buffer", cfg->usb_device);
			goto fail;
		}
		c->maps[i] = p;
		c->lens[i] = b.length;
		if (xioctl(c->fd, VIDIOC_QBUF, &b) < 0) {
			snprintf(msg, sizeof(msg), "%.40s: cannot queue a capture buffer", cfg->usb_device);
			goto fail;
		}
	}
	if (xioctl(c->fd, VIDIOC_STREAMON, &type) < 0) {
		snprintf(msg, sizeof(msg), "%.40s: cannot start capture: %s", cfg->usb_device, strerror(errno));
		goto fail;
	}
	return 0;
fail:
	set_state("error", msg);
	close_capture(c);
	return -1;
}

/* ---- libjpeg ---- */

static void jpeg_fail(j_common_ptr cinfo)
{
	struct jpeg *j = (struct jpeg *)cinfo->client_data;

	/* libjpeg's own handler calls exit(). One bad frame must not take
	 * the sensor stream with it. */
	longjmp(j->fail, 1);
}

static int jpeg_open(struct jpeg *j, int width, int height, int quality)
{
	memset(j, 0, sizeof(*j));
	j->cinfo.err = jpeg_std_error(&j->err);
	j->err.error_exit = jpeg_fail;
	j->cinfo.client_data = j;
	if (setjmp(j->fail))
		return -1;
	jpeg_create_compress(&j->cinfo);
	j->ready = 1;
	j->cinfo.image_width = (JDIMENSION)width;
	j->cinfo.image_height = (JDIMENSION)height;
	j->cinfo.input_components = 3;
	j->cinfo.in_color_space = JCS_YCbCr;
	jpeg_set_defaults(&j->cinfo);
	jpeg_set_colorspace(&j->cinfo, JCS_YCbCr);
	/* 4:2:0, RTP/JPEG type 1. jpeg_set_defaults already picks this for a
	 * YCbCr input; spelled out so it cannot drift. */
	j->cinfo.comp_info[0].h_samp_factor = 2;
	j->cinfo.comp_info[0].v_samp_factor = 2;
	j->cinfo.comp_info[1].h_samp_factor = 1;
	j->cinfo.comp_info[1].v_samp_factor = 1;
	j->cinfo.comp_info[2].h_samp_factor = 1;
	j->cinfo.comp_info[2].v_samp_factor = 1;
	/* Above 95 a baseline JPEG can outgrow the raw frame, and with it
	 * the buffer below. */
	jpeg_set_quality(&j->cinfo, quality > 95 ? 95 : quality, TRUE);
	/* RTP/JPEG has no room for Huffman tables: the standard ones only. */
	j->cinfo.optimize_coding = FALSE;
	j->cinfo.dct_method = JDCT_ISLOW;

	j->line = malloc((size_t)width * 3);
	j->cap = (unsigned long)width * (unsigned long)height * 2;
	j->out = malloc(j->cap);
	return j->line && j->out ? 0 : -1;
}

static void jpeg_close(struct jpeg *j)
{
	if (j->ready)
		jpeg_destroy_compress(&j->cinfo);
	free(j->line);
	free(j->out);
	memset(j, 0, sizeof(*j));
}

/* YUYV (Y0 Cb Y1 Cr per pixel pair) to packed Y Cb Cr, a row at a time,
 * straight into libjpeg, whose downsampler makes the 4:2:0 planes. */
static unsigned long jpeg_encode(struct jpeg *j, const uint8_t *yuyv, int width, int height)
{
	unsigned char *out = j->out;
	unsigned long size = j->cap;
	JSAMPROW row[1] = { j->line };

	if (setjmp(j->fail)) {
		jpeg_abort_compress(&j->cinfo);
		return 0;
	}
	jpeg_mem_dest(&j->cinfo, &out, &size);
	jpeg_start_compress(&j->cinfo, TRUE);
	while (j->cinfo.next_scanline < (JDIMENSION)height) {
		const uint8_t *src = yuyv + (size_t)j->cinfo.next_scanline * (size_t)width * 2;
		uint8_t *dst = j->line;
		int x;

		for (x = 0; x < width; x += 2) {
			dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[3];
			dst[3] = src[2]; dst[4] = src[1]; dst[5] = src[3];
			src += 4;
			dst += 6;
		}
		jpeg_write_scanlines(&j->cinfo, row, 1);
	}
	jpeg_finish_compress(&j->cinfo);
	if (out != j->out) {
		/* libjpeg outgrew the buffer and allocated its own. Keep it. */
		free(j->out);
		j->out = out;
		j->cap = size;
	}
	return size;
}

/* ---- the thread ---- */

static void pin(int cpu)
{
	cpu_set_t set;
	int i, n = (int)sysconf(_SC_NPROCESSORS_ONLN);

	/* The sensor pipeline's threads sit on core 0; a JPEG encoder
	 * sharing it cost the main stream frames (2026-09-28). */
	CPU_ZERO(&set);
	if (cpu >= 0 && cpu < n)
		CPU_SET(cpu, &set);
	else
		for (i = 0; i < n; i++)
			CPU_SET(i, &set);
	pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

/* One session: from opening the camera until a setting changes, the camera
 * goes away, or something fails. Returns how long to wait before the next. */
static int session(const struct config *cfg)
{
	struct capture cap;
	struct jpeg jpg;
	struct pollfd pfd[2];
	struct enc_params p;
	uint64_t last_frame;
	unsigned int dev = 0, inject_failed = 0, frames = 0;
	int hw, venc_up = 0, nfds = 1, wait = 2000, kicked = 0;

	memset(&jpg, 0, sizeof(jpg));
	if (open_capture(&cap, cfg))
		return 2000;
	hw = cfg->usb_codec != CODEC_MJPEG;

	if (hw) {
		memset(&p, 0, sizeof(p));
		p.codec = cfg->usb_codec;
		p.width = cap.width;
		p.height = cap.height;
		p.fps = cap.fps;
		p.rc_mode = RC_CBR;
		p.bitrate = cfg->usb_bitrate;
		p.gop = cfg->usb_gop;
		p.profile = 2;
		if (!venc_create(VENC_CH, &p))
			venc_up = 1;
		if (!venc_up || venc_device(VENC_CH, &dev)) {
			set_state("error", "the hardware encoder refused this frame size; "
				  "see the log, or set the USB codec to mjpeg");
			wait = 5000;
			goto out;
		}
		pfd[1].fd = venc_fd(VENC_CH);
		pfd[1].events = POLLIN;
		nfds = pfd[1].fd >= 0 ? 2 : 1;
	} else if (!cap.mjpeg && jpeg_open(&jpg, cap.width, cap.height, cfg->usb_jpeg_quality)) {
		set_state("error", "the JPEG encoder could not be set up");
		wait = 5000;
		goto out;
	}

	pthread_mutex_lock(&status_lock);
	U.width = cap.width;
	U.height = cap.height;
	U.fps = cap.fps;
	snprintf(U.encoder, sizeof(U.encoder), "%s %s", codec_name(cfg->usb_codec),
		 hw ? "hardware" : cap.mjpeg ? "from camera" : "cpu");
	pthread_mutex_unlock(&status_lock);
	set_state("streaming", "");
	rtsp_set_stream(STREAM_USB, cfg->usb_codec, cap.width, cap.height, cap.fps);
	LOGI("usb: %s %dx%d@%d, %s", cfg->usb_device, cap.width, cap.height, cap.fps, U.encoder);

	pfd[0].fd = cap.fd;
	pfd[0].events = POLLIN;
	last_frame = now_ms();
	while (!g_quit && !pending) {
		struct v4l2_buffer b;
		int r = poll(pfd, (nfds_t)nfds, 200);

		if (r < 0 && errno != EINTR) {
			set_state("error", "poll failed on the camera");
			break;
		}
		if (venc_up) {
			if (rtsp_take_idr_request(STREAM_USB))
				venc_request_idr(VENC_CH);
			/* With no descriptor to wait on, ask after every wake-up. */
			if (nfds == 1 || (r > 0 && pfd[1].revents)) {
				int got = 0;

				while (venc_forward(VENC_CH, STREAM_USB))
					got = 1;
				/* Readable with nothing to read must not become
				 * a busy loop. */
				if (!got && nfds == 2)
					usleep(2000);
			}
		}
		if (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			set_state("camera unplugged", "");
			wait = 1000;
			break;
		}
		if (r <= 0 || !(pfd[0].revents & POLLIN)) {
			/* The InfiRay thermal core (3474:43d1) answers every
			 * second stream start with silence. Measured on it,
			 * 2026-10-04: a start that follows a session which
			 * delivered frames gets none, however long the camera
			 * was left closed in between (1 to 6 s tried), and the
			 * start after that one works, even 0.4 s later. So a
			 * start that brings nothing is simply made again, once;
			 * the encoder channel stays as it is. */
			if (!frames && !kicked && now_ms() - last_frame > 1500) {
				int w = cap.width, h = cap.height;

				kicked = 1;
				close_capture(&cap);
				if (open_capture(&cap, cfg))
					break;
				if (cap.width != w || cap.height != h) {
					wait = 0;
					break;
				}
				pfd[0].fd = cap.fd;
				pfd[0].revents = 0;
				last_frame = now_ms();
				continue;
			}
			/* A UVC camera that stops delivering has usually been
			 * unplugged; its node lingers a moment after. */
			if (now_ms() - last_frame > 5000) {
				set_state("error", "the camera stopped delivering frames");
				break;
			}
			continue;
		}

		memset(&b, 0, sizeof(b));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b.memory = V4L2_MEMORY_MMAP;
		if (xioctl(cap.fd, VIDIOC_DQBUF, &b) < 0) {
			if (errno == EAGAIN)
				continue;
			set_state("camera unplugged", "");
			wait = 1000;
			break;
		}
		last_frame = now_ms();
		frames++;
		if (b.index < NBUF && cap.maps[b.index] && !(b.flags & V4L2_BUF_FLAG_ERROR)) {
			const uint8_t *frame = cap.maps[b.index];
			size_t raw = (size_t)cap.width * (size_t)cap.height * 2;

			if (cap.mjpeg) {
				rtsp_send_jpeg(STREAM_USB, frame, b.bytesused, now_us());
			} else if (b.bytesused < raw) {
				/* a short frame: the camera is still starting up */
			} else if (hw) {
				/* Counted rather than logged: the encoder having
				 * no buffer free is a dropped frame, not an event.
				 * A hundred in a row is the encoder not working. */
				if (!venc_inject_yuyv(VENC_CH, dev, cap.width, cap.height, frame, now_us())) {
					inject_failed = 0;
				} else if (++inject_failed == 100) {
					set_state("error", "the hardware encoder is not accepting frames; "
						  "set the USB codec to mjpeg");
					wait = 5000;
					xioctl(cap.fd, VIDIOC_QBUF, &b);
					break;
				}
			} else if (rtsp_clients(STREAM_USB)) {
				/* Nobody watching: the JPEG encode is the one real
				 * CPU cost in this program, so it is not spent. */
				unsigned long n = jpeg_encode(&jpg, frame, cap.width, cap.height);

				if (n)
					rtsp_send_jpeg(STREAM_USB, jpg.out, n, now_us());
			}
		}
		if (xioctl(cap.fd, VIDIOC_QBUF, &b) < 0) {
			set_state("camera unplugged", "");
			wait = 1000;
			break;
		}
	}
	if (pending)
		wait = 0;
	rtsp_clear_stream(STREAM_USB);
out:
	if (venc_up)
		venc_destroy(VENC_CH);
	jpeg_close(&jpg);
	close_capture(&cap);
	pthread_mutex_lock(&status_lock);
	U.width = U.height = U.fps = 0;
	U.encoder[0] = '\0';
	pthread_mutex_unlock(&status_lock);
	return wait;
}

void *usb_thread(void *arg)
{
	(void)arg;
	while (!g_quit) {
		struct config cfg;
		int wait;

		pending = 0;
		config_get(&cfg);
		pin(cfg.usb_cpu);
		if (!cfg.usb_enable) {
			set_state("disabled", "");
			wait = 1000000;
		} else {
			wait = session(&cfg);
		}
		while (wait > 0 && !g_quit && !pending) {
			usleep(100000);
			wait -= 100;
		}
	}
	return NULL;
}

void usb_status_json(struct sbuf *b)
{
	unsigned int fps = 0, kbps = 0;

	rtsp_stats(STREAM_USB, &fps, &kbps);
	pthread_mutex_lock(&status_lock);
	sb_printf(b, "\"usb\":{\"state\":");
	sb_json_str(b, U.state);
	sb_printf(b, ",\"error\":");
	sb_json_str(b, U.error);
	sb_printf(b, ",\"encoder\":");
	sb_json_str(b, U.encoder);
	sb_printf(b, ",\"width\":%d,\"height\":%d,\"fps\":%d,\"out_fps\":%u.%u,\"kbps\":%u,\"clients\":%d}",
		  U.width, U.height, U.fps, fps / 10, fps % 10, kbps, rtsp_clients(STREAM_USB));
	pthread_mutex_unlock(&status_lock);
}
