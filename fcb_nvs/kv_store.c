/*
 * kv_store.c — generic NVS-backed key/value store (see kv_store.h).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "kv_store.h"

#include <zephyr/storage/flash_map.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>

#define KV_FLASH_AREA_ID FIXED_PARTITION_ID(KV_PARTITION_LABEL)

static struct nvs_fs kv_fs;
static bool          kv_ready;

/* Serialises all access to kv_fs between threads. */
K_MUTEX_DEFINE(kv_mutex);

int kv_init(void)
{
    const struct flash_area *fa;
    int rc;

    k_mutex_lock(&kv_mutex, K_FOREVER);

    if (kv_ready) {
        k_mutex_unlock(&kv_mutex);
        return 0;
    }

    rc = flash_area_open(KV_FLASH_AREA_ID, &fa);
    if (rc) {
        k_mutex_unlock(&kv_mutex);
        return rc;
    }

    kv_fs.flash_device = fa->fa_dev;
    kv_fs.offset       = fa->fa_off;
    kv_fs.sector_size  = KV_SECTOR_SIZE;
    kv_fs.sector_count = KV_SECTOR_COUNT;

    flash_area_close(fa);

    rc = nvs_mount(&kv_fs);
    if (rc == 0) {
        kv_ready = true;
    }

    k_mutex_unlock(&kv_mutex);
    return rc;
}

int kv_save(uint16_t key, const void *data, size_t len, uint32_t flags)
{
    uint8_t verify[KV_VERIFY_MAX_LEN];
    bool do_verify = (flags & KV_SAVE_VERIFY) != 0U;
    ssize_t bytes;
    ssize_t verify_bytes = -1;

    if (data == NULL || len == 0U) {
        return -EINVAL;
    }
    if (do_verify && len > sizeof(verify)) {
        return -EINVAL;
    }

    k_mutex_lock(&kv_mutex, K_FOREVER);

    if (!kv_ready) {
        k_mutex_unlock(&kv_mutex);
        return -ENODEV;
    }

    bytes = nvs_write(&kv_fs, key, data, len);

    if (do_verify && bytes >= 0) {
        verify_bytes = nvs_read(&kv_fs, key, verify, len);
    }

    k_mutex_unlock(&kv_mutex);

    if (bytes < 0) {
        return (int)bytes;
    }
    /* nvs_write() returns 0 when the identical value is already stored. */
    if (bytes != 0 && bytes != (ssize_t)len) {
        return -EIO;
    }

    if (do_verify) {
        if (verify_bytes != (ssize_t)len) {
            return (verify_bytes < 0) ? (int)verify_bytes : -EIO;
        }
        if (memcmp(verify, data, len) != 0) {
            return -EIO;
        }
    }

    return 0;
}

int kv_load(uint16_t key, void *out, size_t len, size_t *actual)
{
    ssize_t bytes;

    if (out == NULL || len == 0U) {
        return -EINVAL;
    }

    if (actual != NULL) {
        memset(out, 0, len);
        *actual = 0U;
    }

    k_mutex_lock(&kv_mutex, K_FOREVER);

    if (!kv_ready) {
        k_mutex_unlock(&kv_mutex);
        return -ENODEV;
    }

    bytes = nvs_read(&kv_fs, key, out, len);

    k_mutex_unlock(&kv_mutex);

    if (bytes < 0) {
        return (int)bytes;          /* includes -ENOENT */
    }

    if (actual != NULL) {
        if ((size_t)bytes > len) {
            memset(out, 0, len);
            return -EIO;
        }
        *actual = (size_t)bytes;
        return 0;
    }

    if ((size_t)bytes != len) {
        memset(out, 0, len);
        return -EIO;
    }

    return 0;
}

int kv_delete(uint16_t key)
{
    int rc;

    k_mutex_lock(&kv_mutex, K_FOREVER);

    if (!kv_ready) {
        k_mutex_unlock(&kv_mutex);
        return -ENODEV;
    }

    rc = nvs_delete(&kv_fs, key);

    k_mutex_unlock(&kv_mutex);

    return (rc == -ENOENT) ? 0 : rc;
}
