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

enum {
	IDLE,
	SEEN_Z1,
	SEEN_Z2,
	HIT,

	NAL_IDR		= 5,
	NAL_SPS		= 7,
	NAL_PPS		= 8,
};

/*
 * The h.264 stream itelf needs to have a set of NAL headers on each frame to
 * work in ffox.
 *
 * We have to parse out the SPS and PPS so we can provide them in the avcC box
 * at the mp4 layer.
 *
 * Some h.264 streams (eg, RPi Zero h264_omx) only have the IDR NAL on each
 * packet, they give the other NAL pieces only once in the stream just after it
 * was opened; this is not available to clients joining the stream later.  To
 * further complicate it, the one-time NAL pieces are provided in the own
 * packet before any IDR content.
 *
 * We keep a copy of any h.264 coming before the avcc could be synthesized, if
 * we see that the subsequent h.264 packets don't have the SPS then we manually
 * apply the stored NAL back on to it before passing it for mp4 framing.
 */

size_t
annex_b_scan(struct src_inst *si, uint8_t *data, size_t len)
{
	uint8_t *p = data, *end = data + len;
	uint8_t *w;

	while (p < end && si->ab_type != NAL_IDR) {
		switch (si->ab_state) {
		case IDLE:
			if (!*p) {
				si->ab_state = SEEN_Z1;
				si->ab_seq = 1;
			} else
				si->ab_seq = 0;
			break;
		case SEEN_Z1:
			if (!*p) {
				si->ab_state = SEEN_Z2;
				si->ab_seq++;
			} else {
				si->ab_state = IDLE;
				si->ab_seq = 0;
			}
			break;
		case SEEN_Z2:
			if (!*p) {
				/* absorb additional leading 00 */
				si->ab_seq++;
				break;
			}

			if (*p != 1) {
				si->ab_state = IDLE;
				si->ab_seq = 0;
				break;
			}
			si->ab_state = HIT;
			si->ab_seq++;
			break;
		case HIT:

			switch (si->ab_type) {
			case 7:
				si->h264_sps_len -= si->ab_seq;
				break;
			case 8:
				si->h264_pps_len -= si->ab_seq;
				break;
			}

			si->ab_type = (*p++) & 0x1f;

			switch (si->ab_type) {
			case NAL_IDR:
				si->avcc_header_len = 0;
				break;
			case NAL_SPS:
				si->h264_sps_len = 0;
				break;
			case NAL_PPS:
				si->h264_pps_len = 0;
				break;
			}

			si->ab_state = IDLE;
			si->ab_seq = 0;
			continue;
		}

		switch (si->ab_type) {
		case NAL_SPS:
			if (si->h264_sps_len < sizeof(si->h264_sps))
				si->h264_sps[si->h264_sps_len++] = *p;
			break;
		case NAL_PPS:
			if (si->h264_pps_len < sizeof(si->h264_pps))
				si->h264_pps[si->h264_pps_len++] = *p;
			break;
		}

		p++;
	}

	if (!si->avcc_header_len && si->h264_sps_len && si->h264_pps_len &&
	    si->ab_type == NAL_IDR) {
		/*
		 * Cook up the AVCC from the extracted Appendix B pieces
		 */

		w = si->avcc_header + LWS_PRE;
		*w++ = 0x01;
		*w++ = si->h264_sps[0];
		*w++ = si->h264_sps[1];
		*w++ = si->h264_sps[2];
		*w++ = 0xff;
		*w++ = 0xe1;

		lws_ser_wu16be(w, si->h264_sps_len + 0);
		w += 2;
	//	*w++ = 0x67;
		memcpy(w, si->h264_sps, si->h264_sps_len);
		w += si->h264_sps_len;

		*w++ = 0x01;

		lws_ser_wu16be(w, si->h264_pps_len + 0);
		w += 2;
	//	*w++ = 0x68;
		memcpy(w, si->h264_pps, si->h264_pps_len);
		w += si->h264_pps_len;

		si->avcc_header_len = lws_ptr_diff_size_t(w,
						si->avcc_header + LWS_PRE);

		//lwsl_notice("%s: AVCC:\n", __func__);
		//lwsl_hexdump_notice(si->avcc_header + LWS_PRE,
		//		    si->avcc_header_len);

		/* append it as formal extradata */

		if (!si->mp4_header_len)
			mp4_header_prepare(si);

		if (!si->avcc_e->extradata) {
			si->avcc_e->extradata = (uint8_t *)av_mallocz(
							si->avcc_header_len);
			if (si->avcc_e->extradata) {
				memcpy(si->avcc_e->extradata,
				       si->avcc_header + LWS_PRE,
							si->avcc_header_len);
				si->avcc_e->extradata_size = si->avcc_header_len;
			}

			si->avs_e->codecpar->extradata = (uint8_t *)av_mallocz(
						si->avcc_header_len +
						AV_INPUT_BUFFER_PADDING_SIZE);
			if (si->avs_e->codecpar->extradata) {
				memcpy(si->avs_e->codecpar->extradata,
				       si->avcc_header + LWS_PRE,
				       si->avcc_header_len);
				si->avs_e->codecpar->extradata_size =
							si->avcc_header_len;
			}
		}
	}

	if (si->ab_type == NAL_IDR)
		si->ab_type = 0;

	return lws_ptr_diff_size_t(p, data);
}
