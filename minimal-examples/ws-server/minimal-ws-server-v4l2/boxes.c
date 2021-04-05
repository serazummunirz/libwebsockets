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

#define PACK __attribute__((packed))


typedef struct bctx {
	uint8_t		*p, *end;
	u32be_t		*stack[8];
	int		sp;
} PACK bctx_t;

typedef struct box {
	u32be_t		len;
	char		name[4];
} PACK box_t;

typedef struct fbox {
	box_t		box;
	uint8_t		ver;
	uint8_t		flags[3];
} PACK fbox_t;

typedef struct box_ftyp1 {
	box_t		box;
	char		name[4];
	uint8_t		miver[4];
	char		name1[8];
} PACK ftyp1_t;

typedef struct fbox_mvhd {
	fbox_t		fbox;
	u32be_t		ctime;
	u32be_t		mtime;
	u32be_t		timescale;
	u32be_t		duration;

	u32be_t		rate;
	u16be_t		vol_full;
	u16be_t		reserved1;
	u32be_t		reserved2[2];

	u32be_t		matrix[9];

	u32be_t		predef[6];
	u32be_t		next_track_id;
} PACK fbox_mvhd_t;

typedef struct fbox_trex {
	fbox_t		fbox;
	u32be_t		track_id;
	u32be_t		def_samp_dec_idx;
	u32be_t		def_samp_duration;
	u32be_t		def_samp_size;
	u32be_t		def_samp_flags;
} PACK trex_t;

typedef struct fbox_tkhd {
	fbox_t		fbox; /* v1 layout */

	u64be_t		ctime;
	u64be_t		mtime;
	u32be_t		track_id;
	u32be_t		reserved;
	u64be_t		track_length;

	u32be_t		reserved2[2];

	u16be_t		layer;
	u16be_t		alt_group;
	u16be_t		volume;
	u16be_t		reserved3;

	u32be_t		matrix[9];

	u32be_t		width;
	u32be_t		height;
} PACK fbox_tkhd_t;

typedef struct fbox_stsd {
	fbox_t		fbox;

	u32be_t		entry_count;

	/* variable */
} PACK fbox_stsd_t;

typedef struct fbox_stsz {
	fbox_t		fbox;

	u32be_t		sample_size;
	u32be_t		entry_count;

	/* variable */
} PACK fbox_stsz_t;

typedef struct box_avc1 {
	box_t		box;

	u16be_t		reserved[3];
	u16be_t		data_reference_index;

	u16be_t		reserved1;
	u16be_t		reserved2;
	u32be_t		reserved3[3];

	u16be_t		width;
	u16be_t		height;

	u32be_t		hres;
	u32be_t		vres;

	u32be_t		reserved4;
	u16be_t		frame_count;

	uint8_t		slen;
	uint8_t		compname[31];

	u16be_t		depth;
	u16be_t		ffff;

} PACK avc1_t;

typedef struct box_avcC {
	box_t		box;

	uint8_t		version;
	uint8_t		avc_profile_ind;
	uint8_t		avc_profile_comp;
	uint8_t		avc_level_ind;
	uint8_t		length_minus_one;
	uint8_t		num_sps;

	/*
	 * SPS size + SPS, then uint8_t num_pps, then PPS size + PPS
	 */

} PACK box_avcC_t;

typedef struct box_uuid_tfxd {
	box_t		box;

	uint8_t		uuid[16];
	uint8_t		version;
	uint8_t		rsv[3];
	u64be_t		frag_abs_time;
	u64be_t		frag_dur;

} PACK box_uuid_tfxd_t;


typedef struct fbox_mdhd {
	fbox_t		fbox; /* v1 */

	u64be_t		ctime;
	u64be_t		mtime;
	u32be_t		timescale;
	u64be_t		duration;

	u16be_t		lang;
	u16be_t		pre_defined;

} PACK fbox_mdhd_t;

typedef struct fbox_hdlr {
	fbox_t		fbox;

	u32be_t		predefined;
	char		handler[4];
	u32be_t		reserved[3];

	/* str with terminating NUL here */

} PACK fbox_hdlr_t;

typedef struct fbox_tfhd {
	fbox_t		fbox;

	u32be_t		track_id;
} PACK fbox_tfhd_t;

typedef struct fbox_tfhd_dsf {
	fbox_t		fbox; /* for flags 0x20 */

	u32be_t		track_id;
//	u32be_t		default_sample_dur;
	u32be_t		default_sample_flags;
} PACK fbox_tfhd_dsf_t;

typedef struct fbox_tfdt {
	fbox_t		fbox; /* version 1 */

	u64be_t		base_media_decode_time;
} PACK fbox_tfdt_t;

typedef struct fbox_trun {
	fbox_t		fbox;

	u32be_t		sample_count;
	u32be_t		data_offset;
	u32be_t		first_sample_flags;
	u32be_t		sample_duration;
	u32be_t		sample_size;
} PACK fbox_trun_t;

typedef struct fbox_trun_subseq {
	fbox_t		fbox;	/* flabs 000305 */

	u32be_t		sample_count;
	u32be_t		data_offset;
	u32be_t		first_sample_flags;
	u32be_t		sample_duration;
	u32be_t		sample_size;
//	u32be_t		sample_composition_time_offset;
} PACK fbox_trun_subseq_t;

typedef struct box_mfhd {
	fbox_t		fbox;

	u32be_t		frag_seq;
} PACK box_mfhd_t;

typedef struct fbox_vmhd {
	fbox_t		fbox;

	u16be_t		gfx_mode;
	u16be_t		gfx_color[3];
} PACK fbox_vmhd_t;

typedef struct fbox_dref {
	fbox_t		fbox;

	u32be_t		entry_count;
} PACK fbox_dref_t;

typedef struct fbox_url {
	fbox_t		fbox;

	/* string + NUL */
} PACK fbox_url_t;

typedef struct fbox_mehd {
	fbox_t		fbox;

	u32be_t		fragment_dur;
} PACK fbox_mehd_t;

#define STACK_PUSH		1
#define STAY			0

/* handles unaligned and BE */
void
add_u32be(u32be_t *dest, unsigned int add_le)
{
	uint8_t *p = (uint8_t *)dest;
	uint32_t src = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3], r;

	r = src + add_le;

	p[0] = (r >> 24) & 0xff;
	p[1] = (r >> 16) & 0xff;
	p[2] = (r >> 8) & 0xff;
	p[3] = (r) & 0xff;
}

static void
prep_box(box_t *box, const char *name, size_t len)
{
	memset(box, 0, len);
	box->len = htonl(len);
	box->name[0] = name[0];
	box->name[1] = name[1];
	box->name[2] = name[2];
	box->name[3] = name[3];
}

static int
write_box(bctx_t *bcx, const box_t *box, int push, size_t bs, size_t adv)
{
	int n;

	if (push)
		bcx->stack[bcx->sp] = (u32be_t *)bcx->p;

	if (bs > lws_ptr_diff_size_t(bcx->end, bcx->p))
		return -1;

	memcpy(bcx->p, box, bs);
	bcx->p += bs + adv;

	for (n = 0; n < bcx->sp; n++)
		add_u32be(bcx->stack[n], (int)(bs + adv));

	if (push)
		bcx->sp++;

	return 0;
}

int
mp4_header_prepare(struct src_inst *si)
{
	bctx_t	bcx;
	uint8_t *start = si->mp4_hdr + LWS_PRE, *p;
	ftyp1_t		f1;
	box_t		moov, mvex, trak, mdia, minf, stbl, dinf;
//	fbox_t		nmhd;
	fbox_mvhd_t	mvhd;
	fbox_tkhd_t	tkhd;
	fbox_stsd_t	stsd;
	fbox_mdhd_t	mdhd;
	fbox_hdlr_t	hdlr;
	trex_t		trex;
	avc1_t		avc1;
	box_avcC_t	avcc;
	fbox_vmhd_t	vmhd;
	fbox_dref_t	dref;
	fbox_url_t	url;
	fbox_mehd_t	mehd;
	fbox_stsz_t	stsz;

	bcx.p = start;
	bcx.end = start + sizeof(si->mp4_hdr) - LWS_PRE;
	bcx.sp = 0;

	/* ftyp */

	prep_box(&f1.box, "styp", sizeof(f1));

	f1.miver[2]			= 2;

	f1.name[0]			= 'm';
	f1.name[1]			= 'p';
	f1.name[2]			= '4';
	f1.name[3]			= '1';

	f1.name1[0]			= 'i';
	f1.name1[1]			= 's';
	f1.name1[2]			= 'o';
	f1.name1[3]			= 'm';
	f1.name1[4]			= 'a';
	f1.name1[5]			= 'v';
	f1.name1[6]			= 'c';
	f1.name1[7]			= '1';

	write_box(&bcx, &f1.box, 0, sizeof(f1), 0);

	/* moov */

	prep_box(&moov, "moov", sizeof(moov));
	write_box(&bcx, &moov, STACK_PUSH, sizeof(moov), 0);

	/* moov -> mvhd */

	prep_box(&mvhd.fbox.box, "mvhd", sizeof(mvhd));
	mvhd.ctime			= htonl(0);
	mvhd.mtime			= htonl(0);
	mvhd.timescale			= htonl(1000);
	mvhd.duration			= htonl(0);
	mvhd.rate			= htonl(1 << 16);
	mvhd.vol_full			= htons(1 << 8);
	mvhd.matrix[0]			= htonl(0x00010000);
	mvhd.matrix[4]			= htonl(0x00010000);
	mvhd.matrix[8]			= htonl(0x40000000);
	mvhd.next_track_id		= htonl(2);
	write_box(&bcx, &mvhd.fbox.box, STAY, sizeof(mvhd), 0);

	/* moov -> trak */

	prep_box(&trak, "trak", sizeof(trak));
	write_box(&bcx, &trak, STACK_PUSH, sizeof(trak), 0);

	/* moov -> trak -> tkhd */

	prep_box(&tkhd.fbox.box, "tkhd", sizeof(tkhd));
	tkhd.fbox.ver			= 1;
	tkhd.fbox.flags[2]		= 3;
	lws_ser_wu64be((uint8_t *)&tkhd.ctime, 0);
	lws_ser_wu64be((uint8_t *)&tkhd.mtime, 0);
	tkhd.track_id			= htonl(1);
	lws_ser_wu64be((uint8_t *)&tkhd.track_length, 0xffffffffffffffffULL);

	tkhd.layer			= htons(0);
	tkhd.alt_group			= htons(0);
	tkhd.volume			= htons(0);

	tkhd.matrix[0]			= htonl(0x00010000);
	tkhd.matrix[4]			= htonl(0x00010000);
	tkhd.matrix[8]			= htonl(0x40000000);

	tkhd.width			= htonl(si->width << 16);
	tkhd.height			= htonl(si->height << 16);
	write_box(&bcx, &tkhd.fbox.box, STAY, sizeof(tkhd), 0);

	/* moov -> trak -> mdia */

	prep_box(&mdia, "mdia", sizeof(mdia));
	write_box(&bcx, &mdia, STACK_PUSH, sizeof(mdia), 0);

	/* moov -> trak -> mdia -> mdhd */

	prep_box(&mdhd.fbox.box, "mdhd", sizeof(mdhd));
	mdhd.fbox.ver			= 1;
	lws_ser_wu64be((uint8_t *)&mdhd.ctime, 0);
	lws_ser_wu64be((uint8_t *)&mdhd.mtime, 0);
	mdhd.timescale			= htonl(0x989680);
	lws_ser_wu64be((uint8_t *)&mdhd.duration, 0xffffffffffffffffULL);
	mdhd.lang			= htons(0x55c4);
	write_box(&bcx, &mdhd.fbox.box, STAY, sizeof(mdhd), 0);

	/* moov -> trak -> mdia -> hdlr */

	prep_box(&hdlr.fbox.box, "hdlr", sizeof(hdlr) + strlen("VideoHandler") + 1);
	hdlr.handler[0]			= 'v';
	hdlr.handler[1]			= 'i';
	hdlr.handler[2]			= 'd';
	hdlr.handler[3]			= 'e';

	p = bcx.p + sizeof(hdlr);

	write_box(&bcx, &hdlr.fbox.box, 0, sizeof(hdlr), strlen("VideoHandler") + 1);

	memcpy(p, "VideoHandler", strlen("VideoHandler") + 1);

	/* moov -> trak -> mdia -> minf */

	prep_box(&minf, "minf", sizeof(minf));
	write_box(&bcx, &minf, STACK_PUSH, sizeof(minf), 0);

	/*
	 * moov -> trak -> mdia -> minf -> vmhd
	 *
	 * shaka requires this, ffox / vlc /mplayer don't care
	 */

	prep_box(&vmhd.fbox.box, "vmhd", sizeof(vmhd));
	vmhd.fbox.flags[2]		= 1;
	write_box(&bcx, &vmhd.fbox.box, 0, sizeof(vmhd), 0);

	/* moov -> trak -> mdia -> minf -> dinf */

	prep_box(&dinf, "dinf", sizeof(dinf));
	write_box(&bcx, &dinf, STACK_PUSH, sizeof(dinf), 0);

	/*
	 * moov -> trak -> mdia -> minf -> dinf -> dref
	 *
	 * shaka requires this, ffox / vlc /mplayer don't care
	 */

	prep_box(&dref.fbox.box, "dref", sizeof(dref));
	dref.entry_count		= htonl(1);
	write_box(&bcx, &dref.fbox.box, STACK_PUSH, sizeof(dref), 0);

	/* moov -> trak -> mdia -> minf -> dinf -> dref -> url
	 *
	 * If the flag is set indicating that the data is in the same file as
	 * this box, then no string (not even an empty one) shall be supplied
	 * in the entry field.
	 */

	prep_box(&url.fbox.box, "url ", sizeof(url));
	url.fbox.flags[2]		= 1;
//	p = bcx.p + sizeof(url);
	write_box(&bcx, &url.fbox.box, STAY, sizeof(url), 0); //1);
//	*p = 0;

	bcx.sp = 4;
#if 0
	/* moov -> trak -> mdia -> minf -> nmhd */

	prep_box(&nmhd.box, "nmhd", sizeof(nmhd));
	write_box(&bcx, &nmhd.box, STAY, sizeof(nmhd), 0);
#endif
	/* moov -> trak -> mdia -> minf -> stbl */

	prep_box(&stbl, "stbl", sizeof(stbl));
	write_box(&bcx, &stbl, STACK_PUSH, sizeof(stbl), 0);

	/* moov -> trak -> mdia -> minf -> stbl -> stsd */

	prep_box(&stsd.fbox.box, "stsd", sizeof(stsd));
	stsd.entry_count		= htonl(1);
	write_box(&bcx, &stsd.fbox.box, STACK_PUSH, sizeof(stsd), 0);

	/* moov -> trak -> mdia -> minf -> stbl -> stsd -> avc1 */

	prep_box(&avc1.box, "avc1", sizeof(avc1));
	avc1.data_reference_index	= htons(1);
	avc1.width			= htons(si->width);
	avc1.height			= htons(si->height);
	avc1.hres			= htonl(0x00480000);
	avc1.vres			= htonl(0x00480000);
	avc1.frame_count		= htons(1);
	avc1.slen			= 3;
	avc1.compname[0]		= 'l';
	avc1.compname[1]		= 'w';
	avc1.compname[2]		= 's';
	avc1.depth			= htons(24);
	avc1.ffff			= htons(0xffff);
	write_box(&bcx, &avc1.box, STACK_PUSH, sizeof(avc1), 0);

	/* moov -> trak -> mdia -> minf -> stbl -> stsd -> avc1 -> avcC */

	prep_box(&avcc.box, "avcC", sizeof(avcc) + 3 + si->h264_sps_len +
			      1 + 3 + si->h264_pps_len);
	avcc.version			= 1;
	avcc.avc_profile_ind		= si->h264_sps[0];
	avcc.avc_profile_comp		= si->h264_sps[1];
	avcc.avc_level_ind		= si->h264_sps[2];

	avcc.length_minus_one		= 0xfc | (4 - 1);
	avcc.num_sps			= 0xe0 | 1;

	/* we need to place spslen + sps, 0x01, ppslen + pps here */

	p = bcx.p + sizeof(avcc);

	write_box(&bcx, &avcc.box, STAY, sizeof(avcc), 3 + si->h264_sps_len +
					      1 + 3 + si->h264_pps_len);

	/* these are fixed 16-bit lengths not subject to length_minus_one */
	lws_ser_wu16be(p, si->h264_sps_len + 1);
	p += 2;
	*p++ = 0x67;

	memcpy(p, si->h264_sps, si->h264_sps_len);
	p += si->h264_sps_len;

	*p++				= 1; /* number of pps blobs */

	/* these are fixed 16-bit lengths not subject to length_minus_one */
	lws_ser_wu16be(p, si->h264_pps_len + 1);
	p += 2;
	*p++ = 0x68;

	memcpy(p, si->h264_pps, si->h264_pps_len);
	p += si->h264_pps_len;

	/* moov -> trak -> mdia -> minf -> stbl -> stts */

	bcx.sp = 5; /* back to stbl */

	prep_box(&stsd.fbox.box, "stts", sizeof(stsd));
	stsd.entry_count		= htonl(0);
	write_box(&bcx, &stsd.fbox.box, STAY, sizeof(stsd), 0);

	/* moov -> trak -> mdia -> minf -> stbl -> stsc */

	prep_box(&stsd.fbox.box, "stsc", sizeof(stsd));
	stsd.entry_count		= htonl(0);
	write_box(&bcx, &stsd.fbox.box, STAY, sizeof(stsd), 0);

	/* moov -> trak -> mdia -> minf -> stbl -> stsz */

	prep_box(&stsz.fbox.box, "stsz", sizeof(stsz));
	stsz.sample_size		= htonl(0);
	stsz.entry_count		= htonl(0);
	write_box(&bcx, &stsz.fbox.box, STAY, sizeof(stsz), 0);

	/* moov -> trak -> mdia -> minf -> stbl -> stco */

	prep_box(&stsd.fbox.box, "stco", sizeof(stsd));
	stsd.entry_count		= htonl(0);
	write_box(&bcx, &stsd.fbox.box, STAY, sizeof(stsd), 0);

	bcx.sp = 1; /* back to moov */

	/* moov -> mvex */

	prep_box(&mvex, "mvex", sizeof(mvex));
	write_box(&bcx, &mvex, STACK_PUSH, sizeof(mvex), 0);

	/* moov -> mvex -> mehd */

	prep_box(&mehd.fbox.box, "mehd", sizeof(mehd));
	mehd.fragment_dur		= htonl(0);
	write_box(&bcx, &mehd.fbox.box, STAY, sizeof(mehd), 0);

	/* moov -> mvex -> trex */

	prep_box(&trex.fbox.box, "trex", sizeof(trex));
	trex.track_id			= htonl(1);
	trex.def_samp_dec_idx		= htonl(1);
	write_box(&bcx, &trex.fbox.box, STAY, sizeof(trex), 0);

	bcx.sp--; /* pop mvex */

	si->mp4_header_len = lws_ptr_diff_size_t(bcx.p, start);

	lwsl_hexdump_notice(si->mp4_hdr + LWS_PRE, si->mp4_header_len);

	return 0;
}

static const uint8_t wellknown_uuid_tfxd[] = {
	0x6D, 0x1D, 0x9B, 0x05, 0x42, 0xD5, 0x44, 0xE6,
	0x80, 0xE2, 0x14, 0x1D, 0xAF, 0xF7, 0x57, 0xB2
};

size_t
moof_prepare(struct src_inst *si, uint8_t *start, struct pss *pss,
	     size_t max, size_t vlen, unsigned int frag_dur)
{
	box_t			moof, traf, mdat;
	box_mfhd_t		mfhd;
//	fbox_tfhd_t		tfhd;
	fbox_tfhd_dsf_t		tfhd_dsf;
//	fbox_trun_t		trun;
	fbox_trun_subseq_t	truns;
	fbox_tfdt_t		tfdt;
	bctx_t			bcx;
	uint8_t			*p, *datum;
	u32be_t			*ofs = NULL;
	box_uuid_tfxd_t		uuid_tfxd;

	datum = bcx.p = start;
	bcx.end = start + max;
	bcx.sp = 0;

	/*
	 * Supposedly mandatory structure
	 *
	 * moof [ mfhd, traf [ tfhd, trik, trun ] ]
	 */

	prep_box(&moof, "moof", sizeof(moof));
	write_box(&bcx, &moof, STACK_PUSH, sizeof(moof), 0);

	/* moof- -> mfhd */

	prep_box(&mfhd.fbox.box, "mfhd", sizeof(mfhd));
	pss->seq++;
	mfhd.frag_seq			= htonl(pss->seq);
	write_box(&bcx, &mfhd.fbox.box, STAY, sizeof(mfhd), 0);

	/* moof -> traf */

	/*
	 * The base‐data‐offset for the first track in the movie fragment is the
	 * position of the first byte of the enclosing Movie Fragment Box, and
	 * for second and subsequent track fragments, the default is the end of
	 * the data defined by the preceding track fragment.
	 */
	if (pss->sent_initial_moof)
		datum = bcx.p;

	prep_box(&traf, "traf", sizeof(traf));
	write_box(&bcx, &traf, STACK_PUSH, sizeof(traf), 0);

	/* moof- -> tfdt (required for ffox streaming) */

	prep_box(&tfdt.fbox.box, "tfdt", sizeof(tfdt));
	tfdt.base_media_decode_time	= htonl(pss->dts);
	write_box(&bcx, &tfdt.fbox.box, STAY, sizeof(tfdt), 0);

	/* moof- -> traf -> tfhd */

	prep_box(&tfhd_dsf.fbox.box, "tfhd", sizeof(tfhd_dsf));
	tfhd_dsf.fbox.flags[2]		= 0x20;
	tfhd_dsf.track_id		= htonl(1);
//	tfhd_dsf.default_sample_dur	= htonl(frag_dur);
	tfhd_dsf.default_sample_flags	= htonl(0x01010000);
	write_box(&bcx, &tfhd_dsf.fbox.box, STAY, sizeof(tfhd_dsf), 0);

	/* moof -> traf -> trun
	 *
	 *  data_offset; It specifies the offset from the beginning of
	 *  the MoofBox. If only one TrunBox is specified, then the
	 *  DataOffset field MUST be the sum of the lengths of the
	 *  MoofBox and all the fields in the MdatBox field
	 *
	 * it means, from the start of the moof box len/"moof", write here how
	 * many bytes to add to get to the encapsulated h.264 payload (ie, the
	 * annex-b stuff in there).
	 */

	prep_box(&truns.fbox.box, "trun", sizeof(truns));
	truns.fbox.ver			= 1;
	truns.fbox.flags[0]		= 0;
	truns.fbox.flags[1]		= 3;
	truns.fbox.flags[2]		= 5;
	truns.sample_count		= htonl(1);
	ofs				= (u32be_t *)(bcx.p +
				offsetof(fbox_trun_subseq_t, data_offset));
	truns.first_sample_flags	= htonl(0x000305);
	truns.sample_duration		= htonl(frag_dur);
	truns.sample_size		= htonl(vlen);
//	truns.sample_composition_time_offset = 0;
	write_box(&bcx, &truns.fbox.box, STAY, sizeof(truns), 0);

	/* moof -> traf -> uuid */

	prep_box(&uuid_tfxd.box, "uuid", sizeof(uuid_tfxd));
	memcpy(uuid_tfxd.uuid, wellknown_uuid_tfxd, sizeof(uuid_tfxd.uuid));
	uuid_tfxd.version		= 1;
	lws_ser_wu64be((uint8_t *)&uuid_tfxd.frag_abs_time, pss->dts);
	lws_ser_wu64be((uint8_t *)&uuid_tfxd.frag_dur, frag_dur);
	write_box(&bcx, &uuid_tfxd.box, STAY, sizeof(uuid_tfxd), 0);

	pss->dts += frag_dur;

	bcx.sp = 0; /* pop everything, this is a toplevel item */

	/* moof -> mdat */

	prep_box(&mdat, "mdat", sizeof(mdat));
	p = bcx.p;
	write_box(&bcx, &mdat, 0, sizeof(mdat), vlen);
	add_u32be((u32be_t *)p, vlen);

	if (ofs)
		add_u32be(ofs, lws_ptr_diff(p + 8, datum));

	pss->mdat_acc += lws_ptr_diff_size_t(bcx.p, start);

//	lwsl_hexdump_warn(start, lws_ptr_diff_size_t(bcx.p - vlen, start));

	return lws_ptr_diff_size_t(bcx.p - vlen, start);
}
