/*
 * Copyright (c) 2026 Filip Stojanovic <filipembedded@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * S6: read-after-write coherency. Read an area first (so caches, DMA buffers
 * and any memory-mapped window hold it), then change it and read again. Stale
 * data shows up as erased bytes where new data was written.
 */

#include <string.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "stress.h"

ZTEST(s6_coherency, test_6_1_read_write_read)
{
	static const struct {
		uint32_t at;
		uint32_t len;
	} cases[] = {
		/* Disjoint ranges: NOR programming can only clear bits. */
		{31, 1}, {32, 31}, {63, 33}, {100, 64}, {192, 32}, {1000, 512}, {3990, 96},
	};
	const uint32_t base = stress_blk(BLK_COHERENCY);
	struct stress_err e = {0};
	int rc;

	for (uint32_t sec = 0; sec < 2U; sec++) {
		uint32_t s = base + sec * STRESS_SECTOR;

		rc = stress_erase(s, STRESS_SECTOR);
		zassert_ok(rc, "erase failed: %d", rc);

		/* Model of what the sector should contain. */
		memset(stress_exp, stress.erase_value, STRESS_SECTOR);

		for (size_t c = 0; c < ARRAY_SIZE(cases); c++) {
			uint8_t data[512];
			uint32_t at = cases[c].at + sec;
			uint32_t len = MIN(cases[c].len, STRESS_SECTOR - at);

			/* 1: read the whole sector, priming every cache on the way. */
			(void)stress_verify(s, stress_exp, STRESS_SECTOR, s, &e);

			/* 2: change part of it. */
			stress_fill_prbs(data, len, stress_seed(0x6100U + sec * 16U + c));
			rc = stress_write(s + at, data, len);
			if (rc != 0) {
				e.ops_failed++;
				continue;
			}
			memcpy(stress_exp + at, data, len);

			/* 3: the next read must see the new bytes, not the cached erased ones. */
			(void)stress_verify(s, stress_exp, STRESS_SECTOR, s + at, &e);
		}
	}

	stress_report("S6 read-write-read", &e);
	zassert_equal(e.bad_bytes + e.ops_failed, 0, "stale or wrong data after a write");
}

/* Same destination buffer, different flash data each time. */
ZTEST(s6_coherency, test_6_2_buffer_reuse)
{
	const uint32_t base = stress_blk(BLK_COHERENCY) + 2U * STRESS_SECTOR;
	struct stress_err e = {0};
	int rc;

	rc = stress_erase(base, 2U * STRESS_SECTOR);
	zassert_ok(rc, "erase failed: %d", rc);

	for (uint32_t k = 0; k < 2U; k++) {
		stress_fill_prbs(stress_exp, STRESS_SECTOR, stress_seed(0x6200U + k));
		rc = stress_write(base + k * STRESS_SECTOR, stress_exp, STRESS_SECTOR);
		zassert_ok(rc, "write failed: %d", rc);
	}

	for (uint32_t i = 0; i < 16U; i++) {
		uint32_t k = i & 1U;

		rc = stress_read(base + k * STRESS_SECTOR, stress_got, STRESS_SECTOR);
		if (rc != 0) {
			e.ops_failed++;
			continue;
		}
		stress_fill_prbs(stress_exp, STRESS_SECTOR, stress_seed(0x6200U + k));
		stress_compare(stress_exp, stress_got, STRESS_SECTOR, base + k * STRESS_SECTOR,
			       base + k * STRESS_SECTOR, &e);
	}

	stress_report("S6 buffer reuse", &e);
	zassert_equal(e.bad_bytes + e.ops_failed, 0, "stale data in a reused buffer");
}

ZTEST_SUITE(s6_coherency, stress_normal_boot, NULL, stress_before, NULL, NULL);
