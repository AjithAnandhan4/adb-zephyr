/*
 * ADB sync service ("sync:") — adb push / pull / ls over the Zephyr VFS.
 *
 * The sync protocol is a byte stream carried inside ADB WRTE packets:
 * a request may span several WRTEs and one WRTE may carry several
 * requests (adb packs SEND+DATA+DONE for small files into one write), so
 * the parser reads from the stream rather than per packet.
 *
 * The protocol runs on its own thread because it must block: waiting for
 * the host's OKAY between our WRTEs and on flash I/O. The ADB RX work
 * item only copies the WRTE payload into sync_rx_buf and wakes the
 * thread. The thread OKAYs a WRTE only once it has fully consumed it,
 * which is the flow control that stops the host outrunning flash writes.
 *
 * Only one session is served at a time (the typical adb push/pull opens
 * exactly one), which keeps the RAM cost to two ADB_MAX_PAYLOAD buffers.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <string.h>

#include "adb.h"
#include "adb_sync.h"

LOG_MODULE_REGISTER(adb_sync, LOG_LEVEL_INF);

extern int adb_send_packet_sync(uint32_t cmd, uint32_t arg0, uint32_t arg1, const void *payload, size_t len, k_timeout_t timeout);

#define SYNC_LOCAL_ID_BASE 0x53590000u /* 'SY' + sequence */
#define SYNC_MAX_PATH      256
#define SYNC_HDR_SIZE      8           /* id + length */
#define SYNC_DATA_MAX      (ADB_MAX_PAYLOAD - SYNC_HDR_SIZE)

/* st_mode bits as the adb host expects them */
#define SYNC_S_IFDIR 0040000u
#define SYNC_S_IFREG 0100000u

enum {
	SYNC_ACTIVE,  /* session allocated (OPEN accepted, thread not yet done) */
	SYNC_RX_BUSY, /* sync_rx_buf holds a WRTE the thread has not drained */
	SYNC_ABORT,   /* host closed the stream or the link went down */
};

static atomic_t sync_flags;
static uint32_t sync_local_id;
static uint32_t sync_remote_id;
static uint16_t sync_id_seq;

static K_SEM_DEFINE(sync_open_sem, 0, 1);
static K_SEM_DEFINE(sync_rx_sem, 0, 1);
static K_SEM_DEFINE(sync_okay_sem, 0, 1);

static uint8_t sync_rx_buf[ADB_MAX_PAYLOAD];
static size_t sync_rx_len;
static size_t sync_rx_pos;
static uint8_t sync_tx_buf[ADB_MAX_PAYLOAD];

/* Thread-owned scratch; static to keep the thread stack small. */
static char sync_path[SYNC_MAX_PATH];
static struct fs_file_t sync_file;
static struct fs_dir_t sync_dir;
static struct fs_dirent sync_dirent;

static bool sync_aborted(void)
{
	return atomic_test_bit(&sync_flags, SYNC_ABORT);
}

static void sync_abort(void)
{
	atomic_set_bit(&sync_flags, SYNC_ABORT);
	k_sem_give(&sync_rx_sem);
	k_sem_give(&sync_okay_sem);
}

/* ── Stream input (thread side) ── */

/* Block until at least one unread byte of the current WRTE is available. */
static int sync_rx_wait(void)
{
	while (!sync_aborted()) {
		if (atomic_test_bit(&sync_flags, SYNC_RX_BUSY) && sync_rx_pos < sync_rx_len) {
			return 0;
		}
		k_sem_take(&sync_rx_sem, K_FOREVER);
	}
	return -ECONNRESET;
}

/* Mark n bytes consumed. A drained WRTE is OKAYed so the host sends the next. */
static void sync_rx_consume(size_t n)
{
	sync_rx_pos += n;
	if (sync_rx_pos == sync_rx_len) {
		atomic_clear_bit(&sync_flags, SYNC_RX_BUSY);
		if (sync_aborted()) {
			return;
		}
		adb_send_packet_sync(ADB_CMD_OKAY, sync_local_id, sync_remote_id, NULL, 0, K_SECONDS(2));
	}
}

static int sync_read(void *dst, size_t len)
{
	uint8_t *out = dst;

	while (len > 0) {
		int rc = sync_rx_wait();

		if (rc) {
			return rc;
		}
		size_t n = MIN(len, sync_rx_len - sync_rx_pos);

		memcpy(out, &sync_rx_buf[sync_rx_pos], n);
		out += n;
		len -= n;
		sync_rx_consume(n);
	}
	return 0;
}

/*
 * Stream len bytes into file, or discard them when file is NULL. A write
 * error is latched in *write_err and the rest is still drained, so the
 * stream stays in sync; the return value is only for link errors.
 */
static int sync_read_to_file(struct fs_file_t *file, size_t len, int *write_err)
{
	while (len > 0) {
		int rc = sync_rx_wait();

		if (rc) {
			return rc;
		}
		size_t n = MIN(len, sync_rx_len - sync_rx_pos);

		if (file != NULL && *write_err == 0) {
			ssize_t wr = fs_write(file, &sync_rx_buf[sync_rx_pos], n);

			if (wr < 0) {
				*write_err = (int)wr;
			} else if ((size_t)wr != n) {
				*write_err = -ENOSPC;
			}
		}
		len -= n;
		sync_rx_consume(n);
	}
	return 0;
}

/* Read a request path into sync_path. Returns -ENAMETOOLONG (stream still in sync) or a link error. */
static int sync_read_path(uint32_t len)
{
	int unused = 0;

	if (len >= SYNC_MAX_PATH) {
		int rc = sync_read_to_file(NULL, len, &unused);

		return rc ? rc : -ENAMETOOLONG;
	}

	int rc = sync_read(sync_path, len);

	if (rc) {
		return rc;
	}
	sync_path[len] = '\0';

	/* The VFS does not accept trailing slashes ("/lfs/" from adb push). */
	while (len > 1 && sync_path[len - 1] == '/') {
		sync_path[--len] = '\0';
	}
	return 0;
}

/* ── Stream output (thread side) ── */

/* Send sync_tx_buf[0..len) as one WRTE, after the host OKAYed our previous one. */
static int sync_send(size_t len)
{
	if (k_sem_take(&sync_okay_sem, K_SECONDS(5)) != 0) {
		LOG_WRN("Timed out waiting for OKAY from host");
		return -ETIMEDOUT;
	}
	if (sync_aborted()) {
		return -ECONNRESET;
	}
	return adb_send_packet_sync(ADB_CMD_WRTE, sync_local_id, sync_remote_id, sync_tx_buf, len, K_SECONDS(2));
}

/* id + length-prefixed message: OKAY, FAIL, and the DONE that ends a RECV. */
static int sync_send_status(uint32_t id, const char *msg)
{
	size_t len = (msg != NULL) ? MIN(strlen(msg), SYNC_DATA_MAX) : 0;

	sys_put_le32(id, sync_tx_buf);
	sys_put_le32(len, sync_tx_buf + 4);
	if (len > 0) {
		memcpy(sync_tx_buf + SYNC_HDR_SIZE, msg, len);
	}
	return sync_send(SYNC_HDR_SIZE + len);
}

static uint32_t sync_mode(const struct fs_dirent *entry)
{
	return (entry->type == FS_DIR_ENTRY_DIR) ? (SYNC_S_IFDIR | 0755) : (SYNC_S_IFREG | 0644);
}

/* ── Requests ── */

/* STAT: reply { id, mode, size, mtime }; all zero means "does not exist". */
static int sync_do_stat(uint32_t path_len)
{
	uint32_t mode = 0;
	uint32_t size = 0;
	int rc = sync_read_path(path_len);

	if (rc && rc != -ENAMETOOLONG) {
		return rc;
	}
	if (rc == 0) {
		if (strcmp(sync_path, "/") == 0) {
			mode = SYNC_S_IFDIR | 0755;
		} else if (fs_stat(sync_path, &sync_dirent) == 0) {
			mode = sync_mode(&sync_dirent);
			size = sync_dirent.size;
		}
	}
	LOG_INF("STAT %s -> mode %o size %u", sync_path, mode, size);

	sys_put_le32(SYNC_STAT, sync_tx_buf);
	sys_put_le32(mode, sync_tx_buf + 4);
	sys_put_le32(size, sync_tx_buf + 8);
	sys_put_le32(0, sync_tx_buf + 12); /* LittleFS keeps no mtime */
	return sync_send(16);
}

/* LIST: one { DENT, mode, size, mtime, namelen, name } per entry, then a DONE of the same shape. */
static int sync_do_list(uint32_t path_len)
{
	int rc = sync_read_path(path_len);

	if (rc && rc != -ENAMETOOLONG) {
		return rc;
	}
	LOG_INF("LIST %s", sync_path);

	fs_dir_t_init(&sync_dir);
	if (rc == 0 && fs_opendir(&sync_dir, sync_path) == 0) {
		while (fs_readdir(&sync_dir, &sync_dirent) == 0 && sync_dirent.name[0] != '\0') {
			size_t namelen = strlen(sync_dirent.name);

			sys_put_le32(SYNC_DENT, sync_tx_buf);
			sys_put_le32(sync_mode(&sync_dirent), sync_tx_buf + 4);
			sys_put_le32(sync_dirent.size, sync_tx_buf + 8);
			sys_put_le32(0, sync_tx_buf + 12);
			sys_put_le32(namelen, sync_tx_buf + 16);
			memcpy(sync_tx_buf + 20, sync_dirent.name, namelen);

			rc = sync_send(20 + namelen);
			if (rc) {
				fs_closedir(&sync_dir);
				return rc;
			}
		}
		fs_closedir(&sync_dir);
	}

	memset(sync_tx_buf, 0, 20);
	sys_put_le32(SYNC_DONE, sync_tx_buf);
	return sync_send(20);
}

/* RECV (pull): stream DATA chunks, then DONE, or FAIL. */
static int sync_do_recv(uint32_t path_len)
{
	int rc = sync_read_path(path_len);

	if (rc == -ENAMETOOLONG) {
		return sync_send_status(SYNC_FAIL, "path too long");
	}
	if (rc) {
		return rc;
	}
	LOG_INF("RECV %s", sync_path);

	fs_file_t_init(&sync_file);
	rc = fs_open(&sync_file, sync_path, FS_O_READ);
	if (rc) {
		return sync_send_status(SYNC_FAIL, (rc == -ENOENT) ? "No such file or directory" : "open failed");
	}

	for (;;) {
		ssize_t n = fs_read(&sync_file, sync_tx_buf + SYNC_HDR_SIZE, SYNC_DATA_MAX);

		if (n < 0) {
			fs_close(&sync_file);
			return sync_send_status(SYNC_FAIL, "read error");
		}
		if (n == 0) {
			break;
		}
		sys_put_le32(SYNC_DATA, sync_tx_buf);
		sys_put_le32((uint32_t)n, sync_tx_buf + 4);
		rc = sync_send(SYNC_HDR_SIZE + n);
		if (rc) {
			fs_close(&sync_file);
			return rc;
		}
	}
	fs_close(&sync_file);
	return sync_send_status(SYNC_DONE, NULL);
}

/* Create missing parent directories of path (like adbd does for push). */
static void sync_mkdirs(char *path)
{
	for (char *p = strchr(path + 1, '/'); p != NULL; p = strchr(p + 1, '/')) {
		*p = '\0';
		if (fs_stat(path, &sync_dirent) == -ENOENT) {
			fs_mkdir(path);
		}
		*p = '/';
	}
}

/* SEND (push): request is "path,mode", followed by DATA chunks and a DONE carrying mtime. */
static int sync_do_send(uint32_t spec_len)
{
	const char *err = NULL;
	bool file_open = false;
	int write_err = 0;
	uint8_t hdr[SYNC_HDR_SIZE];
	int rc = sync_read_path(spec_len);

	if (rc && rc != -ENAMETOOLONG) {
		return rc;
	}
	if (rc == -ENAMETOOLONG) {
		err = "path too long";
	} else {
		char *comma = strrchr(sync_path, ',');

		if (comma != NULL) {
			*comma = '\0';
		}
		LOG_INF("SEND %s", sync_path);

		sync_mkdirs(sync_path);
		fs_file_t_init(&sync_file);
		rc = fs_open(&sync_file, sync_path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
		if (rc) {
			LOG_WRN("Cannot create %s (%d)", sync_path, rc);
			err = "cannot create file";
		} else {
			file_open = true;
		}
	}

	/* Drain to DONE even on error: the host sends the whole file before reading our reply. */
	for (;;) {
		rc = sync_read(hdr, sizeof(hdr));
		if (rc) {
			goto out;
		}
		uint32_t id = sys_get_le32(hdr);
		uint32_t arg = sys_get_le32(hdr + 4);

		if (id == SYNC_DONE) {
			break;
		}
		if (id != SYNC_DATA) {
			LOG_ERR("Unexpected sync id 0x%08x during SEND", id);
			rc = -EPROTO;
			goto out;
		}
		rc = sync_read_to_file(file_open ? &sync_file : NULL, arg, &write_err);
		if (rc) {
			goto out;
		}
	}

	if (file_open) {
		int close_rc = fs_close(&sync_file);

		file_open = false;
		if (write_err == 0) {
			write_err = close_rc;
		}
		if (write_err) {
			LOG_WRN("Write to %s failed (%d)", sync_path, write_err);
			fs_unlink(sync_path);
			err = (write_err == -ENOSPC) ? "No space left on device" : "write error";
		}
	}
	return err ? sync_send_status(SYNC_FAIL, err) : sync_send_status(SYNC_OKAY, NULL);

out:
	if (file_open) {
		fs_close(&sync_file);
		fs_unlink(sync_path);
	}
	return rc;
}

/* Serve requests until QUIT (returns 0) or an error that ends the session. */
static int sync_serve(void)
{
	uint8_t hdr[SYNC_HDR_SIZE];

	for (;;) {
		int rc = sync_read(hdr, sizeof(hdr));

		if (rc) {
			return rc;
		}
		uint32_t id = sys_get_le32(hdr);
		uint32_t len = sys_get_le32(hdr + 4);

		switch (id) {
		case SYNC_STAT:
			rc = sync_do_stat(len);
			break;
		case SYNC_LIST:
			rc = sync_do_list(len);
			break;
		case SYNC_RECV:
			rc = sync_do_recv(len);
			break;
		case SYNC_SEND:
			rc = sync_do_send(len);
			break;
		case SYNC_QUIT:
			return 0;
		default:
			/* v2 requests are only sent if we advertise stat_v2/sendrecv_v2. */
			LOG_WRN("Unsupported sync request 0x%08x", id);
			sync_send_status(SYNC_FAIL, "unsupported sync request");
			return -EPROTO;
		}
		if (rc) {
			return rc;
		}
	}
}

static void sync_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		k_sem_take(&sync_open_sem, K_FOREVER);

		int rc = sync_serve();

		LOG_INF("Sync session 0x%08x ended (%d)", sync_local_id, rc);
		if (!sync_aborted()) {
			adb_send_packet_sync(ADB_CMD_CLSE, sync_local_id, sync_remote_id, NULL, 0, K_SECONDS(2));
		}
		atomic_clear(&sync_flags);
	}
}

K_THREAD_DEFINE(adb_sync_tid, CONFIG_ADB_SYNC_STACK_SIZE, sync_thread_fn, NULL, NULL, NULL,
		CONFIG_ADB_SYNC_THREAD_PRIORITY, 0, 0);

/* ── ADB stream hooks (RX work item context; must not block) ── */

void adb_sync_handle_open(uint32_t remote_id)
{
	if (atomic_test_and_set_bit(&sync_flags, SYNC_ACTIVE)) {
		LOG_WRN("Sync busy, rejecting stream from host %u", remote_id);
		adb_send_packet_sync(ADB_CMD_CLSE, 0, remote_id, NULL, 0, K_NO_WAIT);
		return;
	}

	/* Fresh local id per session so a late CLSE for the old one can't hit the new one. */
	sync_local_id = SYNC_LOCAL_ID_BASE | sync_id_seq++;
	sync_remote_id = remote_id;
	sync_rx_len = 0;
	sync_rx_pos = 0;
	k_sem_reset(&sync_rx_sem);
	k_sem_reset(&sync_okay_sem);
	k_sem_give(&sync_okay_sem); /* our first WRTE may follow our OKAY directly */

	LOG_INF("Sync session 0x%08x opened for host %u", sync_local_id, remote_id);
	adb_send_packet_sync(ADB_CMD_OKAY, sync_local_id, remote_id, NULL, 0, K_NO_WAIT);
	k_sem_give(&sync_open_sem);
}

void adb_sync_handle_wrte(uint32_t local_id, uint32_t remote_id, const uint8_t *data, size_t len)
{
	ARG_UNUSED(remote_id);

	if (!adb_sync_owns_stream(local_id)) {
		return;
	}
	if (len == 0) {
		adb_send_packet_sync(ADB_CMD_OKAY, sync_local_id, sync_remote_id, NULL, 0, K_NO_WAIT);
		return;
	}
	if (len > sizeof(sync_rx_buf) || atomic_test_bit(&sync_flags, SYNC_RX_BUSY)) {
		/* Host ignored flow control; the stream can't be resynchronised. */
		LOG_ERR("Sync WRTE overrun (len %zu), closing stream", len);
		sync_abort();
		adb_send_packet_sync(ADB_CMD_CLSE, sync_local_id, sync_remote_id, NULL, 0, K_NO_WAIT);
		return;
	}

	/* Fill the buffer before publishing it: the thread only reads while RX_BUSY is set. */
	memcpy(sync_rx_buf, data, len);
	sync_rx_pos = 0;
	sync_rx_len = len;
	atomic_set_bit(&sync_flags, SYNC_RX_BUSY);
	k_sem_give(&sync_rx_sem);
}

void adb_sync_handle_okay(uint32_t local_id, uint32_t remote_id)
{
	ARG_UNUSED(remote_id);

	if (adb_sync_owns_stream(local_id)) {
		k_sem_give(&sync_okay_sem);
	}
}

void adb_sync_handle_clse(uint32_t local_id, uint32_t remote_id)
{
	ARG_UNUSED(remote_id);

	if (adb_sync_owns_stream(local_id)) {
		LOG_INF("Host closed sync session 0x%08x", local_id);
		sync_abort();
	}
}

bool adb_sync_owns_stream(uint32_t local_id)
{
	return atomic_test_bit(&sync_flags, SYNC_ACTIVE) && local_id == sync_local_id &&
	       !sync_aborted();
}

void adb_sync_reset(void)
{
	if (atomic_test_bit(&sync_flags, SYNC_ACTIVE)) {
		sync_abort();
	}
}
