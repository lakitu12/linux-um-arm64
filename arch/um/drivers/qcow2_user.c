// SPDX-License-Identifier: GPL-2.0
/*
 * qcow2 backend for ubd -- the read/write subset this project needs.
 *
 * Format authority: qemu docs/interop/qcow2.txt (fetched from tag v10.0.0
 * on 2026-10-17; the file became qcow2.rst on master later).  Every layout
 * constant below cites the spec; nothing here is from memory.
 *
 * Supported: version 2/3 images without backing files, without snapshots
 * (those open read-only), without external data files, with 16-bit
 * refcounts, deflate (zlib) compression, cluster sizes 512..64K.
 * Refused with a reason at open: encryption, unknown or active
 * incompatible features (dirty/corrupt bits included), extended L2
 * entries, zstd compression, backing files, refcount widths != 16.
 *
 * This file is host-side code in the UML sense: at open time it runs in
 * kernel context (like cow_user.c), and during I/O on the ubd helper
 * thread, which is NOT part of the kernel -- only os_* syscall wrappers,
 * preallocated memory (the arena) and the vendored tinfl may be used.
 * No printk, no allocation, no locks in any function below.
 *
 * Crash ordering (the rule is: never a cluster that is referenced but
 * unreferenced-by-refcount; leaks are repairable by qemu-img check, the
 * opposite is not):
 *   - write of a NEW cluster: refcount=1 (during alloc) -> data -> L2
 *     entry -> decrement of the replaced cluster's refcounts.
 *   - write-zeroes/discard of a full cluster: L2 entry cleared -> then
 *     refcount decremented.
 *   - new refcount block: block (with its self-count) -> refcount table
 *     entry.
 *   - new L2 table: zeroed table -> (refcount=1 from alloc) -> L1 entry.
 * A crash in any window leaves at worst an unreferenced cluster with
 * refcount 1 ("leaked cluster" for qemu-img check -r) or an unreferenced
 * refcount block; never a dangling reference to uninitialized content.
 *
 * L2 entries with the COPIED bit (bit 63 = refcount is exactly 1) are
 * written in place; everything else takes the copy-on-write path, so
 * shared clusters are never written through.
 */

#ifdef QCOW2_SELFTEST
#include "selftest_os.h"
#include <stdio.h>
#define qc2_dbg(...) fprintf(stderr, "qc2dbg: " __VA_ARGS__)
#else
#include <os.h>
#define qc2_dbg(...) do { } while (0)
#endif
#include <errno.h>
#include <string.h>

#include "qcow2.h"
#include "qcow2_inflate.h"

/* The vendored tinfl state must fit the arena reserve promised in qcow2.h. */
typedef char qcow2_inflate_reserve_check[(sizeof(tinfl_decompressor) <=
					  QCOW2_INFLATE_RESERVE) ? 1 : -1];

/* L2 entry flags, spec "L2 table entry" / "Standard Cluster Descriptor". */
#define QCOW2_OFF_MASK		0x00fffffffffffe00ULL	/* bits 9-55 */
#define QCOW2_L2_COMPRESSED	(1ULL << 62)
#define QCOW2_OFLAG_ZERO	1ULL		/* bit 0 */
#define QCOW2_COPIED		(1ULL << 63)

/*
 * Bit 0 is the zero flag only on *standard* descriptors; on compressed
 * entries bits 0..x-1 are the host offset (spec "Compressed Clusters
 * Descriptor"), so compressed must be excluded explicitly.
 */
static int qc2_zero_entry(__u64 e)
{
	return !(e & QCOW2_L2_COMPRESSED) && (e & QCOW2_OFLAG_ZERO);
}

/* ---- byte order (spec: "All numbers in qcow2 are stored in Big Endian") */

static __u16 be16(const __u8 *p)
{
	return (__u16)(((__u16)p[0] << 8) | p[1]);
}

static __u32 be32(const __u8 *p)
{
	return ((__u32)p[0] << 24) | ((__u32)p[1] << 16) |
	       ((__u32)p[2] << 8) | (__u32)p[3];
}

static __u64 be64(const __u8 *p)
{
	return ((__u64)be32(p) << 32) | be32(p + 4);
}

static void be16_put(__u8 *p, __u16 v)
{
	p[0] = (__u8)(v >> 8);
	p[1] = (__u8)v;
}

static void be64_put(__u8 *p, __u64 v)
{
	__u32 hi = (__u32)(v >> 32), lo = (__u32)v;

	p[0] = (__u8)(hi >> 24); p[1] = (__u8)(hi >> 16);
	p[2] = (__u8)(hi >> 8);  p[3] = (__u8)hi;
	p[4] = (__u8)(lo >> 24); p[5] = (__u8)(lo >> 16);
	p[6] = (__u8)(lo >> 8);  p[7] = (__u8)lo;
}

/* ---- I/O wrappers: must move exactly len bytes or fail ----------------- */

static int qc2_pread_full(int fd, void *buf, __u64 len, __u64 off)
{
	__u8 *p = buf;
	__u64 done = 0;

	while (done < len) {
		int n = os_pread_file(fd, p + done, (int)(len - done), off + done);

		if (n < 0)
			return n;
		if (n == 0)
			return -EIO;
		done += n;
	}
	return 0;
}

static int qc2_pwrite_full(int fd, const void *buf, __u64 len, __u64 off)
{
	const __u8 *p = buf;
	__u64 done = 0;

	while (done < len) {
		int n = os_pwrite_file(fd, p + done, (int)(len - done), off + done);

		if (n < 0)
			return n;
		if (n == 0)
			return -EIO;
		done += n;
	}
	return 0;
}

/* ---- header parsing (kernel context) ---------------------------------- */

int qcow2_identify(int fd)
{
	__u8 buf[4];
	int n = os_pread_file(fd, buf, 4, 0);

	if (n < 0)
		return n;
	if (n < 4)
		return 0;
	return be32(buf) == QCOW2_MAGIC ? 1 : 0;
}

int qcow2_read_geom(int fd, struct qcow2_geom *g, const char **err)
{
	__u8 h[104];
	__u32 version, crypt, rtc, nb_snap, refcount_order = 4, l1_size;
	__u64 cluster_size, l1_off, rft_off, incompat = 0, autoclear = 0;
	__u64 l1_cov, max_clusters;
	int r;

	*err = "invalid header";
	r = qc2_pread_full(fd, h, 8, 0);
	if (r)
		return r;
	if (be32(h) != QCOW2_MAGIC) {
		*err = "not a qcow2 image";
		return -EINVAL;
	}
	version = be32(h + 4);
	if (version != 2 && version != 3) {
		*err = "unsupported qcow2 version";
		return -EINVAL;
	}
	r = qc2_pread_full(fd, h + 8, version == 3 ? 96 : 64, 8);
	if (r)
		return r;

	/* spec "Header": byte offsets as field starts */
	if (be64(h + 8)) {
		*err = "backing files are not supported";
		return -ENOTSUP;
	}
	g->cluster_bits = be32(h + 20);
	if (g->cluster_bits < 9 || g->cluster_bits > 16) {
		*err = "unsupported cluster size (need 512..64K)";
		return -ENOTSUP;
	}
	g->virt_size = be64(h + 24);
	crypt = be32(h + 32);
	if (crypt) {
		*err = "encrypted images are not supported";
		return -ENOTSUP;
	}
	l1_size = be32(h + 36);
	l1_off = be64(h + 40);
	rft_off = be64(h + 48);
	rtc = be32(h + 56);
	nb_snap = be32(h + 60);
	if (version == 3) {
		incompat = be64(h + 72);
		autoclear = be64(h + 88);
		refcount_order = be32(h + 96);
		if (incompat) {
			if (incompat & (1ULL << 0))
				*err = "refcounts marked dirty (run qemu-img check)";
			else if (incompat & (1ULL << 1))
				*err = "image marked corrupt";
			else if (incompat & (1ULL << 2))
				*err = "external data file";
			else if (incompat & (1ULL << 3))
				*err = "non-deflate (zstd) compression";
			else if (incompat & (1ULL << 4))
				*err = "extended L2 entries";
			else
				*err = "unknown incompatible feature";
			return (incompat & 3) ? -EUCLEAN : -ENOTSUP;
		}
	}
	if (refcount_order != 4) {
		*err = "unsupported refcount width (need 16-bit)";
		return -ENOTSUP;
	}
	cluster_size = (__u64)1 << g->cluster_bits;
	if (!l1_size || l1_size > 1048576) {
		*err = "bad L1 size";
		return -EINVAL;
	}
	if (!l1_off || (l1_off & (cluster_size - 1))) {
		*err = "bad L1 table offset";
		return -EINVAL;
	}
	if (!rft_off || (rft_off & (cluster_size - 1)) || !rtc || rtc > 64) {
		*err = "bad refcount table";
		return -EINVAL;
	}

	g->l1_size = l1_size;
	g->cluster_size = cluster_size;
	g->l1_table_offset = l1_off;
	g->refcount_table_offset = rft_off;
	g->refcount_table_clusters = rtc;
	g->l2_entries = (__u32)(cluster_size / 8);
	g->refcount_bits = 16;
	g->rb_entries = (__u32)(cluster_size / 2);	/* cs * 8 / 16 */

	/* L1 must cover the virtual size (spec "Cluster mapping"). */
	l1_cov = (__u64)l1_size * g->l2_entries * cluster_size;
	if (l1_cov < g->virt_size) {
		*err = "L1 table too small for the virtual size";
		return -EINVAL;
	}
	/* Refcount coverage of the host file ("Host cluster management"). */
	max_clusters = (__u64)rtc * (cluster_size / 8) * g->rb_entries;
	if (!max_clusters) {
		*err = "bad refcount coverage";
		return -EINVAL;
	}

	g->read_only = 0;
	g->ro_reason = NULL;
	if (autoclear) {
		g->read_only = 1;	/* bitmaps et al would go stale */
		g->ro_reason = "autoclear features (bitmaps) present";
	} else if (nb_snap) {
		g->read_only = 1;
		g->ro_reason = "internal snapshots present";
	}
	return 0;
}

/* ---- arena layout / init ---------------------------------------------- */

static unsigned long qcow2_align(unsigned long v, unsigned long a)
{
	return (v + a - 1) & ~(a - 1);
}

static unsigned long qcow2_layout(const struct qcow2_geom *g,
				  struct qcow2_state *st, void *mem)
{
	unsigned long off = qcow2_align(sizeof(struct qcow2_state), 64);
	unsigned long cs = g->cluster_size;
	int i;

#define CARVE(field, bytes) do {					\
		if (st)							\
			st->field = (void *)((__u8 *)mem + off);	\
		off = qcow2_align(off + (bytes), 64);			\
	} while (0)

	CARVE(l1, (unsigned long)g->l1_size * 8);
	CARVE(rft, (unsigned long)g->refcount_table_clusters * cs);
	CARVE(comp, 2 * cs);		/* max compressed cluster run, see spec */
	CARVE(scratch, cs);
	for (i = 0; i < QCOW2_NL2; i++) {
		if (st)
			st->l2[i].buf = (__u8 *)mem + off;
		off = qcow2_align(off + cs, 64);
	}
	for (i = 0; i < QCOW2_NDC; i++) {
		if (st)
			st->dc[i].buf = (__u8 *)mem + off;
		off = qcow2_align(off + cs, 64);
	}
	for (i = 0; i < QCOW2_NRB; i++) {
		if (st)
			st->rb[i].buf = (__u8 *)mem + off;
		off = qcow2_align(off + cs, 64);
	}
	CARVE(inflate_state, QCOW2_INFLATE_RESERVE);
#undef CARVE

	return qcow2_align(off, 4096);
}

unsigned long qcow2_state_size(const struct qcow2_geom *g)
{
	return qcow2_layout(g, NULL, NULL);
}

int qcow2_init(int fd, const struct qcow2_geom *g, void *mem,
	       unsigned long len, struct qcow2_state **st_out)
{
	struct qcow2_state *st = mem;
	unsigned long need = qcow2_layout(g, NULL, NULL);
	__u32 i;
	int r;

	if (len < need)
		return -EINVAL;
	memset(mem, 0, need);
	st->g = *g;
	st->comp_max = 2 * g->cluster_size;
	st->alloc_hint = 1;
	st->max_clusters = (__u64)g->refcount_table_clusters *
			   (g->cluster_size / 8) * g->rb_entries;
	qcow2_layout(g, st, mem);

	r = qc2_pread_full(fd, st->l1, (__u64)g->l1_size * 8,
			   g->l1_table_offset);
	if (r)
		return r;
	for (i = 0; i < g->l1_size; i++)
		st->l1[i] = be64((__u8 *)st->l1 + (__u64)i * 8);

	r = qc2_pread_full(fd, st->rft,
			   (__u64)g->refcount_table_clusters *
			   g->cluster_size, g->refcount_table_offset);
	if (r)
		return r;

	*st_out = st;
	return 0;
}

__u64 qcow2_virtual_size(const struct qcow2_state *st)
{
	return st->g.virt_size;
}

/* ---- caches ------------------------------------------------------------ */

static struct qcow2_l2 *l2_lookup(struct qcow2_state *st, __u64 l2_offset)
{
	int i;

	for (i = 0; i < QCOW2_NL2; i++)
		if (st->l2[i].valid && st->l2[i].l2_offset == l2_offset)
			return &st->l2[i];
	return NULL;
}

static int l2_get(struct qcow2_state *st, int fd, __u32 l1_index,
		  __u64 l2_offset, __u8 **buf_out)
{
	struct qcow2_l2 *l;
	int r;

	l = l2_lookup(st, l2_offset);
	if (!l) {
		l = &st->l2[st->l2_next++ % QCOW2_NL2];
		if (l->valid && l->dirty) {
			r = qc2_pwrite_full(fd, l->buf, st->g.cluster_size,
					    l->l2_offset);
			if (r)
				return r;
		}
		l->l2_offset = l2_offset;
		l->l1_index = l1_index;
		l->valid = 1;
		l->dirty = 0;
		r = qc2_pread_full(fd, l->buf, st->g.cluster_size, l2_offset);
		if (r) {
			l->valid = 0;
			return r;
		}
	}
	*buf_out = l->buf;
	return 0;
}

static struct qcow2_dc *dc_lookup(struct qcow2_state *st, __u64 ci)
{
	int i;

	for (i = 0; i < QCOW2_NDC; i++)
		if (st->dc[i].valid && st->dc[i].cluster == ci)
			return &st->dc[i];
	return NULL;
}

static void dc_invalidate(struct qcow2_state *st, __u64 ci)
{
	struct qcow2_dc *d = dc_lookup(st, ci);

	if (d)
		d->valid = 0;
}

/* ---- compression ------------------------------------------------------- */

/*
 * Compressed cluster descriptor, spec "Compressed Clusters Descriptor"
 * (x = 62 - (cluster_bits - 8)): bits 0..x-1 host offset, bits x..61 the
 * number of additional 512-byte sectors beyond the sector containing the
 * offset.  With cluster_bits <= 16 the run is at most 2 * cluster_size.
 */
static void qc2_comp_desc(const struct qcow2_geom *g, __u64 e, __u64 *off,
			  __u64 *len)
{
	__u32 x = 62 - (g->cluster_bits - 8);
	__u64 o = e & (((__u64)1 << x) - 1);
	__u64 nsec = (e >> x) & ((((__u64)1 << (62 - x)) - 1));

	*off = o;
	*len = 512 - (o & 511) + nsec * 512;
}

static int inflate_cluster(struct qcow2_state *st, const __u8 *in, __u64 in_len,
			   __u8 *out)
{
	tinfl_decompressor *dec = (tinfl_decompressor *)st->inflate_state;
	size_t isz = in_len;
	size_t osz = st->g.cluster_size;	/* capacity on entry */
	tinfl_status s;

	tinfl_init(dec);
	/* Raw deflate, no zlib header ("The deflate compression type is
	 * called zlib in QEMU ... do not have zlib headers").  On DONE,
	 * *pOut_buf_size holds the bytes produced (verified against the
	 * vendored copy; see the selftest). */
	s = tinfl_decompress(dec, in, &isz, out, out, &osz,
			     TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
	if (s != TINFL_STATUS_DONE)
		return -EIO;
	if (osz != st->g.cluster_size)
		return -EIO;		/* short or overlong stream */
	return 0;
}

static int dc_get(struct qcow2_state *st, int fd, __u64 ci, __u64 e,
		  __u8 **buf_out)
{
	struct qcow2_dc *d = dc_lookup(st, ci);
	__u64 off, len;
	int r;

	if (d) {
		*buf_out = d->buf;
		return 0;
	}
	d = &st->dc[st->dc_next++ % QCOW2_NDC];
	qc2_comp_desc(&st->g, e, &off, &len);
	if (len > st->comp_max)
		return -EIO;
	r = qc2_pread_full(fd, st->comp, len, off);
	if (r)
		return r;
	r = inflate_cluster(st, st->comp, len, d->buf);
	if (r)
		return r;
	d->cluster = ci;
	d->valid = 1;
	*buf_out = d->buf;
	return 0;
}

/* ---- refcounts ---------------------------------------------------------- */

static __u64 qc2_table_entries(const struct qcow2_state *st)
{
	return (__u64)st->g.refcount_table_clusters * (st->g.cluster_size / 8);
}

static struct qcow2_rb *rb_lookup(struct qcow2_state *st, __u64 ti)
{
	int i;

	for (i = 0; i < QCOW2_NRB; i++)
		if (st->rb[i].valid && st->rb[i].table_index == ti)
			return &st->rb[i];
	return NULL;
}

static int rb_load(struct qcow2_state *st, int fd, __u64 ti, __u64 blk_off,
		   struct qcow2_rb **out)
{
	struct qcow2_rb *b = rb_lookup(st, ti);
	int r;

	if (!b) {
		b = &st->rb[st->rb_next++ % QCOW2_NRB];
		r = qc2_pread_full(fd, b->buf, st->g.cluster_size, blk_off);
		if (r) {
			b->valid = 0;
			return r;
		}
		b->table_index = ti;
		b->offset = blk_off;
		b->valid = 1;
	}
	*out = b;
	return 0;
}

static int refcount_get(struct qcow2_state *st, int fd, __u64 ci, __u16 *val)
{
	__u64 ti = ci / st->g.rb_entries;
	__u64 te;
	struct qcow2_rb *b;
	int r;

	if (ti >= qc2_table_entries(st))
		return -ENOSPC;
	te = be64(st->rft + ti * 8);
	if (!te) {
		*val = 0;
		return 0;
	}
	r = rb_load(st, fd, ti, te & QCOW2_OFF_MASK, &b);
	if (r)
		return r;
	*val = be16(b->buf + (ci % st->g.rb_entries) * 2);
	return 0;
}

/* The block for ti must already exist (refblock_ensure below). */
static int refcount_set(struct qcow2_state *st, int fd, __u64 ci, __u16 val)
{
	__u64 ti = ci / st->g.rb_entries;
	__u64 te = be64(st->rft + ti * 8);
	struct qcow2_rb *b;
	int r;

	if (!te)
		return -EIO;
	r = rb_load(st, fd, ti, te & QCOW2_OFF_MASK, &b);
	if (r) {
		qc2_dbg("refcount_set: rb_load(%llu) rc=%d\n",
			(unsigned long long)ti, r);
		return r;
	}
	be16_put(b->buf + (ci % st->g.rb_entries) * 2, val);
	r = qc2_pwrite_full(fd, b->buf, st->g.cluster_size, b->offset);
	if (r)
		qc2_dbg("refcount_set: pwrite blk@%llx rc=%d\n",
			(unsigned long long)b->offset, r);
	return r;
}

/*
 * Allocate a fresh refcount block for table index ti.  Placement scan
 * accepts a free cluster b when either b is inside the block's own range
 * (its count then lives in the block itself, the usual sequential case)
 * or b's covering block already exists (its count then goes there).  The
 * block is written before the refcount table points at it.
 */
static int refblock_make(struct qcow2_state *st, int fd, __u64 ti,
			 __u64 avoid, struct qcow2_rb **out)
{
	__u64 b;
	__u16 v;
	struct qcow2_rb *blk;
	int r;

	for (b = st->alloc_hint; ; b++) {
		if (b >= st->max_clusters) {
			qc2_dbg("refblock: scan exhausted b=%llu ti=%llu\n",
				(unsigned long long)b, (unsigned long long)ti);
			return -ENOSPC;
		}
		if (b == avoid || b == 0)
			continue;
		r = refcount_get(st, fd, b, &v);
		if (r)
			return r;
		if (v)
			continue;
		if (b / st->g.rb_entries == ti)
			break;		/* self-covered */
		if (be64(st->rft + (b / st->g.rb_entries) * 8))
			break;		/* covered by an existing block */
	}
	qc2_dbg("refblock: new block at b=%llu ti=%llu avoid=%llu\n",
		(unsigned long long)b, (unsigned long long)ti,
		(unsigned long long)avoid);

	blk = &st->rb[st->rb_next++ % QCOW2_NRB];
	memset(blk->buf, 0, st->g.cluster_size);
	be16_put(blk->buf + (b % st->g.rb_entries) * 2, 1);	/* its own count */
	blk->table_index = ti;
	blk->offset = b << st->g.cluster_bits;
	blk->valid = 1;

	r = qc2_pwrite_full(fd, blk->buf, st->g.cluster_size, b << st->g.cluster_bits);
	if (r)
		return r;
	be64_put(st->rft + ti * 8, b << st->g.cluster_bits);
	r = qc2_pwrite_full(fd, st->rft + ti * 8, 8,
			    st->g.refcount_table_offset + ti * 8);
	if (r)
		return r;
	if (b / st->g.rb_entries != ti) {
		/* covered elsewhere: record its count through that block */
		r = refcount_set(st, fd, b, 1);
		if (r)
			return r;
	}
	st->alloc_hint = b + 1;
	*out = blk;
	return 0;
}

static int refblock_ensure(struct qcow2_state *st, int fd, __u64 ti,
			   __u64 avoid, struct qcow2_rb **out)
{
	__u64 te = be64(st->rft + ti * 8);

	if (te)
		return rb_load(st, fd, ti, te & QCOW2_OFF_MASK, out);
	return refblock_make(st, fd, ti, avoid, out);
}

/* First-fit from a monotonic hint; avoids cluster 0 (the header). */
static int alloc_cluster(struct qcow2_state *st, int fd, __u64 avoid,
			 __u64 *out)
{
	__u64 ci;
	__u16 v;
	struct qcow2_rb *b;
	int r;

	for (ci = st->alloc_hint; ci < st->max_clusters; ci++) {
		if (!ci || ci == avoid)
			continue;
		r = refcount_get(st, fd, ci, &v);
		if (r) {
			qc2_dbg("alloc: refcount_get(%llu) -> %d\n",
				(unsigned long long)ci, r);
			return r;
		}
		if (!v)
			break;
		st->alloc_hint = ci + 1;
	}
	if (ci >= st->max_clusters) {
		qc2_dbg("alloc: scan exhausted (hint=%llu max=%llu)\n",
			(unsigned long long)st->alloc_hint,
			(unsigned long long)st->max_clusters);
		return -ENOSPC;
	}
	qc2_dbg("alloc: free ci=%llu (avoid=%llu)\n",
		(unsigned long long)ci, (unsigned long long)avoid);

	r = refblock_ensure(st, fd, ci / st->g.rb_entries, ci, &b);
	if (r)
		return r;
	r = refcount_set(st, fd, ci, 1);
	if (r)
		return r;
	st->alloc_hint = ci + 1;
	*out = ci;
	return 0;
}

/* Allocation of a new L2 table; the L1 entry is the last thing written. */
static int l2_new(struct qcow2_state *st, int fd, __u32 l1_index, __u8 **buf_out)
{
	struct qcow2_l2 *l;
	__u8 tmp[8];
	__u64 nc;
	int r;

	r = alloc_cluster(st, fd, (__u64)-1, &nc);
	if (r)
		return r;

	l = &st->l2[st->l2_next++ % QCOW2_NL2];
	if (l->valid && l->dirty) {
		r = qc2_pwrite_full(fd, l->buf, st->g.cluster_size, l->l2_offset);
		if (r)
			return r;
	}
	memset(l->buf, 0, st->g.cluster_size);
	l->l2_offset = nc << st->g.cluster_bits;
	l->l1_index = l1_index;
	l->valid = 1;
	l->dirty = 0;

	r = qc2_pwrite_full(fd, l->buf, st->g.cluster_size, l->l2_offset);
	if (r)
		return r;
	st->l1[l1_index] = l->l2_offset | QCOW2_COPIED;
	be64_put(tmp, st->l1[l1_index]);
	r = qc2_pwrite_full(fd, tmp, 8, st->g.l1_table_offset +
			    (__u64)l1_index * 8);
	if (r)
		return r;
	*buf_out = l->buf;
	return 0;
}

/* ---- read-modify-write of one cluster ---------------------------------- */

struct qc2_old {
	int kind;
	__u64 off;
	__u64 len;
};

#define QC2_OLD_NONE	0
#define QC2_OLD_STD	1
#define QC2_OLD_COMP	2

/* Fill scratch with the current content of the cluster behind entry e. */
static int materialize(struct qcow2_state *st, int fd, __u64 e,
		       struct qc2_old *old)
{
	__u64 off, len;
	int r;

	old->kind = QC2_OLD_NONE;
	old->off = 0;
	old->len = 0;

	if (e == 0) {
		memset(st->scratch, 0, st->g.cluster_size);
		return 0;
	}
	if (e & QCOW2_L2_COMPRESSED) {
		qc2_comp_desc(&st->g, e, &off, &len);
		if (len > st->comp_max)
			return -EIO;
		r = qc2_pread_full(fd, st->comp, len, off);
		if (r)
			return r;
		r = inflate_cluster(st, st->comp, len, st->scratch);
		if (r)
			return r;
		old->kind = QC2_OLD_COMP;
		old->off = off;
		old->len = len;
		return 0;
	}
	if (qc2_zero_entry(e)) {
		/* Reads as zeros; a nonzero offset here describes a
		 * preallocation ("the host cluster offset can be used to
		 * describe a preallocation").  Report it so a replacement
		 * can drop the reference, or the cluster leaks. */
		memset(st->scratch, 0, st->g.cluster_size);
		if (e & QCOW2_OFF_MASK) {
			old->kind = QC2_OLD_STD;
			old->off = e & QCOW2_OFF_MASK;
		}
		return 0;
	}
	off = e & QCOW2_OFF_MASK;
	if (!off)
		return -EIO;
	r = qc2_pread_full(fd, st->scratch, st->g.cluster_size, off);
	if (r)
		return r;
	old->kind = QC2_OLD_STD;
	old->off = off;
	return 0;
}

static int decref_cluster(struct qcow2_state *st, int fd, __u64 ci)
{
	__u16 v;
	int r = refcount_get(st, fd, ci, &v);

	if (r) {
		qc2_dbg("decref: get(%llu) rc=%d\n",
			(unsigned long long)ci, r);
		return r;
	}
	if (!v) {
		qc2_dbg("decref: ci=%llu already free\n",
			(unsigned long long)ci);
		return -EIO;		/* freeing something free: inconsistent */
	}
	r = refcount_set(st, fd, ci, (__u16)(v - 1));
	if (r)
		qc2_dbg("decref: set(%llu, %u) rc=%d\n",
			(unsigned long long)ci, (unsigned int)(v - 1), r);
	return r;
}

/* Drop the reference(s) the replaced entry held.  Runs after the L2 write. */
static int old_drop(struct qcow2_state *st, int fd, const struct qc2_old *old)
{
	__u64 c, c0, c1;
	int r;

	if (old->kind == QC2_OLD_STD)
		return decref_cluster(st, fd, old->off >> st->g.cluster_bits);
	if (old->kind == QC2_OLD_COMP) {
		c0 = old->off >> st->g.cluster_bits;
		c1 = (old->off + old->len - 1) >> st->g.cluster_bits;
		for (c = c0; c <= c1; c++) {
			r = decref_cluster(st, fd, c);
			if (r)
				return r;
		}
	}
	return 0;
}

/*
 * Copy-on-write replacement of one cluster: current content + [patch..
 * patch+len) (copied or zeroed) -> a fresh cluster, L2 re-pointed, old
 * references dropped.  Order: alloc (count=1), data, L2, drop.
 */
static int qc2_rmw(struct qcow2_state *st, int fd, __u8 *l2b, __u64 l2_off,
		   __u32 l2i, __u64 ci, __u64 e, const __u8 *src,
		   __u64 patch_ofs, __u64 patch_len, int zero)
{
	struct qc2_old old;
	__u64 nc;
	int r;

	qc2_dbg("rmw: ci=%llu e=%llx ofs=%llu len=%llu zero=%d\n",
		(unsigned long long)ci, (unsigned long long)e,
		(unsigned long long)patch_ofs, (unsigned long long)patch_len,
		zero);
	r = materialize(st, fd, e, &old);
	if (r) {
		qc2_dbg("rmw: materialize rc=%d\n", r);
		return r;
	}
	if (zero)
		memset(st->scratch + patch_ofs, 0, patch_len);
	else
		memcpy(st->scratch + patch_ofs, src, patch_len);
	r = alloc_cluster(st, fd, (__u64)-1, &nc);
	if (r) {
		qc2_dbg("rmw: alloc rc=%d\n", r);
		return r;
	}
	r = qc2_pwrite_full(fd, st->scratch, st->g.cluster_size,
			    nc << st->g.cluster_bits);
	if (r) {
		qc2_dbg("rmw: data pwrite rc=%d\n", r);
		return r;
	}
	be64_put(l2b + (__u64)l2i * 8, (nc << st->g.cluster_bits) | QCOW2_COPIED);
	r = qc2_pwrite_full(fd, l2b, st->g.cluster_size, l2_off);
	if (r) {
		qc2_dbg("rmw: l2 pwrite rc=%d\n", r);
		return r;
	}
	dc_invalidate(st, ci);
	r = old_drop(st, fd, &old);
	if (r) {
		qc2_dbg("rmw: old_drop rc=%d\n", r);
		return r;
	}
	return 0;
}

/* ---- public I/O -------------------------------------------------------- */

int qcow2_read(struct qcow2_state *st, int fd, __u64 off, unsigned long len,
	       void *buf)
{
	__u8 *p = buf;

	while (len) {
		__u64 cs = st->g.cluster_size;
		__u64 ci = off >> st->g.cluster_bits;
		__u64 l1i = ci / st->g.l2_entries;
		__u32 l2i = (__u32)(ci % st->g.l2_entries);
		__u64 cofs = off & (cs - 1);
		__u64 r = cs - cofs < len ? cs - cofs : len;
		__u64 e;
		__u8 *l2b;
		int rc;

		if (l1i >= st->g.l1_size)
			return -EIO;
		if (!st->l1[l1i]) {
			memset(p, 0, r);
			goto next;
		}
		rc = l2_get(st, fd, (__u32)l1i, st->l1[l1i] & QCOW2_OFF_MASK, &l2b);
		if (rc)
			return rc;
		e = be64(l2b + (__u64)l2i * 8);
		if (e & QCOW2_L2_COMPRESSED) {
			__u8 *cb;

			rc = dc_get(st, fd, ci, e, &cb);
			if (rc)
				return rc;
			memcpy(p, cb + cofs, r);
		} else if (e == 0 || qc2_zero_entry(e)) {
			memset(p, 0, r);
		} else {
			__u64 poff = e & QCOW2_OFF_MASK;

			if (!poff)
				return -EIO;
			rc = qc2_pread_full(fd, p, r, poff + cofs);
			if (rc)
				return rc;
		}
next:
		p += r;
		off += r;
		len -= r;
	}
	return 0;
}

int qcow2_write(struct qcow2_state *st, int fd, __u64 off, unsigned long len,
		const void *buf)
{
	const __u8 *p = buf;

	if (st->g.read_only)
		return -EROFS;
	qc2_dbg("write: off=%llu len=%lu\n", (unsigned long long)off, len);
	while (len) {
		__u64 cs = st->g.cluster_size;
		__u64 ci = off >> st->g.cluster_bits;
		__u64 l1i = ci / st->g.l2_entries;
		__u32 l2i = (__u32)(ci % st->g.l2_entries);
		__u64 cofs = off & (cs - 1);
		__u64 r = cs - cofs < (__u64)len ? cs - cofs : (__u64)len;
		__u64 l2_off, e;
		__u8 *l2b;
		int rc;

		if (l1i >= st->g.l1_size)
			return -EIO;
		if (!st->l1[l1i]) {
			rc = l2_new(st, fd, (__u32)l1i, &l2b);
			if (rc)
				return rc;
		} else {
			rc = l2_get(st, fd, (__u32)l1i,
				    st->l1[l1i] & QCOW2_OFF_MASK, &l2b);
			if (rc)
				return rc;
		}
		l2_off = st->l1[l1i] & QCOW2_OFF_MASK;
		e = be64(l2b + (__u64)l2i * 8);
		if (e && !qc2_zero_entry(e) && !(e & QCOW2_L2_COMPRESSED) &&
		    (e & QCOW2_COPIED) && (e & QCOW2_OFF_MASK)) {
			/* refcount is exactly one: write through in place */
			rc = qc2_pwrite_full(fd, p, r,
					     (e & QCOW2_OFF_MASK) + cofs);
			if (rc)
				return rc;
		} else {
			rc = qc2_rmw(st, fd, l2b, l2_off, l2i, ci, e, p,
				     cofs, r, 0);
			if (rc)
				return rc;
		}
		p += r;
		off += r;
		len -= r;
	}
	return 0;
}

/*
 * Zero a range.  Full clusters are dropped (L2 -> zero descriptor) so the
 * space is reclaimed; partial ranges on unique uncompressed clusters are
 * zeroed through, everything else copies aside like a write.
 */
int qcow2_write_zeroes(struct qcow2_state *st, int fd, __u64 off,
		       unsigned long len)
{
	if (st->g.read_only)
		return -EROFS;
	while (len) {
		__u64 cs = st->g.cluster_size;
		__u64 ci = off >> st->g.cluster_bits;
		__u64 l1i = ci / st->g.l2_entries;
		__u32 l2i = (__u32)(ci % st->g.l2_entries);
		__u64 cofs = off & (cs - 1);
		__u64 r = cs - cofs < (__u64)len ? cs - cofs : (__u64)len;
		int full = (cofs == 0 && r == cs);
		__u64 e;
		__u8 *l2b;
		int rc;

		if (l1i >= st->g.l1_size)
			return -EIO;
		if (!st->l1[l1i])
			goto next;		/* nothing mapped: already zero */
		rc = l2_get(st, fd, (__u32)l1i, st->l1[l1i] & QCOW2_OFF_MASK,
			    &l2b);
		if (rc)
			return rc;
		e = be64(l2b + (__u64)l2i * 8);
		if (e == 0 || qc2_zero_entry(e))
			goto next;
		if (full) {
			struct qc2_old old;

			old.kind = QC2_OLD_NONE;
			old.off = 0;
			old.len = 0;
			if (e & QCOW2_L2_COMPRESSED) {
				qc2_comp_desc(&st->g, e, &old.off, &old.len);
				old.kind = QC2_OLD_COMP;
			} else {
				old.off = e & QCOW2_OFF_MASK;
				if (!old.off)
					return -EIO;
				old.kind = QC2_OLD_STD;
			}
			be64_put(l2b + (__u64)l2i * 8, QCOW2_OFLAG_ZERO);
			rc = qc2_pwrite_full(fd, l2b, cs,
					     st->l1[l1i] & QCOW2_OFF_MASK);
			if (rc)
				return rc;
			dc_invalidate(st, ci);
			rc = old_drop(st, fd, &old);	/* L2 written first */
			if (rc)
				return rc;
		} else if (!(e & QCOW2_L2_COMPRESSED) &&
			   (e & QCOW2_OFF_MASK) && (e & QCOW2_COPIED)) {
			memset(st->comp, 0, r);
			rc = qc2_pwrite_full(fd, st->comp, r,
					     (e & QCOW2_OFF_MASK) + cofs);
			if (rc)
				return rc;
		} else {
			rc = qc2_rmw(st, fd, l2b, st->l1[l1i] & QCOW2_OFF_MASK,
				     l2i, ci, e, NULL, cofs, r, 1);
			if (rc)
				return rc;
		}
next:
		off += r;
		len -= r;
	}
	return 0;
}

/*
 * Discard: advisory, so partial clusters are left alone; full clusters
 * drop their mapping (reads return zeros).
 */
int qcow2_discard(struct qcow2_state *st, int fd, __u64 off, unsigned long len)
{
	if (st->g.read_only)
		return -EROFS;
	while (len) {
		__u64 cs = st->g.cluster_size;
		__u64 ci = off >> st->g.cluster_bits;
		__u64 l1i = ci / st->g.l2_entries;
		__u32 l2i = (__u32)(ci % st->g.l2_entries);
		__u64 cofs = off & (cs - 1);
		__u64 r = cs - cofs < (__u64)len ? cs - cofs : (__u64)len;
		__u64 e;
		__u8 *l2b;
		int rc;

		if (cofs != 0 || r != cs)
			goto next;		/* partial: leave as is */
		if (l1i >= st->g.l1_size)
			return -EIO;
		if (!st->l1[l1i])
			goto next;
		rc = l2_get(st, fd, (__u32)l1i, st->l1[l1i] & QCOW2_OFF_MASK,
			    &l2b);
		if (rc)
			return rc;
		e = be64(l2b + (__u64)l2i * 8);
		if (e == 0 || qc2_zero_entry(e))
			goto next;
		{
			struct qc2_old old;

			old.kind = QC2_OLD_NONE;
			old.off = 0;
			old.len = 0;
			if (e & QCOW2_L2_COMPRESSED) {
				qc2_comp_desc(&st->g, e, &old.off, &old.len);
				old.kind = QC2_OLD_COMP;
			} else {
				old.off = e & QCOW2_OFF_MASK;
				if (!old.off)
					return -EIO;
				old.kind = QC2_OLD_STD;
			}
			be64_put(l2b + (__u64)l2i * 8, 0);
			rc = qc2_pwrite_full(fd, l2b, cs,
					     st->l1[l1i] & QCOW2_OFF_MASK);
			if (rc)
				return rc;
			dc_invalidate(st, ci);
			rc = old_drop(st, fd, &old);
			if (rc)
				return rc;
		}
next:
		off += r;
		len -= r;
	}
	return 0;
}