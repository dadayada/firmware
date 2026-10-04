/*
 * The sensor side: sensor -> VIF -> VPE (the ISP and scaler) -> VENC
 * channel 0, and every ISP control the page offers.
 *
 * The call order is the one Divinus' i6 HAL used on this SoC (it ran on the
 * SSC338Q + IMX415 this was written for), expressed with the vendor's own
 * headers instead of hand-copied structures. Where the two disagree the
 * comment says so.
 *
 * Everything here except venc_create/destroy/forward and the status reader
 * runs on the sensor thread.
 */
#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mi_sys.h>
#include <mi_sensor.h>
#include <mi_vif.h>
#include <mi_vpe.h>
#include <mi_venc.h>
#include <mi_isp.h>

#include "fpvcam.h"

#define PAD		E_MI_SNR_PAD_ID_0
#define MAX_MODES	16
#define NV12		E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420

/* Every failed SDK call is logged with its name and code: the camera is
 * usually somewhere a debugger is not, and the code is all there is. */
#define CALL(fn, ...) sdk_check(#fn, fn(__VA_ARGS__))

static int sdk_check(const char *name, int ret)
{
	if (ret)
		LOGE("%s failed: 0x%08x", name, (unsigned int)ret);
	return ret;
}

static struct {
	int pipe_up, enc_up, isp_ready, isp_tries, fd;
	uint64_t isp_due, retry_at, last_frame, last_poll;
	struct config c;	/* what the running pipeline was built from */
	MI_SNR_Res_t modes[MAX_MODES];
	unsigned int mode_count, sensor_fps;
	int mode, out_w, out_h;
	MI_SYS_WindowRect_t capt;
	char sensor[32], error[96];
	MI_ISP_AE_EXPO_INFO_TYPE_t ae;
	MI_ISP_AWB_QUERY_INFO_TYPE_t awb;
	int have_3a;
} S;

/* What the tuning file set, read back right after it is loaded. A setting
 * left at -1 is put back to this, so "default" stays meaningful after the
 * value has been changed and changed back. */
static struct {
	MI_ISP_IQ_BRIGHTNESS_TYPE_t brightness;
	MI_ISP_IQ_CONTRAST_TYPE_t contrast;
	MI_ISP_IQ_LIGHTNESS_TYPE_t lightness;
	MI_ISP_IQ_SATURATION_TYPE_t saturation;
	MI_ISP_IQ_SHARPNESS_TYPE_t sharpness;
	MI_ISP_AE_EXPO_LIMIT_TYPE_t limit;
	MI_ISP_AWB_ATTR_TYPE_t awb;
	/* One bit per member above, set when it was read back successfully.
	 * A member that was not is never written to the ISP: it would be
	 * zeroes, and a zeroed exposure limit or AWB block is a dark or
	 * green picture that nothing afterwards would correct. */
	unsigned int ok;
} base;

enum { B_BRIGHTNESS = 1, B_CONTRAST = 2, B_LIGHTNESS = 4, B_SATURATION = 8,
       B_SHARPNESS = 16, B_LIMIT = 32, B_AWB = 64, B_ALL = 127 };

static pthread_mutex_t status_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t venc_lock = PTHREAD_MUTEX_INITIALIZER;
static int pending;

void sensor_request(int flags)
{
	__sync_fetch_and_or(&pending, flags);
}

int sdk_init(void)
{
	MI_SYS_Version_t ver;

	if (CALL(MI_SYS_Init))
		return -1;
	memset(&ver, 0, sizeof(ver));
	if (!MI_SYS_GetVersion(&ver))
		LOGI("sdk: %.100s", ver.u8Version);
	return 0;
}

void sdk_exit(void)
{
	CALL(MI_SYS_Exit);
}

/* ---- encoder channels, shared with the usb thread ---- */

int venc_device(int chn, unsigned int *dev)
{
	return CALL(MI_VENC_GetChnDevid, chn, dev);
}

int venc_create(int chn, const struct enc_params *p)
{
	MI_VENC_ChnAttr_t a;
	MI_VENC_AttrH264_t *v;
	unsigned int bps = (unsigned int)p->bitrate << 10;
	int h265 = p->codec == CODEC_H265, ret;

	memset(&a, 0, sizeof(a));
	/* The two attribute structures have the same layout; the union lets
	 * one set of assignments serve both. */
	v = &a.stVeAttr.stAttrH264e;
	a.stVeAttr.eType = h265 ? E_MI_VENC_MODTYPE_H265E : E_MI_VENC_MODTYPE_H264E;
	v->u32MaxPicWidth = v->u32PicWidth = p->width;
	v->u32MaxPicHeight = v->u32PicHeight = p->height;
	v->u32BufSize = p->width * p->height;
	v->u32Profile = h265 ? 0 : p->profile;
	v->bByFrame = 1;
	v->u32RefNum = 1;

	switch (p->rc_mode) {
	case RC_VBR:
	case RC_AVBR: {
		/* H264Vbr, H264Avbr, H265Vbr and H265Avbr share one layout. */
		MI_VENC_AttrH264Vbr_t *r = &a.stRcAttr.stAttrH264Vbr;

		if (p->rc_mode == RC_VBR)
			a.stRcAttr.eRcMode = h265 ? E_MI_VENC_RC_MODE_H265VBR : E_MI_VENC_RC_MODE_H264VBR;
		else
			a.stRcAttr.eRcMode = h265 ? E_MI_VENC_RC_MODE_H265AVBR : E_MI_VENC_RC_MODE_H264AVBR;
		r->u32Gop = p->gop;
		r->u32StatTime = 1;
		r->u32SrcFrmRateNum = p->fps;
		r->u32SrcFrmRateDen = 1;
		r->u32MaxBitRate = bps;
		r->u32MaxQp = p->max_qp > p->min_qp ? p->max_qp : p->min_qp;
		r->u32MinQp = p->min_qp;
		break;
	}
	default: {
		MI_VENC_AttrH264Cbr_t *r = &a.stRcAttr.stAttrH264Cbr;

		a.stRcAttr.eRcMode = h265 ? E_MI_VENC_RC_MODE_H265CBR : E_MI_VENC_RC_MODE_H264CBR;
		r->u32Gop = p->gop;
		r->u32StatTime = 1;
		r->u32SrcFrmRateNum = p->fps;
		r->u32SrcFrmRateDen = 1;
		r->u32BitRate = bps;
		r->u32FluctuateLevel = 1;
	}
	}

	pthread_mutex_lock(&venc_lock);
	ret = CALL(MI_VENC_CreateChn, chn, &a);
	if (!ret && (ret = CALL(MI_VENC_StartRecvPic, chn)))
		MI_VENC_DestroyChn(chn);
	pthread_mutex_unlock(&venc_lock);
	if (!ret)
		LOGI("venc%d: %s %dx%d@%d %s %dkbit gop %d", chn, codec_name(p->codec),
		     p->width, p->height, p->fps,
		     p->rc_mode == RC_CBR ? "cbr" : p->rc_mode == RC_VBR ? "vbr" : "avbr",
		     p->bitrate, p->gop);
	return ret;
}

static int venc_fds[2] = { -1, -1 };

void venc_destroy(int chn)
{
	pthread_mutex_lock(&venc_lock);
	CALL(MI_VENC_StopRecvPic, chn);
	if (venc_fds[chn] >= 0) {
		MI_VENC_CloseFd(chn);
		venc_fds[chn] = -1;
	}
	CALL(MI_VENC_DestroyChn, chn);
	pthread_mutex_unlock(&venc_lock);
}

/* Asked for once per channel: the descriptor belongs to the channel and is
 * closed with it in venc_destroy(). */
int venc_fd(int chn)
{
	if (venc_fds[chn] < 0)
		venc_fds[chn] = MI_VENC_GetFd(chn);
	return venc_fds[chn];
}

void venc_request_idr(int chn)
{
	MI_VENC_RequestIdr(chn, 1);
}

int venc_forward(int chn, int stream)
{
	MI_VENC_Pack_t packs[8], *big = NULL;
	MI_VENC_Stream_t st;
	MI_VENC_ChnStat_t stat;
	unsigned int i;

	memset(&stat, 0, sizeof(stat));
	if (MI_VENC_Query(chn, &stat) || !stat.u32CurPacks)
		return 0;
	memset(&st, 0, sizeof(st));
	/* Room for every pack the frame has, as Divinus gave it: whether the
	 * call accepts fewer is not something the header says. */
	st.pstPack = packs;
	if (stat.u32CurPacks > 8) {
		big = calloc(stat.u32CurPacks, sizeof(*big));
		if (!big)
			return 0;
		st.pstPack = big;
	}
	st.u32PackCount = stat.u32CurPacks;
	if (MI_VENC_GetStream(chn, &st, 40)) {
		free(big);
		return 0;
	}
	for (i = 0; i < st.u32PackCount; i++) {
		MI_VENC_Pack_t *p = &st.pstPack[i];

		if (p->u32Len > p->u32Offset)
			rtsp_send_h26x(stream, p->pu8Addr + p->u32Offset, p->u32Len - p->u32Offset,
				       p->u64PTS, i + 1 == st.u32PackCount);
	}
	MI_VENC_ReleaseStream(chn, &st);
	free(big);
	return 1;
}

/* ---- sensor pipeline ---- */

static void set_error(const char *what)
{
	pthread_mutex_lock(&status_lock);
	snprintf(S.error, sizeof(S.error), "%s", what);
	pthread_mutex_unlock(&status_lock);
	if (*what)
		LOGW("sensor: %s", what);
}

static unsigned int mode_w(const MI_SNR_Res_t *m)
{
	return m->stOutputSize.u16Width < m->stCropRect.u16Width ?
	       m->stOutputSize.u16Width : m->stCropRect.u16Width;
}

static unsigned int mode_h(const MI_SNR_Res_t *m)
{
	return m->stOutputSize.u16Height < m->stCropRect.u16Height ?
	       m->stOutputSize.u16Height : m->stCropRect.u16Height;
}

/*
 * The smallest mode that covers the request, not the first one: drivers list
 * their largest mode first, and on the IMX415 the first fit for 1080p60 is a
 * 2952x1656 crop the SSC338Q ISP cannot sustain at 60fps -- it delivered 30
 * ("ISP P0 FIFO FULL", 2026-09-29). If nothing covers the size, the frame
 * rate wins and the largest mode that reaches it is used, the picture then
 * being that mode's size; if nothing reaches the frame rate
 * either, the fastest mode is the least wrong answer.
 */
static int pick_mode(const struct config *c)
{
	unsigned int i, best_area = ~0u, big_area = 0;
	int fit = -1, big = -1, fastest = 0;

	if (c->sensor_mode >= 0 && (unsigned int)c->sensor_mode < S.mode_count)
		return c->sensor_mode;
	for (i = 0; i < S.mode_count; i++) {
		const MI_SNR_Res_t *m = &S.modes[i];
		unsigned int area = mode_w(m) * mode_h(m);

		if (m->u32MaxFps > S.modes[fastest].u32MaxFps)
			fastest = (int)i;
		if ((unsigned int)c->fps > m->u32MaxFps)
			continue;
		if (area > big_area) {
			big = (int)i;
			big_area = area;
		}
		if (mode_w(m) >= (unsigned int)c->width && mode_h(m) >= (unsigned int)c->height &&
		    area < best_area) {
			fit = (int)i;
			best_area = area;
		}
	}
	return fit >= 0 ? fit : big >= 0 ? big : fastest;
}

static void bind_port(MI_SYS_ChnPort_t *p, MI_ModuleId_e mod, unsigned int dev)
{
	memset(p, 0, sizeof(*p));
	p->eModId = mod;
	p->u32DevId = dev;
}

/*
 * The encoder takes NV12: a full-size Y plane, then one interleaved CbCr
 * row for every two picture rows. YUYV has chroma on every row, so each
 * pair of rows is averaged into one; taking only the even rows would alias
 * on the thin horizontal edges a thermal image is full of.
 *
 * The buffer comes from the encoder's own input queue and goes straight
 * back into it, so the frame is copied once, here.
 */
int venc_inject_yuyv(int chn, unsigned int dev, int width, int height,
		     const uint8_t *yuyv, uint64_t pts_us)
{
	MI_SYS_ChnPort_t port;
	MI_SYS_BufConf_t conf;
	MI_SYS_BufInfo_t info;
	MI_SYS_BUF_HANDLE handle;
	uint8_t *y, *uv;
	int row, x;

	bind_port(&port, E_MI_MODULE_ID_VENC, dev);
	port.u32ChnId = (MI_U32)chn;
	memset(&conf, 0, sizeof(conf));
	conf.eBufType = E_MI_SYS_BUFDATA_FRAME;
	conf.u32Flags = MI_SYS_MAP_VA;
	conf.u64TargetPts = pts_us;
	conf.stFrameCfg.u16Width = (MI_U16)width;
	conf.stFrameCfg.u16Height = (MI_U16)height;
	conf.stFrameCfg.eFrameScanMode = E_MI_SYS_FRAME_SCAN_MODE_PROGRESSIVE;
	conf.stFrameCfg.eFormat = NV12;
	memset(&info, 0, sizeof(info));
	if (MI_SYS_ChnInputPortGetBuf(&port, &conf, &info, &handle, 0))
		return -1;
	y = info.stFrameData.pVirAddr[0];
	uv = info.stFrameData.pVirAddr[1];
	if (!y || !uv || info.stFrameData.u32Stride[0] < (MI_U32)width ||
	    info.stFrameData.u32Stride[1] < (MI_U32)width) {
		MI_SYS_ChnInputPortPutBuf(handle, &info, 1);
		return -1;
	}
	for (row = 0; row < height; row++) {
		const uint8_t *src = yuyv + (size_t)row * (size_t)width * 2;
		uint8_t *dst = y + (size_t)row * info.stFrameData.u32Stride[0];

		for (x = 0; x < width; x++)
			dst[x] = src[2 * x];
	}
	for (row = 0; row + 1 < height; row += 2) {
		const uint8_t *a = yuyv + (size_t)row * (size_t)width * 2;
		const uint8_t *b = a + (size_t)width * 2;
		uint8_t *dst = uv + (size_t)(row / 2) * info.stFrameData.u32Stride[1];

		for (x = 0; x < width; x++)
			dst[x] = (uint8_t)((a[2 * x + 1] + b[2 * x + 1] + 1) >> 1);
	}
	/* The encoder reads this memory by its physical address. Whether the
	 * hand-back flushes the CPU cache for a mapped buffer is not in the
	 * header, and a frame with stale blocks in it is the cost of guessing
	 * wrong, so it is flushed here; a refusal changes nothing. */
	MI_SYS_FlushInvCache(y, info.stFrameData.u32BufSize);
	info.u64Pts = pts_us;
	return MI_SYS_ChnInputPortPutBuf(handle, &info, 0) ? -1 : 0;
}

static int pipe_up(void)
{
	MI_SNR_PADInfo_t pad;
	MI_SNR_PlaneInfo_t plane;
	MI_VIF_DevAttr_t dev;
	MI_VIF_ChnPortAttr_t port;
	MI_VPE_ChannelAttr_t chn;
	MI_VPE_ChannelPara_t para;
	MI_SYS_ChnPort_t src, dst;
	MI_SYS_PixelFormat_e bayer;
	MI_U32 count = 0;
	unsigned int i, fps;
	int mode;

	if (CALL(MI_SNR_SetPlaneMode, PAD, 0) || CALL(MI_SNR_QueryResCount, PAD, &count))
		return -1;
	if (count > MAX_MODES)
		count = MAX_MODES;
	pthread_mutex_lock(&status_lock);
	S.mode_count = 0;
	for (i = 0; i < count; i++) {
		memset(&S.modes[i], 0, sizeof(S.modes[i]));
		if (MI_SNR_GetRes(PAD, (MI_U8)i, &S.modes[i]))
			break;
		S.mode_count = i + 1;
	}
	pthread_mutex_unlock(&status_lock);
	if (!S.mode_count) {
		set_error("the sensor driver reports no modes; is the sensor module loaded?");
		return -1;
	}

	mode = pick_mode(&S.c);
	fps = (unsigned int)S.c.fps;
	if (fps > S.modes[mode].u32MaxFps)
		fps = S.modes[mode].u32MaxFps;
	if (fps < S.modes[mode].u32MinFps)
		fps = S.modes[mode].u32MinFps;

	if (CALL(MI_SNR_SetRes, PAD, (MI_U8)mode) ||
	    CALL(MI_SNR_SetFps, PAD, fps) ||
	    CALL(MI_SNR_SetOrien, PAD, (MI_BOOL)S.c.mirror, (MI_BOOL)S.c.flip))
		return -1;
	memset(&pad, 0, sizeof(pad));
	memset(&plane, 0, sizeof(plane));
	if (CALL(MI_SNR_GetPadInfo, PAD, &pad) ||
	    CALL(MI_SNR_GetPlaneInfo, PAD, 0, &plane) ||
	    CALL(MI_SNR_Enable, PAD))
		return -1;

	bayer = plane.eBayerId > E_MI_SYS_PIXEL_BAYERID_MAX ? plane.ePixel :
		(MI_SYS_PixelFormat_e)RGB_BAYER_PIXEL(plane.ePixPrecision, plane.eBayerId);

	memset(&dev, 0, sizeof(dev));
	dev.eIntfMode = pad.eIntfMode;
	dev.eWorkMode = pad.eIntfMode == E_MI_VIF_MODE_BT656 ?
			E_MI_VIF_WORK_MODE_1MULTIPLEX : E_MI_VIF_WORK_MODE_RGB_REALTIME;
	dev.eHDRType = E_MI_VIF_HDR_TYPE_OFF;
	if (pad.eIntfMode == E_MI_VIF_MODE_MIPI) {
		dev.eClkEdge = E_MI_VIF_CLK_EDGE_DOUBLE;
		dev.eDataSeq = pad.unIntfAttr.stMipiAttr.eDataYUVOrder;
	} else if (pad.eIntfMode == E_MI_VIF_MODE_BT656) {
		dev.eClkEdge = pad.unIntfAttr.stBt656Attr.eClkEdge;
		dev.stSyncAttr = pad.unIntfAttr.stBt656Attr.stSyncAttr;
	}
	if (CALL(MI_VIF_SetDevAttr, 0, &dev) || CALL(MI_VIF_EnableDev, 0))
		goto fail_snr;

	memset(&port, 0, sizeof(port));
	port.stCapRect = plane.stCapRect;
	port.stDestSize.u16Width = plane.stCapRect.u16Width;
	port.stDestSize.u16Height = plane.stCapRect.u16Height;
	port.ePixFormat = bayer;
	port.eFrameRate = E_MI_VIF_FRAMERATE_FULL;
	if (CALL(MI_VIF_SetChnPortAttr, 0, 0, &port) || CALL(MI_VIF_EnableChnPort, 0, 0))
		goto fail_vifdev;

	memset(&chn, 0, sizeof(chn));
	chn.u16MaxW = plane.stCapRect.u16Width;
	chn.u16MaxH = plane.stCapRect.u16Height;
	chn.ePixFmt = bayer;
	chn.eHDRType = E_MI_VPE_HDR_TYPE_OFF;
	chn.eSensorBindId = E_MI_VPE_SENSOR0;
	chn.eRunningMode = E_MI_VPE_RUN_REALTIME_MODE;
	if (CALL(MI_VPE_CreateChannel, 0, &chn))
		goto fail_vifport;
	memset(&para, 0, sizeof(para));
	para.eHDRType = E_MI_VPE_HDR_TYPE_OFF;
	para.e3DNRLevel = (MI_VPE_3DNR_Level_e)S.c.nr3d;
	if (CALL(MI_VPE_SetChannelParam, 0, &para) || CALL(MI_VPE_StartChannel, 0))
		goto fail_vpe;

	bind_port(&src, E_MI_MODULE_ID_VIF, 0);
	bind_port(&dst, E_MI_MODULE_ID_VPE, 0);
	if (CALL(MI_SYS_BindChnPort2, &src, &dst, fps, fps, E_MI_SYS_BIND_TYPE_REALTIME, 0)) {
		MI_VPE_StopChannel(0);
		goto fail_vpe;
	}

	pthread_mutex_lock(&status_lock);
	S.mode = mode;
	S.sensor_fps = fps;
	S.capt = plane.stCapRect;
	snprintf(S.sensor, sizeof(S.sensor), "%.31s", (const char *)plane.s8SensorName);
	pthread_mutex_unlock(&status_lock);
	S.pipe_up = 1;
	LOGI("sensor: %s mode %d (%.32s), %ux%u at %ufps", S.sensor, mode,
	     (const char *)S.modes[mode].strResDesc, plane.stCapRect.u16Width,
	     plane.stCapRect.u16Height, fps);
	return 0;

fail_vpe:
	MI_VPE_DestroyChannel(0);
fail_vifport:
	MI_VIF_DisableChnPort(0, 0);
fail_vifdev:
	MI_VIF_DisableDev(0);
fail_snr:
	MI_SNR_Disable(PAD);
	return -1;
}

static void pipe_down(void)
{
	MI_SYS_ChnPort_t src, dst;

	if (!S.pipe_up)
		return;
	bind_port(&src, E_MI_MODULE_ID_VIF, 0);
	bind_port(&dst, E_MI_MODULE_ID_VPE, 0);
	CALL(MI_SYS_UnBindChnPort, &src, &dst);
	CALL(MI_VPE_StopChannel, 0);
	CALL(MI_VPE_DestroyChannel, 0);
	CALL(MI_VIF_DisableChnPort, 0, 0);
	CALL(MI_VIF_DisableDev, 0);
	CALL(MI_SNR_Disable, PAD);
	S.pipe_up = S.isp_ready = 0;
	S.isp_due = 0;
}

static int enc_up(void)
{
	MI_VPE_PortMode_t mode;
	MI_SYS_ChnPort_t src, dst;
	struct enc_params p;
	unsigned int dev = 0;
	int w = S.c.width, h = S.c.height;

	/* The scaler only goes down. Even dimensions, because 4:2:0 has no
	 * half chroma sample to give an odd one. */
	if (w > S.capt.u16Width)
		w = S.capt.u16Width;
	if (h > S.capt.u16Height)
		h = S.capt.u16Height;
	w &= ~1;
	h &= ~1;

	memset(&mode, 0, sizeof(mode));
	mode.u16Width = (MI_U16)w;
	mode.u16Height = (MI_U16)h;
	mode.ePixelFormat = NV12;
	mode.eCompressMode = E_MI_SYS_COMPRESS_MODE_NONE;
	if (CALL(MI_VPE_SetPortMode, 0, 0, &mode))
		return -1;

	p.codec = S.c.codec;
	p.width = w;
	p.height = h;
	p.fps = (int)S.sensor_fps;
	p.rc_mode = S.c.rc_mode;
	p.bitrate = S.c.bitrate;
	p.gop = S.c.gop;
	p.profile = S.c.profile;
	p.min_qp = S.c.min_qp;
	p.max_qp = S.c.max_qp;
	if (venc_create(0, &p))
		return -1;
	if (venc_device(0, &dev) || CALL(MI_VPE_EnablePort, 0, 0))
		goto fail;
	bind_port(&src, E_MI_MODULE_ID_VPE, 0);
	bind_port(&dst, E_MI_MODULE_ID_VENC, dev);
	if (CALL(MI_SYS_BindChnPort2, &src, &dst, S.sensor_fps, S.sensor_fps,
		 E_MI_SYS_BIND_TYPE_FRAME_BASE, 0)) {
		MI_VPE_DisablePort(0, 0);
		goto fail;
	}
	pthread_mutex_lock(&status_lock);
	S.out_w = w;
	S.out_h = h;
	pthread_mutex_unlock(&status_lock);
	rtsp_set_stream(STREAM_MAIN, S.c.codec, w, h, (int)S.sensor_fps);
	S.fd = venc_fd(0);
	S.enc_up = 1;
	S.last_frame = now_ms();
	return 0;
fail:
	venc_destroy(0);
	return -1;
}

static void enc_down(void)
{
	MI_SYS_ChnPort_t src, dst;
	unsigned int dev = 0;

	if (!S.enc_up)
		return;
	S.enc_up = 0;
	venc_device(0, &dev);
	CALL(MI_VPE_DisablePort, 0, 0);
	bind_port(&src, E_MI_MODULE_ID_VPE, 0);
	bind_port(&dst, E_MI_MODULE_ID_VENC, dev);
	CALL(MI_SYS_UnBindChnPort, &src, &dst);
	venc_destroy(0);
}

/* ---- ISP ---- */

/*
 * "IMX415_MIPI" -> /etc/sensors/imx415.bin. The osdrv package installs one
 * tuning file per sensor under that name, and the driver's own name is the
 * one thing here that cannot be out of step with the hardware.
 */
static int isp_load_tuning(void)
{
	MI_ISP_IQ_PARAM_INIT_INFO_TYPE_t init;
	char path[64], name[32];
	size_t i;

	/* Not ready yet: the caller comes back a little later instead of
	 * loading a file into an ISP that will overwrite it. After ten tries
	 * the flag is taken to mean nothing on this SDK and is ignored. */
	memset(&init, 0, sizeof(init));
	if (!MI_ISP_IQ_GetParaInitStatus(0, &init) && !init.stParaAPI.bFlag) {
		if (S.isp_tries < 10)
			return -1;
		LOGW("isp: never reported its parameters initialised, loading the tuning file anyway");
	}

	for (i = 0; S.sensor[i] && S.sensor[i] != '_' && i < sizeof(name) - 1; i++)
		name[i] = (char)tolower((unsigned char)S.sensor[i]);
	name[i] = '\0';
	snprintf(path, sizeof(path), "/etc/sensors/%s.bin", name);
	if (access(path, R_OK)) {
		LOGW("isp: no tuning file %s, the ISP runs on its built-in defaults", path);
	} else if (!CALL(MI_ISP_API_CmdLoadBinFile, 0, path, 1234)) {
		LOGI("isp: loaded %s", path);
	}

	/* Loading the file resets all of these, so this is the moment they
	 * hold exactly what the file says. */
	base.ok = 0;
	if (!CALL(MI_ISP_IQ_GetBrightness, 0, &base.brightness))
		base.ok |= B_BRIGHTNESS;
	if (!CALL(MI_ISP_IQ_GetContrast, 0, &base.contrast))
		base.ok |= B_CONTRAST;
	if (!CALL(MI_ISP_IQ_GetLightness, 0, &base.lightness))
		base.ok |= B_LIGHTNESS;
	if (!CALL(MI_ISP_IQ_GetSaturation, 0, &base.saturation))
		base.ok |= B_SATURATION;
	if (!CALL(MI_ISP_IQ_GetSharpness, 0, &base.sharpness))
		base.ok |= B_SHARPNESS;
	if (!CALL(MI_ISP_AE_GetExposureLimit, 0, &base.limit))
		base.ok |= B_LIMIT;
	if (!CALL(MI_ISP_AWB_GetAttr, 0, &base.awb))
		base.ok |= B_AWB;
	if (base.ok & B_LIMIT)
		LOGI("isp: tuning limits shutter %u-%uus, sensor gain %u-%u, isp gain %u-%u",
		     (unsigned int)base.limit.u32MinShutterUS, (unsigned int)base.limit.u32MaxShutterUS,
		     (unsigned int)base.limit.u32MinSensorGain, (unsigned int)base.limit.u32MaxSensorGain,
		     (unsigned int)base.limit.u32MinISPGain, (unsigned int)base.limit.u32MaxISPGain);
	return base.ok == B_ALL || S.isp_tries >= 10 ? 0 : -1;
}

static unsigned int scale_u16(unsigned int v, int pct)
{
	v = v * (unsigned int)pct / 100;
	return v > 1023 ? 1023 : v;
}

static void isp_apply(void)
{
	const struct config *c = &S.c;
	MI_ISP_IQ_BRIGHTNESS_TYPE_t br = base.brightness;
	MI_ISP_IQ_CONTRAST_TYPE_t co = base.contrast;
	MI_ISP_IQ_LIGHTNESS_TYPE_t li = base.lightness;
	MI_ISP_IQ_SATURATION_TYPE_t sa = base.saturation;
	MI_ISP_IQ_SHARPNESS_TYPE_t sh = base.sharpness;
	MI_ISP_IQ_COLORTOGRAY_TYPE_t gray;
	MI_ISP_AE_EXPO_LIMIT_TYPE_t lim = base.limit;
	MI_ISP_AE_MODE_TYPE_e ae_mode;
	MI_ISP_AE_EV_COMP_TYPE_t ev;
	MI_ISP_AE_FLICKER_TYPE_e flicker;
	MI_ISP_AWB_ATTR_TYPE_t awb = base.awb;
	int i, k;

	if (c->brightness >= 0) {
		br.bEnable = SS_TRUE;
		br.enOpType = SS_OP_TYP_MANUAL;
		br.stManual.stParaAPI.u32Lev = (MI_U32)c->brightness;
	}
	if (base.ok & B_BRIGHTNESS)
		CALL(MI_ISP_IQ_SetBrightness, 0, &br);
	if (c->contrast >= 0) {
		co.bEnable = SS_TRUE;
		co.enOpType = SS_OP_TYP_MANUAL;
		co.stManual.stParaAPI.u32Lev = (MI_U32)c->contrast;
	}
	if (base.ok & B_CONTRAST)
		CALL(MI_ISP_IQ_SetContrast, 0, &co);
	if (c->lightness >= 0) {
		li.bEnable = SS_TRUE;
		li.enOpType = SS_OP_TYP_MANUAL;
		li.stManual.stParaAPI.u32Lev = (MI_U32)c->lightness;
	}
	if (base.ok & B_LIGHTNESS)
		CALL(MI_ISP_IQ_SetLightness, 0, &li);

	/* Saturation and sharpness are tables indexed by gain in the tuning
	 * file. Scaling every row keeps the file's shape (less colour and
	 * less sharpening in the dark) and moves all of it. Saturation is
	 * in the SDK's own unit, where 32 is 1x. */
	if (c->saturation >= 0) {
		for (i = -1; i < MI_ISP_AUTO_NUM; i++) {
			SATURATION_PARAM_t *p = i < 0 ? &sa.stManual.stParaAPI : &sa.stAuto.stParaAPI[i];
			unsigned int v = p->u8SatAllStr * (unsigned int)c->saturation / 32;

			p->u8SatAllStr = (MI_U8)(v > 127 ? 127 : v);
		}
	}
	if (base.ok & B_SATURATION)
		CALL(MI_ISP_IQ_SetSaturation, 0, &sa);
	if (c->sharpness >= 0) {
		for (i = -1; i < MI_ISP_AUTO_NUM; i++) {
			SHARPNESS_PARAM_t *p = i < 0 ? &sh.stManual.stParaAPI : &sh.stAuto.stParaAPI[i];

			for (k = 0; k < 2; k++) {
				p->u16SharpnessUD[k] = (MI_U16)scale_u16(p->u16SharpnessUD[k], c->sharpness);
				p->u16SharpnessD[k] = (MI_U16)scale_u16(p->u16SharpnessD[k], c->sharpness);
			}
		}
	}
	if (base.ok & B_SHARPNESS)
		CALL(MI_ISP_IQ_SetSharpness, 0, &sh);

	memset(&gray, 0, sizeof(gray));
	gray.bEnable = c->grayscale ? SS_TRUE : SS_FALSE;
	CALL(MI_ISP_IQ_SetColorToGray, 0, &gray);

	/* A shutter longer than the frame period makes the sensor drop its
	 * frame rate to fit it: 1080p@90 ran at 10-20fps indoors on the
	 * tuning file's own limit (2026-09-29). The default is the frame
	 * period rounded down to a whole millisecond, 16000us at 60fps: the
	 * figure that held 60fps on this camera, and what OpenIPC's star.c
	 * computes. The exact period, 16666us, is untried. */
	lim.u32MaxShutterUS = c->max_shutter_us ? (MI_U32)c->max_shutter_us :
			      (1000 / S.sensor_fps ? 1000 / S.sensor_fps * 1000 : 1000000 / S.sensor_fps);
	if (lim.u32MaxShutterUS < lim.u32MinShutterUS)
		lim.u32MaxShutterUS = lim.u32MinShutterUS;
	if (c->max_sensor_gain)
		lim.u32MaxSensorGain = (MI_U32)c->max_sensor_gain * 1024;
	if (c->max_isp_gain)
		lim.u32MaxISPGain = (MI_U32)c->max_isp_gain * 1024;
	if (base.ok & B_LIMIT)
		CALL(MI_ISP_AE_SetExposureLimit, 0, &lim);

	if (c->exposure_mode) {
		MI_ISP_AE_EXPO_VALUE_TYPE_t man;

		memset(&man, 0, sizeof(man));
		MI_ISP_AE_GetManualExpo(0, &man);
		man.u32US = (MI_U32)c->shutter_us;
		man.u32SensorGain = (MI_U32)c->sensor_gain * 1024;
		man.u32ISPGain = 1024;
		CALL(MI_ISP_AE_SetManualExpo, 0, &man);
	}
	ae_mode = c->exposure_mode ? SS_AE_MODE_M : SS_AE_MODE_A;
	CALL(MI_ISP_AE_SetExpoMode, 0, &ae_mode);

	ev.s32EV = c->ev_comp;
	ev.u32Grad = 10;
	CALL(MI_ISP_AE_SetEVComp, 0, &ev);

	/* The setting's option order is the SDK's enum order. */
	flicker = (MI_ISP_AE_FLICKER_TYPE_e)c->antiflicker;
	CALL(MI_ISP_AE_SetFlicker, 0, &flicker);

	awb.eState = SS_ISP_STATE_NORMAL;
	if (c->awb_mode) {
		awb.eOpType = SS_AWB_MODE_MANUAL;
		awb.stManualParaAPI.u16Rgain = (MI_U16)c->wb_r_gain;
		awb.stManualParaAPI.u16Grgain = (MI_U16)c->wb_g_gain;
		awb.stManualParaAPI.u16Gbgain = (MI_U16)c->wb_g_gain;
		awb.stManualParaAPI.u16Bgain = (MI_U16)c->wb_b_gain;
	} else {
		awb.eOpType = SS_AWB_MODE_AUTO;
	}
	if (base.ok & B_AWB)
		CALL(MI_ISP_AWB_SetAttr, 0, &awb);
	LOGI("isp: settings applied (shutter limit %uus)", (unsigned int)lim.u32MaxShutterUS);
}

static void isp_poll_3a(void)
{
	MI_ISP_AE_EXPO_INFO_TYPE_t ae;
	MI_ISP_AWB_QUERY_INFO_TYPE_t awb;
	int ok;

	memset(&ae, 0, sizeof(ae));
	memset(&awb, 0, sizeof(awb));
	ok = !MI_ISP_AE_QueryExposureInfo(0, &ae) && !MI_ISP_AWB_QueryInfo(0, &awb);
	pthread_mutex_lock(&status_lock);
	S.ae = ae;
	S.awb = awb;
	S.have_3a = ok;
	pthread_mutex_unlock(&status_lock);
}

/* ---- the thread ---- */

/*
 * note is what the status box shows once the rebuild has succeeded: empty
 * for an ordinary one, or why it happened when nobody asked for it. A
 * failure replaces it with what failed.
 */
static void rebuild(int whole, const char *note)
{
	char before[sizeof(S.error)];

	enc_down();
	if (whole)
		pipe_down();
	config_get(&S.c);
	if (!S.pipe_up) {
		snprintf(before, sizeof(before), "%s", S.error);
		if (pipe_up()) {
			/* pipe_up names the cause when it knows one. */
			if (!strcmp(before, S.error))
				set_error("the sensor pipeline did not start; see the log");
			S.retry_at = now_ms() + 5000;
			return;
		}
		/* The ISP is not ready for its tuning file until frames have
		 * been through it; Divinus slept a second here too. */
		S.isp_due = now_ms() + 1000;
		S.isp_tries = 0;
	}
	if (enc_up()) {
		set_error("the encoder did not start; see the log");
		S.retry_at = now_ms() + 5000;
		return;
	}
	S.retry_at = 0;
	set_error(note);
	/* The exposure ceiling follows the frame rate, which may have moved. */
	if (S.isp_ready)
		isp_apply();
}

void *sensor_thread(void *arg)
{
	(void)arg;
	rebuild(1, "");

	while (!g_quit) {
		int flags = __sync_fetch_and_and(&pending, 0);
		uint64_t now = now_ms();

		if (flags & A_PIPE) {
			rebuild(1, "");
		} else if (flags & A_ENC) {
			rebuild(0, "");
		} else if (S.retry_at && now >= S.retry_at) {
			/* Only what is down is retried: an encoder setting the
			 * hardware refuses must not restart a working sensor
			 * every five seconds. */
			rebuild(0, "");
		} else if ((flags & A_ISP) && S.pipe_up) {
			config_get(&S.c);
			if (S.isp_ready)
				isp_apply();
		}

		/* Only with the encoder running: without it no frame has been
		 * through the ISP, and there is nothing to load a file into. */
		if (S.isp_due && now >= S.isp_due && S.enc_up) {
			S.isp_tries++;
			if (isp_load_tuning()) {
				S.isp_due = now + 500;
			} else {
				S.isp_due = 0;
				isp_apply();
				S.isp_ready = 1;
			}
		}
		if (S.isp_ready && now - S.last_poll >= 1000) {
			S.last_poll = now;
			isp_poll_3a();
		}

		if (!S.enc_up) {
			usleep(100000);
			continue;
		}
		if (rtsp_take_idr_request(STREAM_MAIN))
			venc_request_idr(0);
		{
			struct pollfd pfd = { .fd = S.fd, .events = POLLIN };

			if (pfd.fd < 0) {
				/* No descriptor to wait on: fall back to asking. */
				usleep(5000);
				if (venc_forward(0, STREAM_MAIN))
					S.last_frame = now_ms();
			} else if (poll(&pfd, 1, 200) > 0) {
				if (venc_forward(0, STREAM_MAIN))
					S.last_frame = now_ms();
				else
					/* Readable with nothing to read must not
					 * become a busy loop. */
					usleep(2000);
			}
		}
		/* A sensor that stops delivering (a loose flex cable, a driver
		 * that lost its clock) is put through a full restart rather
		 * than left dark until someone notices. */
		if (now_ms() - S.last_frame > 5000) {
			LOGE("sensor: no frames for 5 seconds, restarting the pipeline");
			rebuild(1, "the sensor stopped delivering frames and was restarted");
		}
	}

	enc_down();
	pipe_down();
	rtsp_clear_stream(STREAM_MAIN);
	return NULL;
}

void sensor_status_json(struct sbuf *b)
{
	unsigned int i, fps = 0, kbps = 0;
	char desc[33];

	rtsp_stats(STREAM_MAIN, &fps, &kbps);
	pthread_mutex_lock(&status_lock);
	sb_printf(b, "\"sensor\":{\"name\":");
	sb_json_str(b, S.sensor);
	sb_printf(b, ",\"error\":");
	sb_json_str(b, S.error);
	sb_printf(b, ",\"mode\":%d,\"fps\":%u,\"width\":%d,\"height\":%d,"
		  "\"out_fps\":%u.%u,\"kbps\":%u,\"clients\":%d,\"modes\":[",
		  S.mode, S.sensor_fps, S.out_w, S.out_h, fps / 10, fps % 10, kbps,
		  rtsp_clients(STREAM_MAIN));
	for (i = 0; i < S.mode_count; i++) {
		sb_printf(b, "%s{\"w\":%u,\"h\":%u,\"max_fps\":%u,\"min_fps\":%u,\"desc\":",
			  i ? "," : "", mode_w(&S.modes[i]), mode_h(&S.modes[i]),
			  (unsigned int)S.modes[i].u32MaxFps, (unsigned int)S.modes[i].u32MinFps);
		/* Thirty-two bytes with no promise of a terminator. */
		snprintf(desc, sizeof(desc), "%.32s", (const char *)S.modes[i].strResDesc);
		sb_json_str(b, desc);
		sb_printf(b, "}");
	}
	sb_printf(b, "]");
	if (S.have_3a)
		sb_printf(b, ",\"ae\":{\"stable\":%d,\"shutter_us\":%u,\"sensor_gain\":%u,"
			  "\"isp_gain\":%u,\"lv_x10\":%u},"
			  "\"awb\":{\"r\":%u,\"g\":%u,\"b\":%u,\"kelvin\":%u}",
			  S.ae.bIsStable == SS_TRUE, (unsigned int)S.ae.stExpoValueLong.u32US,
			  (unsigned int)S.ae.stExpoValueLong.u32SensorGain,
			  (unsigned int)S.ae.stExpoValueLong.u32ISPGain, (unsigned int)S.ae.u32LVx10,
			  S.awb.u16Rgain, S.awb.u16Grgain, S.awb.u16Bgain, S.awb.u16ColorTemp);
	sb_printf(b, "}");
	pthread_mutex_unlock(&status_lock);
}
