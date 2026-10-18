// SPDX-License-Identifier: GPL-2.0
/*
 * umfb -- host-side half: open/map the sink and run the pump thread.
 *
 * Discipline (same as ubd's io thread and qcow2_user.c, for the same
 * reason): the pump thread is a UML helper thread, NOT part of the
 * kernel.  Everything it touches is plain memory -- the framebuffer
 * (kernel memory, read only) and the sink mapping (host memory, written
 * with memcpy) -- so the "os_* wrappers and no kernel facilities" rule
 * is trivially satisfied: the loop below makes no calls at all beyond
 * poll() as its clock.  In particular there is no printk and no
 * allocation on this thread, ever.
 *
 * Why a pump at all: the guests (SurfaceFlinger, Xorg, weston) write
 * the framebuffer however they like -- including through their own
 * mmap of /dev/fb0, which no driver-side hook can observe.  So rather
 * than instrumenting every store, the pump compares the framebuffer
 * against what the sink currently holds, and copies over only the
 * 64x64 tiles that actually differ.  An idle desktop costs a scan; a
 * blinking cursor costs one tile.
 */
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <os.h>

#include "umfb.h"

/*
 * Resolve the sink.  Called from umfb_init() (kernel context), so os_*
 * calls are fine here.
 *
 *   "fd:N"	an inherited descriptor (a memfd on Android; the app
 *		created it and both the app and this guest address it)
 *   <path>	a file, created if absent, grown to the needed size
 *		(one byte written at the end; the rest stays a hole
 *		until the pump fills it)
 */
int umfb_sink_open(const char *src, unsigned long long need, int *fd_out)
{
	struct uml_stat st;
	int fd, err;
	unsigned char zero = 0;

	if (!strncmp(src, "fd:", 3)) {
		char *end;
		unsigned long n = strtoul(src + 3, &end, 10);

		if (end == src + 3 || *end != '\0' || n > 1 << 20)
			return -EINVAL;
		fd = os_dup_file((int) n);
		if (fd < 0)
			return fd;
	} else {
		fd = os_open_file(src, of_rdwr(of_create(OPENFLAGS())), 0666);
		if (fd < 0)
			return fd;
	}

	err = os_stat_fd(fd, &st);
	if (err < 0) {
		os_close_file(fd);
		return err;
	}
	if ((unsigned long long) st.ust_size < need) {
		err = os_pwrite_file(fd, &zero, 1, need - 1);
		if (err != 1) {
			os_close_file(fd);
			return err < 0 ? err : -EIO;
		}
		err = os_stat_fd(fd, &st);
		if (err < 0 || (unsigned long long) st.ust_size < need) {
			os_close_file(fd);
			return err < 0 ? err : -ENOSPC;
		}
	}

	*fd_out = fd;
	return 0;
}

void *umfb_sink_map(int fd, unsigned long long size)
{
	return os_mmap_rw_shared(fd, (size_t) size);
}

/*
 * One pump pass: diff every 64x64 tile against the sink, copy the
 * changed ones, then publish (bitmap, full, seq-release).  Runs on the
 * helper thread; see the file comment for the rules.
 */
static void umfb_pump_once(struct umfb_pump *p)
{
	unsigned int ts = p->tile_shift;
	unsigned int tile_px = 1u << ts;
	unsigned int bytes = p->stride / p->width;	/* 4 */
	unsigned int tx, ty, changed = 0;

	memset(p->bitmap, 0, p->words * sizeof(unsigned int));

	for (ty = 0; ty < p->tiles_y; ty++) {
		unsigned int y0 = ty << ts;
		unsigned int y1 = y0 + tile_px < p->height
				  ? y0 + tile_px : p->height;

		for (tx = 0; tx < p->tiles_x; tx++) {
			unsigned int x0 = tx << ts;
			unsigned int x1 = x0 + tile_px < p->width
					  ? x0 + tile_px : p->width;
			unsigned int n = (x1 - x0) * bytes;
			unsigned int idx = ty * p->tiles_x + tx;
			unsigned char *a, *b;
			unsigned int y;
			int dirty = 0;

			a = p->fb + (unsigned long long) y0 * p->stride
				   + (unsigned long long) x0 * bytes;
			b = p->pix + (unsigned long long) y0 * p->stride
				   + (unsigned long long) x0 * bytes;
			for (y = y0; y < y1; y++) {
				if (memcmp(a, b, n)) {
					dirty = 1;
					break;
				}
				a += p->stride;
				b += p->stride;
			}
			if (!dirty)
				continue;

			p->bitmap[idx >> 5] |= 1u << (idx & 31);
			changed++;

			a = p->fb + (unsigned long long) y0 * p->stride
				   + (unsigned long long) x0 * bytes;
			b = p->pix + (unsigned long long) y0 * p->stride
				   + (unsigned long long) x0 * bytes;
			for (y = y0; y < y1; y++) {
				memcpy(b, a, n);
				a += p->stride;
				b += p->stride;
			}
		}
	}

	if (!changed)
		return;

	/* Publish: pixels are in place; hand out the bitmap, then the seq. */
	memcpy((void *) p->hdr->dirty_words, p->bitmap,
	       p->words * sizeof(unsigned int));
	p->hdr->full = (changed * 2 >= p->tiles_total) ? 1u : 0u;
	p->hdr->state = UMFB_STATE_LIVE;
	__atomic_store_n(&p->hdr->seq, p->hdr->seq + 1, __ATOMIC_RELEASE);
}

static void *umfb_pump(void *arg)
{
	struct umfb_pump *p = arg;

	for (;;) {
		poll(NULL, 0, p->pump_ms);
		umfb_pump_once(p);
	}
	return NULL;	/* not reached: the thread dies with the process */
}

int umfb_pump_start(struct os_helper_thread **td, struct umfb_pump *p)
{
	return os_run_helper_thread(td, umfb_pump, p);
}