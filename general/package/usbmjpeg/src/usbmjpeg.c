/*
 * usbmjpeg: read raw YUYV frames from a V4L2 capture device, JPEG-encode
 * them with libjpeg (libjpeg-turbo on ARM, which has NEON paths for the
 * colour conversion, the DCT and the Huffman stage), and write each frame
 * into a v4l2loopback output node, where v4l2rtspserver picks it up as an
 * MJPEG camera.
 *
 * Exists because ffmpeg's MJPEG encoder has no NEON forward DCT: on the
 * ssc338q it took ~90% of a Cortex-A7 core for 384x288 at 25fps, and the
 * sensor stream dropped frames with it (2026-09-28). The encode here is the
 * same work with SIMD, and nothing else: no scaling, no filters, no
 * timestamps. The supervisor (/usr/sbin/usbtranscode) restarts it when the
 * camera goes away, so any error is fatal and reported once.
 *
 * The output is what RTP/JPEG (RFC 2435) can carry: baseline, 4:2:0, the
 * standard Huffman tables (optimize_coding stays off), both quantisation
 * tables in every frame. Quality 1-100 is libjpeg's scale.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <jpeglib.h>

#define NBUF 4

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do {
		r = ioctl(fd, req, arg);
	} while (r == -1 && errno == EINTR);
	return r;
}

static double now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

struct encoder {
	struct jpeg_compress_struct cinfo;
	struct jpeg_error_mgr jerr;
	int width, height;
	uint8_t *line;          /* one scanline of packed Y Cb Cr */
	unsigned char *out;     /* JPEG output, sized for the worst case */
	unsigned long outcap;
};

static void encoder_init(struct encoder *e, int width, int height, int quality)
{
	e->width = width;
	e->height = height;
	e->cinfo.err = jpeg_std_error(&e->jerr);
	jpeg_create_compress(&e->cinfo);
	e->cinfo.image_width = width;
	e->cinfo.image_height = height;
	e->cinfo.input_components = 3;
	e->cinfo.in_color_space = JCS_YCbCr;
	jpeg_set_defaults(&e->cinfo);
	jpeg_set_colorspace(&e->cinfo, JCS_YCbCr);
	/* 4:2:0 -- RTP/JPEG type 1. jpeg_set_defaults already picks 2x2 for Y
	 * and 1x1 for chroma with a YCbCr input, spelled out so it cannot drift. */
	e->cinfo.comp_info[0].h_samp_factor = 2;
	e->cinfo.comp_info[0].v_samp_factor = 2;
	e->cinfo.comp_info[1].h_samp_factor = 1;
	e->cinfo.comp_info[1].v_samp_factor = 1;
	e->cinfo.comp_info[2].h_samp_factor = 1;
	e->cinfo.comp_info[2].v_samp_factor = 1;
	jpeg_set_quality(&e->cinfo, quality, TRUE);
	e->cinfo.optimize_coding = FALSE;
	e->cinfo.dct_method = JDCT_ISLOW;

	e->line = malloc((size_t)width * 3);
	/* A baseline JPEG never exceeds the raw 4:2:2 size at quality <= 95,
	 * and the loopback's sizeimage is set to the same figure below. */
	e->outcap = (unsigned long)width * height * 2;
	e->out = malloc(e->outcap);
	if (!e->line || !e->out) {
		fprintf(stderr, "usbmjpeg: out of memory\n");
		exit(1);
	}
}

/* YUYV (Y0 Cb Y1 Cr per pixel pair) to packed Y Cb Cr per pixel, one row at
 * a time, straight into libjpeg's scanline input. Its downsampler then makes
 * the 4:2:0 chroma planes, with NEON in libjpeg-turbo. */
static unsigned long encoder_run(struct encoder *e, const uint8_t *yuyv)
{
	unsigned char *out = e->out;
	unsigned long outsize = e->outcap;
	JSAMPROW row[1] = { e->line };

	jpeg_mem_dest(&e->cinfo, &out, &outsize);
	jpeg_start_compress(&e->cinfo, TRUE);
	while (e->cinfo.next_scanline < (JDIMENSION)e->height) {
		const uint8_t *src = yuyv + (size_t)e->cinfo.next_scanline * e->width * 2;
		uint8_t *dst = e->line;
		int x;

		for (x = 0; x < e->width; x += 2) {
			dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[3];
			dst[3] = src[2]; dst[4] = src[1]; dst[5] = src[3];
			src += 4;
			dst += 6;
		}
		jpeg_write_scanlines(&e->cinfo, row, 1);
	}
	jpeg_finish_compress(&e->cinfo);
	if (out != e->out) {
		/* libjpeg had to grow the buffer: the frame was bigger than the
		 * raw image, which quality <= 95 rules out. Keep the bigger
		 * buffer and carry on; the loopback sizeimage still caps it. */
		free(e->out);
		e->out = out;
		e->outcap = outsize > e->outcap ? outsize : e->outcap;
	}
	return outsize;
}

static int open_capture(const char *dev, int width, int height, int fps,
			struct v4l2_buffer *bufs_out, void **maps, size_t *lens)
{
	struct v4l2_format fmt;
	struct v4l2_streamparm parm;
	struct v4l2_requestbuffers req;
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	int fd, i;

	fd = open(dev, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "usbmjpeg: open %s: %s\n", dev, strerror(errno));
		return -1;
	}

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = type;
	fmt.fmt.pix.width = width;
	fmt.fmt.pix.height = height;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
		fprintf(stderr, "usbmjpeg: %s: set format: %s\n", dev, strerror(errno));
		return -1;
	}
	if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
		fprintf(stderr, "usbmjpeg: %s: camera refused YUYV\n", dev);
		return -1;
	}
	if ((int)fmt.fmt.pix.width != width || (int)fmt.fmt.pix.height != height) {
		fprintf(stderr, "usbmjpeg: %s: camera gave %ux%u instead of %dx%d\n",
			dev, fmt.fmt.pix.width, fmt.fmt.pix.height, width, height);
		return -1;
	}

	/* Frame rate is a request; a camera with one rate ignores it. */
	memset(&parm, 0, sizeof(parm));
	parm.type = type;
	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = fps;
	if (fps > 0)
		xioctl(fd, VIDIOC_S_PARM, &parm);

	memset(&req, 0, sizeof(req));
	req.count = NBUF;
	req.type = type;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
		fprintf(stderr, "usbmjpeg: %s: request buffers: %s\n", dev, strerror(errno));
		return -1;
	}
	for (i = 0; i < (int)req.count; i++) {
		struct v4l2_buffer *b = &bufs_out[i];

		memset(b, 0, sizeof(*b));
		b->type = type;
		b->memory = V4L2_MEMORY_MMAP;
		b->index = i;
		if (xioctl(fd, VIDIOC_QUERYBUF, b) < 0) {
			fprintf(stderr, "usbmjpeg: %s: query buffer %d: %s\n", dev, i, strerror(errno));
			return -1;
		}
		lens[i] = b->length;
		maps[i] = mmap(NULL, b->length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b->m.offset);
		if (maps[i] == MAP_FAILED) {
			fprintf(stderr, "usbmjpeg: %s: mmap buffer %d: %s\n", dev, i, strerror(errno));
			return -1;
		}
		if (xioctl(fd, VIDIOC_QBUF, b) < 0) {
			fprintf(stderr, "usbmjpeg: %s: queue buffer %d: %s\n", dev, i, strerror(errno));
			return -1;
		}
	}
	for (; i < NBUF; i++)
		maps[i] = NULL;

	if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
		fprintf(stderr, "usbmjpeg: %s: stream on: %s\n", dev, strerror(errno));
		return -1;
	}
	return fd;
}

static int open_sink(const char *dev, int width, int height, unsigned long sizeimage)
{
	struct v4l2_format fmt;
	int fd;

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "usbmjpeg: open %s: %s\n", dev, strerror(errno));
		return -1;
	}
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width = width;
	fmt.fmt.pix.height = height;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	fmt.fmt.pix.sizeimage = sizeimage;
	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
		fprintf(stderr, "usbmjpeg: %s: set output format: %s\n", dev, strerror(errno));
		return -1;
	}
	return fd;
}

/* -t: encode one raw YUYV frame from a file into a JPEG file. The way the
 * encoder is checked on a build host, where there is no camera. */
static int test_mode(const char *in, const char *out, int width, int height, int quality)
{
	struct encoder e;
	size_t need = (size_t)width * height * 2;
	uint8_t *frame = malloc(need);
	FILE *f;
	unsigned long n;

	if (!frame)
		return 1;
	f = fopen(in, "rb");
	if (!f || fread(frame, 1, need, f) != need) {
		fprintf(stderr, "usbmjpeg: %s: need %zu bytes of YUYV\n", in, need);
		return 1;
	}
	fclose(f);
	encoder_init(&e, width, height, quality);
	n = encoder_run(&e, frame);
	f = fopen(out, "wb");
	if (!f || fwrite(e.out, 1, n, f) != n) {
		fprintf(stderr, "usbmjpeg: %s: write failed\n", out);
		return 1;
	}
	fclose(f);
	printf("%lu bytes\n", n);
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: usbmjpeg [-i capture] [-o loopback] [-W width] [-H height]\n"
		"                [-F fps] [-q quality] [-c cpu] [-v]\n"
		"       usbmjpeg -t in.yuyv out.jpg [-W width] [-H height] [-q quality]\n"
		"defaults: -i /dev/video0 -o /dev/video10 -W 384 -H 288 -F 25 -q 80\n"
		"-c pins the process to one CPU (the image has no taskset)\n");
}

int main(int argc, char **argv)
{
	const char *capture = "/dev/video0", *sink = "/dev/video10";
	const char *test_in = NULL, *test_out = NULL;
	int width = 384, height = 288, fps = 25, quality = 80, verbose = 0, cpu = -1;
	struct v4l2_buffer bufs[NBUF];
	void *maps[NBUF];
	size_t lens[NBUF];
	struct encoder e;
	struct pollfd pfd;
	int cfd, sfd, opt;
	unsigned frames = 0;
	unsigned long bytes = 0;
	double t_stat, enc_ms = 0;

	while ((opt = getopt(argc, argv, "i:o:W:H:F:q:c:t:vh")) != -1) {
		switch (opt) {
		case 'i': capture = optarg; break;
		case 'c': cpu = atoi(optarg); break;
		case 'o': sink = optarg; break;
		case 'W': width = atoi(optarg); break;
		case 'H': height = atoi(optarg); break;
		case 'F': fps = atoi(optarg); break;
		case 'q': quality = atoi(optarg); break;
		case 't': test_in = optarg; break;
		case 'v': verbose = 1; break;
		default: usage(); return 2;
		}
	}
	if (width <= 0 || height <= 0 || (width & 1)) {
		fprintf(stderr, "usbmjpeg: width must be even, both positive\n");
		return 2;
	}
	if (quality < 1)
		quality = 1;
	if (quality > 95)
		quality = 95;   /* see encoder_init: keeps frames within the raw size */
	if (test_in) {
		if (optind >= argc) {
			usage();
			return 2;
		}
		test_out = argv[optind];
		return test_mode(test_in, test_out, width, height, quality);
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	/* Pinning done here because busybox on the image has no taskset. The
	 * sensor pipeline's threads sit on core 0; an encoder sharing it cost
	 * the main stream frames, and core 1 is otherwise idle. */
	if (cpu >= 0) {
		cpu_set_t set;

		CPU_ZERO(&set);
		CPU_SET(cpu, &set);
		if (sched_setaffinity(0, sizeof(set), &set) < 0)
			fprintf(stderr, "usbmjpeg: cannot pin to cpu %d: %s\n", cpu, strerror(errno));
	}

	encoder_init(&e, width, height, quality);
	cfd = open_capture(capture, width, height, fps, bufs, maps, lens);
	if (cfd < 0)
		return 1;
	sfd = open_sink(sink, width, height, e.outcap);
	if (sfd < 0)
		return 1;
	if (verbose)
		fprintf(stderr, "usbmjpeg: %s YUYV %dx%d -> %s MJPEG q%d\n",
			capture, width, height, sink, quality);

	pfd.fd = cfd;
	pfd.events = POLLIN;
	t_stat = now_ms();
	while (!stop) {
		struct v4l2_buffer b;
		unsigned long n;
		double t0;
		int r;

		r = poll(&pfd, 1, 5000);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "usbmjpeg: poll: %s\n", strerror(errno));
			break;
		}
		if (r == 0) {
			/* A UVC camera that stops delivering has usually been
			 * unplugged; the node lingers a moment after. Let the
			 * supervisor start over. */
			fprintf(stderr, "usbmjpeg: no frame for 5s, giving up\n");
			break;
		}
		memset(&b, 0, sizeof(b));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b.memory = V4L2_MEMORY_MMAP;
		if (xioctl(cfd, VIDIOC_DQBUF, &b) < 0) {
			if (errno == EAGAIN)
				continue;
			fprintf(stderr, "usbmjpeg: dequeue: %s\n", strerror(errno));
			break;
		}
		if (b.bytesused >= (unsigned)(width * height * 2)
		    && !(b.flags & V4L2_BUF_FLAG_ERROR)) {
			t0 = now_ms();
			n = encoder_run(&e, maps[b.index]);
			enc_ms += now_ms() - t0;
			if (write(sfd, e.out, n) != (ssize_t)n) {
				fprintf(stderr, "usbmjpeg: write %s: %s\n", sink, strerror(errno));
				break;
			}
			frames++;
			bytes += n;
		}
		if (xioctl(cfd, VIDIOC_QBUF, &b) < 0) {
			fprintf(stderr, "usbmjpeg: requeue: %s\n", strerror(errno));
			break;
		}
		if (verbose && now_ms() - t_stat >= 5000) {
			double dt = (now_ms() - t_stat) / 1000.0;

			fprintf(stderr, "usbmjpeg: %.1f fps, %.0f kbit/s, %.2f ms/frame encode\n",
				frames / dt, bytes * 8 / dt / 1000.0,
				frames ? enc_ms / frames : 0);
			frames = 0;
			bytes = 0;
			enc_ms = 0;
			t_stat = now_ms();
		}
	}

	{
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		int i;

		xioctl(cfd, VIDIOC_STREAMOFF, &type);
		for (i = 0; i < NBUF; i++)
			if (maps[i])
				munmap(maps[i], lens[i]);
	}
	close(cfd);
	close(sfd);
	jpeg_destroy_compress(&e.cinfo);
	return stop ? 0 : 1;
}
