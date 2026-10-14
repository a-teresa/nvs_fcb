#ifndef RECORD_LOG_H
#define RECORD_LOG_H

/*
 * record_log.h — generic fixed-size record log on an FCB flash partition.
 *
 * Records are opaque RLOG_PAYLOAD_SIZE-byte blobs. The layer appends a
 * CRC-16 after each payload, validates it on read, and overwrites the oldest
 * sector when the partition is full (circular log).
 *
 * On-flash entry = payload (RLOG_PAYLOAD_SIZE bytes) + CRC-16 (2 bytes,
 * native endianness).
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Build-time configuration (override via -D or before including) ---- */

/** Devicetree label of the flash partition used by the log. */
#ifndef RLOG_PARTITION_LABEL
#define RLOG_PARTITION_LABEL measurement_data
#endif

/** Sector size in bytes — must equal the flash erase page size. */
#ifndef RLOG_SECTOR_SIZE
#define RLOG_SECTOR_SIZE 0x1000U
#endif

/** Payload size in bytes. MUST be defined by the application. */
#ifndef RLOG_PAYLOAD_SIZE
#error "Define RLOG_PAYLOAD_SIZE (record size in bytes, excluding the CRC)"
#endif

#define RLOG_CRC_SIZE   2U
#define RLOG_ENTRY_SIZE (RLOG_PAYLOAD_SIZE + RLOG_CRC_SIZE)

/**
 * Record callback.
 * @param record  Pointer to a buffer of RLOG_ENTRY_SIZE bytes; the first
 *                RLOG_PAYLOAD_SIZE bytes are the payload. Valid only during
 *                the call.
 * @param arg     Caller context.
 * @return 0 to continue (walk only), non-zero to stop.
 */
typedef int (*rlog_cb_t)(const void *record, void *arg);

/** Mount/initialise the log. Idempotent. */
int rlog_init(void);

/** Append one record (RLOG_PAYLOAD_SIZE bytes read from @p payload). */
int rlog_append(const void *payload);

/**
 * Invoke @p cb on the n-th newest record (n = 1 is the newest).
 * @return cb's return value, or negative errno (-EIO on CRC mismatch,
 *         -EINVAL on bad entry length, FCB error if no such entry).
 */
int rlog_read_last_n(uint32_t n, rlog_cb_t cb, void *arg);

/** Copy the payload of the newest record into @p out (RLOG_PAYLOAD_SIZE bytes). */
int rlog_read_last(void *out);

/**
 * Walk all valid records oldest -> newest. Entries with a wrong length or bad
 * CRC are skipped. @p cb returns 0 to continue, non-zero to stop early.
 */
int rlog_walk_all(rlog_cb_t cb, void *arg);

/** Count valid records (saturates at UINT32_MAX). */
int rlog_count(uint32_t *out_count);

/** Erase the whole partition and re-initialise the log. */
int rlog_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* RECORD_LOG_H */
