/* SPDX-License-Identifier: GPL-2.0 */
/*
 * umfb -- shared-memory display for UML guests: the wire format.
 *
 * The kernel-side driver (umfb_kern.c) registers a framebuffer whose
 * pixels are mirrored, tile by tile, into a host file or descriptor
 * ("the sink").  Host-side viewers -- tools/um-arm64/umview on Linux,
 * the console app on Android -- map the same sink and present the
 * tiles.  Guests see an ordinary /dev/fb0 (redroid's SurfaceFlinger,
 * Xorg's fbdev driver, weston's fbdev backend all land there).
 *
 * File layout:
 *   [0, 1024)		struct umfb_hdr
 *   [1024, ...)	pixels, stride * height bytes, top-down,
 *			format UMFB_FMT_RGBA8888 (bytes R,G,B,A)
 *
 * Update protocol (one producer: the pump thread; any number of
 * readers: the host-side viewers):
 *
 *   1. the producer writes the changed pixel data for this round;
 *   2. the producer stores dirty_words[] and full for this round;
 *   3. the producer bumps seq with release semantics.
 *
 *   A reader: loads seq with acquire semantics; if it moved since the
 *   reader's last look, snapshots full/dirty_words, copies the dirty
 *   tiles out of the pixel area, then re-checks seq and repeats if it
 *   moved again.  A reader can therefore only ever see a torn tile
 *   belonging to a frame that is already being replaced by the next
 *   one -- this is a live display stream, not a database, and the
 *   following frame repairs it.
 *
 * This header is deliberately plain C (no includes at all): a verbatim
 * copy lives in the viewers (tools/um-arm64/umview/umview.c and the
 * app's app/display/umldisp.c).  Bump UMFB_VERSION for any layout
 * change and update the copies; the readers check magic+version.
 */
#ifndef _UMFB_H_
#define _UMFB_H_

#define UMFB_MAGIC		0x4D424655u	/* "UFBM", little-endian */
#define UMFB_VERSION		1

#define UMFB_FMT_RGBA8888	1	/* bytes R,G,B,A (alpha ignored) */

#define UMFB_HDR_SIZE		1024	/* header bytes, pixels follow */
#define UMFB_MAX_TILES		4096	/* covers up to 5120x4096 of 64px tiles */
#define UMFB_MAX_WORDS		(UMFB_MAX_TILES / 32)

#define UMFB_TILE_SHIFT_DEFAULT	6	/* 64x64 pixels */
#define UMFB_HZ_DEFAULT		30

/* state */
#define UMFB_STATE_INIT		0
#define UMFB_STATE_LIVE		1

struct umfb_hdr {
	unsigned int magic;		/* UMFB_MAGIC */
	unsigned int version;		/* UMFB_VERSION */
	unsigned int width;		/* visible pixels */
	unsigned int height;
	unsigned int stride;		/* bytes per row */
	unsigned int bpp;		/* always 32 for now */
	unsigned int format;		/* UMFB_FMT_* */
	unsigned int tile_shift;	/* tile edge = 1 << tile_shift pixels */
	unsigned int tiles_x;
	unsigned int tiles_y;
	unsigned int tiles_total;	/* tiles_x * tiles_y */
	unsigned int hz;		/* pump rate, informational */
	unsigned int state;		/* UMFB_STATE_* */
	volatile unsigned int seq;	/* frames published */
	volatile unsigned int full;	/* this round: whole frame dirty */
	unsigned int reserved0;
	volatile unsigned int dirty_words[UMFB_MAX_WORDS];	/* 4096 tiles */
	unsigned int reserved[112];	/* pad to UMFB_HDR_SIZE; input soon */
};

#ifdef __KERNEL__
/*
 * Kernel-internal plumbing -- NOT part of the wire format.  The readers
 * must not see any of this; it is why the block is fenced off.
 *
 * The pump runs on a UML helper thread (os_run_helper_thread), the same
 * shape ubd's io thread has, because that thread "is not part of the
 * kernel": it may only touch plain memory and host calls, which is all
 * the pump needs (umfb_user.c carries the full explanation).
 */
struct umfb_pump {
	unsigned char *fb;		/* the guest-visible pixels */
	unsigned char *pix;		/* pixels[] in the sink */
	struct umfb_hdr *hdr;
	unsigned int width, height, stride;
	unsigned int tile_shift, tiles_x, tiles_y, tiles_total, words;
	unsigned int pump_ms;
	unsigned int bitmap[UMFB_MAX_WORDS];
};

int umfb_sink_open(const char *src, unsigned long long need, int *fd_out);
void *umfb_sink_map(int fd, unsigned long long size);
int umfb_pump_start(struct os_helper_thread **td, struct umfb_pump *p);
#endif	/* __KERNEL__ */

#endif	/* _UMFB_H_ */