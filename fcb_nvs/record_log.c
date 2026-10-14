/*
 * record_log.c — generic fixed-size record log on FCB (see record_log.h).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "record_log.h"

#include <zephyr/storage/flash_map.h>
#include <zephyr/fs/fcb.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/devicetree/fixed-partitions.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>

#define RLOG_FLASH_AREA_ID    FIXED_PARTITION_ID(RLOG_PARTITION_LABEL)
#define RLOG_PARTITION_SIZE   FIXED_PARTITION_SIZE(RLOG_PARTITION_LABEL)
#define RLOG_SECTOR_COUNT     (RLOG_PARTITION_SIZE / RLOG_SECTOR_SIZE)

BUILD_ASSERT((RLOG_PARTITION_SIZE % RLOG_SECTOR_SIZE) == 0,
             "log partition must be a multiple of RLOG_SECTOR_SIZE");

static struct fcb          rlog_fcb;
static struct flash_sector rlog_sectors[RLOG_SECTOR_COUNT];
static bool                rlog_ready;

K_MUTEX_DEFINE(rlog_mutex);

/* ---------------------------------------------------------------- */

static uint16_t rlog_crc16(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
        }
    }

    return crc;
}

/* Validates the CRC of a raw entry buffer (payload + crc). */
static bool rlog_entry_valid(const uint8_t *raw)
{
    uint16_t stored;

    memcpy(&stored, raw + RLOG_PAYLOAD_SIZE, sizeof(stored));
    return rlog_crc16(raw, RLOG_PAYLOAD_SIZE) == stored;
}

/* ---------------------------------------------------------------- */

static int rlog_init_locked(void)
{
    const struct flash_area *fa;
    const struct flash_parameters *fp;
    uint8_t probe;
    int rc;

    rc = flash_area_open(RLOG_FLASH_AREA_ID, &fa);
    if (rc) {
        return rc;
    }

    fp = flash_get_parameters(fa->fa_dev);
    if (!fp) {
        flash_area_close(fa);
        return -EINVAL;
    }

    for (size_t i = 0; i < RLOG_SECTOR_COUNT; i++) {
        rlog_sectors[i].fs_off  = i * RLOG_SECTOR_SIZE;
        rlog_sectors[i].fs_size = RLOG_SECTOR_SIZE;
    }

    /* Sanity check: the partition must be readable. */
    rc = flash_area_read(fa, 0, &probe, 1);
    if (rc) {
        flash_area_close(fa);
        return rc;
    }

    rlog_fcb.f_erase_value = fp->erase_value;
    rlog_fcb.f_sector_cnt  = RLOG_SECTOR_COUNT;
    rlog_fcb.f_sectors     = rlog_sectors;

    rc = fcb_init(RLOG_FLASH_AREA_ID, &rlog_fcb);

    flash_area_close(fa);

    rlog_ready = (rc == 0);
    return rc;
}

int rlog_init(void)
{
    int rc;

    k_mutex_lock(&rlog_mutex, K_FOREVER);
    rc = rlog_ready ? 0 : rlog_init_locked();
    k_mutex_unlock(&rlog_mutex);

    return rc;
}

/* ---------------------------------------------------------------- */

static int rlog_append_locked(const void *payload)
{
    uint8_t raw[RLOG_ENTRY_SIZE];
    struct fcb_entry loc;
    uint16_t crc;
    int rc;

    if (!payload) {
        return -EINVAL;
    }
    if (!rlog_ready) {
        return -ENODEV;
    }

    memcpy(raw, payload, RLOG_PAYLOAD_SIZE);
    crc = rlog_crc16(raw, RLOG_PAYLOAD_SIZE);
    memcpy(raw + RLOG_PAYLOAD_SIZE, &crc, sizeof(crc));

    for (;;) {
        rc = fcb_append(&rlog_fcb, RLOG_ENTRY_SIZE, &loc);
        if (rc != -ENOSPC) {
            break;
        }
        rc = fcb_rotate(&rlog_fcb);
        if (rc) {
            return rc;
        }
    }
    if (rc) {
        return rc;
    }

    rc = flash_area_write(rlog_fcb.fap, FCB_ENTRY_FA_DATA_OFF(loc),
                          raw, RLOG_ENTRY_SIZE);
    if (rc) {
        return rc;
    }

    return fcb_append_finish(&rlog_fcb, &loc);
}

int rlog_append(const void *payload)
{
    int rc;

    k_mutex_lock(&rlog_mutex, K_FOREVER);
    rc = rlog_append_locked(payload);
    k_mutex_unlock(&rlog_mutex);

    return rc;
}

/* ---------------------------------------------------------------- */

static int rlog_read_last_n_locked(uint32_t n, rlog_cb_t cb, void *arg)
{
    struct fcb_entry loc;
    uint8_t raw[RLOG_ENTRY_SIZE];
    int rc;

    if (!cb) {
        return -EINVAL;
    }
    if (!rlog_ready) {
        return -ENODEV;
    }

    rc = fcb_offset_last_n(&rlog_fcb, n, &loc);
    if (rc) {
        return rc;
    }

    if (loc.fe_data_len != RLOG_ENTRY_SIZE) {
        return -EINVAL;
    }

    rc = flash_area_read(rlog_fcb.fap, FCB_ENTRY_FA_DATA_OFF(loc),
                         raw, RLOG_ENTRY_SIZE);
    if (rc) {
        return rc;
    }

    if (!rlog_entry_valid(raw)) {
        return -EIO;
    }

    return cb(raw, arg);
}

int rlog_read_last_n(uint32_t n, rlog_cb_t cb, void *arg)
{
    int rc;

    k_mutex_lock(&rlog_mutex, K_FOREVER);
    rc = rlog_read_last_n_locked(n, cb, arg);
    k_mutex_unlock(&rlog_mutex);

    return rc;
}

static int rlog_copy_cb(const void *record, void *arg)
{
    memcpy(arg, record, RLOG_PAYLOAD_SIZE);
    return 0;
}

int rlog_read_last(void *out)
{
    if (!out) {
        return -EINVAL;
    }

    return rlog_read_last_n(1, rlog_copy_cb, out);
}

/* ---------------------------------------------------------------- */

struct rlog_walk_ctx {
    rlog_cb_t user_cb;
    void     *user_arg;
};

static int rlog_walk_inner(struct fcb_entry_ctx *entry_ctx, void *arg)
{
    struct rlog_walk_ctx *ctx = arg;
    uint8_t raw[RLOG_ENTRY_SIZE];

    if (entry_ctx->loc.fe_data_len != RLOG_ENTRY_SIZE) {
        return 0; /* wrong-size entry: skip */
    }

    if (flash_area_read(entry_ctx->fap,
                        FCB_ENTRY_FA_DATA_OFF(entry_ctx->loc),
                        raw, RLOG_ENTRY_SIZE) != 0) {
        return 0;
    }

    if (!rlog_entry_valid(raw)) {
        return 0; /* corrupt entry: skip */
    }

    return ctx->user_cb(raw, ctx->user_arg);
}

static int rlog_walk_all_locked(rlog_cb_t cb, void *arg)
{
    struct rlog_walk_ctx ctx = { .user_cb = cb, .user_arg = arg };

    if (!cb) {
        return -EINVAL;
    }
    if (!rlog_ready) {
        return -ENODEV;
    }

    return fcb_walk(&rlog_fcb, NULL, rlog_walk_inner, &ctx);
}

int rlog_walk_all(rlog_cb_t cb, void *arg)
{
    int rc;

    k_mutex_lock(&rlog_mutex, K_FOREVER);
    rc = rlog_walk_all_locked(cb, arg);
    k_mutex_unlock(&rlog_mutex);

    return rc;
}

static int rlog_count_cb(const void *record, void *arg)
{
    uint32_t *count = arg;

    ARG_UNUSED(record);

    if (*count == UINT32_MAX) {
        return 1;
    }

    (*count)++;
    return 0;
}

int rlog_count(uint32_t *out_count)
{
    uint32_t count = 0U;
    int rc;

    if (out_count == NULL) {
        return -EINVAL;
    }

    k_mutex_lock(&rlog_mutex, K_FOREVER);
    rc = rlog_walk_all_locked(rlog_count_cb, &count);
    k_mutex_unlock(&rlog_mutex);

    if (rc < 0) {
        return rc;
    }

    *out_count = count;
    return 0;
}

/* ---------------------------------------------------------------- */

int rlog_clear(void)
{
    const struct flash_area *fa;
    int rc;

    k_mutex_lock(&rlog_mutex, K_FOREVER);

    rc = flash_area_open(RLOG_FLASH_AREA_ID, &fa);
    if (rc == 0) {
        rc = flash_area_flatten(fa, 0, fa->fa_size);
        flash_area_close(fa);
    }
    if (rc == 0) {
        rc = rlog_init_locked();
    }

    k_mutex_unlock(&rlog_mutex);

    return rc;
}
