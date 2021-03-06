/*
 * Copyright 2012-2016, Björn Ståhl
 * License: GPLv2, see COPYING file in arcan source repository.
 * Reference: http://www.libretro.com
 */

#define AGP_ENABLE_UNPURE 1

#include <math.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <inttypes.h>

#include <arcan_shmif.h>
#include <arcan_namespace.h>
#include <arcan_resource.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "frameserver.h"
#include "ntsc/snes_ntsc.h"
#include "ievsched.h"
#include "stateman.h"
#include "sync_plot.h"
#include "libretro.h"

#include "font_8x8.h"
#include "resampler/speex_resampler.h"

#ifdef FRAMESERVER_LIBRETRO_3D
#ifdef ENABLE_RETEXTURE
#include "retexture.h"
#endif
#include "video_platform.h"
#include "platform.h"
#endif

#ifndef MAX_PORTS
#define MAX_PORTS 4
#endif

#ifndef MAX_AXES
#define MAX_AXES 8
#endif

#ifndef MAX_BUTTONS
#define MAX_BUTTONS 16
#endif

#undef BADID

#define COUNT_OF(x) \
	((sizeof(x)/sizeof(0[x])) / ((size_t)(!(sizeof(x) % sizeof(0[x])))))

/* note on synchronization:
 * the async and esync mechanisms will buffer locally and have that buffer
 * flushed by the main application whenever appropriate. For audio, this is
 * likely limited by the buffering capacity of the sound device / pipeline
 * whileas the event queue might be a bit more bursty.
 *
 * however, we will lock to video, meaning that it is the framerate of the
 * frameserver that will decide the actual framerate, that may be locked
 * to VREFRESH (or lower, higher / variable). Thus we also need frameskipping
 * heuristics here.
 */
struct input_port {
	bool buttons[MAX_BUTTONS];
	int16_t axes[MAX_AXES];

/* special offsets into buttons / axes based on device */
	unsigned cursor_x, cursor_y, cursor_btns[5];
};

struct core_variable {
	const char* key;
	const char* value;
	bool updated;
};

typedef void(*pixconv_fun)(const void* data, shmif_pixel* outp,
	unsigned width, unsigned height, size_t pitch, bool postfilter);

static struct {
	struct synch_graphing* sync_data; /* overlaying statistics */

/* flag for rendering callbacks, should the frame be processed or not */
	bool skipframe_a, skipframe_v, empty_v;
	bool pause, in_3d;
	bool hpassing_disabled;

/* miliseconds per frame, 1/fps */
	double mspf;

/* when did we last seed gameplay timing */
	long long int basetime;

/* for debugging / testing, added extra jitter to
 * the cost for the different stages */
	int jitterstep, jitterxfer;

/* add 'n' frames pseudoskipping to populate audiobuffers */
	int preaudiogen;
/* user changeable variable, how synching should be treated */
	int skipmode;
/* for frameskip auto to compensate for jitter in transfer etc. */
	int prewake;

/* statistics / timing */
	unsigned long long aframecount, vframecount;
	struct retro_system_av_info avinfo; /* timing according to libretro */

	int rebasecount, frameskips, transfercost, framecost;
	const char* colorspace;

/* colour conversion / filtering */
	pixconv_fun converter;
	uint16_t* ntsc_imb;
	bool ntscconv;
	snes_ntsc_t* ntscctx;
	int ntscvals[4];
	snes_ntsc_setup_t ntsc_opts;

/* SHM- API input /output */
	struct arcan_shmif_cont shmcont;
	int graphmode;

/* need to resample as speex don't manage buffering internally */
	int16_t* in_audb, (* out_audb);
	size_t audbuf_sz;
	off_t audbuf_ofs;
	SpeexResamplerState* resampler;

/* libretro states / function pointers */
	struct retro_system_info sysinfo;
	struct retro_game_info gameinfo;

/* for core options support:
 * 1. on SET_VARIABLES, expose each as an event to parent.
 *    populate a separate table that acts as a cache.
 *
 * 2. on GET_VARIABLE, lookup against the args and fallback
 *    on the cache. Dynamic switching isn't supported currently. */
	struct arg_arr* inargs;
	struct core_variable* varset;
	bool optdirty;

/* for skipmode = TARGET_SKIP_ROLLBACK,
 * then we maintain a statebuffer (requires savestate support)
 * and when input is "dirty" roll back one frame ignoring output,
 * apply, then fast forward one frame */
 	bool dirty_input;
	float aframesz;
	int rollback_window;
	unsigned rollback_front;
	char* rollback_state;
	size_t state_sz;
	char* syspath;
	bool res_empty;

/*
 * performance trim-values for video/audio buffer synchronization
 */
	uint16_t def_abuf_sz;
	uint8_t abuf_cnt, vbuf_cnt;

#ifdef FRAMESERVER_LIBRETRO_3D
	struct retro_hw_render_callback hwctx;
	struct agp_rendertarget* rtgt;
	int last_handle;
	struct storage_info_t vstore;
#endif

/* parent uses an event->push model for input, libretro uses a poll one, so
 * prepare a lookup table that events gets pushed into and libretro can poll */
	struct input_port input_ports[MAX_PORTS];
	char kbdtbl[RETROK_LAST];

	void (*run)();
	void (*reset)();
	bool (*load_game)(const struct retro_game_info* game);
	size_t (*serialize_size)();
	bool (*serialize)(void*, size_t);
	bool (*deserialize)(const void*, size_t);
	void (*set_ioport)(unsigned, unsigned);
} retroctx = {
	.abuf_cnt = 12,
	.def_abuf_sz = 1,
	.vbuf_cnt = 3,
	.prewake = 10,
	.preaudiogen = 1,
	.skipmode = TARGET_SKIP_AUTO
#ifdef FRAMESERVER_LIBRETRO_3D
	,.last_handle = -1
#endif
};

/* render statistics unto *vidp, at the very end of this .c file */
static void update_ntsc();
static void push_stats();
static void log_msg(char* msg, bool flush);

#ifdef FRAMESERVER_LIBRETRO_3D
static void setup_3dcore(struct retro_hw_render_callback*);
#endif

static void* lastlib, (* globallib);
retro_proc_address_t libretro_requirefun(const char* sym)
{
/* not very relevant here, but proper form is dlerror() */
	if (!sym)
		return NULL;

/*
  if (module){
		if (lastlib)
			return dlsym(lastlib, sym);
		else
			return NULL;
	}
 */

	return dlsym(lastlib, sym);
}

static bool write_handle(const void* const data,
	size_t sz_data, file_handle dst, bool finalize)
{
	bool rv = false;

	if (dst != BADFD)
	{
		off_t ofs = 0;
		ssize_t nw;

		while ( ofs != sz_data){
			nw = write(dst, ((char*) data) + ofs, sz_data - ofs);
			if (-1 == nw)
				switch (errno){
				case EAGAIN: continue;
				case EINTR: continue;
				default:
					LOG("write_handle(dumprawfile) -- write failed (%d),"
					"	reason: %s\n", errno, strerror(errno));
					goto out;
			}

			ofs += nw;
		}
		rv = true;

		out:
		if (finalize)
			close(dst);
	}
	 else
		 LOG("write_handle(dumprawfile) -- request to dump to invalid "
			"file handle ignored, no output set by parent.\n");

	return rv;
}

static void resize_shmpage(int neww, int newh, bool first)
{
/* present error message, synch then terminate.
 * setting a tiny valid buffer size will get the system preferred */
	if (!arcan_shmif_resize_ext(&retroctx.shmcont, neww, newh,
		(struct shmif_resize_ext){
			.abuf_sz = retroctx.def_abuf_sz,
			.abuf_cnt = retroctx.abuf_cnt,
			.vbuf_cnt = retroctx.vbuf_cnt})){
		LOG("resizing shared memory page failed\n");
		exit(1);
	}

#ifdef FRAMESERVER_LIBRETRO_3D
	if (retroctx.rtgt){
		retroctx.shmcont.hints = SHMIF_RHINT_ORIGO_LL;
		agp_activate_rendertarget(NULL);
		agp_resize_rendertarget(retroctx.rtgt, neww, newh);
		if (!getenv("GAME_NORESET"))
			retroctx.hwctx.context_reset();
		shmif_pixel px = RGBA(0x44, 0x44, 0x44, 0xff);

		for (int i = 0; i < 2; i++){
			shmif_pixel* out = retroctx.shmcont.vidp;
			for (int y = 0; y < retroctx.shmcont.h; y++)
				for (int x = 0; x < retroctx.shmcont.w; x++)
					*out++ = px;
			arcan_shmif_signal(&retroctx.shmcont, SHMIF_SIGVID | SHMIF_SIGBLK_NONE);
		}
	}
#endif

	if (retroctx.sync_data)
		retroctx.sync_data->cont_switch(retroctx.sync_data, &retroctx.shmcont);

/* will be reallocated if needed and not set so just free and unset */
	if (retroctx.ntsc_imb){
		free(retroctx.ntsc_imb);
		retroctx.ntsc_imb = NULL;
	}
}

/* overrv / overra are needed for handling rollbacks etc.
 * while still making sure the other frameskipping options are working */
static void process_frames(int nframes, bool overrv, bool overra)
{
	bool cv = retroctx.skipframe_v;
	bool ca = retroctx.skipframe_a;

	if (overrv)
		retroctx.skipframe_v = true;

	if (overra)
		retroctx.skipframe_a = true;

	while(nframes--)
		retroctx.run();

	if (retroctx.skipmode <= TARGET_SKIP_ROLLBACK){
		retroctx.serialize(retroctx.rollback_state +
			(retroctx.rollback_front * retroctx.state_sz), retroctx.state_sz);
		retroctx.rollback_front = (retroctx.rollback_front + 1)
			% retroctx.rollback_window;
	}

	retroctx.skipframe_v = cv;
	retroctx.skipframe_a = ca;
}

#define RGB565(b, g, r) ((uint16_t)(((uint8_t)(r) >> 3) << 11) | \
								(((uint8_t)(g) >> 2) << 5) | ((uint8_t)(b) >> 3))

static void push_ntsc(unsigned width, unsigned height,
	const uint16_t* ntsc_imb, shmif_pixel* outp)
{
	size_t linew = SNES_NTSC_OUT_WIDTH(width) * 4;

/* only draw on every other line, so we can easily mix or
 * blend interleaved (or just duplicate) */
	snes_ntsc_blit(retroctx.ntscctx, ntsc_imb, width, 0,
		width, height, outp, linew * 2);

/* this might be a possible test-case for running two shmif
 * connections and let the compositor do interlacing management */
	assert(ARCAN_SHMPAGE_VCHANNELS == 4);
	for (int row = 1; row < height * 2; row += 2)
		memcpy(& ((char*) retroctx.shmcont.vidp)[row * linew],
			&((char*) retroctx.shmcont.vidp)[(row-1) * linew], linew);
}

/* better distribution for conversion (white is white ..) */
static const uint8_t rgb565_lut5[] = {
  0,   8,  16,  25,  33,  41,  49,  58,  66,   74,  82,  90,  99, 107, 115,123,
132, 140, 148, 156, 165, 173, 181, 189,  197, 206, 214, 222, 230, 239, 247,255
};

static const uint8_t rgb565_lut6[] = {
  0,   4,   8,  12,  16,  20,  24,  28,  32,  36,  40,  45,  49,  53,  57, 61,
 65,  69,  73,  77,  81,  85,  89,  93,  97, 101, 105, 109, 113, 117, 121, 125,
130, 134, 138, 142, 146, 150, 154, 158, 162, 166, 170, 174, 178, 182, 186, 190,
194, 198, 202, 206, 210, 215, 219, 223, 227, 231, 235, 239, 243, 247, 251, 255
};

static void libretro_rgb565_rgba(const uint16_t* data, shmif_pixel* outp,
	unsigned width, unsigned height, size_t pitch)
{
	uint16_t* interm = retroctx.ntsc_imb;
	retroctx.colorspace = "RGB565->RGBA";

/* with NTSC on, the input format is already correct */
	for (int y = 0; y < height; y++){
		for (int x = 0; x < width; x++){
			uint16_t val = data[x];
			uint8_t r = rgb565_lut5[ (val & 0xf800) >> 11 ];
			uint8_t g = rgb565_lut6[ (val & 0x07e0) >> 5  ];
			uint8_t b = rgb565_lut5[ (val & 0x001f)       ];

			if (retroctx.ntscconv)
				*interm++ = RGB565(r, g, b);
			else
				*outp++ = RGBA(r, g, b, 0xff);
		}
		data += pitch >> 1;
	}

	if (retroctx.ntscconv)
		push_ntsc(width, height, retroctx.ntsc_imb, outp);

	return;
}

static void libretro_xrgb888_rgba(const uint32_t* data, uint32_t* outp,
	unsigned width, unsigned height, size_t pitch)
{
	assert( (uintptr_t)data % 4 == 0 );
	retroctx.colorspace = "XRGB888->RGBA";

	uint16_t* interm = retroctx.ntsc_imb;

	for (int y = 0; y < height; y++){
		for (int x = 0; x < width; x++){
			uint8_t* quad = (uint8_t*) (data + x);
			if (retroctx.ntscconv)
				*interm++ = RGB565(quad[2], quad[1], quad[0]);
			else
				*outp++ = RGBA(quad[2], quad[1], quad[0], 0xff);
		}

		data += pitch >> 2;
	}

	if (retroctx.ntscconv)
		push_ntsc(width, height, retroctx.ntsc_imb, outp);
}

static void libretro_rgb1555_rgba(const uint16_t* data, uint32_t* outp,
	unsigned width, unsigned height, size_t pitch, bool postfilter)
{
	uint16_t* interm = retroctx.ntsc_imb;
	retroctx.colorspace = "RGB1555->RGBA";

	unsigned dh = height >= ARCAN_SHMPAGE_MAXH ? ARCAN_SHMPAGE_MAXH : height;
	unsigned dw =  width >= ARCAN_SHMPAGE_MAXW ? ARCAN_SHMPAGE_MAXW : width;

	for (int y = 0; y < dh; y++){
		for (int x = 0; x < dw; x++){
			uint16_t val = data[x];
			uint8_t r = ((val & 0x7c00) >> 10) << 3;
			uint8_t g = ((val & 0x03e0) >>  5) << 3;
			uint8_t b = ( val & 0x001f) <<  3;

			if (postfilter)
				*interm++ = RGB565(r, g, b);
			else
				*outp++ = RGBA(r, g, b, 0xff);
		}

		data += pitch >> 1;
	}

	if (postfilter)
		push_ntsc(width, height, retroctx.ntsc_imb, outp);
}


static int testcounter;
static void libretro_vidcb(const void* data, unsigned width,
	unsigned height, size_t pitch)
{
	testcounter++;

	if (retroctx.in_3d && !data)
		;
	else if (!data || retroctx.skipframe_v){
		retroctx.empty_v = true;
		return;
	}
	else
		retroctx.empty_v = false;

/* width / height can be changed without notice, so we have to be ready for the
 * fact that the cost of conversion can suddenly move outside the allowed
 * boundaries, then NTSC is ignored (or if we have 3d/hw source) */
	unsigned outw = width;
	unsigned outh = height;
	bool ntscconv = retroctx.ntscconv && data != RETRO_HW_FRAME_BUFFER_VALID;

	if (ntscconv && SNES_NTSC_OUT_WIDTH(width)<= ARCAN_SHMPAGE_MAXW
		&& height * 2 <= ARCAN_SHMPAGE_MAXH){
		outh = outh << 1;
		outw = SNES_NTSC_OUT_WIDTH( width );
	}
	else {
		outw = width;
		outh = height;
		ntscconv = false;
	}

/* the shmpage size will be larger than the possible values for width / height,
 * so if we have a mismatch, just change the shared dimensions and toggle
 * resize flag */
	if (outw != retroctx.shmcont.addr->w || outh !=
		retroctx.shmcont.addr->h){
		resize_shmpage(outw, outh, false);
	}

	if (ntscconv && !retroctx.ntsc_imb){
		retroctx.ntsc_imb = malloc(sizeof(uint16_t) * outw * outh);
	}

#ifdef FRAMESERVER_LIBRETRO_3D
/* method one, just read color attachment */
	if (retroctx.in_3d){
/* it seems like tons of cores doesn't actually set this correctly */
		if (1 || data == RETRO_HW_FRAME_BUFFER_VALID){
			struct storage_info_t store = retroctx.vstore;
			store.vinf.text.raw = retroctx.shmcont.vidp;

/* if the underlying LWA platform supports zero-copy handle passing, use that */
			if (!retroctx.hpassing_disabled){
				enum status_handle status;
				retroctx.last_handle = platform_video_output_handle(&retroctx.vstore, &status);

				if (status != READY_TRANSFER){
					LOG("3d(), couldn't get output handle -- direct handle passing "
					"disabled.\n");
					retroctx.hpassing_disabled = true;
					retroctx.last_handle = -1;
				}

				return;
			}
/* or fallback to synchronous expensive readback */
			else{
				agp_activate_rendertarget(NULL);
				agp_readback_synchronous(&store);
			}
		}
		else
			return;
	}
	else
#endif

/* lastly, convert / blit, this will possibly clip */
	if (retroctx.converter)
		retroctx.converter(data, retroctx.shmcont.vidp, width,
			height, pitch, ntscconv);
}

static void do_preaudio()
{
	if (retroctx.preaudiogen == 0)
		return;

	retroctx.skipframe_v = true;
	retroctx.skipframe_a = false;

	int afc = retroctx.aframecount;
	int vfc = retroctx.vframecount;

	for (int i = 0; i < retroctx.preaudiogen; i++)
		retroctx.run();

	retroctx.skipframe_v = false;
	retroctx.aframecount = afc;
	retroctx.vframecount = vfc;
}

static void libretro_skipnframes(unsigned count, bool fastfwd)
{
	retroctx.skipframe_v = true;
	retroctx.skipframe_a = fastfwd;

	long long afc = retroctx.aframecount;

	for (int i = 0; i < count; i++)
		retroctx.run();

	if (fastfwd){
		retroctx.aframecount = afc;
		retroctx.frameskips += count;
	}
	else
		retroctx.vframecount += count;

	retroctx.skipframe_a = false;
	retroctx.skipframe_v = false;
}

static void reset_timing(bool newstate)
{
	retroctx.basetime = arcan_timemillis();
	do_preaudio();
	retroctx.vframecount = 1;
	retroctx.aframecount = 1;
	retroctx.frameskips  = 0;
	if (!newstate){
		retroctx.rebasecount++;
	}

/* since we can't be certain about our current vantage point...*/
	if (newstate && retroctx.skipmode <= TARGET_SKIP_ROLLBACK &&
		retroctx.state_sz > 0){
		retroctx.rollback_window = (TARGET_SKIP_ROLLBACK - retroctx.skipmode) + 1;
		if (retroctx.rollback_window > 10)
			retroctx.rollback_window = 10;

			free(retroctx.rollback_state);
			retroctx.rollback_state = malloc(retroctx.state_sz * retroctx.rollback_window);
			retroctx.rollback_front = 0;

			retroctx.serialize(retroctx.rollback_state, retroctx.state_sz);
			for (int i=1; i < retroctx.rollback_window - 1; i++)
				memcpy(retroctx.rollback_state + (i*retroctx.state_sz),
					retroctx.rollback_state, retroctx.state_sz);

			LOG("setting input rollback (%d)\n", retroctx.rollback_window);
	}
}

static void libretro_audscb(int16_t left, int16_t right)
{
	if (retroctx.skipframe_a)
		return;

	retroctx.aframecount++;

/* can happen if we skip a lot and never transfer */
	if (retroctx.audbuf_ofs + 2 < retroctx.audbuf_sz >> 1){
		retroctx.in_audb[retroctx.audbuf_ofs++] = left;
		retroctx.in_audb[retroctx.audbuf_ofs++] = right;
	}
}

static size_t libretro_audcb(const int16_t* data, size_t nframes)
{
	if (retroctx.skipframe_a)
		return nframes;

	retroctx.aframecount += nframes;

/* local buffer overflow, shouldn't happen.. */
	if ((retroctx.audbuf_ofs << 1) +
		(nframes << 1) + (nframes << 2) > retroctx.audbuf_sz )
		return nframes;

/* 2 bytes per sample, 2 channels */
/* audbuf is in int16_t and ofs used as index */
	memcpy(&retroctx.in_audb[retroctx.audbuf_ofs], data, nframes << 2);
	retroctx.audbuf_ofs += nframes << 1;

	return nframes;
}

/* we ignore these since before pushing for a frame,
 * we've already processed the queue */
static void libretro_pollcb(){}

static const char* lookup_varset( const char* key )
{
	struct core_variable* var = retroctx.varset;
	char buf[ strlen(key) + sizeof("core_") + 1];
	sprintf(buf, "core_%s", key);
	const char* val = NULL;

/* we have an initial preset, only update if dirty,
 * note: this might not be necessary anymore, test and drop */
	if (arg_lookup(retroctx.inargs, buf, 0, &val)){
		if (var)
			while(var->key){
				if (var->updated && strcmp(var->key, key) == 0){
					return var->value;
				}

				var++;
			}

	}
/* no preset, just return the first match */
	else if (var) {
		while (var->key){
			if (strcmp(var->key, key) == 0)
				return var->value;

			var++;
		}
	}

	return val;
}

/* from parent, not all cores support dynamic arguments
 * so this is just a complement to launch arguments */
static void update_corearg(int code, const char* value)
{
	struct core_variable* var = retroctx.varset;
	while (var && var->key && code--)
		var++;

	if (code <= 0){
		free((char*)var->value);
		var->value = strdup(value);
		var->updated = true;
	}
}

static void update_varset( struct retro_variable* data )
{
	int count = 0;
	arcan_event outev = {
		.category = EVENT_EXTERNAL,
		.ext.kind = ARCAN_EVENT(COREOPT)
	};

	size_t msgsz = COUNT_OF(outev.ext.coreopt.data);

/* reset current varset */
	if (retroctx.varset){
		while (retroctx.varset[count].key){
			free((char*)retroctx.varset[count].key);
			free((char*)retroctx.varset[count].value);
			count++;
		}

		free(retroctx.varset);
		retroctx.varset = NULL;
		count = 0;
	}

/* allocate a new set */
	while ( data[count].key )
		count++;

	if (count == 0)
		return;

	count++;
	retroctx.varset = malloc( sizeof(struct core_variable) * count);
	memset(retroctx.varset, '\0', sizeof(struct core_variable) * count);

	count = 0;
	while ( data[count].key ){
		retroctx.varset[count].key = strdup(data[count].key);
		outev.ext.coreopt.index = count;

/* parse, grab the first argument and keep in table,
 * queue the argument as a series of event to the parent */
		if (data[count].value){
			bool gotval = false;
			char* msg = strdup(data[count].value);
			char* workbeg = msg;
			char* workend = msg;

/* message */
			while (*workend && *workend != ';') workend++;

			if (*workend != ';'){
				LOG("malformed core argument (%s:%s)\n", data[count].key,
					data[count].value);
				goto step;
			}
			*workend++ = '\0';

			if (msgsz < strlen(workbeg)){
				LOG("suspiciously long description (%s:%s), %d\n", data[count].key,
					workbeg, (int)msgsz);
				goto step;
			}

/* skip leading whitespace */
		while(*workend && *workend == ' ') workend++;

/* key */
			outev.ext.coreopt.type = 0;
			snprintf((char*)outev.ext.coreopt.data, msgsz, "%s", data[count].key);
			arcan_shmif_enqueue(&retroctx.shmcont, &outev);
/* description */
			outev.ext.coreopt.type = 1;
			snprintf((char*)outev.ext.coreopt.data, msgsz, "%s", workbeg);
			arcan_shmif_enqueue(&retroctx.shmcont, &outev);

/* each option */
startarg:
			workbeg = workend;
			while (*workend && *workend != '|') workend++;

/* treats || as erroneous */
			if (strlen(workbeg) > 0){
				if (*workend == '|')
					*workend++ = '\0';

				if (!gotval && (gotval = true))
					retroctx.varset[count].value = strdup(workbeg);

				outev.ext.coreopt.type = 2;
				snprintf((char*)outev.ext.coreopt.data, msgsz, "%s", workbeg);
				arcan_shmif_enqueue(&retroctx.shmcont, &outev);

				goto startarg;
			}

			const char* curv = lookup_varset(data[count].key);
			if (curv){
				outev.ext.coreopt.type = 3;
				snprintf((char*)outev.ext.coreopt.data, msgsz, "%s", curv);
				arcan_shmif_enqueue(&retroctx.shmcont, &outev);
			}

step:
			free(msg);
		}

		count++;
	}
}

static void libretro_log(enum retro_log_level level, const char* fmt, ...)
{
}

static struct retro_log_callback log_cb = {
	.log = libretro_log
};

static bool libretro_setenv(unsigned cmd, void* data){
	bool rv = true;

	if (!retroctx.shmcont.addr)
		return false;

	switch (cmd){
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:

		switch ( *(enum retro_pixel_format*) data ){
		case RETRO_PIXEL_FORMAT_0RGB1555:
			LOG("pixel format set to RGB1555\n");
			retroctx.converter = (pixconv_fun) libretro_rgb1555_rgba;
		break;

		case RETRO_PIXEL_FORMAT_RGB565:
			LOG("pixel format set to RGB565\n");
			retroctx.converter = (pixconv_fun) libretro_rgb565_rgba;
		break;

		case RETRO_PIXEL_FORMAT_XRGB8888:
			LOG("pixel format set to XRGB8888\n");
			retroctx.converter = (pixconv_fun) libretro_xrgb888_rgba;
		break;

		default:
			LOG("unknown pixelformat encountered (%d).\n", *(unsigned*)data);
			retroctx.converter = NULL;
		}
	break;

	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		*((bool*) data) = true;
	break;

/* ignore for now */
	case RETRO_ENVIRONMENT_SHUTDOWN:
		retroctx.shmcont.addr->dms = true;
		LOG("shutdown requested from lib.\n");
	break;

	case RETRO_ENVIRONMENT_SET_VARIABLES:
		update_varset( (struct retro_variable*) data );
	break;

	case RETRO_ENVIRONMENT_GET_VARIABLE:
		{
			struct retro_variable* arg = (struct retro_variable*) data;
			const char* val = lookup_varset(arg->key);
			if (val){
				arg->value = val;
				LOG("core requested (%s), got (%s)\n", arg->key, arg->value);
				rv = true;
			}
		}
	break;

	case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
/* don't care */
	break;

	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		rv = retroctx.optdirty;
		if (data)
			*(bool*)data = rv;
		retroctx.optdirty = false;
	break;

	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		*((const char**) data) = retroctx.syspath;
		rv = retroctx.syspath != NULL;
		LOG("system directory set to (%s).\n",
			retroctx.syspath ? retroctx.syspath : "MISSING");
	break;

	case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK:
		LOG("frame-time callback unsupported.\n");
		rv = false;
	break;

	case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
		LOG("rumble- interfaces unsupported.\n");
		rv = false;
	break;

	case RETRO_ENVIRONMENT_GET_PERF_INTERFACE:
		LOG("performance- interfaces unsupported.\n");
		rv = false;
	break;

	case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
		retroctx.res_empty = true;
	break;

	case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
		LOG("retro- keyboard callback unsupported.\n");
		rv = false;
	break;

	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		*((struct retro_log_callback*) data) = log_cb;
	break;

	case RETRO_ENVIRONMENT_GET_USERNAME:
		*((const char**) data) = strdup("defusr");
	break;

	case RETRO_ENVIRONMENT_GET_LANGUAGE:
		*((unsigned*) data) = RETRO_LANGUAGE_ENGLISH;
	break;

#ifdef FRAMESERVER_LIBRETRO_3D
	case RETRO_ENVIRONMENT_SET_HW_RENDER | RETRO_ENVIRONMENT_EXPERIMENTAL:
	case RETRO_ENVIRONMENT_SET_HW_RENDER:
	{
/* this should be matched with AGP model rather than statically
 * set that we only care about GL, doesn't look like any core rely
 * on this behavior though */
		struct retro_hw_render_callback* hwrend = data;
		if (hwrend->context_type == RETRO_HW_CONTEXT_OPENGL ||
			hwrend->context_type == RETRO_HW_CONTEXT_OPENGL_CORE){
			setup_3dcore( hwrend );
		}
		else
			LOG("unsupported hw context requested.\n");
	}
	break;
#else
	case RETRO_ENVIRONMENT_SET_HW_RENDER | RETRO_ENVIRONMENT_EXPERIMENTAL:
	case RETRO_ENVIRONMENT_SET_HW_RENDER:
		LOG("trying to load a GL/3D enabled core, but "
			"frameserver was built without 3D support.\n");
		rv = false;
	break;
#endif

	default:
		rv = false;
#ifdef _DEBUG
		LOG("unhandled retro request (%d)\n", cmd);
#endif
	}

	return rv;
}

/*
 * this is quite sensitive to changes in libretro.h
 */
static inline int16_t map_analog_axis(unsigned port, unsigned ind, unsigned id)
{
	ind *= 2;
	ind += id;
	assert(ind < MAX_AXES);

	return (int16_t) retroctx.input_ports[port].axes[ind];
}

/* use the context-tables from retroctx in combination with dev / ind / ... to
 * figure out what to return, this table is populated in flush_eventq(). */
static inline int16_t libretro_inputmain(unsigned port, unsigned dev,
	unsigned ind, unsigned id){
	static bool butn_warning = false;
	static bool port_warning = false;

	if (id > MAX_BUTTONS){
		if (butn_warning == false)
			LOG("unexpectedly high button index (dev:%u)(%u:%%) "
				"requested, ignoring.\n", ind, id);

		butn_warning = true;
		return 0;
	}

	if (port >= MAX_PORTS){
		if (port_warning == false)
			LOG("core requested unknown port (%u:%u:%u), ignored.\n", dev, ind, id);

		port_warning = true;
		return 0;
	}

	int16_t rv = 0;
	struct input_port* inp;

	switch (dev){
		case RETRO_DEVICE_JOYPAD:
			return (int16_t) retroctx.input_ports[port].buttons[id];
		break;

		case RETRO_DEVICE_KEYBOARD:
			if (id < RETROK_LAST)
				rv |= retroctx.kbdtbl[id];
		break;

		case RETRO_DEVICE_MOUSE:
			if (port == 1) port = 0;
			inp = &retroctx.input_ports[port];
			switch (id){
				case RETRO_DEVICE_ID_MOUSE_LEFT:
					return inp->buttons[ inp->cursor_btns[0] ];

				case RETRO_DEVICE_ID_MOUSE_RIGHT:
					return inp->buttons[ inp->cursor_btns[2] ];

				case RETRO_DEVICE_ID_MOUSE_X:
					rv = inp->axes[ inp->cursor_x ];
					inp->axes[ inp->cursor_x ] = 0;
					return rv;

				case RETRO_DEVICE_ID_MOUSE_Y:
					rv = inp->axes[ inp->cursor_y ];
					inp->axes[ inp->cursor_y ] = 0;
					return rv;
			}
		break;

		case RETRO_DEVICE_LIGHTGUN:
			switch (id){
				case RETRO_DEVICE_ID_LIGHTGUN_X:
					return (int16_t) retroctx.input_ports[port].axes[
						retroctx.input_ports[port].cursor_x
					];

				case RETRO_DEVICE_ID_LIGHTGUN_Y:
					return (int16_t) retroctx.input_ports[port].axes[
						retroctx.input_ports[port].cursor_y
					];

				case RETRO_DEVICE_ID_LIGHTGUN_TRIGGER:
					return (int16_t) retroctx.input_ports[port].buttons[
						retroctx.input_ports[port].cursor_btns[0]
					];

				case RETRO_DEVICE_ID_LIGHTGUN_CURSOR:
					return (int16_t) retroctx.input_ports[port].buttons[
						retroctx.input_ports[port].cursor_btns[1]
					];

				case RETRO_DEVICE_ID_LIGHTGUN_START:
					return (int16_t) retroctx.input_ports[port].buttons[
					retroctx.input_ports[port].cursor_btns[2]
				];

					case RETRO_DEVICE_ID_LIGHTGUN_TURBO:
					return (int16_t) retroctx.input_ports[port].buttons[
					retroctx.input_ports[port].cursor_btns[3]
				];

				case RETRO_DEVICE_ID_LIGHTGUN_PAUSE:
					return (int16_t) retroctx.input_ports[port].buttons[
					retroctx.input_ports[port].cursor_btns[4]
				];
		}
		break;

		case RETRO_DEVICE_ANALOG:
			return map_analog_axis(port, ind, id);

		break;

		default:
			LOG("Unknown device ID specified (%d), video will be disabled.\n", dev);
	}

	return 0;
}

static void enable_graphseg()
{
	struct arcan_shmif_cont cont =
		arcan_shmif_acquire(&retroctx.shmcont,
			NULL, SEGID_DEBUG, SHMIF_DISABLE_GUARD);

	if (!cont.addr){
		LOG("segment transfer failed, investigate.\n");
		return;
	}

	if (!arcan_shmif_resize(&cont, 640, 180)){
		LOG("resize failed on debug graph context\n");
		return;
	}

	struct arcan_shmif_cont* pcont = malloc(sizeof(struct arcan_shmif_cont));

	if (retroctx.sync_data)
		retroctx.sync_data->free(&retroctx.sync_data);

	*pcont = cont;
	retroctx.sync_data = setup_synch_graph(pcont, false);
}

static int16_t libretro_inputstate(unsigned port, unsigned dev,
	unsigned ind, unsigned id)
{
	int16_t rv = libretro_inputmain(port, dev, ind, id);
/* indirection to be used for debug graphing what inputs
 * the core actually requested */
	return rv;
}

static int remaptbl[] = {
	RETRO_DEVICE_ID_JOYPAD_A,
	RETRO_DEVICE_ID_JOYPAD_B,
	RETRO_DEVICE_ID_JOYPAD_X,
	RETRO_DEVICE_ID_JOYPAD_Y,
	RETRO_DEVICE_ID_JOYPAD_L,
	RETRO_DEVICE_ID_JOYPAD_R,
	RETRO_DEVICE_ID_JOYPAD_L2,
	RETRO_DEVICE_ID_JOYPAD_R2,
	RETRO_DEVICE_ID_JOYPAD_L3,
	RETRO_DEVICE_ID_JOYPAD_R3
};

/*
 * A static default input layout for apps that provide no
 * higher-level semantic translation on its own.
 */
static void default_map(arcan_ioevent* ioev)
{
	if (ioev->datatype == EVENT_IDATATYPE_TRANSLATED){
		int button = -1;
		int port = -1;

/* happy coincidence, keysyms here match retro_key (as they both
 * originate from SDL) */
		switch(ioev->input.translated.keysym){
		case RETROK_x:
			port = 0;
			button = RETRO_DEVICE_ID_JOYPAD_A;
		break;
		case RETROK_z:
			port = 0;
			button = RETRO_DEVICE_ID_JOYPAD_B;
		break;
		case RETROK_a:
			port = 0;
			button = RETRO_DEVICE_ID_JOYPAD_Y;
		break;
		case RETROK_s:
			port = 0;
			button = RETRO_DEVICE_ID_JOYPAD_X;
		break;
		case RETROK_RETURN:
			port = 0;
			button = RETRO_DEVICE_ID_JOYPAD_START;
		break;
		case RETROK_RSHIFT:
			port = 0;
			button = RETRO_DEVICE_ID_JOYPAD_SELECT;
		break;
		case RETROK_LEFT:
			port = 0;
			button = RETRO_DEVICE_ID_JOYPAD_LEFT;
		break;
		case RETROK_RIGHT:
			port = 0;
			button = RETRO_DEVICE_ID_JOYPAD_RIGHT;
		break;
		case RETROK_UP:
			port = 0;
			button = RETRO_DEVICE_ID_JOYPAD_UP;
		break;
		case RETROK_DOWN:
			port = 0;
			button = RETRO_DEVICE_ID_JOYPAD_DOWN;
		break;
		}

		if (-1 != button && -1 != port){
			retroctx.input_ports[
				port].buttons[button] = ioev->input.translated.active;
		}
	}
	else if (ioev->devkind == EVENT_IDEVKIND_GAMEDEV){
		int port_number = ioev->devid % MAX_PORTS;
		int button_number = ioev->subid % MAX_BUTTONS;
		int button = remaptbl[button_number];
		retroctx.input_ports[
			port_number].buttons[button] = ioev->input.digital.active;
	}
}

static void ioev_ctxtbl(arcan_ioevent* ioev, const char* label)
{
	size_t remaptbl_sz = sizeof(remaptbl) / sizeof(remaptbl[0]) - 1;
	int ind, button = -1, axis;
	char* subtype;

/*
 * if the calling script does no translation of its own
 */
	if (label[0] == '\0'){
		return default_map(ioev);
	}

	if (!retroctx.dirty_input && retroctx.sync_data)
		retroctx.sync_data->mark_input(retroctx.sync_data, arcan_timemillis());

	retroctx.dirty_input = true;

	signed value = ioev->datatype == EVENT_IDATATYPE_TRANSLATED ?
		ioev->input.translated.active : ioev->input.digital.active;

	if (1 == sscanf(label, "PLAYER%d_", &ind) && ind > 0 &&
		ind <= MAX_PORTS && (subtype = strchr(label, '_')) ){
		subtype++;

		if (1 == sscanf(subtype, "BUTTON%d", &button) && button > 0 &&
			button <= MAX_BUTTONS - remaptbl_sz){
			button--;
			button = remaptbl[button];
		}
		else if (1 == sscanf(subtype, "AXIS%d", &axis) && axis > 0
			&& axis <= MAX_AXES){
			retroctx.input_ports[ind-1].axes[ axis - 1 ] =
				ioev->input.analog.axisval[0];
		}
		else if ( strcmp(subtype, "UP") == 0 )
			button = RETRO_DEVICE_ID_JOYPAD_UP;
		else if ( strcmp(subtype, "DOWN") == 0 )
			button = RETRO_DEVICE_ID_JOYPAD_DOWN;
		else if ( strcmp(subtype, "LEFT") == 0 )
			button = RETRO_DEVICE_ID_JOYPAD_LEFT;
		else if ( strcmp(subtype, "RIGHT") == 0 )
			button = RETRO_DEVICE_ID_JOYPAD_RIGHT;
		else if ( strcmp(subtype, "SELECT") == 0 )
			button = RETRO_DEVICE_ID_JOYPAD_SELECT;
		else if ( strcmp(subtype, "START") == 0 )
			button = RETRO_DEVICE_ID_JOYPAD_START;
		else;
		if (button >= 0)
			retroctx.input_ports[ind-1].buttons[button] = value;
	}
	else if (ioev->datatype == EVENT_IDATATYPE_TRANSLATED){
		if (ioev->input.translated.keysym < RETROK_LAST)
			retroctx.kbdtbl[ioev->input.translated.keysym] = value;
	}
}

static void toggle_ntscfilter(int toggle)
{
	if (retroctx.ntscconv && toggle == 0){
		free(retroctx.ntsc_imb);
		retroctx.ntsc_imb = NULL;
		retroctx.ntscconv = false;
	}
	else if (!retroctx.ntscconv && toggle == 1) {
/* malloc etc. happens in resize */
		retroctx.ntscconv = true;
	}
}

static inline void targetev(arcan_event* ev)
{
	arcan_tgtevent* tgt = &ev->tgt;
	switch (tgt->kind){
		case TARGET_COMMAND_RESET:
			retroctx.reset();
		break;

		case TARGET_COMMAND_GRAPHMODE:
			if (tgt->ioevs[0].iv == 1 || tgt->ioevs[1].iv == 2){
				toggle_ntscfilter(tgt->ioevs[1].iv - 1);
			}
			else if (tgt->ioevs[0].iv == 3){
				retroctx.ntscvals[0] = tgt->ioevs[1].fv;
				retroctx.ntscvals[1] = tgt->ioevs[2].fv;
				retroctx.ntscvals[2] = tgt->ioevs[3].fv;
				retroctx.ntscvals[3] = tgt->ioevs[4].fv;
				update_ntsc();
			}
		break;

/* 0 : auto, -1 : single-step, > 0 render every n frames.
 * with 0, the second ioev defines pre-wake. -1 (last frame cost),
 * 0 (whatever), 1..mspf-1
 * ioev[2] audio preemu- frames, whenever the counter is reset,
 * perform n extra run()
 * passes to populate audiobuffer -- increases latency but reduces pops..
 * ioev[3] debugging
 * options -- added emulation cost ms (0 default, +n constant n ms,
 * -n 1..abs(n) random)
 */
		case TARGET_COMMAND_FRAMESKIP:
			retroctx.skipmode    = tgt->ioevs[0].iv;
			retroctx.prewake     = tgt->ioevs[1].iv;
			retroctx.preaudiogen = tgt->ioevs[2].iv;
			retroctx.audbuf_ofs  = 0;
			retroctx.jitterstep  = tgt->ioevs[3].iv;
			retroctx.jitterxfer  = tgt->ioevs[4].iv;
			reset_timing(true);
		break;

/*
 * multiple possible receivers, e.g.
 * retexture transfer page, debugwindow or secondary etc. screens
 */
		case TARGET_COMMAND_NEWSEGMENT:
			if (tgt->ioevs[2].iv == SEGID_DEBUG)
				enable_graphseg();
		break;

/* any event not being UNPAUSE is ignored, no frames are processed
 * and the core is allowed to sleep in between polls */
		case TARGET_COMMAND_PAUSE:
			retroctx.pause = true;
		break;

/* can safely assume there are no other events in the queue after this one,
 * more important for encode etc. that need to flush codecs */
		case TARGET_COMMAND_EXIT:
			exit(EXIT_SUCCESS);
		break;

		case TARGET_COMMAND_UNPAUSE:
			retroctx.pause = false;
			reset_timing(false);
		break;

		case TARGET_COMMAND_DISPLAYHINT:
/* don't do anything about these, scaling is implemented arcan - side */
		break;

		case TARGET_COMMAND_COREOPT:
			retroctx.optdirty = true;
			update_corearg(tgt->code, tgt->message);
		break;

		case TARGET_COMMAND_SETIODEV:
			retroctx.set_ioport(tgt->ioevs[0].iv, tgt->ioevs[1].iv);
		break;

		case TARGET_COMMAND_BUFFER_FAIL:
			retroctx.hpassing_disabled = true;
			LOG("parent requested that we stop try sending buffer handles\n");
		break;

/* should also emit a corresponding event back with the current framenumber */
		case TARGET_COMMAND_STEPFRAME:
			if (tgt->ioevs[0].iv < 0);
				else
					while(tgt->ioevs[0].iv-- && retroctx.shmcont.addr->dms)
						retroctx.run();
		break;

/* store / rewind operate on the last FD set through FDtransfer */
		case TARGET_COMMAND_STORE:
		{
			size_t dstsize = retroctx.serialize_size();
			void* buf;
			if (dstsize && ( buf = malloc( dstsize ) )){

				if ( retroctx.serialize(buf, dstsize) ){
					write_handle( buf, dstsize, ev->tgt.ioevs[0].iv, true );
				} else
					LOG("serialization failed.\n");

				free(buf);
			}
			else
				LOG("snapshot store requested without	any viable target.\n");
		}
		break;

		case TARGET_COMMAND_RESTORE:
		{
			ssize_t dstsize = retroctx.serialize_size();
			size_t ntc = dstsize;
			void* buf;

		if (dstsize && (buf = malloc(dstsize))){
			char* dst = buf;
			while (ntc){
				ssize_t nr = read(ev->tgt.ioevs[0].iv, dst, ntc);
				if (nr == -1){
					if (errno != EINTR && errno != EAGAIN)
						break;
					else
						continue;
				}

				dst += nr;
				ntc -= nr;
			}

			if (ntc == 0){
				retroctx.deserialize( buf, dstsize );
				reset_timing(true);
			}
			else
				LOG("failed restoring from snapshot (%s)\n", strerror(errno));

			free(buf);
		}
		else
			LOG("restore requested but core does not support savestates\n");
		}
		break;

		default:
			LOG("unknown target event (%s), ignored.\n",
				arcan_shmif_eventstr(ev, NULL, 0));
	}
}

/* use labels etc. for trying to populate the context table we also process
 * requests to save state, shutdown, reset, plug/unplug input, here */
static inline void flush_eventq(){
	 arcan_event ev;

	 do
		while (arcan_shmif_poll(&retroctx.shmcont, &ev) > 0){
			switch (ev.category){
				case EVENT_IO:
					ioev_ctxtbl(&(ev.io), ev.io.label);
				break;

				case EVENT_TARGET:
					targetev(&ev);

				default:
				break;
			}
		}
/* Only pause if the DMS isn't released */
		while (retroctx.shmcont.addr->dms &&
			retroctx.pause && (arcan_timesleep(1), 1));
}

void update_ntsc()
{
	static bool init;
	if (!init){
		retroctx.ntsc_opts = snes_ntsc_rgb;
		retroctx.ntscctx = malloc(sizeof(snes_ntsc_t));
		snes_ntsc_init(retroctx.ntscctx, &retroctx.ntsc_opts);
		init = true;
	}

	snes_ntsc_update_setup(
		retroctx.ntscctx, &retroctx.ntsc_opts,
		retroctx.ntscvals[0], retroctx.ntscvals[1],
		retroctx.ntscvals[2], retroctx.ntscvals[3]);
}

/* return true if we're in synch (may sleep),
 * return false if we're lagging behind */
static inline bool retroctx_sync()
{
	long long int timestamp = arcan_timemillis();
	retroctx.vframecount++;

/* only skip (at most) 1 frame */
	if (retroctx.skipframe_v || retroctx.empty_v)
		return true;

	long long int now  = timestamp - retroctx.basetime;
	long long int next = floor( (double)retroctx.vframecount * retroctx.mspf );
	int left = next - now;

/* ntpd, settimeofday, wonky OS etc. or some massive stall, disqualify
 * DEBUGSTALL for the normal timing thing, or even switching 3d settings */
	static int checked;

	if (abs(left) > 200){
		if (checked == 0){
			checked = getenv("ARCAN_FRAMESERVER_DEBUGSTALL") ? -1 : 1;
		}
		else if (checked == 1){
			LOG("frameskip stall detected, resetting timers.\n");
			reset_timing(false);
		}
		return true;
	}

/* more than half a frame behind? skip */
	if ( retroctx.skipmode != TARGET_SKIP_AUTO)
		return true;

	if (left < -0.5 * retroctx.mspf){
		if (retroctx.sync_data)
			retroctx.sync_data->mark_drop(retroctx.sync_data, timestamp);
		LOG("frameskip: at(%lld), next: (%lld), "
			"deviation: (%d)\n", now, next, left);
		retroctx.frameskips++;
		return false;
	}

/* since we have to align the transfer with the parent, and it's better to
 * under- than overshoot- a deadline in that respect, prewake tries to
 * compensate lightly for scheduling jitter etc. */
	if (left > retroctx.prewake){
		arcan_timesleep( left - retroctx.prewake );
	}

	return true;
}

/*
 * used for debugging / testing synchronization during various levels of harsh
 * synchronization costs
 */
static inline long long add_jitter(int num)
{
	long long start = arcan_timemillis();
	if (num < 0)
		arcan_timesleep( rand() % abs(num) );
	else if (num > 0)
		arcan_timesleep( num );
	long long stop = arcan_timemillis();
	return stop - start;
}

/*
 * A selected few cores need a fully working GL context and then
 * emit the output as an FBO. We currently lack a good way of
 * sharing the output texture with the parent process, so
 * we initialize a dumb "1x1" window with the FBO being our
 * desired output resolution, and then doing a readback
 * into the shmpage
 */
#ifdef FRAMESERVER_LIBRETRO_3D
static void* get_gfxsym(const char* symname)
{
	void* ret = platform_video_gfxsym(symname);
#ifdef _DEBUG
	if (!ret)
		LOG("(GL) couldn't resolve %s\n", symname);
#endif
	return ret;
}
static uintptr_t get_framebuffer()
{
	uintptr_t tgt, col, depth;

	if (!retroctx.rtgt){
		agp_empty_vstore(&retroctx.vstore,
			retroctx.shmcont.addr->w, retroctx.shmcont.addr->h);
		retroctx.rtgt = agp_setup_rendertarget(
			&retroctx.vstore, RENDERTARGET_COLOR_DEPTH_STENCIL);
	}

	agp_rendertarget_ids(retroctx.rtgt, &tgt, &col, &depth);
	return tgt;
}

static void setup_3dcore(struct retro_hw_render_callback* ctx)
{
/*
 * cheat with some envvars as the agp_ interface because it was not designed
 * to handle these sort of 'someone else decides which version to use'
 */
	if (ctx->context_type == RETRO_HW_CONTEXT_OPENGL_CORE){
		char tmpbuf[8];
		snprintf(tmpbuf, 8, "%d", ctx->version_major);
		setenv("AGP_GL_MAJOR", tmpbuf, 0);
		snprintf(tmpbuf, 8, "%d", ctx->version_minor);
		setenv("AGP_GL_MINOR", tmpbuf, 0);
		LOG("Switching to GL CORE context (%d, %d)\n",
			ctx->version_major, ctx->version_minor);
	}
/* we just want a dummy window with a valid openGL context
 * bound and then set up a FBO with the proper dimensions,
 * when things are working, just use a 2x2 window and minimize */
	if (!platform_video_init(640, 480, 32, false, true, "libretro")){
		LOG("Couldn't setup OpenGL context\n");
		exit(1);
	}

	agp_init();
	retroctx.in_3d = true;

#ifdef FRAMSESERVER_LIBRETRO_3D_RETEXTURE
	exit(1); /* not ready */
	arcan_retexture_init(NULL, false);

/*
 * allocate an input and an output segment and map up, the socket file
 * descriptors will just be ignored here as the main thread will be used to
 * pump the queues and the shared memory segments will be used to push data
 */
	arcan_event ev = {
		.category = EVENT_EXTERNAL,
		.kind = ARCAN_EVENT(SEGREQ)
	};

	arcan_event_enqueue(&retroctx.shmcont.outev, &ev);
#endif

	ctx->get_current_framebuffer = get_framebuffer;
	ctx->get_proc_address = (retro_hw_get_proc_address_t) get_gfxsym;

	memcpy(&retroctx.hwctx, ctx,
		sizeof(struct retro_hw_render_callback));
}
#endif

static void map_lretrofun()
{
/* map normal functions that will be called repeatedly */
	retroctx.run = (void(*)()) libretro_requirefun("retro_run");
	retroctx.reset = (void(*)()) libretro_requirefun("retro_reset");
	retroctx.load_game = (bool(*)(const struct retro_game_info* game))
		libretro_requirefun("retro_load_game");
	retroctx.serialize = (bool(*)(void*, size_t))
		libretro_requirefun("retro_serialize");
	retroctx.set_ioport = (void(*)(unsigned,unsigned))
		libretro_requirefun("retro_set_controller_port_device");
	retroctx.deserialize = (bool(*)(const void*, size_t))
		libretro_requirefun("retro_unserialize");
	retroctx.serialize_size = (size_t(*)())
		libretro_requirefun("retro_serialize_size");

/* setup callbacks */
	( (void(*)(retro_video_refresh_t) )
		libretro_requirefun("retro_set_video_refresh"))(libretro_vidcb);
	( (size_t(*)(retro_audio_sample_batch_t))
		libretro_requirefun("retro_set_audio_sample_batch"))(libretro_audcb);
	( (void(*)(retro_audio_sample_t))
		libretro_requirefun("retro_set_audio_sample"))(libretro_audscb);
	( (void(*)(retro_input_poll_t))
		libretro_requirefun("retro_set_input_poll"))(libretro_pollcb);
	( (void(*)(retro_input_state_t))
		libretro_requirefun("retro_set_input_state") )(libretro_inputstate);
}

/* might need to add another subgrammar here to handle multiple file-
 * images (another ??, why not just populate an array with images and a
 * swap- function.. */
static bool load_resource(const char* resname)
{
	char logbuf[128];
	size_t logbuf_sz = sizeof(logbuf);

/* rather ugly -- core actually requires file-path */
	if (retroctx.sysinfo.need_fullpath){
		LOG("core(%s), core requires fullpath, resolved to (%s).\n",
			retroctx.sysinfo.library_name, resname );

		retroctx.gameinfo.data = NULL;
		retroctx.gameinfo.path = strdup( resname );
		retroctx.gameinfo.size = 0;
	}
	else {
		retroctx.gameinfo.path = strdup( resname );
		data_source res = arcan_open_resource(resname);
		map_region map = arcan_map_resource(&res, true);
		if (!map.ptr){
			snprintf(logbuf, logbuf_sz, "couldn't map (%s)", resname?resname:"");
			log_msg(logbuf, true);
			LOG("%s\n", logbuf);
			return false;
		}
		retroctx.gameinfo.data = map.ptr;
		retroctx.gameinfo.size = map.sz;
	}

	snprintf(logbuf, logbuf_sz, "loading game...");
	log_msg(logbuf, true);

	if ( retroctx.load_game( &retroctx.gameinfo ) == false ){
		snprintf(logbuf, logbuf_sz, "loading failed");
		log_msg(logbuf, true);
		return false;
	}

	return true;
}

static void setup_av()
{
/* load the game, and if that fails, give up */
#ifdef FRAMESERVER_LIBRETRO_3D
	if (retroctx.hwctx.context_reset)
		retroctx.hwctx.context_reset();
#endif

	( (void(*)(struct retro_system_av_info*))
		libretro_requirefun("retro_get_system_av_info"))(&retroctx.avinfo);

/* setup frameserver, synchronization etc. */
	assert(retroctx.avinfo.timing.fps > 1);
	assert(retroctx.avinfo.timing.sample_rate > 1);
	retroctx.mspf = ( 1000.0 * (1.0 / retroctx.avinfo.timing.fps) );

/* estimate buffer size to store one frame */
	retroctx.aframesz = (float)ARCAN_SHMIF_SAMPLERATE /
		retroctx.avinfo.timing.fps *
		ARCAN_SHMIF_SAMPLE_SIZE * ARCAN_SHMIF_ACHANNELS * 2;
	LOG("audioframe size: %f b\n", retroctx.aframesz);

	retroctx.ntscconv = false;

	LOG("video timing: %f fps (%f ms), audio samplerate: %f Hz\n",
		(float)retroctx.avinfo.timing.fps, (float)retroctx.mspf,
		(float)retroctx.avinfo.timing.sample_rate);

	LOG("setting up resampler, %f => %d.\n",
		(float)retroctx.avinfo.timing.sample_rate, ARCAN_SHMIF_SAMPLERATE);

	int errc;
	retroctx.resampler = speex_resampler_init(ARCAN_SHMIF_ACHANNELS,
		retroctx.avinfo.timing.sample_rate, ARCAN_SHMIF_SAMPLERATE, 5, &errc);

/*
 * just prepare some (overly) large audio resampling buffers
 */
	retroctx.audbuf_sz = retroctx.avinfo.timing.sample_rate * 4;
	retroctx.in_audb = malloc(retroctx.audbuf_sz);
	retroctx.out_audb = malloc(retroctx.audbuf_sz);
}

static void setup_input()
{
/* setup standard device remapping tables, these can be changed
 * by the calling process with a corresponding target event. */
	for (int i = 0; i < MAX_PORTS; i++){
		retroctx.input_ports[i].cursor_x = 0;
		retroctx.input_ports[i].cursor_y = 1;
		retroctx.input_ports[i].cursor_btns[0] = 0;
		retroctx.input_ports[i].cursor_btns[1] = 1;
		retroctx.input_ports[i].cursor_btns[2] = 2;
		retroctx.input_ports[i].cursor_btns[3] = 3;
		retroctx.input_ports[i].cursor_btns[4] = 4;
	}

	retroctx.state_sz = retroctx.serialize_size();
	arcan_shmif_enqueue(&retroctx.shmcont, &(struct arcan_event){
		.category = EVENT_EXTERNAL,
		.ext.kind = ARCAN_EVENT(STATESIZE),
		.ext.stateinf.size = retroctx.state_sz
	});
}

static void dump_help()
{
	fprintf(stdout, "ARCAN_ARG (environment variable, "
		"key1=value:key2:key3=value), arguments:\n"
		"   key   \t   value   \t   description\n"
		"---------\t-----------\t-----------------\n"
		" core    \t filename  \t relative path to libretro core (req)\n"
		" info    \t           \t load core, print information and quit\n"
		" syspath \t path      \t set core system path\n"
		" resource\t filename  \t resource file to load with core\n"
		"---------\t-----------\t-----------------\n"
	);
	fprintf(stdout, "ENVIRONMENT VARIABLES:\n"
		"   key        \t   value   \t   description:\n"
		"--------------\t-----------\t-----------------\n"
		" GAME_ABUFC   \t 1..16 (8) \t number of audio buffers\n"
		" GAME_ABUFSZ  \t bytes (1) \t size of each audio buffer, 1 = probe\n"
		" GAME_VBUFC   \t 1..4 (1)  \t number of video buffers\n"
		" GAME_NORESET \t           \t (3d) set to disable context reset calls\n"
	);
}

/* map up a libretro compatible library resident at fullpath:game,
 * if resource is /info, no loading will occur but a dump of the capabilities
 * of the core will be sent to stdout. */
int	afsrv_game(struct arcan_shmif_cont* cont, struct arg_arr* args)
{
	if (!cont || !args){
		dump_help();
		return EXIT_FAILURE;
	}

	retroctx.converter = (pixconv_fun) libretro_rgb1555_rgba;
	retroctx.inargs = args;
	retroctx.shmcont = *cont;

	const char* libname = NULL;
	const char* resname = NULL;
	const char* val;

	if (arg_lookup(args, "core", 0, &val))
		libname = strdup(val);

	if (arg_lookup(args, "resource", 0, &val))
		resname = strdup(val);

	if (getenv("ARCAN_VIDEO_NO_FDPASS")){
		retroctx.hpassing_disabled = true;
	}

	if ((val = getenv("GAME_ABUFC"))){
		uint8_t bufc = strtoul(val, NULL, 10);
		retroctx.abuf_cnt = bufc > 0 && bufc < 16 ? bufc : 8;
	}

	if ((val = getenv("GAME_VBUFC"))){
		uint8_t bufc = strtoul(val, NULL, 10);
		retroctx.abuf_cnt = bufc > 0 && bufc <= 4 ? bufc : 1;
	}

	if ((val = getenv("GAME_ABUFSZ")))
		retroctx.def_abuf_sz = strtoul(val, NULL, 10);

/* system directory doesn't really match any of arcan namespaces,
 * provide some kind of global-  user overridable way */
	const char* spath = getenv("ARCAN_LIBRETRO_SYSPATH");
	if (!spath)
		spath = "./";

	if (arg_lookup(args, "syspath", 0, &val))
		spath = val;

/* some cores (mednafen-psx, ..) currently breaks on relative paths,
 * so resolve to absolute one for the time being */
	retroctx.syspath = realpath(spath, NULL);

/* set if we only want to dump status about the core, info etc.  (which
 * incidentally was then moved to yet another format to parse and manage as
 * a separate file, not an embedded string in core.. */
	bool info_only = arg_lookup(args, "info", 0, NULL) || cont->addr == NULL;

	if (!libname || *libname == 0){
		LOG("error > No core specified.\n");
		dump_help();

		return EXIT_FAILURE;
	}

	if (!info_only)
		LOG("Loading core (%s) with resource (%s)\n", libname ?
			libname : "missing arg.", resname ? resname : "missing resarg.");

	char logbuf[128] = {0};
	size_t logbuf_sz = sizeof(logbuf);

	resize_shmpage(320, 240, true);

	if (snprintf(logbuf, logbuf_sz, "loading(%s)", libname) >= logbuf_sz)
		logbuf[logbuf_sz-1] = '\0';
	log_msg(logbuf, true);

/* map up functions and test version */
	lastlib = dlopen(libname, RTLD_LAZY);
	if (!globallib)
		globallib = dlopen(NULL, RTLD_LAZY);

	if (!lastlib){
		snprintf(logbuf, logbuf_sz, "Couldn't open (%s), giving up.\n", libname);
		log_msg(logbuf, true);
		LOG("couldn't open library (%s), giving up.\n", libname);
		exit(EXIT_FAILURE);
	}

	void (*initf)() = libretro_requirefun("retro_init");
	unsigned (*apiver)() = (unsigned(*)())
		libretro_requirefun("retro_api_version");

	( (void(*)(retro_environment_t))
		libretro_requirefun("retro_set_environment"))(libretro_setenv);

/* get the lib up and running, ensure that the version matches
 * the one we got from the header */
	if (!( (initf(), true) && apiver() == RETRO_API_VERSION) )
		return EXIT_FAILURE;

	((void(*)(struct retro_system_info*))
	 libretro_requirefun("retro_get_system_info"))(&retroctx.sysinfo);

	if (info_only){
		fprintf(stdout, "arcan_frameserver(info)\nlibrary:%s\n"
			"version:%s\nextensions:%s\n/arcan_frameserver(info)",
			retroctx.sysinfo.library_name, retroctx.sysinfo.library_version,
			retroctx.sysinfo.valid_extensions);
		return EXIT_FAILURE;
	}

	LOG("libretro(%s), version %s loaded. Accepted extensions: %s\n",
		retroctx.sysinfo.library_name, retroctx.sysinfo.library_version,
		retroctx.sysinfo.valid_extensions);

/* map functions to context structure */
/* send some information on what core is actually loaded etc. */
	arcan_event outev = {
		.category = EVENT_EXTERNAL,
		.ext.kind = ARCAN_EVENT(IDENT)
	};

	size_t msgsz = COUNT_OF(outev.ext.message.data);
	snprintf((char*)outev.ext.message.data, msgsz, "%s %s",
		retroctx.sysinfo.library_name, retroctx.sysinfo.library_version);
	arcan_shmif_enqueue(&retroctx.shmcont, &outev);

/* map the functions we need during runtime */
	map_lretrofun();

/* load / start */
	if (!resname && retroctx.res_empty)
		;
	else if (!load_resource(resname ? resname : ""))
		return EXIT_FAILURE;

	snprintf((char*)outev.ext.message.data, msgsz, "loaded");
	arcan_shmif_enqueue(&retroctx.shmcont, &outev);

/* remixing, conversion functions for color formats... */
	setup_av();

/* default input tables, state management */
	setup_input();

/* since we're 'guaranteed' to get at least one input callback each run(),
 * call, we multiplex parent event processing as well */
	outev.ext.framestatus.framenumber = 0;

/* some cores die on this kind of reset, retroctx.reset() e.g. NXengine
 * retro_reset() */

/* since we might have requests to save state before we die, we use the
 * flush_eventq as an atexit */
	atexit(flush_eventq);

	if (retroctx.state_sz > 0)
		retroctx.rollback_state = malloc(retroctx.state_sz);

/* basetime is used as epoch for all other timing calculations, run
 * an initial frame because sometimes first run can introduce a large stall */
	retroctx.skipframe_v = retroctx.skipframe_a = true;
	retroctx.run();
	retroctx.skipframe_v = retroctx.skipframe_a = false;
	retroctx.basetime = arcan_timemillis();

/* pre-audio is a last- resort to work around buffering size issues
 * in audio layers -- run one or more frames of emulation, ignoring
 * timing and input, and just keep the audioframes */
	do_preaudio();
	long long int start, stop;

/* don't want the UI to draw a mouse cursor in this window */
	arcan_shmif_enqueue(&retroctx.shmcont, &(struct arcan_event){
		.category = EVENT_EXTERNAL,
		.ext.kind = ARCAN_EVENT(CURSORHINT),
		.ext.message = "hidden"
	});

	while (retroctx.shmcont.addr->dms){
/* since pause and other timing anomalies are part of the eventq flush,
 * take care of it outside of frame frametime measurements */
		flush_eventq();

		if (retroctx.skipmode >= TARGET_SKIP_FASTFWD)
			libretro_skipnframes(retroctx.skipmode -
				TARGET_SKIP_FASTFWD + 1, true);

		else if (retroctx.skipmode >= TARGET_SKIP_STEP)
			libretro_skipnframes(retroctx.skipmode -
				TARGET_SKIP_STEP + 1, false);

		else if (retroctx.skipmode <= TARGET_SKIP_ROLLBACK &&
			retroctx.dirty_input){
/* last entry will always be the current front */
			retroctx.deserialize(retroctx.rollback_state +
				retroctx.state_sz * retroctx.rollback_front, retroctx.state_sz);

/* rollback to desired "point", run frame (which will consume input)
 * then roll forward to next video frame */
			process_frames(retroctx.rollback_window - 1, true, true);
			retroctx.dirty_input = false;
		}

		testcounter = 0;

/* add jitter, jitterstep, framecost etc. are used for debugging /
 * testing by adding delays at various key synchronization points */
		start = arcan_timemillis();
			add_jitter(retroctx.jitterstep);
			process_frames(1, false, false);
		stop = arcan_timemillis();
		retroctx.framecost = stop - start;
		if (retroctx.sync_data){
			retroctx.sync_data->mark_start(retroctx.sync_data, start);
			retroctx.sync_data->mark_stop(retroctx.sync_data, stop);
		}

/* Some FE applications need a grasp of "where" we are frame-wise,
 * particularly for single-stepping etc. */
		outev.ext.kind = ARCAN_EVENT(FRAMESTATUS);
		outev.ext.framestatus.framenumber++;
		arcan_shmif_enqueue(&retroctx.shmcont, &outev);

#ifdef _DEBUG
		if (testcounter != 1){
			static bool countwarn = 0;
			if (!countwarn && (countwarn = true))
				LOG("inconsistent core behavior, "
					"expected 1 video frame / run(), got %d\n", testcounter);
		}
#endif

/* if we start lagging behind on frametime, try selectively skipping frames */
		retroctx.skipframe_a = false;
		retroctx.skipframe_v = !retroctx_sync();

/* begin with synching video, as it is the one with the biggest deadline
 * penalties and the cost for resampling can be enough if we are close */
		if (!retroctx.empty_v){
			long long elapsed = add_jitter(retroctx.jitterstep);
#ifdef FRAMESERVER_LIBRETRO_3D
			if (-1 != retroctx.last_handle){
				elapsed += arcan_shmif_signalhandle(&retroctx.shmcont,
					SHMIF_SIGVID,
					retroctx.last_handle,
					retroctx.vstore.vinf.text.stride,
					retroctx.vstore.vinf.text.format
				);
				close(retroctx.last_handle);
				retroctx.last_handle = -1;
			}
			else
#endif
			elapsed += arcan_shmif_signal(&retroctx.shmcont, SHMIF_SIGVID);
			retroctx.transfercost = elapsed;
			if (retroctx.sync_data)
				retroctx.sync_data->mark_transfer(retroctx.sync_data,
					stop, retroctx.transfercost);
		}

		if(retroctx.audbuf_ofs){
			spx_uint32_t inc = retroctx.audbuf_ofs >> 1; /* per channel, 2 chan */
			spx_uint32_t left = retroctx.audbuf_sz;
			retroctx.audbuf_ofs = 0;
			speex_resampler_process_interleaved_int(retroctx.resampler,
				retroctx.in_audb, &inc, retroctx.out_audb, &left);

			left *= sizeof(shmif_asample) * ARCAN_SHMIF_ACHANNELS;
			uint8_t* inb = (uint8_t*) retroctx.out_audb;
			size_t bufsz = retroctx.shmcont.abufsize;

			while (left){
				uint8_t* outb = (uint8_t*) retroctx.shmcont.audp;
				size_t limit = retroctx.shmcont.abufsize - retroctx.shmcont.abufused;

				if (left >= limit){
					memcpy(&outb[retroctx.shmcont.abufused], inb, limit);
					left -= limit;
					inb += limit;
					retroctx.shmcont.abufused = retroctx.shmcont.abufsize;
					arcan_shmif_signal(&retroctx.shmcont, SHMIF_SIGAUD);
				}
				else{
					memcpy(&outb[retroctx.shmcont.abufused], inb, left);
					retroctx.shmcont.abufused += left;
					break;
				}
			}
		}

		if (retroctx.sync_data)
				push_stats();
	}
	return EXIT_SUCCESS;
}

static void log_msg(char* msg, bool flush)
{
	draw_box(&retroctx.shmcont, 0, 0,
		retroctx.shmcont.w, retroctx.shmcont.h,
		RGBA(0x00, 0x00, 0x00, 0xff)
	);

	int dw, dh;
	int sw = retroctx.shmcont.w;

/* clip string */
	char* mendp = msg + strlen(msg) + 1;
	do {
		*mendp = '\0';
		text_dimensions(&retroctx.shmcont, msg, &dw, &dh);
		mendp--;
	} while (dw > sw && mendp > msg);

/* center */
	draw_text(&retroctx.shmcont, msg,
		0.5 * (sw - dw), 0.5 * (retroctx.shmcont.h - dh), 0xffffffff);

	if (flush)
		arcan_shmif_signal(&retroctx.shmcont, SHMIF_SIGVID | SHMIF_SIGBLK_NONE);
}

static void push_stats()
{
	char scratch[512];
	long long int timestamp = arcan_timemillis();

	snprintf(scratch, 512, "%s, %s\n"
		"%s, %f fps, %f Hz\n"
		"Mode: %d, Preaudio: %d\n Jitter: %d/%d\n"
		"(A,V - A/V) %lld, %lld - %lld\n"
		"Real (Hz): %f\n"
		"cost,wake,xfer: %d, %d, %d ms \n",
		(char*)retroctx.sysinfo.library_name,
		(char*)retroctx.sysinfo.library_version,
		(char*)retroctx.colorspace,
		(float)retroctx.avinfo.timing.fps,
		(float)retroctx.avinfo.timing.sample_rate,
		retroctx.skipmode, retroctx.preaudiogen,
		retroctx.jitterstep, retroctx.jitterxfer,
		retroctx.aframecount, retroctx.vframecount,
		retroctx.aframecount / retroctx.vframecount,
		1000.0f * (float)retroctx.aframecount /
			(float)(timestamp - retroctx.basetime),
		retroctx.framecost, retroctx.prewake, retroctx.transfercost
	);

	if (!retroctx.sync_data->update(
		retroctx.sync_data, retroctx.mspf, scratch)){
		retroctx.sync_data->free(&retroctx.sync_data);
	}
}
