/*
 * Copyright (c) 2026 Filip Stojanovic <filipembedded@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * S9: wear and traffic summary. Runs last (suites run in name order).
 */

#include <zephyr/sys/printk.h>

#include "stress.h"

ZTEST(s9_report, test_9_1_wear_budget)
{
	uint32_t max = 0;
	uint32_t max_idx = 0;
	uint32_t total = 0;

	for (uint32_t i = 0; i < STRESS_SECTORS; i++) {
		total += stress_wear.erases[i];
		if (stress_wear.erases[i] > max) {
			max = stress_wear.erases[i];
			max_idx = i;
		}
	}

	printk("\n=== mspi_stress summary ===\n");
	printk("  erase calls        : %u\n", stress_wear.erase_ops);
	printk("  sector erases      : %u in the region, %u outside\n", total,
	       stress_wear.erase_outside);
	printk("  most erased sector : 0x%08x, %u times (budget %u)\n",
	       stress.region + max_idx * STRESS_SECTOR, max, CONFIG_MSPI_STRESS_ERASE_BUDGET);
	printk("  bytes written      : %u KiB\n", (uint32_t)(stress_wear.bytes_written / 1024U));
	printk("  bytes read         : %u KiB\n", (uint32_t)(stress_wear.bytes_read / 1024U));
	printk("  NOR endurance is typically 100k cycles per sector\n\n");

	zassert_true(max <= CONFIG_MSPI_STRESS_ERASE_BUDGET,
		     "a sector was erased %u times, more than the budget of %u", max,
		     CONFIG_MSPI_STRESS_ERASE_BUDGET);
}

ZTEST_SUITE(s9_report, stress_normal_boot, NULL, stress_before, NULL, NULL);
