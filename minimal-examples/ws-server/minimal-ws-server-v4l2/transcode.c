/*
 * lws-minimal-ws-server-v4l2
 *
 * Written in 2010-2021 by Andy Green <andy@warmcat.com>
 *
 * This file is made available under the Creative Commons CC0 1.0
 * Universal Public Domain Dedication.
 */

#include <libwebsockets.h>

#include "private.h"

#include <assert.h>

int
transcode(struct src_inst *si)
{
#if defined(OWN_MOOF)
	struct msg msg;
#endif
	int n;

	n = avcodec_send_packet(si->avcc_d, si->avp_d);

	if (n < 0) {
		lwsl_warn("%s: send_pkt failed\n", __func__);

		return 0;
	}

	while (n >= 0) {
		n = avcodec_receive_frame(si->avcc_d, si->avf_d);
		if (n == AVERROR(EAGAIN) ||
		    n == AVERROR_EOF)
			return 0;
		if (n < 0) {
			lwsl_warn("%s: dec failed\n", __func__);

			return 0;
		}

		/*
		 * si->avf_d holds a decoded bitmap... let's give it to the
		 * h.264 encoder we have prepared earlier
		 */

		si->avf_d->pts = av_rescale_q((si->dec_frame++) *
						si->avcc_e->ticks_per_frame,
					      si->avcc_d->time_base,
					      (AVRational) { 1, 10000000 });

		n = avcodec_send_frame(si->avcc_e, si->avf_d);

		if (n < 0) {
			lwsl_err("%s: send_frame failed: %s\n", __func__,
				 av_err2str(n));

			continue;
		}

		while (n >= 0) {
#if defined(OWN_MOOF)
			size_t trim = 0;
#endif
			uint8_t *od;
			size_t os;

			n = avcodec_receive_packet(si->avcc_e, si->avp_e);

			if (n == AVERROR(EAGAIN) ||
			    n == AVERROR_EOF)
				continue;

			if (n < 0) {
				lwsl_warn("%s: receive_pkt failed\n", __func__);
				continue;
			}

			/*
			 * The encoder produces an annex-b TS type header.
			 * Let's parse it to get the SPS and PPS for later
			 */

			if (!si->avcc_header_len)
				lwsl_hexdump_warn(si->avp_e->data, si->avp_e->size);

			od = si->avp_e->data;
			os = si->avp_e->size;

			if (si->avp_e->size) {
#if defined(OWN_MOOF)
				trim =
#endif
					annex_b_scan(si, si->avp_e->data, si->avp_e->size);

//				lwsl_notice("%s: trim 0x%x\n", __func__, (unsigned int)trim);
//				lwsl_hexdump_notice(si->avp_e->data, trim);
				//si->avp_e->data += trim;
				//si->avp_e->size -= trim;

				if (!si->avcc_header_len) {
					assert(si->pre_len + trim < sizeof(si->pre));
					memcpy(si->pre + si->pre_len, si->avp_e->data, trim);
					si->pre_len += trim;
				}

				if (!si->annex_b_len) {
					assert(si->annex_b_len + trim < sizeof(si->annex_b));
					memcpy(si->annex_b + si->annex_b_len, si->avp_e->data, trim);
					si->annex_b_len = trim;
				}
#if defined(OWN_MOOF)
				else
					si->subsequent = 1;
#endif

				//trim = 0;
			}

#if defined(OWN_MOOF)
			msg.len = si->avp_e->size - trim;
			msg.payload = malloc(LWS_PRE + msg.len);
			if (!msg.payload) {
				lwsl_warn("%s: OOM underrun\n", __func__);
				av_packet_unref(si->avp_e);
				return 1;
			}

			memcpy(msg.payload + LWS_PRE,
				       si->avp_e->data + trim, msg.len);

			if (!lws_ring_insert(si->ring, &msg, 1)) {
				__mirror_destroy_message(&msg);
				lwsl_notice("dropping!\n");
			}

#else

			if (si->avp_e->size &&
			    /* don't start emitting until we had the annex B avcc decode */
			    si->avcc_header_len) {

				if (!si->subsequent) {
					if (avformat_write_header(si->avfc_e, NULL))
						lwsl_err("%s: couldn't write header\n", __func__);
					si->subsequent = 1;
				}

				if (si->pre_len) {

					/*
					 * If we're using libav and there was
					 * a packet without an IDR, we have to
					 * make new packet bodies for the IDRs
					 * with the pre NALs prepended
					 */

					memcpy(si->chonk, si->pre, si->pre_len);
					memcpy(si->chonk + si->pre_len,
					       si->avp_e->data, si->avp_e->size);
					si->avp_e->data = si->chonk;
					si->avp_e->size += si->pre_len;
				}

				av_packet_rescale_ts(si->avp_e,
						     si->avcc_d->time_base,
						     si->avcc_e->time_base);

				si->ntt = (uint64_t)si->avp_e->dts;

				if (av_interleaved_write_frame(si->avfc_e, si->avp_e) < 0)
					lwsl_err("%s: mux write fail\n", __func__);
			}
#endif

			si->avp_e->data = od;
			si->avp_e->size = os;

			av_packet_unref(si->avp_e);
		}
	}

	return 0;
}
