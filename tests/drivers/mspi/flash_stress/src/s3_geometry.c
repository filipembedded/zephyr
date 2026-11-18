/*
 * Copyright (c) 2026 Filip Stojanovic <filipembedded@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * S3: geometry and sizes. Write and read lengths, buffer alignment, page and
 * erase boundaries, the 16 MiB (3/4-byte address) boundary and the end of the
 * device.
 */

#include <string.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "stress.h"

#define WL_MAX     300U
#define CANARY     0xa5U
#define READ_FILL  STRESS_BLOCK

/*
 * Writes of 1..300 bytes, back to back with a one byte gap, starting at an
 * odd offset. Data must match and every gap byte must still be erased.
 */
ZTEST(s3_geometry, test_3_1_write_lengths)
{
	const uint32_t base = stress_blk(BLK_WRITE_LEN);
	struct stress_err e = {0};
	struct stress_err gaps = {0};
	uint32_t p = base + 3U;
	int rc;

	rc = stress_erase(base, 2U * STRESS_BLOCK);
	zassert_ok(rc, "erase failed: %d", rc);

	for (uint32_t len = 1; len <= WL_MAX; len++) {
		stress_fill_prbs(stress_exp, len, stress_seed(0x3100U + len));
		rc = stress_write(p, stress_exp, len);
		if (rc != 0) {
			printk("  write len %u at 0x%08x failed: %d\n", len, p, rc);
			e.ops_failed++;
		}
		p += len + 1U;
	}

	p = base + 3U;
	for (uint32_t len = 1; len <= WL_MAX; len++) {
		stress_fill_prbs(stress_exp, len, stress_seed(0x3100U + len));
		if (!stress_verify(p, stress_exp, len, p, &e) && e.bad_bytes < 64U) {
			printk("  write len %u at 0x%08x reads back wrong\n", len, p);
		}
		(void)stress_verify_erased(p + len, 1, &gaps);
		p += len + 1U;
	}

	stress_report("S3 write lengths 1..300", &e);
	stress_report("S3 gap bytes between writes", &gaps);
	zassert_equal(e.bad_bytes + e.ops_failed, 0, "write length errors");
	zassert_equal(gaps.bad_bytes, 0, "a write touched bytes outside its range");
}

static void fill_read_block(uint32_t base, uint32_t tag)
{
	int rc = stress_erase(base, READ_FILL);

	zassert_ok(rc, "erase failed: %d", rc);

	for (uint32_t o = 0; o < READ_FILL; o += STRESS_SECTOR) {
		stress_fill_prbs(stress_exp, STRESS_SECTOR, stress_seed(tag + o / STRESS_SECTOR));
		rc = stress_write(base + o, stress_exp, STRESS_SECTOR);
		zassert_ok(rc, "fill write failed: %d", rc);
	}
}

/* Expected content of the read block at [o, o + len). */
static void expected_read_block(uint8_t *dst, uint32_t o, size_t len, uint32_t tag)
{
	while (len > 0U) {
		uint32_t s = o / STRESS_SECTOR;
		uint32_t in = o % STRESS_SECTOR;
		size_t n = MIN(len, STRESS_SECTOR - in);

		stress_fill_prbs(stress_got, STRESS_SECTOR, stress_seed(tag + s));
		memcpy(dst, stress_got + in, n);
		dst += n;
		o += n;
		len -= n;
	}
}

static bool read_and_check(uint32_t base, uint32_t o, size_t len, size_t buf_o,
			   struct stress_err *e, uint32_t *guard_errors)
{
	uint8_t *buf = stress_big + STRESS_ALIGN;
	int rc;

	memset(buf - STRESS_ALIGN, CANARY, len + 2U * STRESS_ALIGN);
	rc = stress_read(base + o, buf + buf_o, len);
	if (rc != 0) {
		e->ops_failed++;
		return false;
	}

	if (buf[buf_o - 1] != CANARY || buf[buf_o + len] != CANARY) {
		(*guard_errors)++;
		printk("  read len %u ad_o %u buf_o %u wrote outside the buffer\n",
		       (uint32_t)len, o, (uint32_t)buf_o);
	}

	for (size_t done = 0; done < len; done += STRESS_SECTOR) {
		size_t n = MIN(STRESS_SECTOR, len - done);

		expected_read_block(stress_exp, o + done, n, 0x3200U);
		stress_compare(stress_exp, buf + buf_o + done, n, base + o + done, base + o, e);
	}
	return true;
}

/*
 * Reads of many lengths, at unaligned flash offsets and into unaligned
 * buffers, with guard bytes around the buffer. Covers FIFO and DMA edges,
 * cache line splits and packet_data_limit.
 */
ZTEST(s3_geometry, test_3_2_read_lengths)
{
	static const uint32_t special[] = {
		31, 32, 33, 63, 64, 65, 127, 128, 129, 255, 256, 257,
		511, 512, 513, 1023, 1024, 1025, 4095, 4096, 4097,
	};
	const uint32_t base = stress_blk(BLK_READ_LEN);
	struct stress_err e = {0};
	uint32_t guard_errors = 0;

	fill_read_block(base, 0x3200U);

	for (size_t len = 0; len <= 64U; len++) {
		for (uint32_t ad_o = 0; ad_o < 8U; ad_o++) {
			for (size_t buf_o = 1; buf_o <= 8U; buf_o++) {
				(void)read_and_check(base, ad_o, len, buf_o, &e, &guard_errors);
			}
		}
	}

	for (size_t i = 0; i < ARRAY_SIZE(special); i++) {
		for (uint32_t ad_o = 0; ad_o < 2U; ad_o++) {
			(void)read_and_check(base, 0x100U + ad_o, special[i], 1U + ad_o, &e,
					     &guard_errors);
		}
	}

	/* One large read, aligned and unaligned. */
	for (uint32_t ad_o = 0; ad_o < 2U; ad_o++) {
		size_t len = MIN(CONFIG_MSPI_STRESS_BIG_READ_SIZE, READ_FILL - ad_o);
		uint8_t *buf = stress_big + STRESS_ALIGN;
		int rc;

		rc = stress_read(base + ad_o, buf + ad_o, len);
		if (rc != 0) {
			e.ops_failed++;
			continue;
		}
		for (size_t o = 0; o < len; o += STRESS_SECTOR) {
			size_t n = MIN(STRESS_SECTOR, len - o);

			expected_read_block(stress_exp, ad_o + o, n, 0x3200U);
			stress_compare(stress_exp, buf + ad_o + o, n, base + ad_o + o,
				       base + ad_o + o, &e);
		}
	}

	stress_report("S3 read lengths and alignment", &e);
	printk("  guard violations: %u\n", guard_errors);
	zassert_equal(e.bad_bytes + e.ops_failed, 0, "read errors");
	zassert_equal(guard_errors, 0, "reads wrote outside the destination buffer");
}

/*
 * Program three neighbouring blocks, erase part of the middle one with each
 * erase size, and check that exactly that range was erased.
 */
ZTEST(s3_geometry, test_3_3_erase_guard)
{
	static const struct {
		uint32_t size;
		uint32_t at; /* offset inside the middle block */
	} cases[] = {
		{STRESS_SECTOR, 0x5000},
		{0x8000, 0x8000},
		{STRESS_BLOCK, 0},
	};
	const uint32_t base = stress_blk(BLK_ERASE_GUARD);
	struct stress_err e = {0};
	int rc;

	for (size_t c = 0; c < ARRAY_SIZE(cases); c++) {
		const uint32_t er_start = base + STRESS_BLOCK + cases[c].at;
		const uint32_t er_end = er_start + cases[c].size;

		rc = stress_erase(base, 3U * STRESS_BLOCK);
		zassert_ok(rc, "erase failed: %d", rc);

		for (uint32_t o = 0; o < 3U * STRESS_BLOCK; o += STRESS_SECTOR) {
			stress_fill_prbs(stress_exp, STRESS_SECTOR,
					 stress_seed(0x3300U + c * 64U + o / STRESS_SECTOR));
			rc = stress_write(base + o, stress_exp, STRESS_SECTOR);
			zassert_ok(rc, "fill write failed: %d", rc);
		}

		rc = stress_erase(er_start, cases[c].size);
		if (rc != 0) {
			printk("  erase of %u KiB at 0x%08x returned %d, size not supported\n",
			       cases[c].size / 1024U, er_start, rc);
			zassert_true(cases[c].size != STRESS_SECTOR, "4 KiB erase must work");
			continue;
		}

		struct stress_err ce = {0};

		for (uint32_t o = 0; o < 3U * STRESS_BLOCK; o += STRESS_SECTOR) {
			uint32_t s = base + o;

			if (s >= er_start && s < er_end) {
				(void)stress_verify_erased(s, STRESS_SECTOR, &ce);
			} else {
				stress_fill_prbs(stress_exp, STRESS_SECTOR,
						 stress_seed(0x3300U + c * 64U + o / STRESS_SECTOR));
				(void)stress_verify(s, stress_exp, STRESS_SECTOR, s, &ce);
			}
		}

		if (ce.bad_bytes != 0U) {
			printk("  erase %u KiB at 0x%08x: %u bad bytes, first at 0x%08x "
			       "(exp 0x%02x got 0x%02x) %s\n", cases[c].size / 1024U, er_start,
			       ce.bad_bytes, ce.first_off, ce.first_exp, ce.first_got,
			       (ce.first_off >= er_start && ce.first_off < er_end)
				       ? "inside the range (not erased)"
				       : "outside the range (neighbour erased)");
		} else {
			printk("  erase %u KiB at 0x%08x: exact\n", cases[c].size / 1024U,
			       er_start);
		}
		stress_err_merge(&e, &ce);
	}

	zassert_equal(e.bad_bytes + e.ops_failed, 0, "erase did not hit exactly its range");
}

/* Read-only: the far ends of the address space must be readable. */
ZTEST(s3_geometry, test_3_4_far_reads)
{
	uint32_t offs[3] = {0, stress.dev_size - STRESS_SECTOR, 0};
	int n = 2;
	int rc;

	if (stress.dev_size > 0x1000000U) {
		offs[n++] = 0x1000000U - 0x80U;
	}

	for (int i = 0; i < n; i++) {
		rc = stress_read(offs[i], stress_got, STRESS_SECTOR);
		zassert_ok(rc, "read at 0x%08x failed: %d", offs[i], rc);
	}

	printk("  reads at 0x0, end of device%s OK\n",
	       (n == 3) ? " and across 16 MiB" : "");
}

/*
 * Opt-in, erases outside the test region: write across the 16 MiB boundary
 * and in the last sector, and check offset 0 did not change (address wrap).
 */
ZTEST(s3_geometry, test_3_5_boundaries_outside_region)
{
#if defined(CONFIG_MSPI_STRESS_OUTSIDE_REGION)
	struct stress_err e = {0};
	uint32_t targets[2] = {stress.dev_size - STRESS_SECTOR, 0};
	int n = 1;
	int rc;

	rc = stress_read(0, stress_big, STRESS_SECTOR);
	zassert_ok(rc);

	if (stress.dev_size > 0x1000000U) {
		targets[n++] = 0x1000000U - STRESS_SECTOR;
	}

	for (int i = 0; i < n; i++) {
		uint32_t t = targets[i];
		uint32_t len = (i == 0) ? STRESS_SECTOR : 2U * STRESS_SECTOR;
		uint32_t w = (i == 0) ? t : 0x1000000U - 0x80U;
		uint32_t wl = (i == 0) ? STRESS_SECTOR : 0x100U;

		rc = stress_erase(t, len);
		zassert_ok(rc, "erase 0x%08x failed: %d", t, rc);
		stress_fill_prbs(stress_exp, wl, stress_seed(0x3500U + i));
		rc = stress_write(w, stress_exp, wl);
		zassert_ok(rc, "write 0x%08x failed: %d", w, rc);
		(void)stress_verify(w, stress_exp, wl, w, &e);
	}

	rc = stress_read(0, stress_got, STRESS_SECTOR);
	zassert_ok(rc);
	zassert_mem_equal(stress_big, stress_got, STRESS_SECTOR,
			  "offset 0 changed: high addresses wrap around (address width or "
			  "flash size setting)");

	stress_report("S3 16 MiB boundary and last sector", &e);
	zassert_equal(e.bad_bytes, 0, "boundary errors");
#else
	printk("  enable CONFIG_MSPI_STRESS_OUTSIDE_REGION to run (erases outside the region)\n");
	ztest_test_skip();
#endif
}

ZTEST_SUITE(s3_geometry, stress_normal_boot, NULL, stress_before, NULL, NULL);
