/* Arcan-fe, scriptable front-end engine
 *
 * Arcan-fe is the legal property of its developers, please refer
 * to the COPYRIGHT file distributed with this source distribution.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

#define SHMPAGE_QUEUESIZE 64
#define SHMPAGE_MAXAUDIO_FRAMESIZE 192000
#define SHMPAGE_AUDIOBUF_SIZE (192000 * 3 / 2)
#define MAX_SHMSIZE 9582916

enum frameserver_colormode {
	COLOR_RGBA,
	COLOR_RGB155,
	COLOR_YUV420
};

/* setup a named memory / semaphore mapping with the server */
struct frameserver_shmcont{
	struct frameserver_shmpage* addr;
	sem_handle vsem;
	sem_handle asem;
	sem_handle esem;
};

struct frameserver_shmpage {
	bool resized;
	bool loop;

/* these are managed / populated by a queue 
 * context in each process, mapped to the same posix semaphore */
	struct {
		arcan_event evqueue[ SHMPAGE_QUEUESIZE ];
		unsigned front, back;
	} childdevq, parentdevq;
	
	process_handle parent;
	
	volatile uint8_t vready;
	bool glsource;

/* vbuf size = w * h * bpp */
	uint32_t vdts;
	uint16_t w, h;
	uint8_t bpp;

/* audio */
	volatile uint8_t aready;
	
	uint8_t channels;
	uint16_t frequency;
	uint32_t adts;

/* abufbase is a working buffer offset in how far parent has processed */
	off_t abufbase;
	size_t abufused;
};

/* note, frameserver_semcheck is hidden in arcan_frameserver_shmpage.o,
 * this is partly to make it easier to share code between hijacklib and frameserver,
 * while at the same time keeping it out of the frameserver routine in the main app, where
 * that kind of shmcheck is dangerous */

/* try and acquire a lock on the semaphore before mstimeout runs out (-1 == INFINITE, 0 == return immediately) 
 * this will forcibly exit should any error other than timeout occur.*/
int frameserver_semcheck(sem_handle semaphore, int timeout);

/* returns true of the contents of the shmpage seems sound (unless this passes,
 * the server will likely kill or ignore the client */
bool frameserver_shmpage_integrity_check(struct frameserver_shmpage*);

/* return relative offsets from shmpage as baseaddr where the vbuf and audbufs can be found */
void frameserver_shmpage_calcofs(struct frameserver_shmpage*, void** dstvidptr, void** dstaudptr);

/* this code is repeated a little too often so sortof fits here but adds a dependency to arcan_event */
void frameserver_shmpage_setevqs(struct frameserver_shmpage*, sem_handle, arcan_evctx*, arcan_evctx*, bool);

/* (client use only) using a keyname, setup shmpage (with eventqueues etc.) and semaphores */
struct frameserver_shmcont frameserver_getshm(const char* shmkey, bool force_unlink);

/* (client use only) recalculate offsets, synchronize with parent and make sure these new options work */
bool frameserver_shmpage_resize(struct frameserver_shmcont*, unsigned width, unsigned height, unsigned bpp, unsigned nchan, unsigned freq);