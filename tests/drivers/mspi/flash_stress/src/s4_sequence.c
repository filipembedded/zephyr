/*
 * Copyright (c) 2026 Filip Stojanovic <filipembedded@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * S4: command sequences. Many short commands back to back: write then read
 * right away, erase then read, tiny reads, and reads mixed with ID and SFDP
 * commands. Stresses chip select high time, busy polling and mode changes.
 */

#include <string.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <jesd216.h>

#include "stress.h"

#define PAIRS      512U
#define PAIR_LEN   16U
#define TINY_READS 2000U

BUILD_ASSERT(CONFIG_MSPI_STRESS_BIG_READ_SIZE >= PAIRS * PAIR_LEN,
	     "MSPI_STRESS_BIG_READ_SIZE must hold the S4 expected image");

ZTEST(s4_sequence, test_4_1_write_read_pairs)
{
	const uint32_t base = stress_blk(BLK_SEQUENCE);
	struct stress_err e = {0};
	int rc;

	rc = stress_erase(base, PAIRS * PAIR_LEN);
	zassert_ok(rc, "erase failed: %d", rc);

	/* Keep the whole expected image for test_4_3. */
	stress_fill_prbs(stress_big, PAIRS * PAIR_LEN, stress_seed(0x4100U));

	for (uint32_t i = 0; i < PAIRS; i++) {
		uint32_t off = base + i * PAIR_LEN;

		rc = stress_write(off, stress_big + i * PAIR_LEN, PAIR_LEN);
		if (rc != 0) {
			e.ops_failed++;
			continue;
		}
		(void)stress_verify(off, stress_big + i * PAIR_LEN, PAIR_LEN, off, &e);
	}

	stress_report("S4 write+read pairs", &e);
	zassert_equal(e.bad_bytes + e.ops_failed, 0, "write/read pair errors");
}

ZTEST(s4_sequence, test_4_2_erase_then_read)
{
	const uint32_t base = stress_blk(BLK_SEQUENCE) + 0x8000U;
	struct stress_err e = {0};
	int rc;

	for (uint32_t k = 0; k < 8U; k++) {
		uint32_t s = base + k * STRESS_SECTOR;
		uint8_t v = (uint8_t)(0x11U * (k + 1U));

		rc = stress_erase(s, STRESS_SECTOR);
		if (rc != 0) {
			e.ops_failed++;
			continue;
		}
		/* Read immediately: a premature busy release shows up here. */
		(void)stress_verify_erased(s, STRESS_SECTOR, &e);

		rc = stress_write(s + k, &v, 1);
		if (rc != 0) {
			e.ops_failed++;
			continue;
		}
		(void)stress_verify(s + k, &v, 1, s + k, &e);
	}

	stress_report("S4 erase then read", &e);
	zassert_equal(e.bad_bytes + e.ops_failed, 0, "erase/read sequence errors");
}

ZTEST(s4_sequence, test_4_3_tiny_reads)
{
	const uint32_t base = stress_blk(BLK_SEQUENCE);
	struct stress_err e = {0};
	uint32_t rnd = stress_seed(0x4300U);

	/* test_4_1 left PAIRS * PAIR_LEN bytes of prbs at base. */
	stress_fill_prbs(stress_big, PAIRS * PAIR_LEN, stress_seed(0x4100U));

	for (uint32_t i = 0; i < TINY_READS; i++) {
		uint32_t o;
		uint32_t len;

		rnd ^= rnd << 13;
		rnd ^= rnd >> 17;
		rnd ^= rnd << 5;
		len = 1U + (rnd % 3U);
		o = (rnd >> 8) % (PAIRS * PAIR_LEN - len);

		if (stress_read(base + o, stress_got, len) != 0) {
			e.ops_failed++;
			continue;
		}
		stress_compare(stress_big + o, stress_got, len, base + o, base + o, &e);
	}

	stress_report("S4 tiny random reads", &e);
	zassert_equal(e.bad_bytes + e.ops_failed, 0, "tiny read errors");
}

ZTEST(s4_sequence, test_4_4_mixed_commands)
{
#if defined(CONFIG_FLASH_JESD216_API)
	const uint32_t base = stress_blk(BLK_SEQUENCE);
	struct stress_err e = {0};
	uint8_t id0[8] = {0};
	uint8_t id[8];
	uint32_t magic;
	uint32_t id_errors = 0;
	uint32_t sfdp_errors = 0;
	bool have_id;

	bool have_sfdp;

	have_id = (flash_read_jedec_id(stress.dev, id0) == 0);
	have_sfdp = (flash_sfdp_read(stress.dev, 0, &magic, sizeof(magic)) == 0) &&
		    (sys_le32_to_cpu(magic) == JESD216_SFDP_MAGIC);
	if (!have_sfdp) {
		printk("  SFDP not readable in this mode (see S1), mixing in JEDEC ID only\n");
	}
	stress_fill_prbs(stress_big, PAIRS * PAIR_LEN, stress_seed(0x4100U));

	for (uint32_t i = 0; i < 200U; i++) {
		uint32_t o = (i * 37U) % (PAIRS * PAIR_LEN - 64U);

		if (have_id) {
			if (flash_read_jedec_id(stress.dev, id) != 0 || memcmp(id, id0, 3) != 0) {
				id_errors++;
			}
		}
		if (have_sfdp &&
		    (flash_sfdp_read(stress.dev, 0, &magic, sizeof(magic)) != 0 ||
		     sys_le32_to_cpu(magic) != JESD216_SFDP_MAGIC)) {
			sfdp_errors++;
		}
		if (stress_read(base + o, stress_got, 64) != 0) {
			e.ops_failed++;
			continue;
		}
		stress_compare(stress_big + o, stress_got, 64, base + o, base + o, &e);
	}

	stress_report("S4 data reads mixed with ID/SFDP", &e);
	printk("  JEDEC ID errors: %u, SFDP signature errors: %u\n", id_errors, sfdp_errors);
	zassert_equal(e.bad_bytes + e.ops_failed, 0, "data errors between ID/SFDP reads");
	zassert_equal(id_errors + sfdp_errors, 0, "ID/SFDP errors between data reads");
#else
	ztest_test_skip();
#endif
}

ZTEST_SUITE(s4_sequence, stress_normal_boot, NULL, stress_before, NULL, NULL);
