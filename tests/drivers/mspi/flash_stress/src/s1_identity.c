/*
 * Copyright (c) 2026 Filip Stojanovic <filipembedded@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * S1: identity. JEDEC ID and SFDP read from the chip after init, compared with
 * the devicetree, plus a sanity check of the decoded parameters.
 */

#include <string.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

#include <jesd216.h>

#include "stress.h"

#define SFDP_MAX_PH 8

static uint8_t sfdp_hdr_buf[sizeof(struct jesd216_sfdp_header) +
			    SFDP_MAX_PH * sizeof(struct jesd216_param_header)] __aligned(4);
static uint32_t chip_bfp[64] __aligned(4);
static struct jesd216_param_header chip_bfp_ph;
static bool chip_bfp_valid;

#if DT_NODE_HAS_PROP(STRESS_NODE, sfdp_bfp)
static const uint8_t dt_bfp[] = DT_PROP(STRESS_NODE, sfdp_bfp);
#endif
#if DT_NODE_HAS_PROP(STRESS_NODE, sfdp_ff84)
static const uint8_t dt_ff84[] = DT_PROP(STRESS_NODE, sfdp_ff84);
#endif
#if DT_NODE_HAS_PROP(STRESS_NODE, sfdp_ff05)
static const uint8_t dt_ff05[] = DT_PROP(STRESS_NODE, sfdp_ff05);
#endif

static bool id_is_garbage(const uint8_t *id)
{
	/* All three bytes equal is what a floating or unanswered bus reads. */
	return (id[0] == id[1]) && (id[1] == id[2]);
}

ZTEST(s1_identity, test_1_1_jedec_id)
{
#if defined(CONFIG_FLASH_JESD216_API)
	/* Drivers write up to SPI_NOR_MAX_ID_LEN (3) bytes; keep some margin. */
	uint8_t id[3][8];
	int rc;

	/*
	 * Read after init, so the chip is already in its target io mode. This is
	 * the case that broke on octal parts (RDID without address and dummy).
	 */
	for (int i = 0; i < 3; i++) {
		rc = flash_read_jedec_id(stress.dev, id[i]);
		if (rc == -ENOTSUP || rc == -ENOSYS) {
			ztest_test_skip();
		}
		zassert_ok(rc, "flash_read_jedec_id() failed: %d", rc);
	}

	printk("  JEDEC ID read from chip: %02x %02x %02x\n", id[0][0], id[0][1], id[0][2]);

	zassert_mem_equal(id[0], id[1], 3, "JEDEC ID differs between reads (unstable bus)");
	zassert_mem_equal(id[0], id[2], 3, "JEDEC ID differs between reads (unstable bus)");
	zassert_false(id_is_garbage(id[0]),
		      "JEDEC ID %02x %02x %02x looks like an unanswered bus: check the RDID "
		      "address phase and dummy cycles in the current io mode",
		      id[0][0], id[0][1], id[0][2]);

#if DT_NODE_HAS_PROP(STRESS_NODE, jedec_id)
	static const uint8_t dt_id[] = DT_PROP(STRESS_NODE, jedec_id);

	printk("  JEDEC ID in devicetree : %02x %02x %02x\n", dt_id[0], dt_id[1], dt_id[2]);
	zassert_mem_equal(id[0], dt_id, 3,
			  "JEDEC ID mismatch: the chip may be another ordering variant "
			  "(e.g. Winbond -IQ 0x40 vs -IM 0x70)");
#else
	printk("  no jedec-id in devicetree, ID not verified\n");
#endif
#else
	ztest_test_skip();
#endif
}

#if defined(CONFIG_FLASH_JESD216_API) &&                                                    \
	(DT_NODE_HAS_PROP(STRESS_NODE, sfdp_bfp) || DT_NODE_HAS_PROP(STRESS_NODE, sfdp_ff84) ||  \
	 DT_NODE_HAS_PROP(STRESS_NODE, sfdp_ff05))
static int compare_table(const char *name, uint32_t addr, uint32_t chip_len,
			 const uint8_t *dt, size_t dt_len)
{
	static uint8_t chip[256] __aligned(4);
	uint32_t n = MIN(chip_len, sizeof(chip));
	int diffs = 0;
	int rc;

	rc = flash_sfdp_read(stress.dev, addr, chip, n);
	if (rc != 0) {
		printk("  %s: SFDP read failed: %d\n", name, rc);
		return -EIO;
	}

	if (dt_len == 0U) {
		printk("  %s: empty array in devicetree (table not used)\n", name);
		return 0;
	}

	if (dt_len != n) {
		printk("  %s: length differs, chip %u bytes, devicetree %u bytes\n", name, n,
		       (uint32_t)dt_len);
	}

	for (uint32_t i = 0; i < MIN(n, dt_len); i++) {
		if (chip[i] != dt[i]) {
			if (diffs < 16) {
				printk("  %s: DW%u byte %u: chip 0x%02x, devicetree 0x%02x\n",
				       name, i / 4U + 1U, i % 4U, chip[i], dt[i]);
			}
			diffs++;
		}
	}

	if (diffs == 0 && dt_len == n) {
		printk("  %s: devicetree matches the chip (%u bytes)\n", name, n);
	}

	return diffs + ((dt_len != n) ? 1 : 0);
}
#endif

ZTEST(s1_identity, test_1_2_sfdp_vs_devicetree)
{
#if defined(CONFIG_FLASH_JESD216_API)
	const struct jesd216_sfdp_header *hp = (const void *)sfdp_hdr_buf;
	int mismatches = 0;
	int rc;

	rc = flash_sfdp_read(stress.dev, 0, sfdp_hdr_buf, sizeof(sfdp_hdr_buf));
	if (rc == -ENOTSUP || rc == -ENOSYS) {
		ztest_test_skip();
	}
	zassert_ok(rc, "flash_sfdp_read() failed: %d", rc);

	zassert_equal(jesd216_sfdp_magic(hp), JESD216_SFDP_MAGIC,
		      "SFDP signature 0x%08x is wrong: the chip did not answer 5Ah correctly "
		      "in the current io mode", jesd216_sfdp_magic(hp));

	printk("  SFDP rev %u.%u, %u parameter headers\n", hp->rev_major, hp->rev_minor,
	       hp->nph + 1U);

	for (int i = 0; i <= MIN(hp->nph, SFDP_MAX_PH - 1); i++) {
		const struct jesd216_param_header *php = &hp->phdr[i];
		uint16_t id = jesd216_param_id(php);
		uint32_t addr = jesd216_param_addr(php);
		uint32_t len = jesd216_param_len(php);

		printk("  PH%d: id %04x rev %u.%u, %u DW at 0x%06x\n", i, id, php->rev_major,
		       php->rev_minor, php->len_dw, addr);

		if (id == JESD216_SFDP_PARAM_ID_BFP && !chip_bfp_valid) {
			uint32_t n = MIN(len, sizeof(chip_bfp));

			if (flash_sfdp_read(stress.dev, addr, chip_bfp, n) == 0) {
				chip_bfp_ph = *php;
				chip_bfp_ph.len_dw = n / 4U;
				chip_bfp_valid = true;
			}
#if DT_NODE_HAS_PROP(STRESS_NODE, sfdp_bfp)
			rc = compare_table("sfdp-bfp", addr, len, dt_bfp, sizeof(dt_bfp));
			mismatches += (rc < 0) ? 1 : rc;
#endif
		}
#if DT_NODE_HAS_PROP(STRESS_NODE, sfdp_ff84)
		if (id == JESD216_SFDP_PARAM_ID_4B_ADDR_INSTR) {
			rc = compare_table("sfdp-ff84", addr, len, dt_ff84, sizeof(dt_ff84));
			mismatches += (rc < 0) ? 1 : rc;
		}
#endif
#if DT_NODE_HAS_PROP(STRESS_NODE, sfdp_ff05)
		if (id == JESD216_SFDP_PARAM_ID_XSPI_PROFILE_1V0) {
			rc = compare_table("sfdp-ff05", addr, len, dt_ff05, sizeof(dt_ff05));
			mismatches += (rc < 0) ? 1 : rc;
		}
#endif
	}

#if !DT_NODE_HAS_PROP(STRESS_NODE, sfdp_bfp)
	printk("  no sfdp-bfp in devicetree, commands come from DT properties\n");
#endif

	zassert_equal(mismatches, 0,
		      "SFDP tables in devicetree differ from the chip (%d differences): the "
		      "driver is configured with parameters of another part", mismatches);
#else
	ztest_test_skip();
#endif
}

static const char *const qer_names[] = {
	"none", "S2B1v1", "S1B6", "S2B7", "S2B1v4", "S2B1v5", "S2B1v6", "reserved",
};

ZTEST(s1_identity, test_1_3_decoded_parameters)
{
	const struct jesd216_bfp *bfp = (const void *)chip_bfp;
	const struct jesd216_param_header *php = &chip_bfp_ph;
	struct jesd216_bfp_dw15 dw15;
	struct jesd216_bfp_dw16 dw16;
	struct jesd216_erase_type et;
	uint64_t density;
	int warnings = 0;

	if (!chip_bfp_valid) {
		printk("  BFP not read from the chip, nothing to decode\n");
		ztest_test_skip();
	}

	density = jesd216_bfp_density(bfp) / 8U;
	stress.page_size = jesd216_bfp_page_size(php, bfp);

	printk("  density %u KiB, page %u bytes, address bytes code %u\n",
	       (uint32_t)(density / 1024U), stress.page_size, jesd216_bfp_addrbytes(bfp));

	zassert_equal(density, stress.dev_size,
		      "SFDP density %u differs from the device size %u",
		      (uint32_t)density, stress.dev_size);

	for (uint8_t i = 1; i <= JESD216_NUM_ERASE_TYPES; i++) {
		if (jesd216_bfp_erase(bfp, i, &et) == 0) {
			printk("  erase type %u: %u KiB, opcode 0x%02x\n", i,
			       (uint32_t)(BIT(et.exp) / 1024U), et.cmd);
		}
	}

	if (jesd216_bfp_decode_dw15(php, bfp, &dw15) == 0) {
		printk("  QER %u (%s)\n", dw15.qer, qer_names[dw15.qer & 7U]);
		if ((stress.read_lanes == 4U || stress.write_lanes == 4U) && dw15.qer == 0U) {
			printk("  WARNING: quad io mode but QER is 0: the driver will not set QE;\n"
			       "           fine only if the part has no QE bit or it is factory set\n");
			warnings++;
		}
	} else {
		printk("  no DW15 in BFP (JESD216 rev < B)\n");
		if (stress.read_lanes == 4U || stress.write_lanes == 4U) {
			printk("  WARNING: quad io mode without DW15: QER unknown to the driver\n");
			warnings++;
		}
	}

	if (jesd216_bfp_decode_dw16(php, bfp, &dw16) == 0) {
		printk("  4-byte address entry methods 0x%02x, soft reset 0x%02x\n",
		       dw16.enter_4ba, dw16.srrs_support);
	}

	printk("  %d warning(s)\n", warnings);
}

ZTEST(s1_identity, test_1_4_size_and_layout)
{
	const struct flash_parameters *fp = flash_get_parameters(stress.dev);
	struct flash_pages_info info;
	size_t count = flash_get_page_count(stress.dev);
	int rc;

	zassert_equal(stress.dev_size, DT_PROP(STRESS_NODE, size) / 8U,
		      "flash_get_size() %u differs from devicetree size %u", stress.dev_size,
		      (uint32_t)(DT_PROP(STRESS_NODE, size) / 8U));

	rc = flash_get_page_info_by_idx(stress.dev, 0, &info);
	zassert_ok(rc, "flash_get_page_info_by_idx() failed: %d", rc);

	printk("  layout: %u pages of %u bytes, erase value 0x%02x, write block %u\n",
	       (uint32_t)count, (uint32_t)info.size, fp->erase_value,
	       (uint32_t)fp->write_block_size);

	zassert_equal((uint64_t)count * info.size, stress.dev_size,
		      "page layout covers %u bytes, device has %u",
		      (uint32_t)(count * info.size), stress.dev_size);
}

ZTEST_SUITE(s1_identity, stress_normal_boot, NULL, stress_before, NULL, NULL);
