/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __QCOW2_H__
#define __QCOW2_H__

/*
 * qcow2 backend for ubd (read/write subset).
 *
 * The engine (qcow2_user.c) is host-side code: it runs in kernel context
 * at open time (header parsing, L1 load -- the same place cow_user.c runs)
 * and on the ubd IO helper thread for I/O, where only os_* syscall
 * wrappers and preallocated memory may be touched.  All runtime buffers
 * live in one vmalloc'ed arena owned by struct ubd; qcow2_state_size()
 * computes it and qcow2_init() carves it up.
 *
 * Format authority: qemu docs/interop/qcow2.txt, fetched from the v10.0.0
 * tag (the file became qcow2.rst on master later); see qcow2_user.c for
 * the exact subset supported and the crash-ordering rules.
 */

#include <asm/types.h>

#define QCOW2_MAGIC		0x514649fbU	/* "QFI\xfb" */

/* Cache sizes: small on purpose; correctness first, tune with data. */
#define QCOW2_NL2	4	/* cached L2 tables */
#define QCOW2_NDC	4	/* cached decompressed clusters */
#define QCOW2_NRB	2	/* cached refcount blocks */

/* Arena space reserved for the vendored tinfl state (8376 bytes today;
 * qcow2_user.c has a compile-time check against this constant). */
#define QCOW2_INFLATE_RESERVE	9216

struct qcow2_geom {
	__u32 cluster_bits;
	__u32 l1_size;			/* entries in the active L1 table */
	__u64 cluster_size;
	__u64 virt_size;		/* the guest-visible size */
	__u64 l1_table_offset;
	__u64 refcount_table_offset;
	__u32 refcount_table_clusters;
	__u32 l2_entries;		/* entries per L2 table */
	__u32 rb_entries;		/* refcount entries per block */
	__u32 refcount_bits;
	int read_only;			/* derived: writes must be refused */
	const char *ro_reason;		/* why, for the open-time printk */
};

struct qcow2_l2 {
	__u64 l2_offset;		/* host offset of the table, 0 = free slot */
	__u32 l1_index;
	__u8 valid;
	__u8 dirty;
	__u8 *buf;
};

struct qcow2_dc {			/* decompressed-cluster cache */
	__u64 cluster;			/* cluster index it was made from */
	__u8 valid;
	__u8 *buf;
};

struct qcow2_rb {
	__u64 table_index;		/* which refcount block this is */
	__u64 offset;			/* its host offset */
	__u8 valid;
	__u8 *buf;
};

struct qcow2_state {
	struct qcow2_geom g;

	__u64 *l1;			/* host-order copy of the L1 table */
	__u8 *rft;			/* refcount table, raw (big-endian) */
	struct qcow2_l2 l2[QCOW2_NL2];
	struct qcow2_dc dc[QCOW2_NDC];
	struct qcow2_rb rb[QCOW2_NRB];
	__u8 *scratch;			/* one cluster, read-modify-write */
	__u8 *comp;			/* compressed cluster staging (2*cs) */
	__u64 comp_max;
	__u64 alloc_hint;		/* cluster index to start scans at */
	__u64 max_clusters;		/* refcount coverage limit */
	__u8 *inflate_state;		/* tinfl_decompressor lives here */
	__u32 l2_next;			/* round-robin cache cursors */
	__u32 dc_next;
	__u32 rb_next;
};

/* --- kernel context (open time) --------------------------------------- */

/* Is this fd a qcow2 image? (reads the magic; 1 = yes, 0 = no, <0 errno) */
int qcow2_identify(int fd);

/* Parse + validate the header.  On success fills *g and returns 0; on
 * failure returns -errno and points *err at a static reason string. */
int qcow2_read_geom(int fd, struct qcow2_geom *g, const char **err);

/* Size of the arena the engine needs for this geometry (bytes). */
unsigned long qcow2_state_size(const struct qcow2_geom *g);

/* Carve mem[0..len) into a state; reads the L1 table.  0 or -errno. */
int qcow2_init(int fd, const struct qcow2_geom *g, void *mem,
	       unsigned long len, struct qcow2_state **st_out);

/* --- IO helper thread safe -------------------------------------------- */

__u64 qcow2_virtual_size(const struct qcow2_state *st);
int qcow2_read(struct qcow2_state *st, int fd, __u64 off, unsigned long len,
	       void *buf);
int qcow2_write(struct qcow2_state *st, int fd, __u64 off, unsigned long len,
		const void *buf);
int qcow2_write_zeroes(struct qcow2_state *st, int fd, __u64 off,
		       unsigned long len);
int qcow2_discard(struct qcow2_state *st, int fd, __u64 off, unsigned long len);

#endif /* __QCOW2_H__ */