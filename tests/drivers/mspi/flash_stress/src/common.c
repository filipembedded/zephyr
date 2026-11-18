/*
 * Copyright (c) 2026 Filip Stojanovic <filipembedded@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "stress.h"

struct stress_ctx stress;
struct stress_wear stress_wear;

uint8_t stress_exp[STRESS_SECTOR] __aligned(STRESS_ALIGN);
uint8_t stress_got[STRESS_SECTOR] __aligned(STRESS_ALIGN);
uint8_t stress_big[CONFIG_MSPI_STRESS_BIG_READ_SIZE + 2 * STRESS_ALIGN] __aligned(STRESS_ALIGN);

static uint8_t reread_a[STRESS_SECTOR] __aligned(STRESS_ALIGN);
static uint8_t reread_b[STRESS_SECTOR] __aligned(STRESS_ALIGN);

/* Partitions of the device under test, used to pick and protect the region. */
struct stress_part {
	const char *label;
	uint32_t off;
	uint32_t size;
	bool code;
};

#define STRESS_PART_NODE DT_CHILD(STRESS_NODE, partitions)

#define STRESS_IS_CODE(n)                                                                    \
	COND_CODE_1(DT_HAS_CHOSEN(zephyr_code_partition),                                   \
		    (DT_SAME_NODE(n, DT_CHOSEN(zephyr_code_partition))), (0))

#define STRESS_PART_ENTRY(n)                                                                 \
	{                                                                                    \
		.label = DT_PROP_OR(n, label, DT_NODE_FULL_NAME(n)),                         \
		.off = DT_REG_ADDR(n),                                                       \
		.size = DT_REG_SIZE(n),                                                      \
		.code = STRESS_IS_CODE(n),                                                   \
	},

static const struct stress_part parts[] = {
	COND_CODE_1(DT_NODE_EXISTS(STRESS_PART_NODE),
		    (DT_FOREACH_CHILD_STATUS_OKAY(STRESS_PART_NODE, STRESS_PART_ENTRY)), ())
	{ .label = NULL },
};

/* Code runs from this flash when it is the chosen zephyr,flash node. */
#define STRESS_DUT_IS_ZEPHYR_FLASH                                                           \
	COND_CODE_1(DT_HAS_CHOSEN(zephyr_flash),                                            \
		    (DT_SAME_NODE(DT_CHOSEN(zephyr_flash), STRESS_NODE)), (0))

static bool label_is_protected(const char *label)
{
	static const char *const prefixes[] = {
		"image", "slot", "mcuboot", "boot", "app", "code", "firmware",
	};

	for (size_t i = 0; i < ARRAY_SIZE(prefixes); i++) {
		if (strncmp(label, prefixes[i], strlen(prefixes[i])) == 0) {
			return true;
		}
	}

	return false;
}

static bool overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen)
{
	return (a < b + blen) && (b < a + alen);
}

static uint8_t lanes_of(enum mspi_io_mode mode)
{
	switch (mode) {
	case MSPI_IO_MODE_SINGLE:
		return 1;
	case MSPI_IO_MODE_DUAL:
	case MSPI_IO_MODE_DUAL_1_1_2:
	case MSPI_IO_MODE_DUAL_1_2_2:
		return 2;
	case MSPI_IO_MODE_QUAD:
	case MSPI_IO_MODE_QUAD_1_1_4:
	case MSPI_IO_MODE_QUAD_1_4_4:
		return 4;
	default:
		return 8;
	}
}

#define STRESS_IO_MODE DT_STRING_TOKEN(STRESS_NODE, mspi_io_mode)

static int pick_region(void)
{
	const uint32_t need = STRESS_REGION_SIZE;

#if defined(CONFIG_MSPI_STRESS_REGION_MANUAL)
	stress.region = CONFIG_MSPI_STRESS_REGION_OFFSET;
	stress.region_src = "Kconfig MSPI_STRESS_REGION_OFFSET";
#else
	const struct stress_part *storage = NULL;
	bool any_part = false;

	for (const struct stress_part *p = parts; p->label != NULL; p++) {
		any_part = true;
		if ((strcmp(p->label, "storage") == 0) && (p->size >= need)) {
			storage = p;
			break;
		}
	}

	if (storage != NULL) {
		/* Top of the storage partition, aligned to a 64 KiB block. */
		stress.region = ROUND_DOWN(storage->off + storage->size - need, STRESS_BLOCK);
		stress.region_src = "top of the \"storage\" partition";
	} else if (!any_part && !STRESS_DUT_IS_ZEPHYR_FLASH) {
		stress.region = ROUND_DOWN(stress.dev_size - need, STRESS_BLOCK);
		stress.region_src = "top of the device (no partitions defined)";
	} else {
		printk("mspi_stress: no usable test region. Add a \"storage\" partition of at\n"
		       "least %u KiB on the device, or set CONFIG_MSPI_STRESS_REGION_MANUAL.\n",
		       need / 1024U);
		return -ENOSPC;
	}
#endif

	if ((stress.region % STRESS_BLOCK) != 0U ||
	    (stress.region + need) > stress.dev_size) {
		printk("mspi_stress: region 0x%08x + %u KiB does not fit a %u KiB device or is\n"
		       "not 64 KiB aligned\n", stress.region, need / 1024U,
		       stress.dev_size / 1024U);
		return -EINVAL;
	}

	for (const struct stress_part *p = parts; p->label != NULL; p++) {
		if (overlaps(stress.region, need, p->off, p->size) &&
		    (p->code || label_is_protected(p->label))) {
			printk("mspi_stress: region overlaps protected partition \"%s\"\n",
			       p->label);
			return -EPERM;
		}
	}

	return 0;
}

static int stress_init(void)
{
	const struct flash_parameters *fp;
	uint64_t size = 0;
	int rc;

	stress.dev = DEVICE_DT_GET(STRESS_NODE);
	if (!device_is_ready(stress.dev)) {
		printk("mspi_stress: %s is not ready (check the jedec-id and the bus setup)\n",
		       stress.dev->name);
		return -ENODEV;
	}

	rc = flash_get_size(stress.dev, &size);
	stress.dev_size = (rc == 0) ? (uint32_t)size : DT_PROP(STRESS_NODE, size) / 8U;

	fp = flash_get_parameters(stress.dev);
	stress.erase_value = fp->erase_value;
	stress.page_size = 256U;
	stress.read_lanes = lanes_of(DT_STRING_TOKEN_OR(STRESS_NODE, read_io_mode,
							 STRESS_IO_MODE));
	stress.write_lanes = lanes_of(DT_STRING_TOKEN_OR(STRESS_NODE, write_io_mode,
							  STRESS_IO_MODE));

	rc = pick_region();
	if (rc != 0) {
		return rc;
	}

	printk("\n=== mspi_stress ===\n");
	printk("device   : %s (%s), %u KiB\n", stress.dev->name,
	       DT_NODE_FULL_NAME(STRESS_NODE), stress.dev_size / 1024U);
	printk("io mode  : %s, data rate %s, %u MHz max\n",
	       DT_PROP(STRESS_NODE, mspi_io_mode),
	       DT_PROP_OR(STRESS_NODE, mspi_data_rate, "MSPI_DATA_RATE_SINGLE"),
	       DT_PROP_OR(STRESS_NODE, mspi_max_frequency, 0) / 1000000U);
	printk("lanes    : read %u, write %u\n", stress.read_lanes, stress.write_lanes);
	printk("region   : 0x%08x..0x%08x (%s)\n", stress.region,
	       stress.region + STRESS_REGION_SIZE - 1U, stress.region_src);
	printk("prbs seed: 0x%08x\n", (uint32_t)CONFIG_MSPI_STRESS_PRBS_SEED);
	printk("NOTE: data in the region is overwritten\n\n");

	return 0;
}

int stress_ensure_init(void)
{
	static int init_rc = 1;

	if (init_rc == 1) {
		init_rc = stress_init();
		stress.ready = (init_rc == 0);
	}

	return init_rc;
}

void stress_before(void *fixture)
{
	int rc = stress_ensure_init();

	ARG_UNUSED(fixture);

	zassert_ok(rc, "mspi_stress setup failed (%d), see the message above", rc);
}

int stress_erase(uint32_t off, uint32_t len)
{
	int rc = flash_erase(stress.dev, off, len);

	if (rc == 0) {
		stress_wear.erase_ops++;
		for (uint32_t s = off; s < off + len; s += STRESS_SECTOR) {
			if (s >= stress.region && s < stress.region + STRESS_REGION_SIZE) {
				stress_wear.erases[(s - stress.region) / STRESS_SECTOR]++;
			} else {
				stress_wear.erase_outside++;
			}
		}
	}

	return rc;
}

int stress_write(uint32_t off, const void *buf, size_t len)
{
	int rc = flash_write(stress.dev, off, buf, len);

	if (rc == 0) {
		stress_wear.bytes_written += len;
	}

	return rc;
}

int stress_read(uint32_t off, void *buf, size_t len)
{
	int rc = flash_read(stress.dev, off, buf, len);

	if (rc == 0) {
		stress_wear.bytes_read += len;
	}

	return rc;
}

uint32_t stress_seed(uint32_t tag)
{
	uint32_t s = (uint32_t)CONFIG_MSPI_STRESS_PRBS_SEED ^ (tag * 0x9e3779b9U);

	return (s == 0U) ? 0x1U : s;
}

void stress_fill_prbs(uint8_t *buf, size_t len, uint32_t seed)
{
	uint32_t x = (seed == 0U) ? 1U : seed;

	for (size_t i = 0; i < len; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		buf[i] = (uint8_t)(x >> 24);
	}
}

static uint32_t pos_in_cmd(uint32_t off, uint32_t ws)
{
	const uint32_t page = stress.page_size;

	/* The first program command starts at ws, the next ones at page boundaries. */
	if ((off / page) == (ws / page)) {
		return off - ws;
	}

	return off % page;
}

void stress_compare(const uint8_t *exp, const uint8_t *got, size_t len, uint32_t off,
		    uint32_t ws, struct stress_err *e)
{
	e->checked += len;

	for (size_t i = 0; i < len; i++) {
		uint8_t diff = exp[i] ^ got[i];

		if (diff == 0U) {
			continue;
		}

		e->bad_bytes++;
		if (!e->have_first) {
			e->have_first = true;
			e->first_off = off + i;
			e->first_exp = exp[i];
			e->first_got = got[i];
		}

		for (int b = 0; b < 8; b++) {
			if ((diff & BIT(b)) == 0U) {
				continue;
			}
			if ((got[i] & BIT(b)) != 0U) {
				e->bit01[b]++;
			} else {
				e->bit10[b]++;
			}
		}

		e->pos_cmd[MIN(pos_in_cmd(off + i, ws), STRESS_POS_BUCKETS)]++;
	}
}

bool stress_verify(uint32_t off, const uint8_t *exp, size_t len, uint32_t ws,
		   struct stress_err *e)
{
	uint32_t before = e->bad_bytes;
	size_t done = 0;

	while (done < len) {
		size_t n = MIN(len - done, sizeof(reread_a));
		int rc = stress_read(off + done, reread_a, n);

		if (rc != 0) {
			e->ops_failed++;
			printk("  read 0x%08x+%u failed: %d\n", off + (uint32_t)done,
			       (uint32_t)n, rc);
			return false;
		}

		uint32_t chunk_before = e->bad_bytes;

		stress_compare(exp + done, reread_a, n, off + done, ws, e);

		if (e->bad_bytes != chunk_before) {
			/* Re-read twice: same wrong data means it is stored wrong. */
			for (int r = 0; r < 2; r++) {
				if (stress_read(off + done, reread_b, n) != 0) {
					e->ops_failed++;
					break;
				}
				e->rereads++;
				for (size_t i = 0; i < n; i++) {
					if (reread_a[i] != reread_b[i]) {
						e->unstable++;
					}
				}
			}
		}

		done += n;
	}

	return e->bad_bytes == before;
}

bool stress_verify_erased(uint32_t off, size_t len, struct stress_err *e)
{
	static uint8_t erased_ref[STRESS_SECTOR];
	bool ok = true;

	memset(erased_ref, stress.erase_value, sizeof(erased_ref));
	for (size_t done = 0; done < len; done += sizeof(erased_ref)) {
		size_t n = MIN(len - done, sizeof(erased_ref));

		/* ws = off: an erase has no program command, positions stay relative. */
		ok &= stress_verify(off + done, erased_ref, n, off + done, e);
	}

	return ok;
}

void stress_err_merge(struct stress_err *dst, const struct stress_err *src)
{
	dst->checked += src->checked;
	dst->bad_bytes += src->bad_bytes;
	dst->rereads += src->rereads;
	dst->unstable += src->unstable;
	dst->ops_failed += src->ops_failed;
	for (int b = 0; b < 8; b++) {
		dst->bit01[b] += src->bit01[b];
		dst->bit10[b] += src->bit10[b];
	}
	for (int p = 0; p <= STRESS_POS_BUCKETS; p++) {
		dst->pos_cmd[p] += src->pos_cmd[p];
	}
	if (!dst->have_first && src->have_first) {
		dst->have_first = true;
		dst->first_off = src->first_off;
		dst->first_exp = src->first_exp;
		dst->first_got = src->first_got;
	}
}

void stress_report(const char *title, const struct stress_err *e)
{
	const uint8_t lanes = stress.read_lanes;

	if (e->bad_bytes == 0U && e->ops_failed == 0U) {
		printk("  [%s] OK, %u bytes checked\n", title, e->checked);
		return;
	}

	printk("\n  [%s] %u bad bytes of %u checked, %u failed operations\n", title,
	       e->bad_bytes, e->checked, e->ops_failed);

	if (e->bad_bytes == 0U) {
		return;
	}

	printk("  first: 0x%08x expected 0x%02x read 0x%02x\n", e->first_off, e->first_exp,
	       e->first_got);

	printk("  bit     0->1   1->0\n");
	for (int b = 7; b >= 0; b--) {
		printk("  bit%d %7u %6u\n", b, e->bit01[b], e->bit10[b]);
	}

	if (lanes > 1U) {
		printk("  lane    0->1   1->0   (%u data lanes, bit b on IO(b %% %u))\n", lanes,
		       lanes);
		for (uint8_t l = 0; l < lanes; l++) {
			uint32_t up = 0;
			uint32_t down = 0;

			for (int b = l; b < 8; b += lanes) {
				up += e->bit01[b];
				down += e->bit10[b];
			}
			printk("  IO%u  %7u %6u\n", l, up, down);
		}
	} else {
		printk("  single line mode: writes use IO0, reads use IO1\n");
	}

	printk("  position in page program command:");
	for (int p = 0; p < STRESS_POS_BUCKETS; p++) {
		if ((p % 16) == 0) {
			printk("\n   %3d:", p);
		}
		printk(" %u", e->pos_cmd[p]);
	}
	printk("\n   >=%d: %u\n", STRESS_POS_BUCKETS, e->pos_cmd[STRESS_POS_BUCKETS]);

	if (e->rereads == 0U) {
		printk("  re-read: not done\n");
	} else if (e->unstable == 0U) {
		printk("  re-read x%u: stable -> data is stored wrong (write/program/erase path)\n",
		       e->rereads);
	} else {
		printk("  re-read x%u: %u bytes changed between reads -> unstable read path\n",
		       e->rereads, e->unstable);
	}
	printk("\n");
}
