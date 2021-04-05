/* ffmpeg-devel package on Fedora */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>

#include <linux/videodev2.h>

#define QUEUELEN 64

/* comment this to use the MOOF from libav*, otherwise we generate both the
 * onetime mp4 headers and the individual moof headers from scratch.  When
 * using OWN_MOOF, in fact we only use libav as a way to start the h264 encoder.
 */
#define OWN_MOOF

typedef uint64_t u64be_t;
typedef uint32_t u32be_t;
typedef uint16_t u16be_t;

/* one of these created for each message */

struct msg {
	void *payload; /* is malloc'd */
	size_t len;
};

/* one of these per video source */

struct src_inst {
	lws_dll2_t			list;
	lws_pthread_mutex(lock) /* protects all mirror instance data */
	lws_dll2_owner_t		owner;	/* pss list */
	/**< must hold the the per_vhost_data__lws_mirror.lock as well
	 * to change si list membership */
	struct lws_ring			*ring;
	int				messages_allocated;

#if !defined(OWN_MOOF)
	uint8_t				iobuf[256 * 1024];
#endif
	char				name[30];

	uint8_t				pre[4096], annex_b[4096];
	unsigned int			pre_len, annex_b_len;
	uint8_t				chonk[256 * 1024];

	uint8_t				mp4_hdr[2048];
	unsigned int			mp4_header_len;

	uint8_t				h264_sps[128];
	unsigned int			h264_sps_len;
	uint8_t				h264_pps[128];
	unsigned int			h264_pps_len;

	uint8_t				avcc_header[LWS_PRE + 384];
	unsigned int			avcc_header_len;

	uint64_t			ntt, dtt;

	char				ab_type;
	unsigned int			ab_state;
	unsigned int			ab_seq;

	struct lws			*capture_wsi;
	int				filefd;

	struct msg			*buffers;
	unsigned int			bcount;

	int				frame;
	int				width;
	int				height;

	int				dec_frame;
	int				enc_frame;

	char				subsequent;
	char				issued;

	AVDictionary			*opt;

	AVCodecParserContext		*avcpc_d;
//	struct SwsContext		*swsc;
	AVCodec				*avc_d;
	AVCodecContext			*avcc_d;
	AVFrame				*avf_d;
	AVPacket			*avp_d;

	AVCodec				*avc_e;
	AVCodecContext			*avcc_e;
	AVFormatContext			*avfc_e;
	AVPacket			*avp_e;
	AVStream			*avs_e;
	AVIOContext			*avioc_e;
};

/* one of these is created for each client connecting to us */

struct pss {
	lws_dll2_t			list; /* si owner */

	lws_sorted_usec_list_t		sul;

	uint64_t			mdat_acc, dts;
	uint32_t			mfhd_cnt;
	uint32_t			seq;

	struct lws			*wsi;
	uint32_t			tail;
	size_t				inside;

	char				inserted_mp4;
	char				atomic;
	char				sent_initial_moof;
	char				did_moof;
};

struct raw_vhd {

	struct lws_context		*context;
	struct lws_vhost		*vhost;
	const struct lws_protocols	*protocol;

	lws_pthread_mutex(lock) /* protects mi_list membership changes */

	lws_dll2_owner_t		owner;	/* src_inst list */
};

int
init_device(struct src_inst *si);
int
deinit_device(struct src_inst *si);
int
start_capturing(struct src_inst *si);
int
stop_capturing(struct src_inst *si);

int
create_si(struct raw_vhd *vhd, struct lws *wsi);

size_t
annex_b_scan(struct src_inst *si, uint8_t *data, size_t len);

int
lws_v4l2_dq(struct src_inst *si, struct v4l2_buffer *buf);

int
lws_v4l2_q(struct src_inst *si, struct v4l2_buffer *buf);

void
add_u32be(u32be_t *dest, unsigned int add_le);

void
__mirror_destroy_message(void *_msg);

int
transcode(struct src_inst *si);

int
mp4_header_prepare(struct src_inst *si);

size_t
moof_prepare(struct src_inst *si, uint8_t *start, struct pss *pss,
	     size_t max, size_t vlen, unsigned int frag_dur);

