// SPDX-License-Identifier: GPL-2.0
/*
 * Host-side functional test for the ubd qcow2 engine
 * (arch/um/drivers/qcow2_user.c).
 *
 * The engine is driven directly with the same os_* shims the UML build
 * uses (selftest_os.h), against images produced by qemu-img.  Correctness
 * is judged by an external model, not by the engine itself:
 *
 *   run     phase 1  read the whole image, compare against the source raw
 *           phase 2  apply a deterministic mix of writes, write-zeroes and
 *                    discards (<= 64K per call, the ubd request limit),
 *                    tracking the expected content; then a heavy sequential
 *                    write span that exercises the allocator and the
 *                    refcount-block creation path
 *           phase 3  dump the expected model to <out>
 *   verify  reopen from scratch; whole-image read plus random point reads
 *           against <out>
 *
 * The shell script then runs qemu-img check (accounting truth) and
 * converts the image back to raw for a byte comparison (content truth).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>

#include "qcow2.h"

#define MAX_IO (64 * 1024)

static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;

static uint32_t rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return (uint32_t)(rng_state >> 32);
}

/*
 * Discard semantics as implemented: ranges fully covering a cluster drop
 * its mapping (reads become zeros); partial clusters are left alone.
 */
static void model_discard(uint8_t *buf, uint64_t off, uint64_t len,
			  uint64_t cs)
{
	uint64_t end = off + len, c;

	for (c = off / cs; c <= (end - 1) / cs; c++) {
		uint64_t cstart = c * cs, cend = cstart + cs;
		uint64_t s = cstart > off ? cstart : off;
		uint64_t e = cend < end ? cend : end;

		if (s == cstart && e == cend)
			memset(buf + cstart, 0, cs);
	}
}

static void fail(const char *msg, int rc)
{
	fprintf(stderr, "FAIL: %s (rc=%d)\n", msg, rc);
	exit(1);
}

int main(int argc, char **argv)
{
	const char *img, *raw, *out, *mode;
	struct qcow2_geom g;
	struct qcow2_state *st;
	const char *err;
	void *arena;
	uint8_t *expected, *got, *data;
	uint64_t size, i;
	int fd, rc;

	if (argc != 5) {
		fprintf(stderr,
			"usage: %s <img.qcow2> <raw|-> <expected.raw> run|verify\n",
			argv[0]);
		return 2;
	}
	img = argv[1]; raw = argv[2]; out = argv[3]; mode = argv[4];

	fd = open(img, O_RDWR);
	if (fd < 0) fail("open image", -errno);
	rc = qcow2_identify(fd);
	if (rc != 1) fail("identify (not a qcow2 image?)", rc);
	rc = qcow2_read_geom(fd, &g, &err);
	if (rc) {
		fprintf(stderr, "read_geom: %s (rc=%d)\n", err, rc);
		return 1;
	}
	fprintf(stderr,
		"[%s] geom: cluster=%llu virt=%llu l1=%u l2ent=%u rbent=%u%s\n",
		mode, (unsigned long long)g.cluster_size,
		(unsigned long long)g.virt_size, g.l1_size, g.l2_entries,
		g.rb_entries, g.read_only ? " (read-only)" : "");

	size = g.virt_size;
	expected = malloc(size);
	got = malloc(size);
	data = malloc(MAX_IO);
	if (!expected || !got || !data)
		fail("malloc", -ENOMEM);

	arena = malloc(qcow2_state_size(&g));
	if (!arena)
		fail("arena malloc", -ENOMEM);
	rc = qcow2_init(fd, &g, arena, qcow2_state_size(&g), &st);
	if (rc)
		fail("qcow2_init", rc);

	{
		FILE *f = fopen(strcmp(mode, "run") ? out : raw, "rb");

		if (!f)
			fail("open model source", -errno);
		if (fread(expected, 1, size, f) != size)
			fail("short model source", -EIO);
		fclose(f);
	}

	/* whole-image read: phase 1 in run mode, the core check in verify */
	rc = qcow2_read(st, fd, 0, (unsigned long)size, got);
	if (rc)
		fail("read whole image", rc);
	if (memcmp(got, expected, size)) {
		for (i = 0; i < size && got[i] == expected[i]; i++)
			;
		fprintf(stderr, "differ at %llu: got %02x want %02x\n",
			(unsigned long long)i, got[i], expected[i]);
		fail("whole-image read vs model", -EIO);
	}
	fprintf(stderr, "[%s] whole-image read OK (%llu bytes)\n", mode,
		(unsigned long long)size);

	if (!strcmp(mode, "run")) {
		uint64_t off, span;

		/* mixed op stream: 70% writes / 15% zeroes / 15% discards */
		for (i = 0; i < 600; i++) {
			uint32_t op = rnd() % 100;
			unsigned int len;

			off = rnd() % size;
			len = 1 + rnd() % MAX_IO;
			if (off + len > size)
				len = (unsigned int)(size - off);

			if (op < 70) {
				unsigned int j;

				for (j = 0; j < len; j++)
					data[j] = (uint8_t)(rnd() >> 24);
				rc = qcow2_write(st, fd, off, len, data);
				if (rc)
					fail("write", rc);
				memcpy(expected + off, data, len);
			} else if (op < 85) {
				rc = qcow2_write_zeroes(st, fd, off, len);
				if (rc)
					fail("write_zeroes", rc);
				memset(expected + off, 0, len);
			} else {
				rc = qcow2_discard(st, fd, off, len);
				if (rc)
					fail("discard", rc);
				model_discard(expected, off, len, g.cluster_size);
			}
		}
		fprintf(stderr, "[run] mixed ops (600) applied\n");

		/* heavy sequential span at the end: allocator, new L2 tables,
		 * and (with 4K clusters) new refcount blocks */
		span = size / 4;
		if (span > 12 * 1024 * 1024)
			span = 12 * 1024 * 1024;
		for (off = size - span; off < size; off += MAX_IO) {
			unsigned int j, n;

			n = (unsigned int)((size - off < MAX_IO) ?
					   (size - off) : MAX_IO);
			for (j = 0; j < n; j++)
				data[j] = (uint8_t)((off + j) * 31 + 7);
			rc = qcow2_write(st, fd, off, n, data);
			if (rc)
				fail("heavy write", rc);
			memcpy(expected + off, data, n);
		}
		fprintf(stderr, "[run] heavy span (%llu bytes) applied\n",
			(unsigned long long)span);

		{
			FILE *f = fopen(out, "wb");

			if (!f)
				fail("open model out", -errno);
			if (fwrite(expected, 1, size, f) != size)
				fail("write model out", -EIO);
			fclose(f);
		}
		fprintf(stderr, "[run] model dumped to %s\n", out);
	}

	/* random point reads (both modes) */
	for (i = 0; i < 400; i++) {
		uint64_t off = rnd() % size;
		unsigned int len = 1 + rnd() % MAX_IO;

		if (off + len > size)
			len = (unsigned int)(size - off);
		rc = qcow2_read(st, fd, off, len, got);
		if (rc)
			fail("point read", rc);
		if (memcmp(got, expected + off, len)) {
			fprintf(stderr, "point read differs at %llu+%u\n",
				(unsigned long long)off, len);
			fail("point read vs model", -EIO);
		}
	}
	fprintf(stderr, "[%s] point reads OK\n", mode);
	fprintf(stderr, "[%s] PASS\n", mode);
	return 0;
}