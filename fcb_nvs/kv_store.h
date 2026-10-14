#ifndef KV_STORE_H
#define KV_STORE_H

/*
 * kv_store.h — generic, thread-safe, NVS-backed key/value store.
 *
 * Values are opaque byte blobs addressed by a 16-bit key. Key allocation and
 * the meaning/validation of each value belong to the caller.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Build-time configuration (override before including / via -D) ---- */

/** Devicetree label of the flash partition used by the store. */
#ifndef KV_PARTITION_LABEL
#define KV_PARTITION_LABEL storage_conf
#endif

/** Sector size in bytes — must equal the flash erase page size. */
#ifndef KV_SECTOR_SIZE
#define KV_SECTOR_SIZE 0x1000U
#endif

/** Number of sectors given to NVS (NVS needs at least 2). */
#ifndef KV_SECTOR_COUNT
#define KV_SECTOR_COUNT 4U
#endif

/** Largest value that can be saved with KV_SAVE_VERIFY. */
#ifndef KV_VERIFY_MAX_LEN
#define KV_VERIFY_MAX_LEN 64U
#endif

/* ---- Flags for kv_save() ---- */

#define KV_SAVE_DEFAULT 0U
/** Read the value back after writing and compare it. */
#define KV_SAVE_VERIFY  (1U << 0)

/**
 * @brief Mount the store. Idempotent; must be called before any other call.
 * @return 0 on success, negative errno on failure.
 */
int kv_init(void);

/**
 * @brief Store a value under @p key.
 *
 * @param key    Caller-defined key.
 * @param data   Value bytes.
 * @param len    Value length in bytes (> 0).
 * @param flags  KV_SAVE_DEFAULT or KV_SAVE_VERIFY.
 * @return 0 on success, -EINVAL bad args, -ENODEV not initialised,
 *         -EIO short write / verify mismatch, other negative on NVS error.
 */
int kv_save(uint16_t key, const void *data, size_t len, uint32_t flags);

/**
 * @brief Load the value stored under @p key.
 *
 * @param key     Caller-defined key.
 * @param out     Destination buffer.
 * @param len     Size of @p out in bytes.
 * @param actual  NULL  -> stored length must equal @p len exactly, else -EIO.
 *                !NULL -> a stored length <= @p len is accepted; @p out is
 *                         zero-filled first and *actual receives the stored
 *                         length (use for variable-size / versioned values).
 *                         A stored length > @p len returns -EIO.
 * @return 0 on success, -ENOENT if the key does not exist, -EINVAL bad args,
 *         -ENODEV not initialised, -EIO size mismatch, other negative on error.
 *         On -EIO @p out is zeroed.
 */
int kv_load(uint16_t key, void *out, size_t len, size_t *actual);

/**
 * @brief Delete @p key. Deleting a missing key is not an error.
 * @return 0 on success, negative errno on failure.
 */
int kv_delete(uint16_t key);

#ifdef __cplusplus
}
#endif

#endif /* KV_STORE_H */
