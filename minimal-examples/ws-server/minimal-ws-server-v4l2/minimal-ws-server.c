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

static const char *vdev = "/dev/video0";

#include "private.h"

// uncomment this to copy the stream to a file "/tmp/str.mp4"
#define DUMP_TO_FILE

static void
pss_to(struct lws_sorted_usec_list *sul)
{
	struct pss *pss = lws_container_of(sul, struct pss, sul);

	lwsl_warn("%s: timedout\n", __func__);

	lws_wsi_close(pss->wsi, LWS_TO_KILL_ASYNC);
}


static int
__mirror_update_worst_tail(struct src_inst *si)
{
	uint32_t wai, worst = 0, worst_tail = 0, oldest;
	struct pss *worst_pss = NULL;
	struct raw_vhd *vhd = lws_container_of(si->list.owner,
					       struct raw_vhd, owner);

	oldest = lws_ring_get_oldest_tail(si->ring);

	lws_start_foreach_dll(struct lws_dll2 *, d, si->owner.head) {
		struct pss *ps = lws_container_of(d, struct pss, list);

		wai = (uint32_t)lws_ring_get_count_waiting_elements(si->ring,
								    &ps->tail);
		if (wai >= worst) {
			worst = wai;
			worst_tail = ps->tail;
			worst_pss = ps;
		}
	} lws_end_foreach_dll(d);

	if (!worst_pss)
		return 0;

	lws_ring_update_oldest_tail(si->ring, worst_tail);
	if (oldest == lws_ring_get_oldest_tail(si->ring))
		return 0;

	/* if nothing in queue, no timeout needed */
	if (!worst)
		return 1;

	/*
	 * The guy(s) with the oldest tail block the ringbuffer from recycling
	 * the FIFO entries he has not read yet.  Don't allow those guys to
	 * block the FIFO operation for very long.
	 */
	lws_start_foreach_dll(struct lws_dll2 *, d, si->owner.head) {
		struct pss *ps = lws_container_of(d, struct pss, list);

		if (ps->tail == worst_tail)
			/*
			 * Our policy is if you are the slowest connection,
			 * you had better take something to help with that
			 * within 3s, or we will hang up on you to stop you
			 * blocking the FIFO for everyone else.
			 */
			lws_sul_schedule(vhd->context, 0, &ps->sul,
					 pss_to, 3 * LWS_US_PER_SEC);
	} lws_end_foreach_dll(d);

	return 1;
}

void
__mirror_destroy_message(void *_msg)
{
	struct msg *pmsg = _msg;

	free(pmsg->payload);
	pmsg->payload = NULL;
	pmsg->len = 0;
}

#if defined(DUMP_TO_FILE)
int qfd = -1;
#endif


/* v4l2 frame capture */

static int
callback_v4l2(struct lws *wsi, enum lws_callback_reasons reason, void *user,
	      void *in, size_t len)
{
	struct raw_vhd *vhd = (struct raw_vhd *)lws_protocol_vh_priv_get(
			lws_get_vhost(wsi), lws_get_protocol(wsi));
	struct pss *pss = (struct pss *)user;
	struct src_inst *si = NULL;
	lws_sock_file_fd_type u;
	struct v4l2_buffer buf;
	struct msg msg, *pmsg;
	char name[300], t[1400 + LWS_PRE];
	const char *pn;
	size_t chunk;
	int n, fd;

	switch (reason) {
	case LWS_CALLBACK_PROTOCOL_INIT:
		vhd = lws_protocol_vh_priv_zalloc(lws_get_vhost(wsi),
						  lws_get_protocol(wsi),
						  sizeof(struct raw_vhd));
		vhd->context = lws_get_context(wsi);
		vhd->protocol = lws_get_protocol(wsi);
		vhd->vhost = lws_get_vhost(wsi);

		lws_pthread_mutex_init(&vhd->lock);

		fd = open(vdev, O_RDWR | O_NONBLOCK, 0);
		if (fd == -1) {
			lwsl_err("Unable to open v4l2 device %s\n", vdev);

			return 1;
		}
		u.filefd = (lws_filefd_type)(long long)fd;
		if (!lws_adopt_descriptor_vhost(lws_get_vhost(wsi),
				LWS_ADOPT_RAW_FILE_DESC, u,
				"lws-v4l2", NULL)) {
			lwsl_err("%s: Failed to adopt capture fd\n", __func__);

			goto bail;
		}

		lwsl_notice("%s: leaving PROTOCOL_INIT OK\n", __func__);

		return 0;

bail:
		close(fd);

		return -1;

		/*
		 * callbacks related to capture file (no pss)
		 */

	case LWS_CALLBACK_RAW_ADOPT_FILE:
		lwsl_notice("LWS_CALLBACK_RAW_ADOPT_FILE\n");

		if (create_si(vhd, wsi))
			return -1;

		lws_rx_flow_control(wsi, 0);

		lwsl_notice("%s: adopt completed ok\n", __func__);
		break;

	case LWS_CALLBACK_RAW_CLOSE_FILE:
		lwsl_notice("LWS_CALLBACK_RAW_CLOSE_FILE\n");

		if (!vhd)
			break;

		lws_pthread_mutex_lock(&vhd->lock); /* vhost lock { */
		si = (struct src_inst *)lws_get_opaque_user_data(wsi);

		deinit_device(si);

		lws_pthread_mutex_unlock(&vhd->lock); /* } vhost lock */

		break;

	case LWS_CALLBACK_RAW_RX_FILE:

		if (!vhd)
			break;

		si = (struct src_inst *)lws_get_opaque_user_data(wsi);

		if (lws_v4l2_dq(si, &buf))
			return -1;

		if (si->avcpc_d) {
			/*
			 * Transcoding needed
			 *
			 * decode MJPEG frame to bitmap
			 */

			n = av_parser_parse2(si->avcpc_d, si->avcc_d,
					     &si->avp_d->data,
					     &si->avp_d->size,
					     si->buffers[buf.index].payload,
					     buf.bytesused, AV_NOPTS_VALUE,
					     AV_NOPTS_VALUE, 0);
			if (n < 0) {
				lwsl_warn("%s: Parsing failed\n", __func__);

				return 0;
			}

			if (si->avp_d->size)
				/* encode the decoded bitmap into the
				 * output stream */
				transcode(si);

		} else {
			msg.payload = malloc(LWS_PRE + buf.bytesused);
			msg.len = buf.bytesused;

			if (!msg.payload)
				lwsl_warn("%s: OOM underrun\n", __func__);
			else {

				memcpy(msg.payload + LWS_PRE,
				       si->buffers[buf.index].payload,
				       msg.len);

				if (!lws_ring_insert(si->ring, &msg, 1)) {
					__mirror_destroy_message(&msg);
					lwsl_notice("dropping!\n");
				}
			}

			si->frame++;
		}

		lws_pthread_mutex_lock(&si->lock); /* si lock { */

		lws_start_foreach_dll(struct lws_dll2 *, d, si->owner.head) {
			struct pss *ps = lws_container_of(d, struct pss, list);

			lws_callback_on_writable(ps->wsi);
		} lws_end_foreach_dll(d);

		lws_pthread_mutex_unlock(&si->lock); /* } si lock */

		if (lws_v4l2_q(si, &buf))
			return -1;

		break;

	/*
	 * callbacks related to ws serving (pss valid)
	 */

	case LWS_CALLBACK_ESTABLISHED:

		if (!vhd->owner.head)
			return 0;

		pss->wsi = wsi;

#if defined(DUMP_TO_FILE)
		if (qfd != -1)
			close(qfd);
		qfd = open("/tmp/str.mp4", O_CREAT|O_TRUNC|O_WRONLY, 0600);
#endif

		name[0] = '\0';
		pn = "0";
		if (!lws_get_urlarg_by_name(wsi, "src", name, sizeof(name) - 1))
			lwsl_debug("get urlarg failed\n");

		/* is there a source instance of this name already extant? */

		lws_pthread_mutex_lock(&vhd->lock); /* vhost lock { */

		si = NULL;
		lws_start_foreach_dll(struct lws_dll2 *, d, vhd->owner.head) {
			struct src_inst *mi1 = lws_container_of(d,
						struct src_inst, list);

			if (!strcmp(pn, mi1->name)) {
				/* yes... we will join it */
				si = mi1;
				break;
			}
		} lws_end_foreach_dll(d);

		/* no existing source instance for name, join first si */
		if (!si)
			si = lws_container_of(vhd->owner.head,
					      struct src_inst,
					      owner);

		/* add ourselves to the list of live pss held in the si */
		lws_dll2_add_tail(&pss->list, &si->owner);

		pss->tail = lws_ring_get_oldest_tail(si->ring);

		lws_pthread_mutex_unlock(&vhd->lock); /* } vh lock */
		lws_callback_on_writable(wsi);

		if (si->owner.count == 1)
			start_capturing(si);
		break;

	case LWS_CALLBACK_CLOSED:

		if (!pss->list.owner)
			break;

		lws_sul_cancel(&pss->sul);

		si = lws_container_of(pss->list.owner,
				      struct src_inst, owner);

		/* remove our closing pss from the list of live pss */
		lws_dll2_remove(&pss->list);

		if (si->owner.count == 0)
			stop_capturing(si);

#if defined(DUMP_TO_FILE)
		if (qfd != -1)
			close(qfd);
#endif
		break;

	case LWS_CALLBACK_SERVER_WRITEABLE:

		if (!pss->list.owner)
			break;

		si = lws_container_of(pss->list.owner, struct src_inst, owner);

		if (!pss->inserted_mp4) {
			if (!si->mp4_header_len || !si->subsequent)
				return 0;

#if defined(DUMP_TO_FILE)
			write(qfd, si->mp4_hdr + LWS_PRE, si->mp4_header_len);
#endif

			lwsl_err("%s: writing %d\n", __func__, (int)si->mp4_header_len);
			lwsl_hexdump_notice(si->mp4_hdr + LWS_PRE, si->mp4_header_len);
			n = lws_write(wsi, si->mp4_hdr + LWS_PRE,
				      si->mp4_header_len, LWS_WRITE_BINARY);
			if (n < (int)si->mp4_header_len) {
				lwsl_err("ERROR %d writing to ws\n", n);

				return -1;
			}
			pss->inserted_mp4 = 1;
			lws_callback_on_writable(wsi);

			return 0;
		}

		lws_pthread_mutex_lock(&si->lock); /* instance lock { */
		do {
			char first = 0, doing_moof = 0;
			uint8_t *place;

			pmsg = (struct msg *)lws_ring_get_element(si->ring, &pss->tail);
			if (!pmsg)
				continue;

			/*
			 * Chrome insists to see moof->traf->tfdt boxes,
			 * these seem to be needed to be added later
			 * so they are relative to the time the peer
			 * joined the stream
			 */

			/*
			 * To work with ws-over-h2, we must restrict the size of
			 * the encapsulated frames we are sending.  It's a good
			 * idea to handle h1 ws the same way in user code.
			 */
#ifdef OWN_MOOF
			if (!pss->did_moof) {
				int xlen = !si->annex_b_len || pss->sent_initial_moof ? 5 : si->annex_b_len;

				place = (uint8_t *)t + LWS_PRE;
				chunk = moof_prepare(si, place, pss,
						     sizeof(t) - LWS_PRE,
						     pmsg->len + xlen,
						     0x51615);
				first = 1;
				pss->did_moof = 1;
				doing_moof = 1;
#if 1
				if (si->annex_b_len && !pss->sent_initial_moof) {
					/*
					 * Let's give the whole set of
					 * NALs the first time we join the
					 * stream
					 */

					lwsl_hexdump_err(si->annex_b, si->annex_b_len);

					memcpy(place + chunk, si->annex_b, si->annex_b_len);
					chunk += si->annex_b_len;
				} else {
					/*
					 * For the rest of the fragments, we
					 * just need the IDR NAL
					 */
					place[chunk++] = 0x00;
					place[chunk++] = 0x00;
					place[chunk++] = 0x00;
					place[chunk++] = 0x01;
					place[chunk++] = 0x65;
				}
#endif
				pss->sent_initial_moof = 1;

//				lwsl_notice("%s: doing moof\n", __func__);
//				lwsl_hexdump_notice(place, chunk);

			} else
#endif
			{
				chunk = pmsg->len - pss->inside;
				place = pmsg->payload + LWS_PRE + pss->inside;
#ifdef OWN_MOOF
				first = 0;
#else
				first = !pss->inside;
#endif
			}

			if (chunk > 1400)
				chunk = 1400;

			if (place != (uint8_t *)t + LWS_PRE)
				memcpy(t + LWS_PRE, place, chunk);

			n = lws_write_ws_flags(LWS_WRITE_BINARY, first,
					       !doing_moof &&
					       pss->inside + chunk == pmsg->len);
#if defined(DUMP_TO_FILE)
			write(qfd, t + LWS_PRE, chunk);
#endif

			n = lws_write(wsi, (uint8_t *)(t + LWS_PRE), chunk, n);
			if (n < (int)chunk) {
				lwsl_err("ERROR %d writing to ws\n", n);
				lws_pthread_mutex_unlock(&si->lock); /* } instance lock */

				return -1;
			}

			if (!doing_moof)
				pss->inside += chunk;

			if (pss->inside == pmsg->len) {
				/* ready for new one */
				pss->inside = 0;
				pss->did_moof = 0;
//				lwsl_notice("%s: sent %d\n", __func__, (int)pmsg->len);
				lws_ring_consume(si->ring, &pss->tail, NULL, 1);
				__mirror_update_worst_tail(si);
				lws_sul_cancel(&pss->sul);
			}

		} while (pmsg && !lws_send_pipe_choked(wsi));

		if (pss->inside ||
		    lws_ring_get_count_waiting_elements(si->ring, &pss->tail))
			lws_callback_on_writable(wsi);

		lws_pthread_mutex_unlock(&si->lock); /* } instance lock */
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
	{ "lws-v4l2", callback_v4l2,  sizeof(struct pss), 1800, 1800, NULL, 0 },
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
	int n = 0, logs = LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE;

	signal(SIGINT, sigint_handler);

	if ((p = lws_cmdline_option(argc, argv, "-d")))
		logs = atoi(p);

	if ((p = lws_cmdline_option(argc, argv, "-v")))
		vdev = p;

	lws_set_log_level(logs, NULL);
	lwsl_user("LWS minimal ws server V4L2 | visit http://localhost:7681\n");

	memset(&info, 0, sizeof info); /* otherwise uninitialized garbage */
	info.port		= 7681;
	info.mounts		= &mount;
	info.protocols		= protocols;
	info.vhost_name		= "localhost";
	info.options		= 0;
	info.headers		= pvo_hsbph;

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
