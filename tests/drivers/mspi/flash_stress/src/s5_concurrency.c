/*
 * Copyright (c) 2026 Filip Stojanovic <filipembedded@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * S5: concurrency. A writer, a reader and an ID thread use the same flash at
 * the same time, on disjoint areas. Exercises the controller session lock
 * (mspi_dev_config / mspi_get_channel_status) and the driver acquire/release.
 */

#include <string.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "stress.h"

#define STACK_SIZE   2048
#define WRITER_PASSES 2U
#define READ_MAX     512U

K_THREAD_STACK_DEFINE(writer_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(reader_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(id_stack, STACK_SIZE);

static struct k_thread writer_thread;
static struct k_thread reader_thread;
static struct k_thread id_thread;

static struct stress_err writer_err;
static struct stress_err reader_err;
static uint32_t writer_ops;
static uint32_t reader_ops;
static uint32_t id_ops;
static uint32_t id_errors;
static int64_t deadline;

static uint8_t writer_buf[STRESS_SECTOR] __aligned(STRESS_ALIGN);
static uint8_t writer_rd[STRESS_SECTOR] __aligned(STRESS_ALIGN);
static uint8_t reader_buf[READ_MAX] __aligned(STRESS_ALIGN);
static uint8_t reader_exp[STRESS_SECTOR] __aligned(STRESS_ALIGN);

static void writer_fn(void *a, void *b, void *c)
{
	const uint32_t base = stress_blk(BLK_CONC_WRITE);

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	/* Bounded passes, so the wear stays small however long the test runs. */
	for (uint32_t pass = 0; pass < WRITER_PASSES; pass++) {
		for (uint32_t s = 0; s < STRESS_BLOCK / STRESS_SECTOR; s++) {
			uint32_t off = base + s * STRESS_SECTOR;

			if (k_uptime_get() > deadline) {
				return;
			}
			if (flash_erase(stress.dev, off, STRESS_SECTOR) != 0) {
				writer_err.ops_failed++;
				continue;
			}
			stress_wear.erases[(off - stress.region) / STRESS_SECTOR]++;
			stress_fill_prbs(writer_buf, STRESS_SECTOR,
					 stress_seed(0x5100U + pass * 16U + s));
			if (flash_write(stress.dev, off, writer_buf, STRESS_SECTOR) != 0 ||
			    flash_read(stress.dev, off, writer_rd, STRESS_SECTOR) != 0) {
				writer_err.ops_failed++;
				continue;
			}
			stress_compare(writer_buf, writer_rd, STRESS_SECTOR, off, off, &writer_err);
			writer_ops++;
		}
	}
}

static void reader_fn(void *a, void *b, void *c)
{
	const uint32_t base = stress_blk(BLK_CONC_READ);
	uint32_t rnd = stress_seed(0x5200U);

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (k_uptime_get() <= deadline) {
		uint32_t s;
		uint32_t in;
		uint32_t len;

		rnd ^= rnd << 13;
		rnd ^= rnd >> 17;
		rnd ^= rnd << 5;
		s = rnd % (STRESS_BLOCK / STRESS_SECTOR);
		len = 1U + ((rnd >> 8) % READ_MAX);
		in = (rnd >> 16) % (STRESS_SECTOR - len);

		if (flash_read(stress.dev, base + s * STRESS_SECTOR + in, reader_buf, len) != 0) {
			reader_err.ops_failed++;
			continue;
		}
		stress_fill_prbs(reader_exp, STRESS_SECTOR, stress_seed(0x5300U + s));
		stress_compare(reader_exp + in, reader_buf, len, base + s * STRESS_SECTOR + in,
			       base + s * STRESS_SECTOR + in, &reader_err);
		reader_ops++;
	}
}

static void id_fn(void *a, void *b, void *c)
{
	uint64_t size;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (k_uptime_get() <= deadline) {
#if defined(CONFIG_FLASH_JESD216_API)
		uint8_t id[8];
		static uint8_t id0[8];
		static bool have_id0;

		if (flash_read_jedec_id(stress.dev, id) == 0) {
			if (!have_id0) {
				memcpy(id0, id, sizeof(id0));
				have_id0 = true;
			} else if (memcmp(id, id0, 3) != 0) {
				id_errors++;
			}
		}
#endif
		if (flash_get_size(stress.dev, &size) != 0 || size != stress.dev_size) {
			id_errors++;
		}
		id_ops++;
		k_sleep(K_MSEC(1));
	}
}

ZTEST(s5_concurrency, test_5_1_three_threads)
{
	const uint32_t rbase = stress_blk(BLK_CONC_READ);
	int rc;

	/* Prefill the reader block before the threads start. */
	rc = stress_erase(rbase, STRESS_BLOCK);
	zassert_ok(rc, "erase failed: %d", rc);
	for (uint32_t s = 0; s < STRESS_BLOCK / STRESS_SECTOR; s++) {
		stress_fill_prbs(stress_exp, STRESS_SECTOR, stress_seed(0x5300U + s));
		rc = stress_write(rbase + s * STRESS_SECTOR, stress_exp, STRESS_SECTOR);
		zassert_ok(rc, "prefill failed: %d", rc);
	}

	memset(&writer_err, 0, sizeof(writer_err));
	memset(&reader_err, 0, sizeof(reader_err));
	deadline = k_uptime_get() + CONFIG_MSPI_STRESS_CONCURRENCY_SECONDS * 1000;

	k_thread_create(&writer_thread, writer_stack, STACK_SIZE, writer_fn, NULL, NULL, NULL,
			K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
	k_thread_create(&reader_thread, reader_stack, STACK_SIZE, reader_fn, NULL, NULL, NULL,
			K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
	k_thread_create(&id_thread, id_stack, STACK_SIZE, id_fn, NULL, NULL, NULL,
			K_PRIO_PREEMPT(5), 0, K_NO_WAIT);

	k_thread_join(&writer_thread, K_FOREVER);
	k_thread_join(&reader_thread, K_FOREVER);
	k_thread_join(&id_thread, K_FOREVER);

	printk("  %u s: writer %u sectors, reader %u reads, id thread %u calls\n",
	       CONFIG_MSPI_STRESS_CONCURRENCY_SECONDS, writer_ops, reader_ops, id_ops);
	stress_report("S5 writer thread", &writer_err);
	stress_report("S5 reader thread", &reader_err);
	printk("  ID/size errors: %u\n", id_errors);

	zassert_true(writer_ops > 0U && reader_ops > 0U, "a thread made no progress");
	zassert_equal(writer_err.bad_bytes + writer_err.ops_failed, 0, "writer errors");
	zassert_equal(reader_err.bad_bytes + reader_err.ops_failed, 0, "reader errors");
	zassert_equal(id_errors, 0, "ID thread errors");
}

ZTEST_SUITE(s5_concurrency, stress_normal_boot, NULL, stress_before, NULL, NULL);
