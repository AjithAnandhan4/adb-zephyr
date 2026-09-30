#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/ring_buffer.h>
#include <string.h>

#include "adb.h"
#include "adb_shell.h"

LOG_MODULE_REGISTER(adb_shell, LOG_LEVEL_INF);

/* External function to send ADB packets (defined in main.c) */
extern int adb_send_packet_sync(uint32_t cmd, uint32_t arg0, uint32_t arg1, const void *payload, size_t len, k_timeout_t timeout);

struct adb_shell_session {
	bool in_use;
	uint32_t local_id;
	uint32_t remote_id;
	struct shell *sh;
	struct k_sem okay_sem;
	struct ring_buf rx_ring;
	uint8_t rx_buf[CONFIG_ADB_SHELL_RX_BUF_SIZE];
	shell_transport_handler_t shell_handler;
	void *shell_context;
};

static struct adb_shell_session shell_sessions[CONFIG_ADB_SHELL_COUNT];
static K_MUTEX_DEFINE(shell_lock);

static int adb_shell_transport_init(const struct shell_transport *transport,
				    const void *config,
				    shell_transport_handler_t evt_handler,
				    void *context)
{
	struct adb_shell_session *sess = (struct adb_shell_session *)transport->ctx;

	sess->shell_handler = evt_handler;
	sess->shell_context = context;
	ring_buf_reset(&sess->rx_ring);

	return 0;
}

static int adb_shell_transport_uninit(const struct shell_transport *transport)
{
	return 0;
}

static int adb_shell_transport_enable(const struct shell_transport *transport, bool blocking_tx)
{
	return 0;
}

static int adb_shell_transport_write(const struct shell_transport *transport,
				     const void *data, size_t length, size_t *cnt)
{
	struct adb_shell_session *sess = (struct adb_shell_session *)transport->ctx;

	if (!sess->in_use) {
		*cnt = 0;
		return -ENODEV;
	}

	/* Wait for OKAY from previous WRTE (host must ack before we send more).
	 * Use a bounded timeout: if the host drops OKAY (e.g. disconnected or
	 * CNXN not fully established) K_FOREVER would deadlock the shell thread. */
	if (k_sem_take(&sess->okay_sem, K_SECONDS(5)) != 0) {
		LOG_WRN("Timed out waiting for OKAY from host (sess local=%u)", sess->local_id);
		*cnt = 0;
		return -EIO;
	}

	/* Send WRTE to host. Use K_SECONDS(2) so a full TX queue doesn't
	 * stall the shell thread indefinitely. */
	int ret = adb_send_packet_sync(ADB_CMD_WRTE, sess->local_id, sess->remote_id, data, length, K_SECONDS(2));
	if (ret != 0) {
		LOG_WRN("TX enqueue failure for WRTE: err=%d", ret);
	}
	if (ret != 0) {
		k_sem_give(&sess->okay_sem);
	}
	if (ret != 0) {
		*cnt = 0;
		return ret;
	}

	*cnt = length;
	return 0;
}

static int adb_shell_transport_read(const struct shell_transport *transport,
				    void *data, size_t length, size_t *cnt)
{
	struct adb_shell_session *sess = (struct adb_shell_session *)transport->ctx;

	if (!sess->in_use) {
		*cnt = 0;
		return -ENODEV;
	}

	*cnt = ring_buf_get(&sess->rx_ring, data, length);
	return 0;
}

const struct shell_transport_api adb_shell_transport_api = {
	.init = adb_shell_transport_init,
	.uninit = adb_shell_transport_uninit,
	.enable = adb_shell_transport_enable,
	.write = adb_shell_transport_write,
	.read = adb_shell_transport_read
};

/* Define the shell transports and instances statically */
#define ADB_SHELL_NAME(n, _) &adb_shell_##n

#define ADB_SHELL_DEFINE_INST(n, _) \
	static const struct shell_transport shell_transport_adb_##n = { \
		.api = &adb_shell_transport_api, \
		.ctx = &shell_sessions[n], \
	}; \
	SHELL_DEFINE(adb_shell_##n, "adb:$ ", &shell_transport_adb_##n, 10, 10, SHELL_FLAG_OLF_CRLF);

LISTIFY(CONFIG_ADB_SHELL_COUNT, ADB_SHELL_DEFINE_INST, (;))

static const struct shell *const adb_shell_instances[] = {
	LISTIFY(CONFIG_ADB_SHELL_COUNT, ADB_SHELL_NAME, (,))
};

void adb_shell_init(void)
{
	for (size_t i = 0; i < CONFIG_ADB_SHELL_COUNT; i++) {
		shell_sessions[i].in_use = false;
		ring_buf_init(&shell_sessions[i].rx_ring, CONFIG_ADB_SHELL_RX_BUF_SIZE, shell_sessions[i].rx_buf);
		shell_sessions[i].sh = (struct shell *)adb_shell_instances[i];
		k_sem_init(&shell_sessions[i].okay_sem, 1, 1);
	}
}

static void shell_uninit_completed(const struct shell *sh, int res)
{
	k_mutex_lock(&shell_lock, K_FOREVER);
	for (size_t i = 0; i < CONFIG_ADB_SHELL_COUNT; i++) {
		if (shell_sessions[i].sh == sh) {
			shell_sessions[i].in_use = false;
			LOG_INF("Shell slot %zu uninitialized fully.", i);
			break;
		}
	}
	k_mutex_unlock(&shell_lock);
}

void adb_shell_handle_open(uint32_t remote_id, const char *name)
{
	/* For now we only handle "shell:" or "shell:something". */
	LOG_INF("Processing OPEN service string: '%s'", name);
	if (strncmp(name, "shell:", 6) != 0) {
		LOG_WRN("Unsupported open request: %s", name);
		return;
	}

	k_mutex_lock(&shell_lock, K_FOREVER);
	struct adb_shell_session *sess = NULL;
	size_t slot = 0;
	for (size_t i = 0; i < CONFIG_ADB_SHELL_COUNT; i++) {
		if (!shell_sessions[i].in_use) {
			sess = &shell_sessions[i];
			slot = i;
			break;
		}
	}

	if (!sess) {
		k_mutex_unlock(&shell_lock);
		LOG_WRN("Shell session allocation FAILED for host %u (slots full or busy)", remote_id);
		/* Could send CLSE immediately, but host should just timeout if we don't OKAY */
		adb_send_packet_sync(ADB_CMD_CLSE, 0, remote_id, NULL, 0, K_NO_WAIT);
		return;
	}

	sess->in_use = true;
	sess->local_id = 0x54590000 | slot; /* 'TY' + slot */
	sess->remote_id = remote_id;
	ring_buf_reset(&sess->rx_ring);
	k_sem_reset(&sess->okay_sem);
	k_sem_give(&sess->okay_sem);
	k_mutex_unlock(&shell_lock);

	static const struct shell_backend_config_flags cfg_flags = SHELL_DEFAULT_BACKEND_CONFIG_FLAGS;
	int ret = shell_init(sess->sh, NULL, cfg_flags, false, LOG_LEVEL_NONE);
	if (ret != 0 && ret != -EINVAL) {
		LOG_ERR("Failed to init shell (err %d)", ret);
		k_mutex_lock(&shell_lock, K_FOREVER);
		sess->in_use = false;
		k_mutex_unlock(&shell_lock);
		adb_send_packet_sync(ADB_CMD_CLSE, sess->local_id, remote_id, NULL, 0, K_NO_WAIT);
		return;
	}
	if (ret == -EINVAL) {
		LOG_INF("Shell already initialized. Starting it...");
		shell_start(sess->sh);
	}

	LOG_INF("Mapped host %u to shell slot %zu (local_id %u), shell_init returned %d", remote_id, slot, sess->local_id, ret);
	adb_send_packet_sync(ADB_CMD_OKAY, sess->local_id, remote_id, NULL, 0, K_NO_WAIT);

	if (strlen(name) > 6) {
		ring_buf_put(&sess->rx_ring, (const uint8_t *)(name + 6), strlen(name + 6));
		ring_buf_put(&sess->rx_ring, (const uint8_t *)"\r", 1);
		if (sess->shell_handler) {
			sess->shell_handler(SHELL_TRANSPORT_EVT_RX_RDY, sess->shell_context);
		}
	}
}

void adb_shell_handle_wrte(uint32_t local_id, uint32_t remote_id, const uint8_t *data, size_t len)
{
	struct adb_shell_session *sess = NULL;

	k_mutex_lock(&shell_lock, K_FOREVER);
	for (size_t i = 0; i < CONFIG_ADB_SHELL_COUNT; i++) {
		if (shell_sessions[i].in_use && shell_sessions[i].local_id == local_id) {
			sess = &shell_sessions[i];
			break;
		}
	}
	k_mutex_unlock(&shell_lock);

	if (!sess) {
		LOG_WRN("WRTE to unknown local_id %u", local_id);
		adb_send_packet_sync(ADB_CMD_CLSE, local_id, remote_id, NULL, 0, K_NO_WAIT);
		return;
	}

	adb_send_packet_sync(ADB_CMD_OKAY, sess->local_id, remote_id, NULL, 0, K_NO_WAIT);

	if (len > 0) {
		uint32_t wrote = ring_buf_put(&sess->rx_ring, data, len);
		if (wrote < len) {
			LOG_WRN("Shell RX ring buffer full! Dropped %u bytes.", len - wrote);
		}
		if (sess->shell_handler) {
			sess->shell_handler(SHELL_TRANSPORT_EVT_RX_RDY, sess->shell_context);
		}
	}
}

void adb_shell_handle_clse(uint32_t local_id, uint32_t remote_id)
{
	struct adb_shell_session *sess = NULL;

	k_mutex_lock(&shell_lock, K_FOREVER);
	for (size_t i = 0; i < CONFIG_ADB_SHELL_COUNT; i++) {
		if (shell_sessions[i].in_use && shell_sessions[i].local_id == local_id) {
			sess = &shell_sessions[i];
			break;
		}
	}
	k_mutex_unlock(&shell_lock);

	if (!sess) {
		return;
	}

	LOG_INF("Closing shell session (local %u, remote %u)", local_id, remote_id);
	
	/* Acknowledge CLSE if it came from host. If it's internal, the other side handles it. 
	 * ADB protocol typically says just send CLSE back. */
	adb_send_packet_sync(ADB_CMD_CLSE, sess->local_id, remote_id, NULL, 0, K_NO_WAIT);

	/* uninit asynchronously; completion callback clears in_use */
	shell_uninit(sess->sh, shell_uninit_completed);
}

void adb_shell_handle_okay(uint32_t local_id, uint32_t remote_id)
{
	struct adb_shell_session *sess = NULL;

	k_mutex_lock(&shell_lock, K_FOREVER);
	for (size_t i = 0; i < CONFIG_ADB_SHELL_COUNT; i++) {
		if (shell_sessions[i].in_use && shell_sessions[i].local_id == local_id) {
			sess = &shell_sessions[i];
			break;
		}
	}
	k_mutex_unlock(&shell_lock);

	if (sess) {
		k_sem_give(&sess->okay_sem);
	}
}

void adb_shell_reset(void)
{
	k_mutex_lock(&shell_lock, K_FOREVER);
	for (size_t i = 0; i < CONFIG_ADB_SHELL_COUNT; i++) {
		if (shell_sessions[i].in_use) {
			LOG_INF("Resetting shell session %zu due to host reconnect/disconnect", i);
			/* Unblock any shell thread blocked in write() waiting for OKAY. */
			k_sem_give(&shell_sessions[i].okay_sem);
			/*
			 * Force-clear in_use now so the next OPEN can claim this slot
			 * immediately on reconnect. shell_uninit() is async — without
			 * clearing here, shell_uninit_completed() may not have fired by
			 * the time the host re-sends OPEN, leaving all slots occupied.
			 */
			shell_sessions[i].in_use = false;
			ring_buf_reset(&shell_sessions[i].rx_ring);
			shell_uninit(shell_sessions[i].sh, NULL);
		}
	}
	k_mutex_unlock(&shell_lock);
}
