/*
 Arcan Shared Memory Interface

 Copyright (c) 2012-2016, Bjorn Stahl
 All rights reserved.

 Redistribution and use in source and binary forms,
 with or without modification, are permitted provided that the
 following conditions are met:

 1. Redistributions of source code must retain the above copyright notice,
 this list of conditions and the following disclaimer.

 2. Redistributions in binary form must reproduce the above copyright notice,
 this list of conditions and the following disclaimer in the documentation
 and/or other materials provided with the distribution.

 3. Neither the name of the copyright holder nor the names of its contributors
 may be used to endorse or promote products derived from this software without
 specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
 OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef _HAVE_ARCAN_SHMIF_CONTROL
#define _HAVE_ARCAN_SHMIF_CONTROL

/*
 * This header defines the interface and support functions for
 * shared memory- based communication between the arcan parent
 * and frameservers/non-authoritative clients.
 *
 * For extended documentation on how this interface works, design
 * rationale, changes and so on, please refer to the wiki @
 * https://github.com/letoram/arcan/wiki/Shmif
 */

/*
 * These prefixes change the search and namespacing rules for how a
 * non-authoritative connection should find a running arcan server based
 * on a key.
 */
#ifdef __linux
#ifndef ARCAN_SHMIF_PREFIX
#define ARCAN_SHMIF_PREFIX "\0arcan_"
#endif

/* If the first character does not begin with /, HOME env will be used. */
#else
#ifndef ARCAN_SHMIF_PREFIX
#define ARCAN_SHMIF_PREFIX ".arcan_"
#endif
#endif

/*
 * Default permissions / mask that listening sockets will be created under
 */
#ifndef ARCAN_SHM_UMASK
#define ARCAN_SHM_UMASK (S_IRWXU | S_IRWXG)
#endif

/*
 * Compile-time constants that define the size and layout
 * of the shared structure. These values are part in defining the ABI
 * and should therefore only be tuned when you have control of the
 * whole-system compilation and packaging (OS distributions, embedded
 * systems).
 */

/*
 * Define the reserved ring-buffer space used for input and output events
 * must be 0 < PP_QUEUE_SZ < 256
 */
#ifndef PP_QUEUE_SZ
#define PP_QUEUE_SZ 32
#endif
static const int ARCAN_SHMIF_QUEUE_SZ = PP_QUEUE_SZ;

/*
 * Audio format and basic parameters, this is kept primitive on purpose.
 * This will be revised shortly, but modifying still breaks ABI and may
 * break current frameservers.
 */
#ifndef AUDIO_SAMPLE_TYPE
#define AUDIO_SAMPLE_TYPE int16_t
#endif

/*
 * ALWAYS interleaved
 */
typedef VIDEO_PIXEL_TYPE shmif_asample;
static const int ARCAN_SHMIF_SAMPLERATE = 48000;
static const int ARCAN_SHMIF_ACHANNELS = 2;
static const int ARCAN_SHMIF_SAMPLE_SIZE = sizeof(shmif_asample);

/*
 * Both SHMIF_AFLOAT / SHMIF_AINT16 macros and expansion to later support
 * a static configurable format / packing (and the similar version for
 * SHMIF_RGBA, ...) would probably benefit from a reimplementation as C11
 * type generic macros (are there any tricks to get these to expand on
 * variadic as well in order to satisfy channels and downmixing)
 */
#ifndef SHMIF_AFLOAT
#define SHMIF_AFLOAT(X) ( (int16_t) ((X) * 32767.0) ) /* sacrifice -32768 */
#endif

#ifndef SHMIF_AINT16
#define SHMIF_AINT16(X) ( (int16_t) ((X))
#endif

/*
 * This is the TOTAL size of the audio output buffer, we can slice this
 * into evenly sized chunks during _resize using _ext version should we
 * want to work with smaller buffers and less blocking.
 *
 * shmpage->abufsize is the user- exposed, currently negotiated size.
 */
static const int ARCAN_SHMIF_AUDIOBUF_SZ = 65535;

/*
 * These are technically limited by the combination of graphics and video
 * platforms. Since the buffers are placed at the end of the struct, they
 * can be changed without breaking ABI though several resize requests may
 * be rejected.
 */
#ifndef PP_SHMPAGE_MAXW
#define PP_SHMPAGE_MAXW 4096
#endif
static const int ARCAN_SHMPAGE_MAXW = PP_SHMPAGE_MAXW;


#ifndef PP_SHMPAGE_MAXH
#define PP_SHMPAGE_MAXH 2048
#endif
static const int ARCAN_SHMPAGE_MAXH = PP_SHMPAGE_MAXH;

/*
 * Identification token that may need to be passed when making a socket
 * connection to the main arcan process.
 */
#ifndef PP_SHMPAGE_SHMKEYLIM
#define PP_SHMPAGE_SHMKEYLIM 32
#endif

/*
 * We abstract the base type for a pixel and provide a packing macro in order
 * to permit systems with lower memory to switch to uint16 RGB565 style
 * formats, and to permit future switches to higher depth/range.  The
 * separation between video_platform definition of these macros also allows a
 * comparison between engine internals and interface to warn or convert.
 */
#ifndef VIDEO_PIXEL_TYPE
#define VIDEO_PIXEL_TYPE uint32_t
#endif

#ifndef ARCAN_SHMPAGE_VCHANNELS
#define ARCAN_SHMPAGE_VCHANNELS 4
#endif

#ifndef ARCAN_SHMPAGE_DEFAULT_PPCM
#define ARCAN_SHMPAGE_DEFAULT_PPCM 28.34
#endif

static const float shmif_ppcm_default = ARCAN_SHMPAGE_DEFAULT_PPCM;

typedef VIDEO_PIXEL_TYPE shmif_pixel;
#ifndef SHMIF_RGBA
#define SHMIF_RGBA(r, g, b, a)( ((uint32_t)(a) << 24) | ((uint32_t) (b) << 16)\
| ((uint32_t) (g) << 8) | ((uint32_t) (r)) )
#endif

#ifndef SHMIF_RGBA_DECOMP
static inline void SHMIF_RGBA_DECOMP(shmif_pixel val,
	uint8_t* r, uint8_t* g, uint8_t* b, uint8_t* a)
{
	*r = (val & 0x000000ff);
	*g = (val & 0x0000ff00) >>  8;
	*b = (val & 0x00ff0000) >> 16;
	*a = (val & 0xff000000) >> 24;
}
#endif

/*
 * Reasonable starting dimensions, this can be changed without breaking ABI
 * as parent/client will initiate a resize based on gain relative to the
 * current size.
 *
 * It should, at least, fit 32*32*sizeof(shmif_pixel) + sizeof(struct) +
 * sizeof event*PP_QUEUE_SIZE*2 + PP_AUDIOBUF_SZ with alignment padding.
 */
#ifndef PP_SHMPAGE_STARTSZ
#define PP_SHMPAGE_STARTSZ 2014088
#endif

/*
 * This is calculated through MAXW*MAXH*sizeof(shmif_pixel) + sizeof
 * struct + sizeof event*PP_QUEUE_SIZE*2 + PP_AUDIOBUF_SZ with alignment.
 * (too bad constexpr isn't part of C11)
 * It is primarily of concern when OVERCOMMIT build is used where it isn't
 * possible to resize dynamically.
 */
#ifndef PP_SHMPAGE_MAXSZ
#define PP_SHMPAGE_MAXSZ 48294400
#endif
static const int ARCAN_SHMPAGE_MAX_SZ = PP_SHMPAGE_MAXSZ;

/*
 * Overcommit is a specialized build mode (that should be avoided if possible)
 * that sets the initial segment size to PP_SHMPAGE_STARTSZ and no new buffer
 * dimension negotiation will occur.
 */
#ifdef ARCAN_SHMIF_OVERCOMMIT
static const int ARCAN_SHMPAGE_START_SZ = PP_SHMPAGE_MAXSZ;
#else
static const int ARCAN_SHMPAGE_START_SZ = PP_SHMPAGE_STARTSZ;
#endif

/*
 * Two primary transfer operation types, from the perspective of the
 * main arcan application (i.e. normally frameservers feed INPUT but
 * specialized recording segments are flagged as OUTPUT. Internally,
 * these have different synchronization rules.
 */
enum arcan_shmif_type {
	SHMIF_INPUT = 1,
	SHMIF_OUTPUT
};

/*
 * This enum defines the possible operations for audio and video
 * synchronization (both or either) and how locking should behave.
 */
enum arcan_shmif_sigmask {
	SHMIF_SIGVID = 1,
	SHMIF_SIGAUD = 2,

/* synchronous, wait for parent to acknowledge */
	SHMIF_SIGBLK_FORCE = 0,

/* return immediately, further writes may cause tearing and other
 * visual/aural artifacts */
	SHMIF_SIGBLK_NONE  = 4,

/* return immediately unless there is already a transfer pending */
	SHMIF_SIGBLK_ONCE = 8
};

struct arcan_shmif_cont;
struct shmif_hidden;
struct arcan_shmif_page;

typedef enum arcan_shmif_sigmask(
	*shmif_trigger_hook)(struct arcan_shmif_cont*);

enum ARCAN_FLAGS {
/* by default, the connection IPC resources are unlinked, this
 * may not always be desired (debugging, monitoring, ...) */
	SHMIF_DONT_UNLINK = 1,

/* a guard thread is usually allocated to monitor the status of
 * the server, setting this flag explicitly prevents the creation of
 * that thread */
	SHMIF_DISABLE_GUARD = 2,

/* failure to acquire a segment should be exit(EXIT_FAILURE); */
	SHMIF_ACQUIRE_FATALFAIL = 4,

/* if FATALFAIL, do we have a custom function? should be first argument */
	SHMIF_FATALFAIL_FUNC = 8,

/* set to sleep- try spin until a connection is established */
	SHMIF_CONNECT_LOOP = 16,

/* don't implement pause/resume management in backend, forward the
 * events to frontend */
	SHMIF_MANUAL_PAUSE = 32
};

/*
 * Convenience wrapper function of checking environment variables
 * for packed arguments, connection path / key etc.
 *
 * Will also clean-up / reset related environments
 * to prevent propagation.
 *
 * If no arguments could be unpacked, *arg_arr will be set to NULL.
 */
struct arg_arr;
struct arcan_shmif_cont arcan_shmif_open(
	enum ARCAN_SEGID type, enum ARCAN_FLAGS flags, struct arg_arr**);

/*
 * This is used to make a non-authoritative connection using
 * a domain- socket as a connection point (as specified by the
 * connpath and optional connkey).
 *
 * Will return NULL or a user-managed string with a key
 * suitable for shmkey, along with a file descriptor to the
 * connected socket in *conn_ch
 */
char* arcan_shmif_connect(const char* connpath,
	const char* connkey, file_handle* conn_ch);

/*
 * Using a identification string (implementation defined connection
 * mechanism)
 */
struct arcan_shmif_cont arcan_shmif_acquire(
	struct arcan_shmif_cont* parent, /* should only be NULL internally */
	const char* shmkey,    /* provided in ENV or from shmif_connect below */
	enum ARCAN_SEGID type, /* archetype, defined in shmif_event.h */
	enum ARCAN_FLAGS flags, ...
);

/*
 * Used internally by _control etc. but also in ARCAN for mapping the
 * different buffer positions / pointers, very limited use outside those
 * contexts. Returns size: (end of last buffer) - addr
 */
uintptr_t arcan_shmif_mapav(
	struct arcan_shmif_page* addr,
	shmif_pixel* vbuf[], size_t vbufc, size_t vbuf_sz,
	shmif_asample* abuf[], size_t abufc, size_t abuf_sz
);

/*
 * There can be one "post-flag, pre-semaphore" hook that will occur
 * before triggering a sigmask and can be used to synch audio to video
 * or video to audio during transfers.
 * 'mask' argument defines the signal mask slot (A xor B only, A or B is
 * undefined behavior).
 */
shmif_trigger_hook arcan_shmif_signalhook(struct arcan_shmif_cont*,
	enum arcan_shmif_sigmask mask, shmif_trigger_hook, void* data);

/*
 * Using the specified shmpage state, synchronization semaphore handle,
 * construct two event-queue contexts. Parent- flag should be set
 * to false for frameservers
 */
void arcan_shmif_setevqs(struct arcan_shmif_page*,
	sem_handle, arcan_evctx* inevq, arcan_evctx* outevq, bool parent);

/* resize/synchronization protocol to issue a resize of the output video buffer.
 *
 * This request can be declined (false return value) and should be considered
 * expensive (may block indefinitely). Anything that depends on the contents of
 * the shared-memory dependent parts of shmif_cont (eventqueue, vidp/audp, ...)
 * should be considered invalid during/after a call to shmif_resize and the
 * function will internally rebuild these structures.
 *
 * This function is not thread-safe -- While a resize is pending, none of the
 * other operations (drop, signal, en/de- queue) are safe.  If events are
 * managed on a separate thread, these should be treated in mutual exclusion
 * with the size operation.
 *
 * There are four possible outcomes here:
 * a. resize fails, dimensions exceed hard-coded limits.
 * b. resize succeeds, vidp/audp are re-aligned.
 * c. resize succeeds, the segment is truncated to a new size.
 * d. resize succeeds, we switch to a new shared memory connection.
 *
 * Note that the actual effects / resize behavior in the running appl may be
 * delayed until the first shmif_signal call on the resized segment. This is
 * done in order to avoid visual artifacts that would stem from having source
 * material in one resolution while metadata refers to another.
 */
bool arcan_shmif_resize(struct arcan_shmif_cont*,
	unsigned width, unsigned height);

/*
 * Extended version of resize that supports requesting more
 * audio / video buffers for better swap/synch control.
 */
struct shmif_resize_ext {
	size_t abuf_sz;
	int abuf_cnt;
	int vbuf_cnt;
};

bool arcan_shmif_resize_ext(struct arcan_shmif_cont*,
	unsigned width, unsigned height, struct shmif_resize_ext);
/*
 * Unmap memory, release semaphores and related resources
 */
void arcan_shmif_drop(struct arcan_shmif_cont*);

/*
 * Signal that a synchronized transfer should take place. The contents of the
 * mask determine buffers to synch and blocking behavior.
 *
 * Returns the number of miliseconds that the synchronization reportedly, and
 * is a value that can be used to adjust local rendering/buffer.
 */
unsigned arcan_shmif_signal(struct arcan_shmif_cont*, enum arcan_shmif_sigmask);

/*
 * Signal a video transfer that is based on buffer sharing rather than on data
 * in the shmpage. Otherwise it behaves like [arcan_shmif_signal] but with a
 * possible reserved variadic argument for future use.
 */
unsigned arcan_shmif_signalhandle(struct arcan_shmif_cont* ctx,
	enum arcan_shmif_sigmask,	int handle, size_t stride, int format, ...);

/*
 * Support function to set/unset the primary access segment (one slot for
 * input. one slot for output), manually managed. This is just a static member
 * helper with no currently strongly negotiated meaning.
 */
struct arcan_shmif_cont* arcan_shmif_primary(enum arcan_shmif_type);
void arcan_shmif_setprimary( enum arcan_shmif_type, struct arcan_shmif_cont*);

/*
 * This should be called periodically to prevent more subtle bugs from
 * cascading and be caught at an earlier stage, it checks the shared memory
 * context against a series of cookies and known guard values, returning
 * [false] if not everything checks out.
 *
 * The guard thread (if active) uses this function as part of its monitoring
 * heuristic.
 */
bool arcan_shmif_integrity_check(struct arcan_shmif_cont*);

struct arcan_shmif_region {
	uint16_t x1,x2,y1,y2;
};

struct arcan_shmif_cont {
	struct arcan_shmif_page* addr;

/* offset- pointers into addr, can change between calls to
 * shmif_ functions so aliasing is not recommended */
	shmif_pixel* vidp;
	shmif_asample* audp;

/*
 * This cookie is set/kept to some implementation defined value
 * and will be verified during integrity_check. It is placed here
 * to quickly detect overflows in video or audio management.
 */
	int16_t oflow_cookie;

/*
 * the event handle is provided and used for signal event delivery
 * in order to allow multiplexation with other input/output sources
 */
	file_handle epipe;

/*
 * Maintain a connection to the shared memory handle in order
 * to handle resizing (on platforms that support it, otherwise
 * define ARCAN_SHMIF_OVERCOMMIT which will only recalc pointers
 * on resize
 */
	file_handle shmh;
	size_t shmsize;

/*
 * Used internally for synchronization (and mapped / managed outside
 * the regular shmpage). system-defined but typically named semaphores.
 */
	sem_handle vsem, asem, esem;

/*
 * Should be used to index vidp, i.e. vidp[y * pitch + x] = RGBA(r, g, b, a)
 * stride and pitch account for padding, with stride being a row length in
 * bytes and pitch a row length in pixels.
 */
	size_t w, h, stride, pitch;

/*
 * The cookie act as overflow monitor and trigger for ABI incompatibilities
 * between arcan main and program using the shmif library. Combined from
 * shmpage struct offsets and type sizes. Periodically monitored (using
 * arcan_shmif_integrity_check calls) and incompatibilities is a terminal
 * state transition.
 */
	uint64_t cookie;

/*
 * User-tag, primarily to support attaching ancilliary data to subsegments
 * that are run and synchronized in separate threads.
 */
	void* user;

/*
 * Opaque struct for implementation defined tracking (guard thread handles
 * and related data).
 */
	struct shmif_hidden* priv;
};

enum rhint_mask {
	SHMIF_RHINT_ORIGO_UL = 0,
	SHMIF_RHINT_ORIGO_LL = 1,
	SHMIF_RHINT_SUBREGION = 2
};

struct arcan_shmif_page {
/*
 * These will be statically set to the ARCAN_VERSION_MAJOR and
 * ARCAN_VERSION_MAJOR defines, a mismatch will cause the integrity_check to
 * fail and both sides may decide to terminate. Thus, they also act as a header
 * guard.
 */
	int8_t major;
	int8_t minor;

/* [FSRV-REQ, ARCAN-ACK]
 * Will be checked periodically and before transfers. When set, FSRV should
 * treat other contents of page as UNDEFINED until acknowledged.
 * RELATES-TO: width, height */
	 volatile int8_t resized;

/* [FSRV-SET or ARCAN-SET]
 * Dead man's switch, set to 1 when a connection is active and released
 * if parent or child detects an anomaly that would indicate misuse or
 * data corruption. This will trigger guard-threads and similar structures
 * to release semaphores and attempt to shut down gracefully.
 */
	volatile uint8_t dms;

/* [FSRV-SET, ARCAN-ACK(fl+sem)]
 * Set whenever a buffer is ready to be synchronized.
 * [vready-1, aready-1] indicate the negotiated buffer(s) to synchronize
 * and |hints] indicate any specific synchronization options to consider
 * apending / vpending is used internally to determine the next buffer
 * to write to.
 */
	volatile int8_t aready, apending;
	volatile uint8_t vready, vpending;

/*
 * Presentation hints, see mask above.
 */
	volatile uint8_t hints;

/*
 * IF the contraints:
 * [Hints & SHMIF_RHINT_SUBREGION] and (X2>X1,(X2-X1)<=W,Y2>Y1,(Y2-Y1<=H))
 * valid, [ARCAN] MAY synch only the specified region.
 */
	volatile struct arcan_shmif_region dirty;

/* [FSRV-SET]
 * Unique (or 0) segment identifier. Prvodes a local namespace for specifying
 * relative properties (e.g. VIEWPORT command from popups) between subsegments,
 * is always 0 for subsegments.
 */
	uint32_t segment_token;

/* [ARCAN-SET]
 * Calculated once when initializing segment, and verified periodically from
 * both [FSRV] and [ARCAN]. Any deviation MAY have the [dms] be pulled.
 */
	uint64_t cookie;

/*
 * [ARCAN-SET (parent), FSRV-SET (child)]
 * Uses the event model provded in shmif/arcan_event and tightly couples
 * structure / event layout which introduces a number of implementation defined
 * constraints, making this interface a poor choice for a protocol.
 */
	struct {
		struct arcan_event evqueue[ PP_QUEUE_SZ ];
		uint8_t front, back;
	} childevq, parentevq;

/* [ARCAN-SET (parent), FSRV-CHECK]
 * Arcan mandates segment size, will only change during resize negotiation.
 * If this differs from the previous known size (tracked inside shmif_cont),
 * the segment should be remapped.
 *
 * Not all operations will lead to a change in segment_size, OVERCOMMIT
 * builds has its size fixed, and parent may heuristically determine if
 * a shrinking- operation is worth the overhead or not.
 */
	volatile uint32_t segment_size;

/*
 * [FSRV-SET (resize), ARCAN-ACK]
 * Current video output dimensions. If these deviate from the agreed upon
 * dimensions (i.e. change w,h and set the resized flag to !0) ARCAN will
 * simply ignore the data presented.
 */
	volatile uint16_t w, h;

/*
 * [FSRV-SET (aready signal), ARCAN-ACK]
 * Video buffers are planar transfers of a pre-determined size. Audio,
 * on the other hand, can be appended and wholly or partially consumed
 * by the side that currently holds the synch- semaphore.
*/
	volatile uint16_t abufused, abufsize;

/*
 * [FSRV-SET, ARCAN-ACK (vready signal)]
 * Timing related data to a video frame can be attached in order to assist the
 * parent in determining when/if synchronization should be released and the
 * frame rendered. This value is a hint, do not rely on it as a clock/sleep
 * mechanism.
 */
	volatile int64_t vpts;

/*
 * [ARCAN-SET]
 * Set during segment initalization, provides some identifier to determine
 * if the parent process is still allowed (used internally by GUARDTHREAD).
 * Can also be updated in relation to a RESET event.
 */
	process_handle parent;
};
#endif
