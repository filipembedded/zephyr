/*
 * Copyright (c) 2026 Filip Stojanovic <filipembedded@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MSPI_STRESS_H_
#define MSPI_STRESS_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/mspi.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

/* Device under test: the ext-nor0 alias, or the first enabled jedec,nor node. */
#if DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(ext_nor0))
#define STRESS_NODE DT_ALIAS(ext_nor0)
#elif DT_HAS_COMPAT_STATUS_OKAY(jedec_nor)
#define STRESS_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(jedec_nor)
#else
#error "No MSPI NOR flash: define the ext-nor0 alias or enable a jedec,nor node"
#endif

BUILD_ASSERT(DT_NODE_HAS_COMPAT(STRESS_NODE, jedec_nor),
	     "The device under test must be a jedec,nor node");

#define STRESS_SECTOR      0x1000U
#define STRESS_BLOCK       0x10000U
#define STRESS_BLOCKS      16U
#define STRESS_REGION_SIZE (STRESS_BLOCKS * STRESS_BLOCK)
#define STRESS_SECTORS     (STRESS_REGION_SIZE / STRESS_SECTOR)

/* Fixed use of the 16 blocks of the test region, so suites never overlap. */
enum stress_block {
	BLK_LANES = 0,       /* 4 blocks: S2 patterns, one 4 KiB sector each */
	BLK_WRITE_LEN = 4,   /* 2 blocks: S3 write lengths 1..300 */
	BLK_READ_LEN = 6,    /* S3 read lengths, prefilled */
	BLK_ERASE_GUARD = 7, /* 3 blocks: S3 erase guard */
	BLK_SEQUENCE = 10,   /* S4 command sequences */
	BLK_CONC_WRITE = 11, /* S5 writer thread */
	BLK_CONC_READ = 12,  /* S5 reader thread, prefilled */
	BLK_COHERENCY = 13,  /* S6 read-after-write coherency */
	BLK_SPARE = 14,
	BLK_PERSIST = 15,    /* S7 data kept between runs */
};

#ifdef CONFIG_DCACHE_LINE_SIZE
#define STRESS_ALIGN MAX(CONFIG_DCACHE_LINE_SIZE, 4)
#else
#define STRESS_ALIGN 32
#endif

/* Number of tracked positions inside one page program command. */
#define STRESS_POS_BUCKETS 32

struct stress_err {
	uint32_t checked;
	uint32_t bad_bytes;
	uint32_t bit01[8]; /* expected 0, read 1 */
	uint32_t bit10[8]; /* expected 1, read 0 */
	uint32_t pos_cmd[STRESS_POS_BUCKETS + 1];
	uint32_t rereads;
	uint32_t unstable;  /* bytes that changed between re-reads */
	uint32_t ops_failed;
	bool have_first;
	uint32_t first_off;
	uint8_t first_exp;
	uint8_t first_got;
};

struct stress_ctx {
	const struct device *dev;
	uint32_t dev_size;
	uint32_t region;      /* absolute offset of the test region */
	uint32_t page_size;   /* program page size */
	uint8_t erase_value;
	uint8_t read_lanes;
	uint8_t write_lanes;
	const char *region_src;
	bool ready;
};

extern struct stress_ctx stress;

/* Pick the region and print the setup once; returns 0 when usable. */
int stress_ensure_init(void);

/* Common per-test hook: makes sure the device and region are usable. */
void stress_before(void *fixture);

/* Predicate that skips everything except S7 while a reboot cycle runs. */
bool stress_normal_boot(const void *global_state);

static inline uint32_t stress_blk(enum stress_block b)
{
	return stress.region + (uint32_t)b * STRESS_BLOCK;
}

/* Flash wrappers: assert on driver errors and keep wear statistics. */
int stress_erase(uint32_t off, uint32_t len);
int stress_write(uint32_t off, const void *buf, size_t len);
int stress_read(uint32_t off, void *buf, size_t len);

/* Data generators. */
uint32_t stress_seed(uint32_t tag);
void stress_fill_prbs(uint8_t *buf, size_t len, uint32_t seed);

/*
 * Compare expected data with what was read. ws is the flash offset where the
 * flash_write() call that stored this data started, used to locate each bad
 * byte inside its page program command.
 */
void stress_compare(const uint8_t *exp, const uint8_t *got, size_t len, uint32_t off,
		    uint32_t ws, struct stress_err *e);

/*
 * Read len bytes at off and compare with exp. On a mismatch the area is read
 * two more times to tell unstable reads from wrongly stored data.
 * Returns true when the data matches.
 */
bool stress_verify(uint32_t off, const uint8_t *exp, size_t len, uint32_t ws,
		   struct stress_err *e);

/* Check that an area reads back as the erase value. */
bool stress_verify_erased(uint32_t off, size_t len, struct stress_err *e);

void stress_report(const char *title, const struct stress_err *e);
void stress_err_merge(struct stress_err *dst, const struct stress_err *src);

/* Shared scratch buffers; tests run one at a time. */
extern uint8_t stress_exp[STRESS_SECTOR];
extern uint8_t stress_got[STRESS_SECTOR];
extern uint8_t stress_big[CONFIG_MSPI_STRESS_BIG_READ_SIZE + 2 * STRESS_ALIGN];

/* Wear and traffic counters, printed by the final report. */
struct stress_wear {
	uint16_t erases[STRESS_SECTORS];
	uint32_t erase_ops;
	uint32_t erase_outside;
	uint64_t bytes_written;
	uint64_t bytes_read;
};

extern struct stress_wear stress_wear;

#endif /* MSPI_STRESS_H_ */
