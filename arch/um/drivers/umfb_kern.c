// SPDX-License-Identifier: GPL-2.0
/*
 * umfb -- a UML framebuffer whose pixels are mirrored to the host side.
 *
 * The problem this solves: a UML guest draws its screen into /dev/fb0
 * (redroid's SurfaceFlinger through gralloc, Xorg through its fbdev
 * driver, weston through its fbdev backend -- all of them end up
 * there), but a UML guest cannot reach any host display API, and the
 * host cannot read the guest's kernel memory.  What the host CAN read
 * is a file both sides map: the sink.  This driver registers an
 * ordinary framebuffer for the guest and mirrors changed pixels into
 * the sink through a pump thread (umfb_user.c), where host-side
 * viewers -- tools/um-arm64/umview on Linux (X11 and Wayland sessions
 * alike), the console app on Android -- pick them up.
 *
 * The guest interface is deliberately plain fbdev, and its mode is
 * deliberately fixed: the sink's header (umfb.h) is the contract with
 * the viewers and it is written once, before boot.  There is no
 * resolution switching, no hardware cursor, no panning -- a viewer is
 * a mirror, and a mirror that renegotiates its size mid-stream is a
 * bug, not a feature.
 *
 * Run it with:  umlfb=<path|fd:N>:<W>x<H>[-32][,hz=N][,tile=N]
 * (see the --help text at the bottom).  Without that argument the
 * driver stays completely dormant -- a kernel with it compiled in
 * boots and behaves exactly as before.
 *
 * Why not just enable CONFIG_FB_VIRTUAL: vfb's memory is anonymous
 * kernel memory.  Only this side can see it, so the app would have to
 * ship pixels out through some slower channel (a hostfs dd loop, a
 * syscall).  Backing the mirror with a shared file moves that cost to
 * a memcpy of the tiles that actually changed, with no guest-side
 * cooperation at all -- it works for guest mmaps, write(), and any
 * compositor's private drawing path equally.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/log2.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/fb.h>
#include <linux/slab.h>

#include <init.h>
#include <os.h>

#include "umfb.h"

/* ------------------------------------------------------------------ */
/* command line:  umlfb=<source>:<W>x<H>[-32][,opt=val,...]            */
/* ------------------------------------------------------------------ */

static char umfb_src[160];		/* "fd:N" or a path */
static unsigned int umfb_w, umfb_h, umfb_bpp = 32;
static unsigned int umfb_tile_shift = UMFB_TILE_SHIFT_DEFAULT;
static unsigned int umfb_hz = UMFB_HZ_DEFAULT;
static int umfb_cfg_ok;

static int __init umfb_parse_options(char *opts)
{
	char *o;

	while ((o = strsep(&opts, ",")) != NULL) {
		char *v = strchr(o, '=');
		char *end;
		unsigned long n;

		if (!*o)
			continue;
		if (!v) {
			printk(KERN_ERR "umfb: option '%s' has no value\n", o);
			return -EINVAL;
		}
		*v++ = '\0';
		n = simple_strtoul(v, &end, 10);

		if (!strcmp(o, "hz")) {
			if (!*v || *end || n < 5 || n > 120) {
				printk(KERN_ERR
				       "umfb: hz wants 5..120, got '%s'\n", v);
				return -EINVAL;
			}
			umfb_hz = n;
		} else if (!strcmp(o, "tile")) {
			if (!*v || *end || n < 16 || n > 256 ||
			    !is_power_of_2(n)) {
				printk(KERN_ERR
				       "umfb: tile wants a power of two in 16..256, got '%s'\n",
				       v);
				return -EINVAL;
			}
			umfb_tile_shift = ilog2(n);
		} else {
			printk(KERN_ERR "umfb: unknown option '%s'\n", o);
			return -EINVAL;
		}
	}
	return 0;
}

static int __init umfb_parse_geom(char *g, unsigned int *w_out,
				  unsigned int *h_out)
{
	unsigned int w, h, bpp = 32;
	char *end;

	w = simple_strtoul(g, &end, 10);
	if (end == g || *end != 'x')
		return -EINVAL;
	g = end + 1;
	h = simple_strtoul(g, &end, 10);
	if (end == g)
		return -EINVAL;
	if (*end == '-') {
		bpp = simple_strtoul(end + 1, &end, 10);
		if (bpp != 32)
			return -EINVAL;	/* the wire format is 32bpp */
	}
	if (*end || w < 16 || w > 8192 || h < 16 || h > 8192)
		return -EINVAL;
	*w_out = w;
	*h_out = h;
	return 0;
}

static int __init umfb_setup(char *line, int *add)
{
	char buf[256];
	char *colon, *opts, *geom;
	unsigned int w, h, ts, tx, ty;

	*add = 0;	/* consumed here; keep it off the generic cmdline */

	if (strlen(line) >= sizeof(buf)) {
		printk(KERN_ERR "umfb: argument too long\n");
		return 0;
	}
	strscpy(buf, line, sizeof(buf));

	/*
	 * The geometry separator is the LAST colon: the source may carry
	 * one of its own ("fd:N").
	 */
	colon = strrchr(buf, ':');
	if (!colon || colon == buf || colon[1] == '\0') {
		printk(KERN_ERR
		       "umfb: want <path|fd:N>:<W>x<H>[-32][,opt=v], got '%s'\n",
		       line);
		return 0;
	}
	*colon = '\0';
	geom = colon + 1;
	opts = strchr(geom, ',');
	if (opts) {
		*opts = '\0';
		opts++;
	}

	if (umfb_parse_geom(geom, &w, &h)) {
		printk(KERN_ERR "umfb: bad geometry '%s'\n", geom);
		return 0;
	}
	if ((opts && *opts && umfb_parse_options(opts)))
		return 0;

	/* The tile grid must fit the dirty bitmap: reject, do not clamp. */
	ts = umfb_tile_shift;
	tx = DIV_ROUND_UP(w, 1u << ts);
	ty = DIV_ROUND_UP(h, 1u << ts);
	if (tx * ty > UMFB_MAX_TILES) {
		printk(KERN_ERR "umfb: %ux%u is %u tiles, more than %u (raise tile=)\n",
		       w, h, tx * ty, UMFB_MAX_TILES);
		return 0;
	}

	strscpy(umfb_src, buf, sizeof(umfb_src));
	umfb_w = w;
	umfb_h = h;
	umfb_cfg_ok = 1;
	return 0;
}

__uml_setup("umlfb=", umfb_setup,
"umlfb=<source>:<W>x<H>[-32][,hz=<n>][,tile=<n>]\n"
"    Register a UML framebuffer whose changed pixels are mirrored into\n"
"    <source>, which host-side viewers map and present.  <source> is a\n"
"    path (created and sized here if absent) or fd:<n>, an inherited\n"
"    descriptor (e.g. a memfd handed in by the Android console app).\n"
"    hz=5..120 pump rate (default 30); tile=16..256 tile edge for dirty\n"
"    tracking (default 64).  Without this argument the driver is off.\n"
);

/* ------------------------------------------------------------------ */
/* the framebuffer device                                              */
/* ------------------------------------------------------------------ */

static struct umfb_pump umfb_pump_state;
static struct os_helper_thread *umfb_pump_td;
static unsigned char *umfb_mem;		/* the guest-visible pixels */
static void *umfb_map;			/* the sink's mapping */
static struct fb_info *umfb_info;
static int umfb_fd = -1;

static int umfb_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	/*
	 * Fixed geometry: viewers agreed on the sink layout before boot,
	 * so the only mode this device serves is the one it was built
	 * with.  Guests that query first (all of them do) see it and
	 * adapt; a mode-set away from it is refused.
	 */
	if (var->xres != umfb_w || var->yres != umfb_h ||
	    var->bits_per_pixel != umfb_bpp)
		return -EINVAL;

	var->xres_virtual = var->xres;
	var->yres_virtual = var->yres;
	var->red.offset = 0;		/* bytes R,G,B,A; UMFB_FMT_RGBA8888 */
	var->red.length = 8;
	var->green.offset = 8;
	var->green.length = 8;
	var->blue.offset = 16;
	var->blue.length = 8;
	var->transp.offset = 24;
	var->transp.length = 8;
	var->red.msb_right = 0;
	var->green.msb_right = 0;
	var->blue.msb_right = 0;
	var->transp.msb_right = 0;
	return 0;
}

static int umfb_set_par(struct fb_info *info)
{
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->fix.line_length = umfb_w * 4;
	return 0;
}

#define CNVT_TOHW(val, width) ((((val) << (width)) + 0x7FFF - (val)) >> 16)
static int umfb_setcolreg(unsigned int regno, unsigned int red,
			  unsigned int green, unsigned int blue,
			  unsigned int transp, struct fb_info *info)
{
	u32 v;

	if (regno >= 256)
		return 1;

	red = CNVT_TOHW(red, info->var.red.length);
	green = CNVT_TOHW(green, info->var.green.length);
	blue = CNVT_TOHW(blue, info->var.blue.length);
	transp = CNVT_TOHW(transp, info->var.transp.length);

	if (info->fix.visual == FB_VISUAL_TRUECOLOR) {
		if (regno >= 16)
			return 1;

		v = (red << info->var.red.offset) |
		    (green << info->var.green.offset) |
		    (blue << info->var.blue.offset) |
		    (transp << info->var.transp.offset);
		((u32 *) info->pseudo_palette)[regno] = v;
	}
	return 0;
}
#undef CNVT_TOHW

static int umfb_pan_display(struct fb_var_screeninfo *var,
			    struct fb_info *info)
{
	/* virtual size == visible size: offset 0 is the only valid view */
	if (var->xoffset || var->yoffset)
		return -EINVAL;
	return 0;
}

static int umfb_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	vma->vm_page_prot = pgprot_decrypted(vma->vm_page_prot);

	return remap_vmalloc_range(vma, (void *) info->fix.smem_start,
				   vma->vm_pgoff);
}

static const struct fb_ops umfb_ops = {
	.owner		= THIS_MODULE,
	__FB_DEFAULT_SYSMEM_OPS_RDWR,
	.fb_check_var	= umfb_check_var,
	.fb_set_par	= umfb_set_par,
	.fb_setcolreg	= umfb_setcolreg,
	.fb_pan_display	= umfb_pan_display,
	__FB_DEFAULT_SYSMEM_OPS_DRAW,
	.fb_mmap	= umfb_mmap,
};

static struct fb_fix_screeninfo umfb_fix = {
	.id		= "umfb",
	.type		= FB_TYPE_PACKED_PIXELS,
	.visual		= FB_VISUAL_TRUECOLOR,
	.xpanstep	= 1,
	.ypanstep	= 1,
	.ywrapstep	= 1,
	.accel		= FB_ACCEL_NONE,
};

static void umfb_kill_pump(void)
{
	if (umfb_pump_td) {
		os_kill_helper_thread(umfb_pump_td);
		umfb_pump_td = NULL;
	}
}
__uml_exitcall(umfb_kill_pump);

static int __init umfb_init(void)
{
	struct umfb_hdr *h;
	unsigned long long need;
	unsigned int stride, ts, tiles_x, tiles_y, words;
	int err;

	if (!umfb_cfg_ok)
		return -ENODEV;		/* no umlfb=: stay dormant */

	BUILD_BUG_ON(sizeof(struct umfb_hdr) != UMFB_HDR_SIZE);
	BUILD_BUG_ON(UMFB_MAX_WORDS != 128);

	stride = umfb_w * 4;
	ts = umfb_tile_shift;
	tiles_x = DIV_ROUND_UP(umfb_w, 1u << ts);
	tiles_y = DIV_ROUND_UP(umfb_h, 1u << ts);
	words = DIV_ROUND_UP(tiles_x * tiles_y, 32);
	need = UMFB_HDR_SIZE + (unsigned long long) stride * umfb_h;

	err = umfb_sink_open(umfb_src, need, &umfb_fd);
	if (err) {
		printk(KERN_ERR "umfb: cannot open sink '%s': %d\n",
		       umfb_src, err);
		return err;
	}

	umfb_map = umfb_sink_map(umfb_fd, need);
	if (!umfb_map) {
		printk(KERN_ERR "umfb: cannot map sink '%s'\n", umfb_src);
		os_close_file(umfb_fd);
		umfb_fd = -1;
		return -ENOMEM;
	}

	umfb_mem = vmalloc_32_user(PAGE_ALIGN((unsigned long) stride * umfb_h));
	if (!umfb_mem) {
		printk(KERN_ERR "umfb: no memory for the framebuffer\n");
		return -ENOMEM;
	}
	memset(umfb_mem, 0, stride * umfb_h);

	/* Publish the wire-format header before anything can attach. */
	h = umfb_map;
	memset(h, 0, UMFB_HDR_SIZE);
	h->magic = UMFB_MAGIC;
	h->version = UMFB_VERSION;
	h->width = umfb_w;
	h->height = umfb_h;
	h->stride = stride;
	h->bpp = umfb_bpp;
	h->format = UMFB_FMT_RGBA8888;
	h->tile_shift = ts;
	h->tiles_x = tiles_x;
	h->tiles_y = tiles_y;
	h->tiles_total = tiles_x * tiles_y;
	h->hz = umfb_hz;
	h->state = UMFB_STATE_INIT;
	h->full = 1;
	h->seq = 0;

	umfb_info = framebuffer_alloc(sizeof(u32) * 256, NULL);
	if (!umfb_info) {
		printk(KERN_ERR "umfb: framebuffer_alloc failed\n");
		return -ENOMEM;
	}
	umfb_info->flags |= FBINFO_VIRTFB;
	umfb_info->screen_buffer = umfb_mem;
	umfb_info->fbops = &umfb_ops;

	umfb_fix.smem_start = (unsigned long) umfb_mem;
	umfb_fix.smem_len = stride * umfb_h;
	umfb_fix.line_length = stride;
	umfb_info->fix = umfb_fix;
	umfb_info->pseudo_palette = umfb_info->par;
	umfb_info->par = NULL;

	umfb_info->var.xres = umfb_w;
	umfb_info->var.yres = umfb_h;
	umfb_info->var.xres_virtual = umfb_w;
	umfb_info->var.yres_virtual = umfb_h;
	umfb_info->var.bits_per_pixel = umfb_bpp;
	umfb_check_var(&umfb_info->var, umfb_info);
	umfb_set_par(umfb_info);

	err = fb_alloc_cmap(&umfb_info->cmap, 256, 0);
	if (err) {
		framebuffer_release(umfb_info);
		return err;
	}

	err = register_framebuffer(umfb_info);
	if (err) {
		printk(KERN_ERR "umfb: register_framebuffer failed: %d\n", err);
		fb_dealloc_cmap(&umfb_info->cmap);
		framebuffer_release(umfb_info);
		return err;
	}

	umfb_pump_state.fb = umfb_mem;
	umfb_pump_state.pix = (unsigned char *) umfb_map + UMFB_HDR_SIZE;
	umfb_pump_state.hdr = umfb_map;
	umfb_pump_state.width = umfb_w;
	umfb_pump_state.height = umfb_h;
	umfb_pump_state.stride = stride;
	umfb_pump_state.tile_shift = ts;
	umfb_pump_state.tiles_x = tiles_x;
	umfb_pump_state.tiles_y = tiles_y;
	umfb_pump_state.tiles_total = tiles_x * tiles_y;
	umfb_pump_state.words = words;
	umfb_pump_state.pump_ms = 1000 / umfb_hz;

	err = umfb_pump_start(&umfb_pump_td, &umfb_pump_state);
	if (err) {
		printk(KERN_ERR "umfb: cannot start the pump thread: %d\n", err);
		unregister_framebuffer(umfb_info);
		fb_dealloc_cmap(&umfb_info->cmap);
		framebuffer_release(umfb_info);
		return err;
	}

	printk(KERN_INFO
	       "umfb: fb0 %ux%u-32 from '%s', stride %u, %ux%u %upx tiles, %u Hz pump\n",
	       umfb_w, umfb_h, umfb_src, stride, tiles_x, tiles_y,
	       1u << ts, umfb_hz);
	return 0;
}
device_initcall(umfb_init);