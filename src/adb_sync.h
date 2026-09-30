/*
 * ADB sync service ("sync:") — adb push / pull / ls over the Zephyr VFS.
 */
#ifndef ADB_SYNC_H
#define ADB_SYNC_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Sync message IDs: four ASCII chars read as a little-endian uint32. */
#define SYNC_ID(a, b, c, d) \
	((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

#define SYNC_STAT SYNC_ID('S', 'T', 'A', 'T')
#define SYNC_LIST SYNC_ID('L', 'I', 'S', 'T')
#define SYNC_SEND SYNC_ID('S', 'E', 'N', 'D')
#define SYNC_RECV SYNC_ID('R', 'E', 'C', 'V')
#define SYNC_DENT SYNC_ID('D', 'E', 'N', 'T')
#define SYNC_DONE SYNC_ID('D', 'O', 'N', 'E')
#define SYNC_DATA SYNC_ID('D', 'A', 'T', 'A')
#define SYNC_OKAY SYNC_ID('O', 'K', 'A', 'Y')
#define SYNC_FAIL SYNC_ID('F', 'A', 'I', 'L')
#define SYNC_QUIT SYNC_ID('Q', 'U', 'I', 'T')

#ifdef CONFIG_ADB_SYNC
/*
 * All handlers below are called from the ADB RX work item and never block.
 * The sync protocol itself runs on a dedicated thread.
 */
void adb_sync_handle_open(uint32_t remote_id);
void adb_sync_handle_wrte(uint32_t local_id, uint32_t remote_id, const uint8_t *data, size_t len);
void adb_sync_handle_clse(uint32_t local_id, uint32_t remote_id);
void adb_sync_handle_okay(uint32_t local_id, uint32_t remote_id);
bool adb_sync_owns_stream(uint32_t local_id);

/* Abort the active session (USB disconnect / stream resync). Safe from any context. */
void adb_sync_reset(void);
#else
#include <zephyr/kernel.h>
#include "adb.h"
/* Without the sync service no stream is ever owned, so only OPEN needs a reply. */
extern int adb_send_packet_sync(uint32_t cmd, uint32_t arg0, uint32_t arg1, const void *payload,
				size_t len, k_timeout_t timeout);
static inline void adb_sync_handle_open(uint32_t remote_id)
{
	adb_send_packet_sync(ADB_CMD_CLSE, 0, remote_id, NULL, 0, K_NO_WAIT);
}
static inline void adb_sync_handle_wrte(uint32_t l, uint32_t r, const uint8_t *d, size_t n) {}
static inline void adb_sync_handle_clse(uint32_t l, uint32_t r) {}
static inline void adb_sync_handle_okay(uint32_t l, uint32_t r) {}
static inline bool adb_sync_owns_stream(uint32_t l) { return false; }
static inline void adb_sync_reset(void) {}
#endif /* CONFIG_ADB_SYNC */

#endif
