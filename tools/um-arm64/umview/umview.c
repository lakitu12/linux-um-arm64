// SPDX-License-Identifier: GPL-2.0
/*
 * umview -- a host window for a UML on-screen framebuffer (umfb).
 *
 * The guest draws into /dev/fb0; the UML kernel's umfb driver mirrors
 * the changed 64x64 tiles into a shared sink file (arch/um/drivers/
 * umfb.h is the wire format; a verbatim copy of the header sits next
 * to this file).  This program maps the sink and presents it -- under
 * X11, under Wayland, or wherever else SDL2 finds a video driver.  The
 * Android console app's display screen speaks the same protocol, so
 * the file is the interface; this viewer is one client of it.
 *
 * Usage:
 *   umview [options] <sink>          sink: a path, or fd:N
 *
 *   --title S       window title (default "umfb <W>x<H>")
 *   --scale F       initial window scale (default 1)
 *   --fullscreen    start fullscreen (F11 toggles, Esc quits)
 *   --probe         no window: wait for the sink, print its state, exit
 *   --wait SEC      wait up to SEC for a valid sink (default 30; probe
 *                   waits for the first published frame instead)
 *   --shot FILE     save a BMP of the first presented frame
 *   --frames N      exit after presenting N frames (needs a live guest)
 *   --seconds S     exit after S seconds regardless (scripts)
 *   --px X,Y        print the sink pixel at X,Y as hex (diagnostics)
 *
 * Build: make   (needs libsdl2-dev)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <SDL.h>

#include "umfb.h"

struct uv {
	const char *src;
	int fd;
	unsigned char *map;
	size_t maplen;
	struct umfb_hdr *hdr;
	unsigned char *pix;

	unsigned int w, h, stride, ts, tx, ty, total, words;

	SDL_Window *win;
	SDL_Renderer *ren;
	SDL_Texture *tex;

	uint32_t last_seq;
	int have_last;
	unsigned int shown;

	const char *shot;	/* save one BMP once the first frame is up */
	unsigned int frame_limit;	/* exit after N presented frames */
	long seconds;			/* exit after S seconds (0: never) */
};

static volatile sig_atomic_t interrupted;

static void on_sigint(int sig)
{
	(void) sig;
	interrupted = 1;
}

static void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "umview: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(1);
}

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * Open the sink and wait for the kernel to have published a header.
 * The file may exist empty (created by a launcher) or not exist yet
 * when the viewer races the guest; wait_ms bounds the wait.
 */
static void uv_open(struct uv *u, long wait_ms)
{
	struct stat st;
	long t0;
	int waited = 0;

	if (!strncmp(u->src, "fd:", 3)) {
		char *end;
		long n = strtol(u->src + 3, &end, 10);

		if (end == u->src + 3 || *end || n < 0 || n > 1 << 20)
			die("bad sink '%s'", u->src);
		u->fd = dup((int) n);
	} else {
		u->fd = open(u->src, O_RDONLY);
	}
	if (u->fd < 0)
		die("open %s: %s", u->src, strerror(errno));

	if (fstat(u->fd, &st) || st.st_size < UMFB_HDR_SIZE) {
		if (wait_ms <= 0)
			die("%s: too small for a umfb header (%lld bytes)",
			    u->src, (long long) st.st_size);
		waited = 1;	/* size grows when the kernel sizes it */
	}

	u->maplen = st.st_size > 0 ? (size_t) st.st_size : 0;
	if (waited || !u->maplen) {
		/* wait for a first plausible size before mapping */
		t0 = now_ms();
		while (now_ms() - t0 < wait_ms) {
			usleep(20000);
			if (!fstat(u->fd, &st) && st.st_size >= UMFB_HDR_SIZE) {
				u->maplen = (size_t) st.st_size;
				break;
			}
		}
		if (u->maplen < UMFB_HDR_SIZE)
			die("%s: no umfb header appeared (guest up with umlfb=?)",
			    u->src);
	}

	u->map = mmap(NULL, u->maplen, PROT_READ, MAP_SHARED, u->fd, 0);
	if (u->map == MAP_FAILED)
		die("mmap %s: %s", u->src, strerror(errno));
	u->hdr = (struct umfb_hdr *) u->map;

	/* wait for the magic (the kernel fills the header at driver init) */
	t0 = now_ms();
	while (__atomic_load_n(&u->hdr->magic, __ATOMIC_ACQUIRE) != UMFB_MAGIC ||
	       __atomic_load_n(&u->hdr->version, __ATOMIC_ACQUIRE) != UMFB_VERSION) {
		if (now_ms() - t0 >= wait_ms)
			die("%s: no valid umfb header (magic=%08x version=%u) "
			    "-- is the guest running with umlfb=?",
			    u->src, u->hdr->magic, u->hdr->version);
		usleep(20000);
	}

	u->w = u->hdr->width;
	u->h = u->hdr->height;
	u->stride = u->hdr->stride;
	u->ts = u->hdr->tile_shift;
	u->tx = u->hdr->tiles_x;
	u->ty = u->hdr->tiles_y;
	u->total = u->hdr->tiles_total;

	if (!u->w || !u->h || u->hdr->format != UMFB_FMT_RGBA8888 ||
	    u->hdr->bpp != 32 || u->stride < u->w * 4 ||
	    u->total != u->tx * u->ty || u->total > UMFB_MAX_TILES ||
	    u->ts < 4 || u->ts > 8)
		die("%s: implausible header (%ux%u stride %u fmt %u)",
		    u->src, u->w, u->h, u->stride, u->hdr->format);

	u->words = (u->total + 31) / 32;
	if (u->maplen < UMFB_HDR_SIZE + (size_t) u->stride * u->h)
		die("%s: file holds %zu bytes, the mode needs %zu", u->src,
		    u->maplen, UMFB_HDR_SIZE + (size_t) u->stride * u->h);

	u->pix = u->map + UMFB_HDR_SIZE;
}

static void uv_paint_full(struct uv *u)
{
	SDL_UpdateTexture(u->tex, NULL, u->pix, (int) u->stride);
}

static void uv_paint_tiles(struct uv *u, const uint32_t *words)
{
	unsigned int wi;

	for (wi = 0; wi < u->words; wi++) {
		uint32_t bits = words[wi];

		while (bits) {
			int b = __builtin_ctz(bits);
			unsigned int idx = wi * 32 + (unsigned int) b;
			unsigned int x0, y0, tw, th;
			SDL_Rect r;

			bits &= bits - 1;
			if (idx >= u->total)
				continue;
			x0 = (idx % u->tx) << u->ts;
			y0 = (idx / u->tx) << u->ts;
			tw = (1u << u->ts) < u->w - x0 ? (1u << u->ts) : u->w - x0;
			th = (1u << u->ts) < u->h - y0 ? (1u << u->ts) : u->h - y0;
			r.x = (int) x0;
			r.y = (int) y0;
			r.w = (int) tw;
			r.h = (int) th;
			/* le32 0xAABBGGRR == bytes R,G,B,A == ABGR8888 */
			SDL_UpdateTexture(u->tex, &r,
					  u->pix + (size_t) y0 * u->stride +
					  (size_t) x0 * 4, (int) u->stride);
		}
	}
}

static void uv_present(struct uv *u)
{
	SDL_RenderClear(u->ren);
	SDL_RenderCopy(u->ren, u->tex, NULL, NULL);
	SDL_RenderPresent(u->ren);
}

static void uv_shot(struct uv *u, const char *file)
{
	SDL_Surface *s;
	int ww, wh;
	void *pix;
	int pitch;

	SDL_GetWindowSize(u->win, &ww, &wh);
	pitch = ww * 4;
	pix = malloc((size_t) pitch * wh);
	if (!pix) {
		fprintf(stderr, "umview: shot: out of memory\n");
		return;
	}
	/*
	 * Re-render before reading: a back buffer's contents after
	 * RenderPresent are not defined (GL swap), so read the frame we
	 * draw here, then present it.
	 */
	SDL_RenderClear(u->ren);
	SDL_RenderCopy(u->ren, u->tex, NULL, NULL);
	if (SDL_RenderReadPixels(u->ren, NULL, SDL_PIXELFORMAT_ARGB8888,
				 pix, pitch) != 0) {
		fprintf(stderr, "umview: shot: %s\n", SDL_GetError());
		free(pix);
		return;
	}
	SDL_RenderPresent(u->ren);
	s = SDL_CreateRGBSurfaceWithFormatFrom(pix, ww, wh, 32, pitch,
					       SDL_PIXELFORMAT_ARGB8888);
	if (s && SDL_SaveBMP(s, file) == 0)
		fprintf(stderr, "umview: shot saved: %s (%dx%d)\n", file, ww, wh);
	else
		fprintf(stderr, "umview: shot save failed: %s\n", SDL_GetError());
	if (s)
		SDL_FreeSurface(s);
	free(pix);
}

static void uv_probe_px(struct uv *u, const char *spec)
{
	unsigned int x, y;

	if (sscanf(spec, "%u,%u", &x, &y) != 2 || x >= u->w || y >= u->h)
		die("--px wants X,Y within the mode");
	printf("UMVIEW-PX %u,%u=%08x\n", x, y,
	       *(uint32_t *) (u->pix + (size_t) y * u->stride + x * 4));
}

int main(int argc, char **argv)
{
	struct uv u;
	const char *sink = NULL, *title = NULL, *pxspec = NULL, *shot = NULL;
	const char *scale = "1";
	long wait_ms = 30000;
	int probe = 0, fullscreen = 0, i, rc = 0;

	memset(&u, 0, sizeof(u));

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (!strcmp(a, "--title") && i + 1 < argc)
			title = argv[++i];
		else if (!strcmp(a, "--scale") && i + 1 < argc)
			scale = argv[++i];
		else if (!strcmp(a, "--wait") && i + 1 < argc)
			wait_ms = atol(argv[++i]) * 1000;
		else if (!strcmp(a, "--shot") && i + 1 < argc)
			shot = argv[++i];
		else if (!strcmp(a, "--frames") && i + 1 < argc)
			u.frame_limit = (unsigned int) atol(argv[++i]);
		else if (!strcmp(a, "--seconds") && i + 1 < argc)
			u.seconds = atol(argv[++i]);
		else if (!strcmp(a, "--px") && i + 1 < argc)
			pxspec = argv[++i];
		else if (!strcmp(a, "--probe"))
			probe = 1;
		else if (!strcmp(a, "--fullscreen"))
			fullscreen = 1;
		else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
			printf("usage: umview [--title S] [--scale F] [--fullscreen]\n"
			       "              [--probe] [--wait SEC] [--shot FILE]\n"
			       "              [--frames N] [--seconds S] [--px X,Y] <sink>\n");
			return 0;
		} else if (a[0] == '-' && a[1] == '-' && a[2] != '\0') {
			die("unknown option %s (see --help)", a);
		} else if (!sink) {
			sink = a;
		} else {
			die("extra argument '%s'", a);
		}
	}
	if (!sink)
		die("no sink given (see --help)");

	u.src = sink;
	uv_open(&u, wait_ms);

	if (probe) {
		/* probe waits for a live frame, not just a header */
		long t0 = now_ms();

		while (__atomic_load_n(&u.hdr->seq, __ATOMIC_ACQUIRE) == 0) {
			if (now_ms() - t0 >= wait_ms) {
				rc = 9;
				break;
			}
			usleep(20000);
		}
		printf("UMVIEW-PROBE %s magic=%08x version=%u mode=%ux%u "
		       "stride=%u tiles=%ux%ux%u seq=%u full=%u state=%u hz=%u\n",
		       rc ? "STALE" : "OK", u.hdr->magic, u.hdr->version,
		       u.w, u.h, u.stride, u.tx, u.ty, u.total,
		       u.hdr->seq, u.hdr->full, u.hdr->state, u.hdr->hz);
		if (pxspec)
			uv_probe_px(&u, pxspec);
		return rc;
	}

	if (SDL_Init(SDL_INIT_VIDEO))
		die("SDL_Init: %s", SDL_GetError());
	signal(SIGINT, on_sigint);

	{
		char tbuf[128];
		double sc = atof(scale);
		int ww = (int) (u.w * sc + 0.5), wh = (int) (u.h * sc + 0.5);

		if (!title) {
			snprintf(tbuf, sizeof(tbuf), "umfb %ux%u", u.w, u.h);
			title = tbuf;
		}
		u.win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED,
					 SDL_WINDOWPOS_CENTERED, ww, wh,
					 SDL_WINDOW_RESIZABLE |
					 (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
		if (!u.win)
			die("SDL_CreateWindow: %s", SDL_GetError());
		u.ren = SDL_CreateRenderer(u.win, -1, SDL_RENDERER_ACCELERATED);
		if (!u.ren)
			u.ren = SDL_CreateRenderer(u.win, -1, SDL_RENDERER_SOFTWARE);
		if (!u.ren)
			die("SDL_CreateRenderer: %s", SDL_GetError());
		u.tex = SDL_CreateTexture(u.ren, SDL_PIXELFORMAT_ABGR8888,
					  SDL_TEXTUREACCESS_STREAMING,
					  (int) u.w, (int) u.h);
		if (!u.tex)
			die("SDL_CreateTexture: %s", SDL_GetError());
		/* logical size == the guest's screen: resizing letterboxes */
		SDL_RenderSetLogicalSize(u.ren, (int) u.w, (int) u.h);

		fprintf(stderr,
			"umview: SDL video driver '%s', sink %ux%u stride %u, %ux%u %upx tiles\n",
			SDL_GetCurrentVideoDriver(), u.w, u.h, u.stride,
			u.tx, u.ty, 1u << u.ts);
	}

	u.shot = shot;
	uv_paint_full(&u);
	uv_present(&u);
	u.shown = 1;
	u.last_seq = __atomic_load_n(&u.hdr->seq, __ATOMIC_ACQUIRE);
	u.have_last = 1;
	if (u.shot) {
		uv_shot(&u, u.shot);
		u.shot = NULL;
	}

	{
	long t_end = u.seconds > 0 ? now_ms() + u.seconds * 1000 : 0;

	while (!interrupted && !(u.frame_limit && u.shown >= u.frame_limit) &&
	       !(t_end && now_ms() >= t_end)) {
		SDL_Event ev;

		while (SDL_PollEvent(&ev)) {
			if (ev.type == SDL_QUIT)
				goto done;
			if (ev.type == SDL_KEYDOWN) {
				if (ev.key.keysym.sym == SDLK_ESCAPE)
					goto done;
				if (ev.key.keysym.sym == SDLK_F11) {
					Uint32 fl = SDL_GetWindowFlags(u.win);

					SDL_SetWindowFullscreen(u.win,
						(fl & SDL_WINDOW_FULLSCREEN_DESKTOP) ?
						0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
				}
			}
			if (ev.type == SDL_WINDOWEVENT &&
			    (ev.window.event == SDL_WINDOWEVENT_EXPOSED ||
			     ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED))
				uv_present(&u);
		}

		{
			uint32_t s1 = __atomic_load_n(&u.hdr->seq, __ATOMIC_ACQUIRE);

			if (s1 != u.last_seq) {
				uint32_t full = u.hdr->full;
				uint32_t words[UMFB_MAX_WORDS];
				uint32_t s2;

				memcpy(words, (const void *) u.hdr->dirty_words,
				       u.words * sizeof(uint32_t));
				__atomic_thread_fence(__ATOMIC_ACQUIRE);
				s2 = __atomic_load_n(&u.hdr->seq, __ATOMIC_ACQUIRE);
				if (s2 == s1) {
					if (full || !u.have_last)
						uv_paint_full(&u);
					else
						uv_paint_tiles(&u, words);
					uv_present(&u);
					u.last_seq = s1;
					u.have_last = 1;
					u.shown++;
					if (u.shot) {
						uv_shot(&u, u.shot);
						u.shot = NULL;
					}
					if (u.frame_limit && u.shown >= u.frame_limit)
						goto done;
				}
			}
		}
		SDL_Delay(8);
	}
	}

done:
	SDL_Quit();
	return 0;
}