/*
 * lws-minimal-ws-server-v4l2
 *
 * Written in 2010-2021 by Andy Green <andy@warmcat.com>
 *
 * This file is made available under the Creative Commons CC0 1.0
 * Universal Public Domain Dedication.
 *
 * This serves a few JPEGs a second from a v4l2 device like a webcam.
 * Visit http://127.0.0.1:7681 to see it
 */

#include <libwebsockets.h>

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

static const char *vdev = "/dev/video0";

/* one of these created for each message */

struct msg {
	void *payload; /* is malloc'd */
	size_t len;
};

/* one of these is created for each client connecting to us */

struct pss {
	lws_dll2_t		list;

	uint8_t			last[500 * 1024];

	struct lws		*wsi;
	int			last_sent; /* the last message number we sent */
	int			fready;
	size_t			last_len;
	size_t			inside;
};

struct raw_vhd {

	struct lws_context	*context;
	struct lws_vhost	*vhost;
	const struct lws_protocols *protocol;

	lws_dll2_owner_t	owner;	/* pss list */

	struct msg amsg; /* the one pending message... */
	int current; /* the current message number we are caching */

	struct lws		*capture_wsi;

	int			filefd;
	struct msg		*buffers;
	unsigned int		bcount;
	int			out_buf;

	int			frame;
};

static int
xioctl(int fh, unsigned long request, void *arg)
{
	int r;

	do {
		r = ioctl(fh, request, arg);
	} while (-1 == r && EINTR == errno);

	return r;
}

static int
init_device(int fd)
{
	struct v4l2_cropcap cropcap;
	struct v4l2_queryctrl ctrl;
	struct v4l2_capability cap;
	struct v4l2_format fmt;
	struct v4l2_crop crop;
	struct v4l2_input inp;
	int n = 0;

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

	memset(&fmt, 0, sizeof(fmt));

	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width       = 1280;
	fmt.fmt.pix.height      = 720;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG; // V4L2_PIX_FMT_H264;
	fmt.fmt.pix.field       = V4L2_FIELD_ANY;

	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
		lwsl_err("%s: Can't set FMT\n", __func__);

		return 1;
	}

	/* Preserve original settings as set by v4l2-ctl for example */
	//               if (-1 == xioctl(fd, VIDIOC_G_FMT, &fmt))
	//                     errno_exit("VIDIOC_G_FMT");

	lwsl_notice("%d %d %d %d %d\n", fmt.type, fmt.fmt.pix.width,
		fmt.fmt.pix.height, fmt.fmt.pix.field, fmt.fmt.pix.sizeimage);
#if 0
	/* Buggy driver paranoia. */
	min = fmt.fmt.pix.width * 2;
	if (fmt.fmt.pix.bytesperline < min)
		fmt.fmt.pix.bytesperline = min;
	min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
	if (fmt.fmt.pix.sizeimage < min)
		fmt.fmt.pix.sizeimage = min;
#endif

	/* Try the extended control API first */

	ctrl.id = V4L2_CTRL_FLAG_NEXT_CTRL;
	if (!xioctl (fd, VIDIOC_QUERYCTRL, &ctrl)) {
		do {
			fprintf(stderr, "%s\n", ctrl.name);
			//		mw->add_control(ctrl, fd, grid, gridLayout);
			ctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
		} while (!xioctl (fd, VIDIOC_QUERYCTRL, &ctrl));
	}

	return 0;
}

static int
stop_capturing(struct raw_vhd *vhd)
{
	enum v4l2_buf_type type;

	lws_rx_flow_control(vhd->capture_wsi, 0);

	lwsl_notice("%s\n", __func__);
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	return xioctl(vhd->filefd, VIDIOC_STREAMOFF, &type) == -1;
}

static int
start_capturing(struct raw_vhd *vhd)
{
	unsigned int i;
	enum v4l2_buf_type type;

	for (i = 0; i < vhd->bcount; ++i) {
		struct v4l2_buffer buf;

		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;

		if (xioctl(vhd->filefd, VIDIOC_QBUF, &buf) < 0) {
			lwsl_warn("%s: unable to start cap\n", __func__);
			return -1;
		}
	}
	lwsl_notice("%s: stream on\n", __func__);

	lws_rx_flow_control(vhd->capture_wsi, 1);

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	return xioctl(vhd->filefd, VIDIOC_STREAMON, &type) == -1;
}

/* v4l2 frame capture */

static int
callback_v4l2(struct lws *wsi, enum lws_callback_reasons reason, void *user,
	      void *in, size_t len)
{
	struct raw_vhd *vhd = (struct raw_vhd *)lws_protocol_vh_priv_get(
			lws_get_vhost(wsi), lws_get_protocol(wsi));
	struct pss *pss = (struct pss *)user;
	struct v4l2_requestbuffers req;
	lws_sock_file_fd_type u;
	struct v4l2_buffer buf;
	//uint8_t buf[1024];
	size_t chunk;
	int n;

	switch (reason) {
	case LWS_CALLBACK_PROTOCOL_INIT:
		vhd = lws_protocol_vh_priv_zalloc(lws_get_vhost(wsi),
						  lws_get_protocol(wsi),
						  sizeof(struct raw_vhd));
		vhd->context = lws_get_context(wsi);
		vhd->protocol = lws_get_protocol(wsi);
		vhd->vhost = lws_get_vhost(wsi);

		vhd->filefd =  open(vdev, O_RDWR | O_NONBLOCK, 0);
		if (vhd->filefd == -1) {
			lwsl_err("Unable to open v4l2 device %s\n", vdev);

			return 1;
		}
		u.filefd = (lws_filefd_type)(long long)vhd->filefd;
		if (!lws_adopt_descriptor_vhost(lws_get_vhost(wsi),
				LWS_ADOPT_RAW_FILE_DESC, u,
				"lws-v4l2", NULL)) {
			lwsl_err("%s: Failed to adopt capture fd\n", __func__);

			goto bail;
		}

		lwsl_notice("%s: leaving PROTOCOL_INIT OK\n", __func__);

		return 0;

bail:
		close(vhd->filefd);
		vhd->filefd = -1;

		return -1;

	case LWS_CALLBACK_PROTOCOL_DESTROY:

		if (vhd && vhd->filefd != -1)
			close(vhd->filefd);
		break;

		/*
		 * callbacks related to capture file (no pss)
		 */

	case LWS_CALLBACK_RAW_ADOPT_FILE:
		lwsl_notice("LWS_CALLBACK_RAW_ADOPT_FILE\n");

		if (init_device(vhd->filefd)) {
			lwsl_err("%s: device init failed\n", __func__);

			return 1;
		}

		vhd->capture_wsi = wsi;

		memset(&req, 0, sizeof(req));

		req.count = 4;
		req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		req.memory = V4L2_MEMORY_MMAP;

		if (xioctl(vhd->filefd, VIDIOC_REQBUFS, &req) < 0) {
			lwsl_err("%s: mmap req bufs failed\n", __func__);
			return 1;
		}

		if (req.count < 2) {
			lwsl_err("%s: Insufficient buffer memory\n", __func__);
			return 1;
		}

		vhd->buffers = calloc(req.count, sizeof(*vhd->buffers));

		if (!vhd->buffers) {
			lwsl_err("%s: OOM\n", __func__);
			return 1;
		}

		for (vhd->bcount = 0; vhd->bcount < req.count; vhd->bcount++) {
			struct v4l2_buffer buf;

			memset(&buf, 0, sizeof(buf));

			buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			buf.memory      = V4L2_MEMORY_MMAP;
			buf.index       = vhd->bcount;

			if (xioctl(vhd->filefd, VIDIOC_QUERYBUF, &buf) < 0) {
				lwsl_err("%s: querybuf failed\n", __func__);

				goto bail1;
			}

			vhd->buffers[vhd->bcount].len = buf.length;
			vhd->buffers[vhd->bcount].payload = (void *)
			      mmap(NULL /* start anywhere */,
					buf.length,
					PROT_READ | PROT_WRITE /* required */,
					MAP_SHARED /* recommended */,
					vhd->filefd, buf.m.offset);

			if (vhd->buffers[vhd->bcount].payload == MAP_FAILED) {
				lwsl_err("%s: map failed\n", __func__);
				goto bail1;
			}
		}

		lws_rx_flow_control(wsi, 0);

		lwsl_notice("%s: adopt completed ok\n", __func__);
		break;

bail1:
		free(vhd->buffers);
		vhd->buffers = NULL;

		return 1;

	case LWS_CALLBACK_RAW_CLOSE_FILE:
		lwsl_notice("LWS_CALLBACK_RAW_CLOSE_FILE\n");

		if (!vhd)
			break;

		for (n = 0; n < (int)vhd->bcount; n++)
			munmap(vhd->buffers[n].payload, vhd->buffers[n].len);

		free(vhd->buffers);
		break;

	case LWS_CALLBACK_RAW_RX_FILE:

		memset(&buf, 0, sizeof(buf));

		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;

		if (xioctl(vhd->filefd, VIDIOC_DQBUF, &buf) < 0) {
			if (errno == EAGAIN)
				return 0;
			lwsl_warn("%s: DQBUF ioctl fail: %d\n",
					__func__, errno);
		}

		assert(buf.index < vhd->bcount);

		vhd->frame++;

		lws_start_foreach_dll(struct lws_dll2 *, d, vhd->owner.head) {
			struct pss *ps = lws_container_of(d, struct pss, list);

			if (!ps->inside) {
				ps->last_len = buf.bytesused;
				if (ps->last_len > sizeof(ps->last) - LWS_PRE)
					ps->last_len = sizeof(ps->last) - LWS_PRE;

				memcpy(ps->last + LWS_PRE,
				       vhd->buffers[buf.index].payload,
				       ps->last_len);

				ps->fready = vhd->frame;
			}

			lws_callback_on_writable(ps->wsi);
		} lws_end_foreach_dll(d);

		if (xioctl(vhd->filefd, VIDIOC_QBUF, &buf) < 0) {
			lwsl_err("%s: QBUF failed\n", __func__);
			return -1;
		}

		break;

	case LWS_CALLBACK_RAW_WRITEABLE_FILE:
		lwsl_notice("LWS_CALLBACK_RAW_WRITEABLE_FILE\n");
		/*
		 * you can call lws_callback_on_writable() on a raw file wsi as
		 * usual, and then write directly into the raw filefd here.
		 */
		break;

	/*
	 * callbacks related to ws serving (pss valid)
	 */

	case LWS_CALLBACK_ESTABLISHED:
		pss->wsi = wsi;

		/* add ourselves to the list of live pss held in the vhd */
		lws_dll2_add_tail(&pss->list, &vhd->owner);
		lws_callback_on_writable(wsi);

		if (vhd->owner.count == 1)
			start_capturing(vhd);
		break;

	case LWS_CALLBACK_CLOSED:
		/* remove our closing pss from the list of live pss */
		lws_dll2_remove(&pss->list);

		if (vhd->owner.count == 0)
			stop_capturing(vhd);
		break;

	case LWS_CALLBACK_SERVER_WRITEABLE:

		if (!pss->inside && pss->last_sent == pss->fready)
			break;

		if (!pss->inside)
			pss->last_sent = pss->fready;

		/*
		 * To work with ws-over-h2, we must restrict the size of
		 * the encapsulated frames we are sending.  It's a good idea
		 * to handle h1 ws the same way in user code.
		 */

		chunk = pss->last_len - pss->inside;
		if (chunk > 1400)
			chunk = 1400;

		/* notice we allowed for LWS_PRE in the payload already */
		n = lws_write(wsi, pss->last + LWS_PRE + pss->inside,
			      chunk, lws_write_ws_flags(LWS_WRITE_BINARY,
			      !pss->inside, pss->inside + chunk == pss->last_len));
		if (n < (int)chunk) {
			lwsl_err("ERROR %d writing to ws\n", n);
			return -1;
		}

		pss->inside += chunk;
		if (pss->inside == pss->last_len)
			/* ready for new one */
			pss->inside = 0;
		else
			lws_callback_on_writable(wsi);

		break;

	case LWS_CALLBACK_RECEIVE:
		break;

	default:
		break;
	}

	return 0;
}

static struct lws_protocols protocols[] = {
	{ "http", lws_callback_http_dummy, 0, 0 },
	{ "lws-v4l2", callback_v4l2,  sizeof(struct pss), 1300, 1300, NULL, 0 },
	{ NULL, NULL, 0, 0 } /* terminator */
};

static const lws_retry_bo_t retry = {
	.secs_since_valid_ping = 30,
	.secs_since_valid_hangup = 40,
};

static int interrupted;

static const struct lws_http_mount mount = {
	/* .mount_next */		NULL,		/* linked-list "next" */
	/* .mountpoint */		"/",		/* mountpoint URL */
	/* .origin */			"./mount-origin",  /* serve from dir */
	/* .def */			"index.html",	/* default filename */
	/* .protocol */			NULL,
	/* .cgienv */			NULL,
	/* .extra_mimetypes */		NULL,
	/* .interpret */		NULL,
	/* .cgi_timeout */		0,
	/* .cache_max_age */		0,
	/* .auth_mask */		0,
	/* .cache_reusable */		0,
	/* .cache_revalidate */		0,
	/* .cache_intermediaries */	0,
	/* .origin_protocol */		LWSMPRO_FILE,	/* files in a dir */
	/* .mountpoint_len */		1,		/* char count */
	/* .basic_auth_login_file */	NULL,
};

void sigint_handler(int sig)
{
	interrupted = 1;
}

static const
struct lws_protocol_vhost_options pvo_hsbph[] = {{
	NULL, NULL, "referrer-policy:", "no-referrer"
}, {
	&pvo_hsbph[0], NULL, "x-frame-options:", "deny"
}, {
	&pvo_hsbph[1], NULL, "x-xss-protection:", "1; mode=block"
}, {
	&pvo_hsbph[2], NULL, "x-content-type-options:", "nosniff"
}, {
	&pvo_hsbph[3], NULL, "content-security-policy:",
	"default-src 'none'; img-src 'self' blob: ; "
		"script-src 'self'; font-src 'self'; "
		"style-src 'self'; connect-src 'self' ws: wss:; "
		"frame-ancestors 'none'; base-uri 'none';"
		"form-action 'self';"
}};

int main(int argc, const char **argv)
{
	struct lws_context_creation_info info;
	struct lws_context *context;
	const char *p;
	int n = 0, logs = LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE
			/* for LLL_ verbosity above NOTICE to be built into lws,
			 * lws must have been configured and built with
			 * -DCMAKE_BUILD_TYPE=DEBUG instead of =RELEASE */
			/* | LLL_INFO */ /* | LLL_PARSER */ /* | LLL_HEADER */
			/* | LLL_EXT */ /* | LLL_CLIENT */ /* | LLL_LATENCY */
			/* | LLL_DEBUG */;

	signal(SIGINT, sigint_handler);

	if ((p = lws_cmdline_option(argc, argv, "-d")))
		logs = atoi(p);

	if ((p = lws_cmdline_option(argc, argv, "-v")))
		vdev = p;

	lws_set_log_level(logs, NULL);
	lwsl_user("LWS minimal ws server V4L2 | visit http://localhost:7681\n");

	memset(&info, 0, sizeof info); /* otherwise uninitialized garbage */
	info.port = 7681;
	info.mounts = &mount;
	info.protocols = protocols;
	info.vhost_name = "localhost";
	info.options = 0;
	info.headers = pvo_hsbph;

#if defined(LWS_WITH_TLS)
	if (lws_cmdline_option(argc, argv, "-s")) {
		lwsl_user("Server using TLS\n");
		info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
		info.ssl_cert_filepath = "localhost-100y.cert";
		info.ssl_private_key_filepath = "localhost-100y.key";
	}
#endif

	if (lws_cmdline_option(argc, argv, "-h"))
		info.options |= LWS_SERVER_OPTION_VHOST_UPG_STRICT_HOST_CHECK;

	if (lws_cmdline_option(argc, argv, "-v"))
		info.retry_and_idle_policy = &retry;

	context = lws_create_context(&info);
	if (!context) {
		lwsl_err("lws init failed\n");
		return 1;
	}

	while (n >= 0 && !interrupted)
		n = lws_service(context, 0);

	lws_context_destroy(context);

	return 0;
}
