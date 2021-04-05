#include <libwebsockets.h>

#include "private.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <linux/videodev2.h>

static int
xioctl(int fh, unsigned long request, void *arg)
{
	int r;

	do {
		r = ioctl(fh, request, arg);
	} while (-1 == r && EINTR == errno);

	return r;
}

#if !defined(OWN_MOOF)
static int
avio_wcb(void* ptr, uint8_t* buf, int buf_size)
{
	struct src_inst *si = (struct src_inst *)ptr;
	struct msg msg;

	msg.len = buf_size;
	msg.payload = malloc(LWS_PRE + msg.len);

//	lwsl_notice("%s: %d (subs %d)\n", __func__, buf_size, si->subsequent);

	if (!si->subsequent) {
#if 0
//		lwsl_hexdump_notice(buf, buf_size);

		memcpy(si->mp4_hdr + LWS_PRE + si->mp4_header_len,
		       buf, buf_size);
		si->mp4_header_len += buf_size;
#endif
		return buf_size;
	}

	if (!msg.payload)
		lwsl_warn("%s: OOM underrun\n", __func__);
	else {
		memcpy(msg.payload + LWS_PRE, buf, buf_size);
//		lwsl_hexdump_notice(buf, buf_size);

//		lwsl_hexdump_notice(msg.payload + LWS_PRE, buf_size);

		if (!lws_ring_insert(si->ring, &msg, 1)) {
			__mirror_destroy_message(&msg);
			lwsl_notice("dropping!\n");
		}
	}

	return buf_size;
}
#endif

int
create_si(struct raw_vhd *vhd, struct lws *wsi)
{
	struct v4l2_requestbuffers req;
	struct src_inst *si = NULL;
	int fd;

	/*
	 * Create the associated si
	 */

	si = malloc(sizeof(*si));
	if (!si)
		goto bail1;

	memset(si, 0, sizeof(*si));
	si->ring = lws_ring_create(sizeof(struct msg), QUEUELEN,
				   __mirror_destroy_message);
	if (!si->ring) {
		free(si);
		goto bail1;
	}

	lws_snprintf(si->name, sizeof(si->name) - 1, "%d", vhd->owner.count);

	lws_pthread_mutex_init(&si->lock);
	lws_set_opaque_user_data(wsi, si);

	lws_dll2_add_tail(&si->list, &vhd->owner);

	lwsl_notice("Created new si '%s'\n", si->name);

	fd = (int)(intptr_t)lws_get_socket_fd(wsi);
	si->filefd = fd;
	if (init_device(si)) {
		lwsl_err("%s: device init failed\n", __func__);

		return 1;
	}

	si->capture_wsi = wsi;

	memset(&req, 0, sizeof(req));

	req.count = 8;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;

	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
		lwsl_err("%s: mmap req bufs failed\n", __func__);
		return 1;
	}

	if (req.count < 2) {
		lwsl_err("%s: Insufficient buffer memory\n", __func__);
		return 1;
	}

	si->buffers = calloc(req.count, sizeof(*si->buffers));

	if (!si->buffers) {
		lwsl_err("%s: OOM\n", __func__);
		return 1;
	}

	for (si->bcount = 0; si->bcount < req.count; si->bcount++) {
		struct v4l2_buffer buf;

		memset(&buf, 0, sizeof(buf));

		buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory      = V4L2_MEMORY_MMAP;
		buf.index       = si->bcount;

		if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
			lwsl_err("%s: querybuf failed\n", __func__);

			goto bail1;
		}

		si->buffers[si->bcount].len = buf.length;
		si->buffers[si->bcount].payload = (void *)
		      mmap(NULL /* start anywhere */,
				buf.length, PROT_READ | PROT_WRITE,
				MAP_SHARED /* recommended */, fd,
				buf.m.offset);

		if (si->buffers[si->bcount].payload == MAP_FAILED) {
			lwsl_err("%s: map failed\n", __func__);
			goto bail1;
		}
	}

	return 0;

bail1:
		free(si->buffers);
		si->buffers = NULL;

		return 1;
}

int
init_device(struct src_inst *si)
{
	struct v4l2_cropcap cropcap;
//	struct v4l2_queryctrl ctrl;
	struct v4l2_capability cap;
	struct v4l2_format fmt;
	struct v4l2_crop crop;
	struct v4l2_input inp;
	struct v4l2_streamparm parm;
	int n = 0, fd = si->filefd;

	if (xioctl(fd, VIDIOC_S_INPUT, &n) == -1)
		return 1;

	if (xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
		lwsl_err("%s: QUERYCAP failed\n", __func__);

		return 1;
	}

	lwsl_notice("cap 0x%x, bus_info %s, driver %s, card %s\n",
		(int)cap.capabilities, cap.bus_info, cap.driver, cap.card);

	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
		lwsl_err("%s: Device not capable of capture\n", __func__);

		return 1;
	}

	if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
		lwsl_err("%s: Device not capable of streaming\n", __func__);

		return 1;
	}

	/* Select video input, video standard and tune here. */

	memset(&inp, 0, sizeof(inp));
	if (xioctl(fd, VIDIOC_ENUMINPUT, &inp) != -1)
		lwsl_notice("%d %s %d\n", inp.index, inp.name, inp.type);

	memset(&cropcap, 0, sizeof(cropcap));
	cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (!xioctl(fd, VIDIOC_CROPCAP, &cropcap)) {
		crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		crop.c = cropcap.defrect; /* reset to default */

		xioctl(fd, VIDIOC_S_CROP, &crop);
	}

	/* indicate our preferred streaming rate */

	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = 30;

	xioctl(fd, VIDIOC_S_PARM, &parm);

	/* indicate our preferred frame type */

	memset(&fmt, 0, sizeof(fmt));

	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	xioctl(fd, VIDIOC_G_FMT, &fmt);

	/* if the source offers h.264 directly, take that... */
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG; // V4L2_PIX_FMT_H264;
	fmt.fmt.pix.colorspace = V4L2_COLORSPACE_DEFAULT;

	/* we indicate our preferred size, then... */

	fmt.fmt.pix.width = 640;
	fmt.fmt.pix.height = 360;

	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
		lwsl_err("%s: Can't set FMT\n", __func__);

		return 1;
	}

	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	xioctl(fd, VIDIOC_G_FMT, &fmt);

	/*...we align with the actual size the video source chose */

	si->width = fmt.fmt.pix.width;
	si->height = fmt.fmt.pix.height;

	/* get the frame rate that we ended up with too */

	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	xioctl(fd, VIDIOC_G_PARM, &parm);

	lwsl_notice("%d pixfmt: %c%c%c%c %d %d %d %d (%d/%d)\n", fmt.type,
			(fmt.fmt.pix.pixelformat) & 0xff,
			(fmt.fmt.pix.pixelformat >> 8) & 0xff,
			(fmt.fmt.pix.pixelformat >> 16) & 0xff,
			(fmt.fmt.pix.pixelformat >> 24) & 0xff,
			fmt.fmt.pix.width, fmt.fmt.pix.height,
			fmt.fmt.pix.field, fmt.fmt.pix.sizeimage,
			parm.parm.capture.timeperframe.numerator,
			parm.parm.capture.timeperframe.denominator);

	if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_H264) {

		lwsl_user("%s: initializing transcoding\n", __func__);

		/*
		 * Most webcams and HDMI capture devices can only issue MJPEG
		 * at speed, we have to transcode it to h.264 for streaming
		 */

		/*
		 * Pipeline: MJPEG decode
		 */

		si->avc_d = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
		if (!si->avc_d)
			return -1;

		si->avcpc_d = av_parser_init(si->avc_d->id);

		si->avcc_d = avcodec_alloc_context3(si->avc_d);

		avcodec_get_context_defaults3(si->avcc_d, si->avc_d);

		si->avcc_d->width		= si->width;
		si->avcc_d->height		= si->height;
		si->avcc_d->time_base		= (AVRational){ 1, 1000000 };
		si->avcc_d->framerate		= (AVRational){
				parm.parm.capture.timeperframe.denominator,
				parm.parm.capture.timeperframe.numerator
		};
	//	si->avcc_d->pix_fmt		= AV_PIX_FMT_YUV420P;

		avcodec_open2(si->avcc_d, si->avc_d, NULL);

		si->avf_d = av_frame_alloc();
		if (!si->avf_d)
			return -1;

		si->avp_d = av_packet_alloc();
		if (!si->avp_d)
		        return -1;

		/*
		 * Pipeline: H.264 encode
		 */

		si->avc_e = avcodec_find_encoder_by_name("h264_omx");
		if (!si->avc_e) {
			si->avc_e = avcodec_find_encoder(AV_CODEC_ID_H264);
			if (!si->avc_e) {
				lwsl_err("%s: we don't have an H264 enc\n", __func__);
				return -1;
			}
		}

		si->avcc_e = avcodec_alloc_context3(si->avc_e);
		if (!si->avcc_e) {
			lwsl_err("%s: unable to instantiate H264 encoder\n",
					__func__);
			return -1;
		}

		si->avp_e = av_packet_alloc();
		if (!si->avp_e)
		        return -1;

#if !defined(OWN_MOOF)
		/* avcc_e packet abstraction */

		si->avioc_e = avio_alloc_context(si->iobuf, sizeof(si->iobuf), 1,
						 si, NULL, avio_wcb, NULL);
		if (!si->avioc_e) {
			lwsl_err("%s: unable to create io ctx\n", __func__);
			return -1;
		}
#endif
		if (avformat_alloc_output_context2(&si->avfc_e, NULL,
						   "ismv", NULL) < 0) {
			lwsl_err("%s: unable to alloc output ctx\n", __func__);
			return -1;
		}
		si->avfc_e->pb = si->avioc_e;

		si->avs_e = avformat_new_stream(si->avfc_e, si->avc_e);
		if (!si->avs_e) {
			lwsl_err("%s: failed to get stream\n", __func__);
		}
		si->avs_e->time_base		= si->avcc_d->time_base;
		//si->avs_e->r_frame_rate	= si->avcc_d->framerate;
		si->avs_e->codecpar->codec_type	= AVMEDIA_TYPE_VIDEO;
		si->avs_e->codecpar->codec_id	= AV_CODEC_ID_H264;
		si->avs_e->codecpar->codec_tag	= V4L2_PIX_FMT_H264_NO_SC;
		si->avs_e->codecpar->width	= si->width;
		si->avs_e->codecpar->height	= si->height;
		si->avs_e->avg_frame_rate	= si->avcc_d->framerate;

		si->avcc_e->bit_rate		= 8000000;
		si->avcc_e->width		= si->width;
		si->avcc_e->height		= si->height;
		si->avcc_e->time_base		= si->avcc_d->time_base;
		si->avcc_e->framerate		= si->avcc_d->framerate;
		si->avcc_e->gop_size		= 3;
		si->avcc_e->max_b_frames	= 3;
		si->avcc_e->pix_fmt		= AV_PIX_FMT_YUV420P;

		si->avcc_e->ticks_per_frame	= 2;

		av_dict_set(&si->opt, "profile",  "main", 0);
		av_dict_set(&si->opt, "tune",     "zerolatency", 0);
		av_dict_set(&si->opt, "movflags", ""
						  "isml"
						  "+faststart"
						  "+frag_keyframe"
						  //"+empty_moov"
						  //"+separate_moof"
						  //"+delay_moov"
						  "+default_base_moof"
				, 0);
		av_opt_set_dict(si->avcc_e->priv_data, &si->opt);
		av_opt_set_dict(si->avcc_e, &si->opt);

		n = avcodec_open2(si->avcc_e, si->avc_e, NULL);
		if (n < 0) {
			lwsl_err("%s: can't open codec: err %d\n", __func__, n);
			return -1;
		}

		si->avf_d->format		= AV_PIX_FMT_YUV420P;
		si->avf_d->width		= si->width;
		si->avf_d->height		= si->height;

		n = av_frame_get_buffer(si->avf_d, /* alignment */ 32);
		if (n < 0) {
			lwsl_err("Could not allocate the video frame data\n");
			return -1;
		}
	}

#if 0
	/* Try the extended control API first */

	ctrl.id = V4L2_CTRL_FLAG_NEXT_CTRL;
	if (!xioctl (fd, VIDIOC_QUERYCTRL, &ctrl)) {
		do {
			fprintf(stderr, "%s\n", ctrl.name);
			//	mw->add_control(ctrl, fd, grid, gridLayout);
			ctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
		} while (!xioctl (fd, VIDIOC_QUERYCTRL, &ctrl));
	}
#endif

	return 0;
}

int
deinit_device(struct src_inst *si)
{
	int n;

	for (n = 0; n < (int)si->bcount; n++)
		munmap(si->buffers[n].payload, si->buffers[n].len);

	free(si->buffers);
	lws_dll2_remove(&si->list);

	if (si->filefd != -1)
		close(si->filefd);

	if (si->avfc_e)
		avformat_free_context(si->avfc_e);

	if (si->avcc_e)
		avcodec_free_context(&si->avcc_e);
	if (si->avp_e)
		av_packet_free(&si->avp_e);

	if (si->avcpc_d)
		av_parser_close(si->avcpc_d);

	if (si->avcc_d) {
		avcodec_close(si->avcc_d);
		avcodec_free_context(&si->avcc_d);
	}
	if (si->avf_d)
		av_frame_free(&si->avf_d);
	if (si->avp_d)
		av_packet_free(&si->avp_d);

//		if (si->swsc)
//			sws_freeContext(si->swsc);

	if (si->avs_e)
		av_packet_free(&si->avp_d);

	lws_ring_destroy(si->ring);
	lws_pthread_mutex_destroy(&si->lock);
	free(si);

	return 0;
}

int
start_capturing(struct src_inst *si)
{
	unsigned int i;
	enum v4l2_buf_type type;

	for (i = 0; i < si->bcount; ++i) {
		struct v4l2_buffer buf;

		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;

		if (xioctl(si->filefd, VIDIOC_QBUF, &buf) < 0) {
			lwsl_warn("%s: unable to start cap %d\n", __func__,
					si->filefd);
			return -1;
		}
	}
	lwsl_notice("%s: stream on\n", __func__);

	lws_rx_flow_control(si->capture_wsi, 1);

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	return xioctl(si->filefd, VIDIOC_STREAMON, &type) == -1;
}

int
stop_capturing(struct src_inst *si)
{
	enum v4l2_buf_type type;
	unsigned int i;
	int ret;

	lws_rx_flow_control(si->capture_wsi, 0);

	lwsl_notice("%s\n", __func__);
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	for (i = 0; i < si->bcount; ++i) {
		struct v4l2_buffer buf;

		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;

		xioctl(si->filefd, VIDIOC_DQBUF, &buf);
	}

	ret = xioctl(si->filefd, VIDIOC_STREAMOFF, &type);
	if (ret < 0)
		lwsl_err("%s: failed to stop stream\n", __func__);

	return ret == -1;
}

int
lws_v4l2_dq(struct src_inst *si, struct v4l2_buffer *buf)
{
	memset(buf, 0, sizeof(*buf));

	buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf->memory = V4L2_MEMORY_MMAP;

	if (xioctl(si->filefd, VIDIOC_DQBUF, buf) < 0) {
		if (errno == EAGAIN)
			return 0;

		lwsl_warn("%s: DQBUF ioctl fail: %d\n", __func__, errno);

		return 1;
	}

	assert(buf->index < si->bcount);

	return 0;
}

int
lws_v4l2_q(struct src_inst *si, struct v4l2_buffer *buf)
{
	if (xioctl(si->filefd, VIDIOC_QBUF, buf) < 0) {
		lwsl_err("%s: QBUF failed\n", __func__);

		return -1;
	}

	return 0;
}
