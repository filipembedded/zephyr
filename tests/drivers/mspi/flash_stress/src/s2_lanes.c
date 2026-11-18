/*
 * Copyright (c) 2026 Filip Stojanovic <filipembedded@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * S2: data integrity per IO lane. Every pattern gets its own freshly erased
 * 4 KiB sector, is written once and read back. Mismatches are collected into
 * a per-bit, per-lane and per-position map instead of stopping at the first
 * bad byte.
 */

#include <string.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "stress.h"

enum pat_kind {
	PAT_CONST,
	PAT_ALT,      /* two alternating bytes */
	PAT_LANE_ON,  /* only the bits of one lane set */
	PAT_LANE_OFF, /* all bits set except one lane */
	PAT_INC,
	PAT_DEC,
	PAT_PRBS,
};

struct pattern {
	char name[16];
	enum pat_kind kind;
	uint8_t a;
	uint8_t b;
};

#define MAX_PATTERNS 32

static struct pattern patterns[MAX_PATTERNS];
static int pattern_count;
static struct stress_err s2_total;
static uint32_t next_sector;

static uint8_t lane_mask(uint8_t lane, uint8_t lanes)
{
	uint8_t m = 0;

	for (int b = lane; b < 8; b += lanes) {
		m |= BIT(b);
	}

	return m;
}

static void add(const char *name, enum pat_kind kind, uint8_t a, uint8_t b)
{
	struct pattern *p = &patterns[pattern_count++];

	strncpy(p->name, name, sizeof(p->name) - 1U);
	p->kind = kind;
	p->a = a;
	p->b = b;
}

static void build_patterns(void)
{
	const uint8_t lanes = MAX(stress.read_lanes, stress.write_lanes);
	char name[16];

	if (pattern_count != 0) {
		return;
	}

	add("0x00", PAT_CONST, 0x00, 0);
	add("0xff", PAT_CONST, 0xff, 0);
	add("0x55", PAT_CONST, 0x55, 0);
	add("0xaa", PAT_CONST, 0xaa, 0);
	add("0x0f", PAT_CONST, 0x0f, 0);
	add("0xf0", PAT_CONST, 0xf0, 0);
	add("0x33", PAT_CONST, 0x33, 0);
	add("0xcc", PAT_CONST, 0xcc, 0);
	add("0x5a/0xa5", PAT_ALT, 0x5a, 0xa5);
	add("0x00/0xff", PAT_ALT, 0x00, 0xff);

	if (lanes > 1U) {
		for (uint8_t l = 0; l < lanes; l++) {
			snprintk(name, sizeof(name), "only IO%u", l);
			add(name, PAT_LANE_ON, lane_mask(l, lanes), 0);
			snprintk(name, sizeof(name), "all but IO%u", l);
			add(name, PAT_LANE_OFF, (uint8_t)~lane_mask(l, lanes), 0);
		}
	} else {
		for (uint8_t b = 0; b < 8; b++) {
			snprintk(name, sizeof(name), "walk1 bit%u", b);
			add(name, PAT_CONST, BIT(b), 0);
		}
	}

	add("increment", PAT_INC, 0, 0);
	add("decrement", PAT_DEC, 0, 0);
	add("prbs", PAT_PRBS, 0, 0);
}

static void generate(const struct pattern *p, uint8_t *buf, size_t len, uint32_t tag)
{
	switch (p->kind) {
	case PAT_CONST:
	case PAT_LANE_ON:
	case PAT_LANE_OFF:
		memset(buf, p->a, len);
		break;
	case PAT_ALT:
		for (size_t i = 0; i < len; i++) {
			buf[i] = (i & 1U) ? p->b : p->a;
		}
		break;
	case PAT_INC:
		for (size_t i = 0; i < len; i++) {
			buf[i] = (uint8_t)i;
		}
		break;
	case PAT_DEC:
		for (size_t i = 0; i < len; i++) {
			buf[i] = (uint8_t)(0xffU - i);
		}
		break;
	case PAT_PRBS:
		stress_fill_prbs(buf, len, stress_seed(tag));
		break;
	}
}

static uint32_t take_sector(void)
{
	uint32_t off = stress_blk(BLK_LANES) + next_sector * STRESS_SECTOR;

	zassert_true(next_sector < 4U * STRESS_BLOCK / STRESS_SECTOR,
		     "S2 ran out of sectors in its 4 blocks");
	next_sector++;

	return off;
}

/*
 * Erase one sector, check it is erased, write the pattern at start with len
 * bytes in a single flash_write() call, then verify.
 */
static void run_one(const struct pattern *p, uint32_t start_in_sector, size_t len,
		    const char *where, uint32_t tag)
{
	struct stress_err e = {0};
	uint32_t sector = take_sector();
	uint32_t off = sector + start_in_sector;
	int rc;

	rc = stress_erase(sector, STRESS_SECTOR);
	zassert_ok(rc, "erase 0x%08x failed: %d", sector, rc);

	if (!stress_verify_erased(sector, STRESS_SECTOR, &e)) {
		printk("  erase of 0x%08x left non-erased bytes\n", sector);
	}

	generate(p, stress_exp, len, tag);
	rc = stress_write(off, stress_exp, len);
	if (rc != 0) {
		printk("  write '%s' at 0x%08x failed: %d\n", p->name, off, rc);
		e.ops_failed++;
	} else {
		(void)stress_verify(off, stress_exp, len, off, &e);
	}

	if (e.bad_bytes != 0U || e.ops_failed != 0U) {
		printk("  FAIL '%s' %s at 0x%08x: %u bad bytes, first 0x%08x exp 0x%02x "
		       "got 0x%02x\n", p->name, where, off, e.bad_bytes, e.first_off,
		       e.first_exp, e.first_got);
	}

	stress_err_merge(&s2_total, &e);
}

ZTEST(s2_lanes, test_2_1_patterns_page_aligned)
{
	struct stress_err before = s2_total;

	build_patterns();
	printk("  %d patterns, %u data lanes\n", pattern_count,
	       MAX(stress.read_lanes, stress.write_lanes));

	for (int i = 0; i < pattern_count; i++) {
		run_one(&patterns[i], 0, STRESS_SECTOR, "aligned", 0x2100U + i);
	}

	zassert_equal(s2_total.bad_bytes, before.bad_bytes, "bad bytes, see the S2 report");
	zassert_equal(s2_total.ops_failed, before.ops_failed, "failed operations");
}

ZTEST(s2_lanes, test_2_2_patterns_unaligned)
{
	struct stress_err before = s2_total;

	build_patterns();

	/* Start mid-page so the first program command is short and later ones span pages. */
	for (int i = 0; i < pattern_count; i++) {
		run_one(&patterns[i], 0x83, STRESS_SECTOR - 0x100, "unaligned", 0x2200U + i);
	}

	zassert_equal(s2_total.bad_bytes, before.bad_bytes, "bad bytes, see the S2 report");
	zassert_equal(s2_total.ops_failed, before.ops_failed, "failed operations");
}

/*
 * Write in chunks of varying size, so program commands start at many
 * different offsets. This is how flash_copy() and file systems write.
 */
ZTEST(s2_lanes, test_2_3_chunked_writes)
{
	static const char *const names[] = {"increment", "prbs"};
	struct stress_err before = s2_total;

	build_patterns();

	for (int n = 0; n < ARRAY_SIZE(names); n++) {
		const struct pattern *p = NULL;
		struct stress_err e = {0};
		uint32_t sector = take_sector();
		uint32_t rnd = stress_seed(0x2300U + n);
		uint32_t done = 0;
		int rc;

		for (int i = 0; i < pattern_count; i++) {
			if (strcmp(patterns[i].name, names[n]) == 0) {
				p = &patterns[i];
			}
		}
		zassert_not_null(p);

		rc = stress_erase(sector, STRESS_SECTOR);
		zassert_ok(rc, "erase failed: %d", rc);

		generate(p, stress_exp, STRESS_SECTOR, 0x2300U + n);

		while (done < STRESS_SECTOR) {
			uint32_t chunk;

			rnd ^= rnd << 13;
			rnd ^= rnd >> 17;
			rnd ^= rnd << 5;
			chunk = MIN(1U + (rnd % 64U), STRESS_SECTOR - done);

			rc = stress_write(sector + done, stress_exp + done, chunk);
			if (rc != 0) {
				e.ops_failed++;
				break;
			}
			/* Verify each chunk against its own program command start. */
			(void)stress_verify(sector + done, stress_exp + done, chunk, sector + done,
					    &e);
			done += chunk;
		}

		/* Final full read: later chunks must not have disturbed earlier ones. */
		(void)stress_verify(sector, stress_exp, STRESS_SECTOR, sector, &e);

		if (e.bad_bytes != 0U || e.ops_failed != 0U) {
			printk("  FAIL chunked '%s': %u bad bytes\n", p->name, e.bad_bytes);
		}
		stress_err_merge(&s2_total, &e);
	}

	zassert_equal(s2_total.bad_bytes, before.bad_bytes, "bad bytes, see the S2 report");
	zassert_equal(s2_total.ops_failed, before.ops_failed, "failed operations");
}

ZTEST(s2_lanes, test_2_9_report)
{
	stress_report("S2 lanes, all patterns", &s2_total);
}

ZTEST_SUITE(s2_lanes, stress_normal_boot, NULL, stress_before, NULL, NULL);
