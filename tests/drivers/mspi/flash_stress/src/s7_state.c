/*
 * Copyright (c) 2026 Filip Stojanovic <filipembedded@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * S7: state and reset. Device PM suspend/resume, data kept between runs
 * (checks retention over power cycles and resets), and an optional warm
 * reboot loop that catches a chip left in octal or 4-byte mode by the
 * previous boot.
 */

#include <string.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>

#include "stress.h"

#define PERSIST_MAGIC 0x5350534dU /* "MSPS" */
#define REBOOT_MAGIC  0x52424f54U /* "RBOT" */

struct persist_hdr {
	uint32_t magic;
	uint32_t run;
	uint32_t seed;
	uint32_t crc;
};

struct reboot_state {
	uint32_t magic;
	uint32_t remaining;
	uint32_t total;
	uint32_t crc;
};

/*
 * The reboot loop state lives in flash, not in RAM: RAM retention across a
 * reset is not guaranteed (Renode clears it), and a lost counter would loop
 * forever.
 */
static struct reboot_state rb;
static int rb_known = -1;

static uint32_t rb_off(void)
{
	return stress_blk(BLK_PERSIST) + 2U * STRESS_SECTOR;
}

static uint32_t rb_crc(const struct reboot_state *r)
{
	return crc32_ieee((const uint8_t *)r, offsetof(struct reboot_state, crc));
}

static bool reboot_cycle_active(void)
{
	if (rb_known < 0) {
		rb_known = 0;
		if (stress_ensure_init() == 0 && stress_read(rb_off(), &rb, sizeof(rb)) == 0 &&
		    rb.magic == REBOOT_MAGIC && rb.crc == rb_crc(&rb) &&
		    rb.remaining <= rb.total) {
			rb_known = 1;
		}
	}

	return rb_known == 1;
}

bool stress_normal_boot(const void *global_state)
{
	ARG_UNUSED(global_state);

	return !reboot_cycle_active();
}

static bool always(const void *global_state)
{
	ARG_UNUSED(global_state);

	return true;
}

ZTEST(s7_state, test_7_1_pm_suspend_resume)
{
#if defined(CONFIG_PM_DEVICE)
	const uint32_t off = stress_blk(BLK_PERSIST) + STRESS_SECTOR;
	static uint8_t ref[256];
	struct stress_err e = {0};
	int rc;

	rc = stress_read(off, ref, sizeof(ref));
	zassert_ok(rc);

	for (int i = 0; i < 5; i++) {
		rc = pm_device_action_run(stress.dev, PM_DEVICE_ACTION_SUSPEND);
		if (rc == -ENOTSUP || rc == -ENOSYS) {
			ztest_test_skip();
		}
		zassert_ok(rc, "suspend failed: %d", rc);
		rc = pm_device_action_run(stress.dev, PM_DEVICE_ACTION_RESUME);
		zassert_ok(rc, "resume failed: %d", rc);

		(void)stress_verify(off, ref, sizeof(ref), off, &e);
	}

	stress_report("S7 data after 5 suspend/resume cycles", &e);
	zassert_equal(e.bad_bytes + e.ops_failed, 0, "data changed across suspend/resume");
#else
	printk("  CONFIG_PM_DEVICE is off, nothing to test\n");
	ztest_test_skip();
#endif
}

/*
 * Verify the block written by the previous run, then write a new one. Run the
 * test, power cycle or reset the board, run it again: the second run checks
 * that the data survived.
 */
ZTEST(s7_state, test_7_2_persistence_between_runs)
{
	const uint32_t hdr_off = stress_blk(BLK_PERSIST);
	const uint32_t data_off = hdr_off + STRESS_SECTOR;
	struct persist_hdr hdr;
	struct stress_err e = {0};
	uint32_t run = 0;
	int rc;

	rc = stress_read(hdr_off, &hdr, sizeof(hdr));
	zassert_ok(rc);

	if (hdr.magic == PERSIST_MAGIC &&
	    hdr.crc == crc32_ieee((const uint8_t *)&hdr, offsetof(struct persist_hdr, crc))) {
		stress_fill_prbs(stress_exp, STRESS_SECTOR, hdr.seed);
		(void)stress_verify(data_off, stress_exp, STRESS_SECTOR, data_off, &e);
		printk("  block from previous run #%u: %s\n", hdr.run,
		       (e.bad_bytes == 0U) ? "intact" : "CORRUPTED");
		run = hdr.run;
	} else {
		printk("  no block from a previous run (first run on this region)\n");
	}

	/* Write the block for the next run. */
	hdr.magic = PERSIST_MAGIC;
	hdr.run = run + 1U;
	hdr.seed = stress_seed(0x7200U + hdr.run);
	hdr.crc = crc32_ieee((const uint8_t *)&hdr, offsetof(struct persist_hdr, crc));

	rc = stress_erase(hdr_off, 2U * STRESS_SECTOR);
	zassert_ok(rc, "erase failed: %d", rc);
	stress_fill_prbs(stress_exp, STRESS_SECTOR, hdr.seed);
	rc = stress_write(data_off, stress_exp, STRESS_SECTOR);
	zassert_ok(rc, "write failed: %d", rc);
	rc = stress_write(hdr_off, &hdr, sizeof(hdr));
	zassert_ok(rc, "write failed: %d", rc);
	(void)stress_verify(data_off, stress_exp, STRESS_SECTOR, data_off, &e);

	printk("  wrote block for run #%u\n", hdr.run);
	stress_report("S7 persistence", &e);
	zassert_equal(e.bad_bytes + e.ops_failed, 0, "persistent data corrupted");
}

/*
 * Warm reboot loop (CONFIG_MSPI_STRESS_REBOOT_CYCLES > 0). A warm reset only
 * resets the MCU: the flash keeps whatever mode the driver left it in, and
 * the next init must still find it. Every boot re-runs S7 only.
 */
ZTEST(s7_state, test_7_3_warm_reboot_loop)
{
#if defined(CONFIG_MSPI_STRESS_REBOOT_LOOP) && CONFIG_MSPI_STRESS_REBOOT_CYCLES > 0
	int rc;

	if (!reboot_cycle_active()) {
		rb.magic = REBOOT_MAGIC;
		rb.total = CONFIG_MSPI_STRESS_REBOOT_CYCLES;
		rb.remaining = CONFIG_MSPI_STRESS_REBOOT_CYCLES;
	} else {
		printk("  warm reboot %u of %u: device ready, data checks passed\n",
		       rb.total - rb.remaining, rb.total);
	}

	rc = stress_erase(rb_off(), STRESS_SECTOR);
	zassert_ok(rc, "erase of the reboot state failed: %d", rc);

	if (rb.remaining == 0U) {
		printk("  %u warm reboots completed\n", rb.total);
		return;
	}

	rb.remaining--;
	rb.crc = rb_crc(&rb);
	rc = stress_write(rb_off(), &rb, sizeof(rb));
	zassert_ok(rc, "write of the reboot state failed: %d", rc);

	printk("  warm reboot now (%u left)\n", rb.remaining);
	k_sleep(K_MSEC(100));
	sys_reboot(SYS_REBOOT_WARM);
#else
	printk("  enable CONFIG_MSPI_STRESS_REBOOT_LOOP to run\n");
	ztest_test_skip();
#endif
}

/* JEDEC ID check that also runs in reboot boots, where S1 is skipped. */
ZTEST(s7_state, test_7_0_device_ready)
{
	zassert_true(device_is_ready(stress.dev), "device not ready after this boot");
}

ZTEST_SUITE(s7_state, always, NULL, stress_before, NULL, NULL);
