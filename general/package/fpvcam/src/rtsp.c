/*
 * RTSP server and RTP packetisers for H.264 (RFC 6184), H.265 (RFC 7798)
 * and JPEG (RFC 2435). Two streams, a handful of clients, RTP over UDP or
 * interleaved in the RTSP connection.
 *
 * The rtsp thread only talks RTSP. Frames are packetised and sent by the
 * thread that produced them, straight from the encoder's buffer, under one
 * lock that also covers the client table: a 1080p60 stream is a few hundred
 * small sends a second, and a queue between two threads would add a copy
 * and a wake-up to every one of them for nothing.
 *
 * A producer must never be held up by a viewer. UDP cannot block. A TCP
 * client whose socket buffer is filling is skipped a whole frame at a time
 * and resumed at the next keyframe, and one that still stalls a write is
 * dropped.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/sockios.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include "fpvcam.h"

#define MAX_CLIENTS	8
#define TCP_SNDBUF	(512 * 1024)

struct client {
	int fd;			/* RTSP connection; -1 marks a free slot */
	int stream;		/* -1 until a request names one */
	int playing, tcp, wait_key, skip;
	int rtp_fd, rtcp_fd;
	int ch_rtp, ch_rtcp;
	int sndbuf;
	unsigned int session;
	struct sockaddr_in peer;
	char in[4096];
	size_t in_len;
};

struct stream {
	int active, codec, width, height, fps;
	int idr_request, in_frame;
	uint16_t seq;
	uint32_t ssrc, ts, packets, octets;
	uint64_t next_sr;
	uint8_t vps[96], sps[160], pps[96];
	size_t vps_len, sps_len, pps_len;
	/* measured, for the status box */
	unsigned int frames, bytes, fps_x10, kbps;
	uint64_t stat_t0;
};

static struct client clients[MAX_CLIENTS];
static struct stream streams[STREAM_COUNT];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int listen_fd = -1;
static volatile int restart;

static void drop_client(struct client *c)
{
	if (c->fd < 0)
		return;
	if (c->rtp_fd >= 0)
		close(c->rtp_fd);
	if (c->rtcp_fd >= 0)
		close(c->rtcp_fd);
	close(c->fd);
	LOGI("rtsp: %s disconnected", inet_ntoa(c->peer.sin_addr));
	c->fd = c->rtp_fd = c->rtcp_fd = -1;
	c->playing = 0;
}

static void drop_stream_clients(int id)
{
	int i;

	for (i = 0; i < MAX_CLIENTS; i++)
		if (clients[i].fd >= 0 && clients[i].stream == id)
			drop_client(&clients[i]);
}

void rtsp_set_stream(int id, int codec, int width, int height, int fps)
{
	struct stream *s = &streams[id];

	pthread_mutex_lock(&lock);
	/* A client holds an SDP that described the old stream; it cannot
	 * follow a codec or size change, so it is sent away to reconnect. */
	if (s->active && (s->codec != codec || s->width != width || s->height != height))
		drop_stream_clients(id);
	if (!s->ssrc)
		s->ssrc = (uint32_t)now_ms() * 2654435761u + (uint32_t)id + 1;
	s->codec = codec;
	s->width = width;
	s->height = height;
	s->fps = fps;
	s->vps_len = s->sps_len = s->pps_len = 0;
	s->active = 1;
	s->in_frame = 0;
	s->idr_request = 1;
	pthread_mutex_unlock(&lock);
}

void rtsp_clear_stream(int id)
{
	pthread_mutex_lock(&lock);
	streams[id].active = 0;
	drop_stream_clients(id);
	pthread_mutex_unlock(&lock);
}

int rtsp_clients(int id)
{
	int i, n = 0;

	pthread_mutex_lock(&lock);
	for (i = 0; i < MAX_CLIENTS; i++)
		n += clients[i].fd >= 0 && clients[i].stream == id && clients[i].playing;
	pthread_mutex_unlock(&lock);
	return n;
}

int rtsp_take_idr_request(int id)
{
	int r;

	pthread_mutex_lock(&lock);
	r = streams[id].idr_request;
	streams[id].idr_request = 0;
	pthread_mutex_unlock(&lock);
	return r;
}

void rtsp_stats(int id, unsigned int *fps_x10, unsigned int *kbps)
{
	pthread_mutex_lock(&lock);
	/* A stream that stopped must read as stopped, not as its last second. */
	if (now_ms() - streams[id].stat_t0 > 3000)
		streams[id].fps_x10 = streams[id].kbps = 0;
	*fps_x10 = streams[id].fps_x10;
	*kbps = streams[id].kbps;
	pthread_mutex_unlock(&lock);
}

void rtsp_request_restart(void)
{
	restart = 1;
}

/* ---- sending; all of it runs with the lock held ---- */

static void client_send(struct client *c, int rtcp, struct iovec *iov, int iovcnt, size_t total)
{
	struct msghdr msg;
	uint8_t frame[4];
	ssize_t n;

	memset(&msg, 0, sizeof(msg));
	if (c->tcp) {
		/* iov[0] is left free by the callers for this header. */
		frame[0] = '$';
		frame[1] = (uint8_t)(rtcp ? c->ch_rtcp : c->ch_rtp);
		frame[2] = (uint8_t)(total >> 8);
		frame[3] = (uint8_t)total;
		iov[0].iov_base = frame;
		iov[0].iov_len = 4;
		msg.msg_iov = iov;
		msg.msg_iovlen = (size_t)iovcnt;
		n = sendmsg(c->fd, &msg, MSG_NOSIGNAL);
		/* A short write would leave half a packet in the byte stream
		 * and every later one misframed. */
		if (n != (ssize_t)(total + 4)) {
			LOGW("rtsp: %s is not keeping up, dropping it", inet_ntoa(c->peer.sin_addr));
			drop_client(c);
		}
	} else {
		msg.msg_iov = iov + 1;
		msg.msg_iovlen = (size_t)iovcnt - 1;
		/* Loss is the network's to report, not ours: an unreachable
		 * viewer is noticed through its RTSP connection. */
		sendmsg(rtcp ? c->rtcp_fd : c->rtp_fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
	}
}

static void send_rtp(struct stream *s, int id, int pt, int marker,
		     const uint8_t *hdr, size_t hdr_len, const uint8_t *payload, size_t len)
{
	uint8_t rtp[12];
	struct iovec iov[4];
	int i;

	rtp[0] = 0x80;
	rtp[1] = (uint8_t)(pt | (marker ? 0x80 : 0));
	rtp[2] = (uint8_t)(s->seq >> 8);
	rtp[3] = (uint8_t)s->seq;
	rtp[4] = (uint8_t)(s->ts >> 24);
	rtp[5] = (uint8_t)(s->ts >> 16);
	rtp[6] = (uint8_t)(s->ts >> 8);
	rtp[7] = (uint8_t)s->ts;
	rtp[8] = (uint8_t)(s->ssrc >> 24);
	rtp[9] = (uint8_t)(s->ssrc >> 16);
	rtp[10] = (uint8_t)(s->ssrc >> 8);
	rtp[11] = (uint8_t)s->ssrc;
	s->seq++;
	s->packets++;
	s->octets += (uint32_t)(hdr_len + len);

	for (i = 0; i < MAX_CLIENTS; i++) {
		struct client *c = &clients[i];

		if (c->fd < 0 || c->stream != id || !c->playing || c->wait_key || c->skip)
			continue;
		iov[1].iov_base = rtp;
		iov[1].iov_len = sizeof(rtp);
		iov[2].iov_base = (void *)hdr;
		iov[2].iov_len = hdr_len;
		iov[3].iov_base = (void *)payload;
		iov[3].iov_len = len;
		client_send(c, 0, iov, 4, sizeof(rtp) + hdr_len + len);
	}
}

static void send_sr(struct stream *s, int id)
{
	uint8_t sr[28];
	struct timeval tv;
	struct iovec iov[2];
	uint32_t sec, frac;
	int i;

	gettimeofday(&tv, NULL);
	sec = (uint32_t)tv.tv_sec + 2208988800u;
	frac = (uint32_t)(((uint64_t)tv.tv_usec << 32) / 1000000);
	sr[0] = 0x80;
	sr[1] = 200;
	sr[2] = 0;
	sr[3] = 6;
#define PUT32(o, v) do { sr[o] = (uint8_t)((v) >> 24); sr[(o) + 1] = (uint8_t)((v) >> 16); \
			 sr[(o) + 2] = (uint8_t)((v) >> 8); sr[(o) + 3] = (uint8_t)(v); } while (0)
	PUT32(4, s->ssrc);
	PUT32(8, sec);
	PUT32(12, frac);
	PUT32(16, s->ts);
	PUT32(20, s->packets);
	PUT32(24, s->octets);
#undef PUT32
	for (i = 0; i < MAX_CLIENTS; i++) {
		struct client *c = &clients[i];

		if (c->fd < 0 || c->stream != id || !c->playing)
			continue;
		iov[1].iov_base = sr;
		iov[1].iov_len = sizeof(sr);
		client_send(c, 1, iov, 2, sizeof(sr));
	}
}

/*
 * Runs once per frame, before its first packet. Decides which TCP clients
 * sit this frame out, sends the periodic sender report, and keeps the
 * numbers the status box shows.
 */
static void frame_begin(struct stream *s, int id, uint64_t pts_us, size_t len)
{
	uint64_t now = now_ms();
	int i;

	if (!pts_us)
		pts_us = now * 1000;
	s->ts = (uint32_t)(pts_us * 9 / 100);
	s->in_frame = 1;

	for (i = 0; i < MAX_CLIENTS; i++) {
		struct client *c = &clients[i];
		int queued = 0;

		if (c->fd < 0 || c->stream != id || !c->playing || !c->tcp)
			continue;
		c->skip = 0;
		if (!ioctl(c->fd, SIOCOUTQ, &queued) && queued > c->sndbuf / 2) {
			/* A decoder cannot use the frames after a gap, so
			 * nothing more goes out until a keyframe does. */
			c->skip = 1;
			c->wait_key = s->codec != CODEC_MJPEG;
			s->idr_request = 1;
		}
	}
	if (now >= s->next_sr) {
		s->next_sr = now + 5000;
		send_sr(s, id);
	}

	s->frames++;
	s->bytes += (unsigned int)len;
	if (!s->stat_t0)
		s->stat_t0 = now;
	if (now - s->stat_t0 >= 1000) {
		unsigned int ms = (unsigned int)(now - s->stat_t0);

		s->fps_x10 = s->frames * 10000 / ms;
		s->kbps = (unsigned int)((uint64_t)s->bytes * 8 / ms);
		s->frames = s->bytes = 0;
		s->stat_t0 = now;
	}
}

static void keyframe_seen(int id)
{
	int i;

	for (i = 0; i < MAX_CLIENTS; i++)
		if (clients[i].fd >= 0 && clients[i].stream == id && !clients[i].skip)
			clients[i].wait_key = 0;
}

static void send_nal(struct stream *s, int id, const uint8_t *nal, size_t len, int marker,
		     size_t max)
{
	uint8_t fu[3];
	size_t hdr, skip;

	if (len <= max) {
		send_rtp(s, id, 96, marker, NULL, 0, nal, len);
		return;
	}
	if (s->codec == CODEC_H265) {
		fu[0] = (uint8_t)((nal[0] & 0x81) | (49 << 1));
		fu[1] = nal[1];
		fu[2] = (uint8_t)(0x80 | ((nal[0] >> 1) & 0x3f));
		hdr = 3;
		skip = 2;
	} else {
		fu[0] = (uint8_t)((nal[0] & 0xe0) | 28);
		fu[1] = (uint8_t)(0x80 | (nal[0] & 0x1f));
		hdr = 2;
		skip = 1;
	}
	nal += skip;
	len -= skip;
	while (len) {
		size_t chunk = len > max - hdr ? max - hdr : len;

		if (chunk == len)
			fu[hdr - 1] |= 0x40;
		send_rtp(s, id, 96, marker && chunk == len, fu, hdr, nal, chunk);
		fu[hdr - 1] &= 0x3f;
		nal += chunk;
		len -= chunk;
	}
}

static void keep(uint8_t *dst, size_t *dst_len, size_t cap, const uint8_t *nal, size_t len)
{
	if (len <= cap) {
		memcpy(dst, nal, len);
		*dst_len = len;
	}
}

/* Returns the offset of the next 00 00 01 at or after from, or len. */
static size_t next_start(const uint8_t *d, size_t len, size_t from)
{
	size_t i;

	for (i = from; i + 3 <= len; i++)
		if (!d[i] && !d[i + 1] && d[i + 2] == 1)
			return i;
	return len;
}

void rtsp_send_h26x(int id, const uint8_t *data, size_t len, uint64_t pts_us, int last)
{
	struct stream *s = &streams[id];
	struct config c;
	size_t pos, max;

	config_get(&c);
	max = (size_t)c.rtp_payload;

	pthread_mutex_lock(&lock);
	if (!s->active) {
		pthread_mutex_unlock(&lock);
		return;
	}
	if (!s->in_frame)
		frame_begin(s, id, pts_us, 0);
	s->bytes += (unsigned int)len;

	pos = next_start(data, len, 0);
	while (pos < len) {
		size_t nal = pos + 3, end = next_start(data, len, nal), nlen;
		int type, key;

		/* A four-byte start code leaves its leading zero on the tail
		 * of the unit before it. */
		nlen = end - nal;
		while (nlen && end < len && !data[nal + nlen - 1])
			nlen--;
		if (nlen) {
			if (s->codec == CODEC_H265) {
				type = (data[nal] >> 1) & 0x3f;
				key = type >= 16 && type <= 21;
				if (type == 32)
					keep(s->vps, &s->vps_len, sizeof(s->vps), data + nal, nlen);
				else if (type == 33)
					keep(s->sps, &s->sps_len, sizeof(s->sps), data + nal, nlen);
				else if (type == 34)
					keep(s->pps, &s->pps_len, sizeof(s->pps), data + nal, nlen);
				key |= type == 32;
			} else {
				type = data[nal] & 0x1f;
				key = type == 5;
				if (type == 7)
					keep(s->sps, &s->sps_len, sizeof(s->sps), data + nal, nlen);
				else if (type == 8)
					keep(s->pps, &s->pps_len, sizeof(s->pps), data + nal, nlen);
				key |= type == 7;
			}
			/* The encoder puts the parameter sets in front of every
			 * keyframe, so a waiting client is let in at the first
			 * of them and gets the whole set. */
			if (key)
				keyframe_seen(id);
			send_nal(s, id, data + nal, nlen, last && end >= len, max);
		}
		pos = end;
	}
	if (last)
		s->in_frame = 0;
	pthread_mutex_unlock(&lock);
}

/*
 * RFC 2435 carries the scan data only; the receiver rebuilds the JPEG
 * headers from the eight-byte header and, with Q=255, from the quantisation
 * tables sent in the first packet of each frame. That limits what can be
 * sent to what usbmjpeg already produced for this path: baseline, 4:2:0 or
 * 4:2:2, the standard Huffman tables, eight-bit tables 0 and 1.
 */
void rtsp_send_jpeg(int id, const uint8_t *d, size_t len, uint64_t pts_us)
{
	struct stream *s = &streams[id];
	struct config c;
	uint8_t tables[4][64], hdr[8 + 4 + 4 + 128];
	size_t i = 2, scan = 0, off = 0, max, hlen;
	unsigned int width = 0, height = 0, dri = 0, tq_y = 0, tq_c = 1;
	int type = -1, have_q = 0;

	if (len < 4 || d[0] != 0xff || d[1] != 0xd8)
		return;
	while (i + 4 <= len && d[i] == 0xff) {
		unsigned int marker = d[i + 1], seg = ((unsigned int)d[i + 2] << 8) | d[i + 3];

		if (i + 2 + seg > len)
			return;
		if (marker == 0xdb) {
			size_t p = i + 4, e = i + 2 + seg;

			while (p + 65 <= e) {
				if ((d[p] >> 4) == 0 && (d[p] & 15) < 4) {
					memcpy(tables[d[p] & 15], d + p + 1, 64);
					have_q |= 1 << (d[p] & 15);
				}
				p += 65;
			}
		} else if (marker == 0xc0 && seg >= 17 && d[i + 9] == 3) {
			height = ((unsigned int)d[i + 5] << 8) | d[i + 6];
			width = ((unsigned int)d[i + 7] << 8) | d[i + 8];
			type = d[i + 11] == 0x22 ? 1 : d[i + 11] == 0x21 ? 0 : -1;
			/* The receiver always gives luma table 0 and chroma table
			 * 1. An encoder is free to number them otherwise, or to
			 * use one table for both, so they are sent by role. */
			tq_y = d[i + 12] & 3;
			tq_c = d[i + 15] & 3;
		} else if (marker == 0xdd && seg >= 4) {
			dri = ((unsigned int)d[i + 4] << 8) | d[i + 5];
		} else if (marker == 0xda) {
			scan = i + 2 + seg;
			break;
		}
		i += 2 + seg;
	}
	if (!scan || type < 0 || !(have_q & (1 << tq_y)) || !(have_q & (1 << tq_c)) ||
	    !width || width > 2040 || height > 2040)
		return;
	if (len >= scan + 2 && d[len - 2] == 0xff && d[len - 1] == 0xd9)
		len -= 2;
	d += scan;
	len -= scan;

	config_get(&c);
	max = (size_t)c.rtp_payload;

	pthread_mutex_lock(&lock);
	if (!s->active) {
		pthread_mutex_unlock(&lock);
		return;
	}
	frame_begin(s, id, pts_us, len);
	while (len) {
		size_t chunk;

		hdr[0] = 0;
		hdr[1] = (uint8_t)(off >> 16);
		hdr[2] = (uint8_t)(off >> 8);
		hdr[3] = (uint8_t)off;
		hdr[4] = (uint8_t)(type | (dri ? 0x40 : 0));
		hdr[5] = 255;
		hdr[6] = (uint8_t)((width + 7) / 8);
		hdr[7] = (uint8_t)((height + 7) / 8);
		hlen = 8;
		if (dri) {
			hdr[hlen++] = (uint8_t)(dri >> 8);
			hdr[hlen++] = (uint8_t)dri;
			hdr[hlen++] = 0xff;
			hdr[hlen++] = 0xff;
		}
		if (!off) {
			hdr[hlen++] = 0;
			hdr[hlen++] = 0;
			hdr[hlen++] = 0;
			hdr[hlen++] = 128;
			memcpy(hdr + hlen, tables[tq_y], 64);
			memcpy(hdr + hlen + 64, tables[tq_c], 64);
			hlen += 128;
		}
		chunk = len > max - hlen ? max - hlen : len;
		send_rtp(s, id, 26, chunk == len, hdr, hlen, d, chunk);
		d += chunk;
		off += chunk;
		len -= chunk;
	}
	s->in_frame = 0;
	pthread_mutex_unlock(&lock);
}

/* ---- RTSP ---- */

static size_t base64(char *out, const uint8_t *in, size_t len)
{
	static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t i, o = 0;

	for (i = 0; i < len; i += 3) {
		unsigned int v = (unsigned int)in[i] << 16;

		if (i + 1 < len)
			v |= (unsigned int)in[i + 1] << 8;
		if (i + 2 < len)
			v |= in[i + 2];
		out[o++] = tab[v >> 18];
		out[o++] = tab[(v >> 12) & 63];
		out[o++] = i + 1 < len ? tab[(v >> 6) & 63] : '=';
		out[o++] = i + 2 < len ? tab[v & 63] : '=';
	}
	out[o] = '\0';
	return o;
}

static size_t make_sdp(const struct stream *s, char *out, size_t cap)
{
	struct sbuf b = { out, 0, cap };
	char a[256], p[256], v[160];

	sb_printf(&b, "v=0\r\no=- %u 1 IN IP4 0.0.0.0\r\ns=fpvcam\r\nc=IN IP4 0.0.0.0\r\n"
		  "t=0 0\r\na=control:*\r\na=range:npt=0-\r\n", (unsigned int)s->ssrc);
	if (s->codec == CODEC_MJPEG) {
		sb_printf(&b, "m=video 0 RTP/AVP 26\r\n");
	} else if (s->codec == CODEC_H265) {
		sb_printf(&b, "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H265/90000\r\n");
		if (s->vps_len && s->sps_len && s->pps_len) {
			base64(v, s->vps, s->vps_len);
			base64(a, s->sps, s->sps_len);
			base64(p, s->pps, s->pps_len);
			sb_printf(&b, "a=fmtp:96 sprop-vps=%s;sprop-sps=%s;sprop-pps=%s\r\n", v, a, p);
		}
	} else {
		sb_printf(&b, "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
			  "a=fmtp:96 packetization-mode=1");
		if (s->sps_len >= 4 && s->pps_len) {
			base64(a, s->sps, s->sps_len);
			base64(p, s->pps, s->pps_len);
			sb_printf(&b, ";profile-level-id=%02x%02x%02x;sprop-parameter-sets=%s,%s",
				  s->sps[1], s->sps[2], s->sps[3], a, p);
		}
		sb_printf(&b, "\r\n");
	}
	sb_printf(&b, "a=framerate:%d\r\na=control:track0\r\n", s->fps);
	return b.len;
}

static const char *header(const char *req, const char *name)
{
	size_t n = strlen(name);
	const char *p = req;

	while ((p = strstr(p, "\r\n"))) {
		p += 2;
		if (!strncasecmp(p, name, n) && p[n] == ':') {
			p += n + 1;
			while (*p == ' ')
				p++;
			return p;
		}
	}
	return NULL;
}

static void reply(struct client *c, const char *req, const char *status, const char *extra,
		  const char *body, size_t body_len)
{
	const char *cseq = header(req, "CSeq");
	char out[2048];
	struct sbuf b = { out, 0, sizeof(out) };

	sb_printf(&b, "RTSP/1.0 %s\r\nCSeq: %d\r\nServer: fpvcam\r\n", status, cseq ? atoi(cseq) : 0);
	if (c->session)
		sb_printf(&b, "Session: %08x;timeout=60\r\n", c->session);
	if (extra)
		sb_printf(&b, "%s", extra);
	if (body)
		sb_printf(&b, "Content-Type: application/sdp\r\nContent-Length: %u\r\n",
			  (unsigned int)body_len);
	sb_printf(&b, "\r\n");
	if (body)
		sb_printf(&b, "%.*s", (int)body_len, body);
	if (send(c->fd, out, b.len, MSG_NOSIGNAL) != (ssize_t)b.len)
		drop_client(c);
}

/* Two UDP sockets on consecutive ports, the even one for RTP. */
static int open_udp_pair(struct client *c, int rtp_port, int rtcp_port, int *server_port)
{
	struct sockaddr_in a, to;
	int port;

	for (port = 50000; port < 50400; port += 2) {
		int r = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
		int t = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

		memset(&a, 0, sizeof(a));
		a.sin_family = AF_INET;
		a.sin_port = htons((uint16_t)port);
		if (r >= 0 && t >= 0 && !bind(r, (struct sockaddr *)&a, sizeof(a))) {
			a.sin_port = htons((uint16_t)(port + 1));
			if (!bind(t, (struct sockaddr *)&a, sizeof(a))) {
				to = c->peer;
				to.sin_port = htons((uint16_t)rtp_port);
				connect(r, (struct sockaddr *)&to, sizeof(to));
				to.sin_port = htons((uint16_t)rtcp_port);
				connect(t, (struct sockaddr *)&to, sizeof(to));
				c->rtp_fd = r;
				c->rtcp_fd = t;
				*server_port = port;
				return 0;
			}
		}
		if (r >= 0)
			close(r);
		if (t >= 0)
			close(t);
	}
	return -1;
}

/* "/usb", "/usb/track0", "/usb?x" name the USB camera; anything else, the
 * bare address Divinus served included, is the sensor. */
static int url_stream(const char *req)
{
	const char *u = strchr(req, ' '), *p;

	if (!u)
		return STREAM_MAIN;
	u++;
	p = strstr(u, "://");
	p = p && p < u + 8 ? strchr(p + 3, '/') : u;
	if (p && !strncmp(p, "/usb", 4) && (p[4] == ' ' || p[4] == '/' || p[4] == '?'))
		return STREAM_USB;
	return STREAM_MAIN;
}

static void handle_request(struct client *c, char *req)
{
	char url[256], extra[512];
	struct stream *s;
	const char *t;
	int id;

	if (sscanf(req, "%*15s %255s", url) != 1) {
		reply(c, req, "400 Bad Request", NULL, NULL, 0);
		return;
	}
	if (!strncmp(req, "OPTIONS ", 8)) {
		reply(c, req, "200 OK", "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, "
		      "GET_PARAMETER, SET_PARAMETER\r\n", NULL, 0);
		return;
	}
	if (!strncmp(req, "GET_PARAMETER ", 14) || !strncmp(req, "SET_PARAMETER ", 14)) {
		reply(c, req, "200 OK", NULL, NULL, 0);
		return;
	}

	id = url_stream(req);
	s = &streams[id];
	if (!strncmp(req, "DESCRIBE ", 9)) {
		char sdp[1024];
		size_t n;
		int tries;

		if (!s->active) {
			reply(c, req, "404 Not Found", NULL, NULL, 0);
			return;
		}
		/* A description without the parameter sets works with most
		 * players and not with all. They arrive with a keyframe, so
		 * ask for one and give it a moment. */
		for (tries = 0; tries < 25 && s->codec != CODEC_MJPEG && !s->sps_len; tries++) {
			s->idr_request = 1;
			pthread_mutex_unlock(&lock);
			usleep(20000);
			pthread_mutex_lock(&lock);
			if (c->fd < 0)
				return;
		}
		c->stream = id;
		n = make_sdp(s, sdp, sizeof(sdp));
		/* Strip a trailing slash so that "base + track0" has one. */
		url[strcspn(url, "?")] = '\0';
		if (*url && url[strlen(url) - 1] == '/')
			url[strlen(url) - 1] = '\0';
		snprintf(extra, sizeof(extra), "Content-Base: %s/\r\n", url);
		reply(c, req, "200 OK", extra, sdp, n);
	} else if (!strncmp(req, "SETUP ", 6)) {
		int a = 0, b = 1, server = 0;

		t = header(req, "Transport");
		if (!t || !s->active) {
			reply(c, req, s->active ? "400 Bad Request" : "404 Not Found", NULL, NULL, 0);
			return;
		}
		if (c->rtp_fd >= 0) {
			close(c->rtp_fd);
			close(c->rtcp_fd);
			c->rtp_fd = c->rtcp_fd = -1;
		}
		c->stream = id;
		if (!c->session)
			c->session = (unsigned int)now_ms() * 2246822519u + (unsigned int)c->fd + 1;
		if (!strncasecmp(t, "RTP/AVP/TCP", 11)) {
			const char *p = strstr(t, "interleaved=");

			if (p)
				sscanf(p, "interleaved=%d-%d", &a, &b);
			c->tcp = 1;
			c->ch_rtp = a;
			c->ch_rtcp = b;
			snprintf(extra, sizeof(extra),
				 "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d;ssrc=%08x\r\n",
				 a, b, (unsigned int)s->ssrc);
		} else {
			const char *p = strstr(t, "client_port=");

			if (!p || sscanf(p, "client_port=%d-%d", &a, &b) < 1 || a <= 0 || a > 65535) {
				reply(c, req, "461 Unsupported Transport", NULL, NULL, 0);
				return;
			}
			if (b <= 0 || b > 65535)
				b = a + 1;
			if (open_udp_pair(c, a, b, &server)) {
				reply(c, req, "500 Internal Server Error", NULL, NULL, 0);
				return;
			}
			c->tcp = 0;
			snprintf(extra, sizeof(extra),
				 "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d;ssrc=%08x\r\n",
				 a, b, server, server + 1, (unsigned int)s->ssrc);
		}
		reply(c, req, "200 OK", extra, NULL, 0);
	} else if (!strncmp(req, "PLAY ", 5)) {
		if (c->stream < 0 || (!c->tcp && c->rtp_fd < 0)) {
			reply(c, req, "455 Method Not Valid in This State", NULL, NULL, 0);
			return;
		}
		s = &streams[c->stream];
		url[strcspn(url, "?")] = '\0';
		if (*url && url[strlen(url) - 1] == '/')
			url[strlen(url) - 1] = '\0';
		snprintf(extra, sizeof(extra), "Range: npt=0.000-\r\nRTP-Info: url=%s/track0;seq=%u;rtptime=%u\r\n",
			 url, (unsigned int)s->seq, (unsigned int)s->ts);
		reply(c, req, "200 OK", extra, NULL, 0);
		if (c->fd < 0)
			return;
		c->wait_key = s->codec != CODEC_MJPEG;
		c->skip = 0;
		c->playing = 1;
		s->idr_request = 1;
		s->next_sr = 0;
		LOGI("rtsp: %s plays %s over %s", inet_ntoa(c->peer.sin_addr),
		     c->stream == STREAM_USB ? "/usb" : "/main", c->tcp ? "TCP" : "UDP");
	} else if (!strncmp(req, "PAUSE ", 6)) {
		c->playing = 0;
		reply(c, req, "200 OK", NULL, NULL, 0);
	} else if (!strncmp(req, "TEARDOWN ", 9)) {
		reply(c, req, "200 OK", NULL, NULL, 0);
		drop_client(c);
	} else {
		reply(c, req, "501 Not Implemented", NULL, NULL, 0);
	}
}

/* Consumes whatever complete requests and interleaved packets are buffered. */
static void client_input(struct client *c)
{
	while (c->fd >= 0 && c->in_len) {
		char *end;
		const char *cl;
		size_t used, body = 0;

		if (c->in[0] == '$') {
			/* RTCP from the client, interleaved. Nothing in it is
			 * needed; it only has to be stepped over. */
			if (c->in_len < 4)
				return;
			used = 4 + (((size_t)(uint8_t)c->in[2] << 8) | (uint8_t)c->in[3]);
			if (used > sizeof(c->in) - 1) {
				drop_client(c);
				return;
			}
			if (c->in_len < used)
				return;
		} else {
			c->in[c->in_len] = '\0';
			end = strstr(c->in, "\r\n\r\n");
			if (!end)
				return;
			cl = header(c->in, "Content-Length");
			if (cl && cl < end)
				body = (size_t)atoi(cl);
			used = (size_t)(end - c->in) + 4 + body;
			if (used > sizeof(c->in) - 1) {
				drop_client(c);
				return;
			}
			if (c->in_len < used)
				return;
			end[2] = '\0';
			handle_request(c, c->in);
			if (c->fd < 0)
				return;
		}
		memmove(c->in, c->in + used, c->in_len - used);
		c->in_len -= used;
	}
}

static void accept_client(void)
{
	struct sockaddr_in peer;
	socklen_t len = sizeof(peer);
	struct timeval tv = { 0, 200000 };
	int fd = accept4(listen_fd, (struct sockaddr *)&peer, &len, SOCK_CLOEXEC);
	int i, one = 1, v;
	socklen_t vl = sizeof(v);

	if (fd < 0)
		return;
	for (i = 0; i < MAX_CLIENTS && clients[i].fd >= 0; i++)
		;
	if (i == MAX_CLIENTS) {
		LOGW("rtsp: %s refused, %d clients already connected", inet_ntoa(peer.sin_addr), MAX_CLIENTS);
		close(fd);
		return;
	}
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	/* A viewer that goes out of range never closes its connection.
	 * Keepalive notices within about half a minute, which stops the
	 * camera sending video at an address nobody is behind. */
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
	v = 10;
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &v, sizeof(v));
	v = 5;
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &v, sizeof(v));
	v = 3;
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &v, sizeof(v));
	v = TCP_SNDBUF;
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	memset(&clients[i], 0, sizeof(clients[i]));
	clients[i].fd = fd;
	clients[i].stream = -1;
	clients[i].rtp_fd = clients[i].rtcp_fd = -1;
	clients[i].peer = peer;
	v = 0;
	getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, &vl);
	clients[i].sndbuf = v > 0 ? v : 65536;
	LOGI("rtsp: %s connected", inet_ntoa(peer.sin_addr));
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
		LOGE("rtsp: cannot listen on port %d: %s", port, strerror(errno));
		close(fd);
		return -1;
	}
	LOGI("rtsp: listening on port %d", port);
	return fd;
}

static void *rtsp_thread(void *arg)
{
	struct pollfd pfd[MAX_CLIENTS + 1];
	int map[MAX_CLIENTS + 1];
	uint64_t retry = 0;

	(void)arg;
	while (!g_quit) {
		int i, n = 0;

		if (restart || (listen_fd < 0 && now_ms() >= retry)) {
			struct config c;

			config_get(&c);
			pthread_mutex_lock(&lock);
			restart = 0;
			if (listen_fd >= 0)
				close(listen_fd);
			for (i = 0; i < MAX_CLIENTS; i++)
				drop_client(&clients[i]);
			listen_fd = open_listener(c.rtsp_port);
			pthread_mutex_unlock(&lock);
			retry = now_ms() + 3000;
		}

		pthread_mutex_lock(&lock);
		if (listen_fd >= 0) {
			pfd[n].fd = listen_fd;
			pfd[n].events = POLLIN;
			map[n++] = -1;
		}
		for (i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i].fd < 0)
				continue;
			pfd[n].fd = clients[i].fd;
			pfd[n].events = POLLIN;
			map[n++] = i;
		}
		pthread_mutex_unlock(&lock);

		if (poll(pfd, (nfds_t)n, 300) <= 0)
			continue;

		pthread_mutex_lock(&lock);
		for (i = 0; i < n; i++) {
			struct client *c;
			ssize_t r;

			if (!pfd[i].revents)
				continue;
			if (map[i] < 0) {
				accept_client();
				continue;
			}
			c = &clients[map[i]];
			/* The slot may have been dropped, and even refilled, by
			 * a producer while this thread was in poll(). */
			if (c->fd != pfd[i].fd)
				continue;
			r = recv(c->fd, c->in + c->in_len, sizeof(c->in) - 1 - c->in_len, MSG_DONTWAIT);
			if (r > 0) {
				c->in_len += (size_t)r;
				client_input(c);
				if (c->fd >= 0 && c->in_len >= sizeof(c->in) - 1)
					drop_client(c);
			} else if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
				drop_client(c);
			}
		}
		pthread_mutex_unlock(&lock);
	}
	return NULL;
}

int rtsp_start(void)
{
	pthread_t t;
	int i;

	for (i = 0; i < MAX_CLIENTS; i++)
		clients[i].fd = clients[i].rtp_fd = clients[i].rtcp_fd = -1;
	restart = 1;
	return pthread_create(&t, NULL, rtsp_thread, NULL);
}
