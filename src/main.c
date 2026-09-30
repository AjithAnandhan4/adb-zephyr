/*
 * ADB-Zephyr: Minimal ADB CNXN Handshake
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_adb.h>
#include <zephyr/drivers/usb/usb_buf.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>

#include "adb.h"
#include "adb_shell.h"
#include "adb_sync.h"

LOG_MODULE_REGISTER(adb_main, LOG_LEVEL_DBG);

/* ── USB device context ── */
USBD_DEVICE_DEFINE(adb_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), 0x2FE3, 0xADB1);
USBD_DESC_LANG_DEFINE(adb_lang);
USBD_DESC_MANUFACTURER_DEFINE(adb_mfr, "Zephyr Project");
USBD_DESC_PRODUCT_DEFINE(adb_product, "Zephyr ADB");
USBD_DESC_CONFIG_DEFINE(adb_fs_cfg_desc, "FS Configuration");
USBD_DESC_CONFIG_DEFINE(adb_hs_cfg_desc, "HS Configuration");
USBD_CONFIGURATION_DEFINE(adb_fs_config, USB_SCD_SELF_POWERED, 250, &adb_fs_cfg_desc);
USBD_CONFIGURATION_DEFINE(adb_hs_config, USB_SCD_SELF_POWERED, 250, &adb_hs_cfg_desc);

/* ── LittleFS on internal flash (storage_partition, 64 KB) ── */
FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(lfs_storage);
static struct fs_mount_t adb_lfs_mount = {
	.type       = FS_LITTLEFS,
	.fs_data    = &lfs_storage,
	.storage_dev = (void *)PARTITION_ID(storage_partition),
	.mnt_point  = "/lfs",
};

/* ── ADB State ── */
enum adb_state {
	ADB_DISCONNECTED,
	ADB_WAITING_CNXN,
	ADB_CONNECTED
};
static enum adb_state current_state = ADB_DISCONNECTED;

/* ── RX Stream Parser ── */
#define RX_RING_SIZE 8192
RING_BUF_DECLARE(rx_ring, RX_RING_SIZE);

#define RX_STREAM_MAX (ADB_HEADER_SIZE + ADB_MAX_PAYLOAD)
static uint8_t rx_stream[RX_STREAM_MAX];
static size_t rx_len = 0;

/* ── TX Buffer Pool ── */
#define TX_BUF_COUNT 2
USB_STATIC_BUF_DEFINE(tx_buf_0, RX_STREAM_MAX);
USB_STATIC_BUF_DEFINE(tx_buf_1, RX_STREAM_MAX);

static uint8_t *const tx_pool[TX_BUF_COUNT] = { tx_buf_0, tx_buf_1 };

USB_STATIC_BUF_DEFINE(tx_payload_0, ADB_MAX_PAYLOAD);
USB_STATIC_BUF_DEFINE(tx_payload_1, ADB_MAX_PAYLOAD);

static uint8_t *const tx_payload_pool[TX_BUF_COUNT] = { tx_payload_0, tx_payload_1 };

/*
 * Per-slot packet storage for adb_send_packet_sync().
 * NEVER put struct adb_packet on the stack — it is 4120 bytes and will
 * overflow the Zephyr main/workqueue stack silently. Each slot is protected
 * by tx_free_q: only the caller that holds idx can access tx_pkt_pool[idx].
 */
static struct adb_packet tx_pkt_pool[TX_BUF_COUNT];

K_MSGQ_DEFINE(tx_free_q, sizeof(uint8_t), TX_BUF_COUNT, 1);
K_MSGQ_DEFINE(tx_pending_q, sizeof(uint8_t), TX_BUF_COUNT, 1);

static ATOMIC_DEFINE(tx_active, 1);
static uint8_t tx_inflight_idx;
static size_t tx_inflight_len[TX_BUF_COUNT];

enum tx_phase {
	TX_PHASE_IDLE,
	TX_PHASE_HEADER,
	TX_PHASE_PAYLOAD
};
static enum tx_phase tx_current_phase = TX_PHASE_IDLE;

static struct adb_packet pkt_tx;
static struct adb_packet pkt_rx;

static void tx_work_fn(struct k_work *work);
K_WORK_DEFINE(tx_work, tx_work_fn);

static void adb_work_fn(struct k_work *work);
K_WORK_DEFINE(adb_work, adb_work_fn);

/* ── Implementation ── */

int adb_send_packet_sync(uint32_t cmd, uint32_t arg0, uint32_t arg1, const void *payload, size_t len, k_timeout_t timeout)
{
	uint8_t idx;
	if (k_msgq_get(&tx_free_q, &idx, timeout) != 0) {
		return -ENOMEM;
	}

	/* Use pre-allocated per-slot storage — never stack-allocate struct adb_packet
	 * (it is 4120 bytes and will silently overflow any Zephyr thread stack). */
	struct adb_packet *pkt = &tx_pkt_pool[idx];
	pkt->command = cmd;
	pkt->arg0 = arg0;
	pkt->arg1 = arg1;
	pkt->length = len;
	
	if (len > 0 && payload != NULL) {
		memcpy(pkt->payload, payload, len);
	}

	adb_packet_finalize(pkt);

	size_t encoded_len;
	if (adb_encode(pkt, tx_pool[idx], RX_STREAM_MAX, &encoded_len) != 0) {
		k_msgq_put(&tx_free_q, &idx, K_NO_WAIT);
		return -EINVAL;
	}
	if (pkt->length > 0) {
		memcpy(tx_payload_pool[idx], tx_pool[idx] + ADB_HEADER_SIZE, pkt->length);
	}

	tx_inflight_len[idx] = encoded_len;
	k_msgq_put(&tx_pending_q, &idx, K_NO_WAIT);
	k_work_submit(&tx_work);

	return 0;
}

static void handle_cnxn(struct adb_packet *rx)
{
	uint8_t idx;

	LOG_INF("Received CNXN: version=0x%08X maxdata=%u banner='%.*s'",
		rx->arg0, rx->arg1, rx->length, rx->payload);

	current_state = ADB_CONNECTED;

	if (k_msgq_get(&tx_free_q, &idx, K_NO_WAIT) != 0) {
		LOG_ERR("No free TX buffer for CNXN reply");
		return;
	}

	pkt_tx.command = ADB_CMD_CNXN;
	pkt_tx.arg0 = ADB_VERSION;
	pkt_tx.arg1 = ADB_MAX_PAYLOAD;

	const char *banner = "device::ro.product.name=Zephyr;ro.product.model=FRDM-MCXN236;ro.product.device=mcxn236;";
	pkt_tx.length = strlen(banner) + 1;
	memcpy(pkt_tx.payload, banner, pkt_tx.length);

	adb_packet_finalize(&pkt_tx);

	size_t encoded_len;
	if (adb_encode(&pkt_tx, tx_pool[idx], RX_STREAM_MAX, &encoded_len) != 0) {
		LOG_ERR("Failed to encode CNXN reply");
		k_msgq_put(&tx_free_q, &idx, K_NO_WAIT);
		return;
	}

	tx_inflight_len[idx] = encoded_len;
	k_msgq_put(&tx_pending_q, &idx, K_NO_WAIT);
	k_work_submit(&tx_work);
}

static void handle_packet(struct adb_packet *rx)
{
	switch (rx->command) {
	case ADB_CMD_CNXN:
		handle_cnxn(rx);
		break;
	case ADB_CMD_OPEN: {
		LOG_INF("Received OPEN request. Length=%u", rx->length);
		if (rx->length > 0 && rx->length <= sizeof(rx->payload)) {
			rx->payload[rx->length - 1] = 0;
		}
		if (strncmp((const char *)rx->payload, "sync:", 5) == 0) {
			adb_sync_handle_open(rx->arg0);
		} else {
			adb_shell_handle_open(rx->arg0, (const char *)rx->payload);
		}
		break;
	}
	/* Stream packets carry our local id in arg1; route them to its owner. */
	case ADB_CMD_WRTE:
		LOG_DBG("Received WRTE: local=%u remote=%u len=%u", rx->arg1, rx->arg0, rx->length);
		if (adb_sync_owns_stream(rx->arg1)) {
			adb_sync_handle_wrte(rx->arg1, rx->arg0, rx->payload, rx->length);
		} else {
			adb_shell_handle_wrte(rx->arg1, rx->arg0, rx->payload, rx->length);
		}
		break;
	case ADB_CMD_OKAY:
		LOG_DBG("Received OKAY: local=%u remote=%u", rx->arg1, rx->arg0);
		if (adb_sync_owns_stream(rx->arg1)) {
			adb_sync_handle_okay(rx->arg1, rx->arg0);
		} else {
			adb_shell_handle_okay(rx->arg1, rx->arg0);
		}
		break;
	case ADB_CMD_CLSE:
		LOG_INF("Received CLSE: local=%u remote=%u", rx->arg1, rx->arg0);
		if (adb_sync_owns_stream(rx->arg1)) {
			adb_sync_handle_clse(rx->arg1, rx->arg0);
		} else {
			adb_shell_handle_clse(rx->arg1, rx->arg0);
		}
		break;
	default:
		LOG_WRN("Unhandled ADB command: 0x%08X", rx->command);
		break;
	}
}

static void adb_work_fn(struct k_work *work)
{
	uint32_t to_read = RX_STREAM_MAX - rx_len;
	if (to_read > 0) {
		uint32_t read = ring_buf_get(&rx_ring, &rx_stream[rx_len], to_read);
		rx_len += read;
	}

	while (rx_len >= ADB_HEADER_SIZE) {
		uint32_t payload_len = (uint32_t)rx_stream[12] | ((uint32_t)rx_stream[13] << 8) |
				       ((uint32_t)rx_stream[14] << 16) | ((uint32_t)rx_stream[15] << 24);

		if (payload_len > ADB_MAX_PAYLOAD) {
			LOG_ERR("Payload len %u exceeds max %u. Dropping stream.", payload_len, ADB_MAX_PAYLOAD);
			rx_len = 0;
			ring_buf_reset(&rx_ring);
			current_state = ADB_WAITING_CNXN;
			adb_sync_reset();
			adb_shell_init();
			return;
		}

		uint32_t total_len = ADB_HEADER_SIZE + payload_len;
		if (rx_len < total_len) {
			break; /* Need more data */
		}

		int rc = adb_decode(&pkt_rx, rx_stream, total_len);
		if (rc == 0) {
			handle_packet(&pkt_rx);
		} else {
			LOG_ERR("ADB decode failure: err=%d, rx_len=%u, total_len=%u. Dropping stream.", rc, rx_len, total_len);
			rx_len = 0;
			ring_buf_reset(&rx_ring);
			current_state = ADB_WAITING_CNXN;
			adb_sync_reset();
			adb_shell_init();
			return;
		}

		rx_len -= total_len;
		if (rx_len > 0) {
			memmove(rx_stream, rx_stream + total_len, rx_len);
		}
	}
}

static void tx_work_fn(struct k_work *work)
{
	uint8_t idx;
	int ret;

	if (atomic_test_and_set_bit(tx_active, 0)) {
		return;
	}

	if (tx_current_phase == TX_PHASE_PAYLOAD) {
		idx = tx_inflight_idx;
		size_t payload_len = tx_inflight_len[idx] - ADB_HEADER_SIZE;
		LOG_DBG("TX payload: idx=%u len=%zu", idx, payload_len);
		ret = usbd_adb_write(tx_payload_pool[idx], payload_len);
		if (ret == 0) {
			return; /* sent() will clear flag and resubmit */
		}
		goto error;
	}

	if (k_msgq_get(&tx_pending_q, &idx, K_NO_WAIT) != 0) {
		atomic_clear_bit(tx_active, 0);
	tx_current_phase = TX_PHASE_IDLE;
		return;
	}

	tx_inflight_idx = idx;
	tx_current_phase = TX_PHASE_HEADER;

	LOG_DBG("TX start: idx=%u cmd=0x%08X len=%u", idx,
		sys_get_le32(tx_pool[idx] + 0), sys_get_le32(tx_pool[idx] + 12));

	ret = usbd_adb_write(tx_pool[idx], ADB_HEADER_SIZE);
	if (ret == 0) {
		return; /* sent() will clear flag and resubmit */
	}

error:
	LOG_WRN("TX write error %d", ret);
	k_msgq_put(&tx_free_q, &tx_inflight_idx, K_NO_WAIT);
	tx_current_phase = TX_PHASE_IDLE;
	atomic_clear_bit(tx_active, 0);
	k_work_submit(&tx_work);
}

/* ── USB Callbacks ── */

static void adb_connected_cb(struct usbd_class_data *c_data)
{
	LOG_INF("USB connected");
	current_state = ADB_WAITING_CNXN;
	adb_shell_init();
}

static void adb_disconnected_cb(struct usbd_class_data *c_data)
{
	uint8_t idx;

	LOG_INF("USB disconnected callback triggered!");
	current_state = ADB_DISCONNECTED;
	adb_sync_reset();

	atomic_clear_bit(tx_active, 0);
	tx_current_phase = TX_PHASE_IDLE;

	ring_buf_reset(&rx_ring);
	rx_len = 0;

	while (k_msgq_get(&tx_pending_q, &idx, K_NO_WAIT) == 0) {
		k_msgq_put(&tx_free_q, &idx, K_NO_WAIT);
	}
}

static void adb_received_cb(struct usbd_class_data *c_data, const uint8_t *data, size_t len)
{
	if (len > 0) {
		uint32_t written = ring_buf_put(&rx_ring, data, len);
		if (written < len) {
			LOG_WRN("RX ring buffer overflow");
		}
		k_work_submit(&adb_work);
	}
}

static void adb_sent_cb(struct usbd_class_data *c_data, const uint8_t *buf, size_t len, int err)
{
	const uint8_t *expected_buf = (tx_current_phase == TX_PHASE_HEADER) ?
				      tx_pool[tx_inflight_idx] :
				      tx_payload_pool[tx_inflight_idx];

	if (buf != expected_buf) {
		LOG_ERR("sent() buf mismatch: expected %p, got %p", expected_buf, buf);
		k_msgq_put(&tx_free_q, &tx_inflight_idx, K_NO_WAIT);
		tx_current_phase = TX_PHASE_IDLE;
		atomic_clear_bit(tx_active, 0);
		k_work_submit(&tx_work);
		return;
	}

	if (err != 0) {
		LOG_WRN("TX aborted: err=%d idx=%u phase=%d", err, tx_inflight_idx, tx_current_phase);
		k_msgq_put(&tx_free_q, &tx_inflight_idx, K_NO_WAIT);
		tx_current_phase = TX_PHASE_IDLE;
	} else {
		if (tx_current_phase == TX_PHASE_HEADER) {
			if (tx_inflight_len[tx_inflight_idx] > ADB_HEADER_SIZE) {
				tx_current_phase = TX_PHASE_PAYLOAD;
				/* Do not free buffer, payload is next */
			} else {
				k_msgq_put(&tx_free_q, &tx_inflight_idx, K_NO_WAIT);
				tx_current_phase = TX_PHASE_IDLE;
			}
		} else if (tx_current_phase == TX_PHASE_PAYLOAD) {
			LOG_DBG("TX complete: idx=%u", tx_inflight_idx);
			k_msgq_put(&tx_free_q, &tx_inflight_idx, K_NO_WAIT);
			tx_current_phase = TX_PHASE_IDLE;
		}
	}

	atomic_clear_bit(tx_active, 0);
	k_work_submit(&tx_work);
}

static const struct usbd_adb_ops adb_ops = {
	.connected    = adb_connected_cb,
	.disconnected = adb_disconnected_cb,
	.received     = adb_received_cb,
	.sent         = adb_sent_cb,
};

static void usbd_msg_cb(struct usbd_context *const ctx, const struct usbd_msg *msg)
{
	if (usbd_can_detect_vbus(ctx)) {
		if (msg->type == USBD_MSG_VBUS_READY) {
			usbd_enable(ctx);
		}
		if (msg->type == USBD_MSG_VBUS_REMOVED) {
			usbd_disable(ctx);
		}
	}
}

static int usb_init(void)
{
	struct usbd_context *ctx = &adb_usbd;

	usbd_add_descriptor(ctx, &adb_lang);
	usbd_add_descriptor(ctx, &adb_mfr);
	usbd_add_descriptor(ctx, &adb_product);

	usbd_adb_register(&adb_ops);

	usbd_add_configuration(ctx, USBD_SPEED_FS, &adb_fs_config);
	usbd_register_class(ctx, "adb_0", USBD_SPEED_FS, 1);

	if (usbd_caps_speed(ctx) == USBD_SPEED_HS) {
		usbd_add_configuration(ctx, USBD_SPEED_HS, &adb_hs_config);
		usbd_register_class(ctx, "adb_0", USBD_SPEED_HS, 1);
	}

	usbd_device_set_code_triple(ctx, USBD_SPEED_FS, 0, 0, 0);
	if (usbd_caps_speed(ctx) == USBD_SPEED_HS) {
		usbd_device_set_code_triple(ctx, USBD_SPEED_HS, 0, 0, 0);
	}

	usbd_init(ctx);
	usbd_msg_register_cb(ctx, usbd_msg_cb);

	/* Always enable USB. With VBUS detection, the normal path waits for a
	 * rising-edge interrupt on the VBUS pin. But after a soft reboot
	 * (e.g. west flash), the cable is already plugged in so no edge fires
	 * and the device never shows up on the host. Calling usbd_enable()
	 * unconditionally avoids this miss. */
	usbd_enable(ctx);

	return 0;
}

int main(void)
{
	for (uint8_t i = 0; i < TX_BUF_COUNT; i++) {
		k_msgq_put(&tx_free_q, &i, K_NO_WAIT);
	}

	printk("=== ADB-Zephyr Minimal CNXN ===\n");

	/* Mount LittleFS — auto-formats on first boot if flash is blank */
	int rc = fs_mount(&adb_lfs_mount);
	if (rc < 0) {
		printk("LittleFS mount failed (%d) — filesystem unavailable\n", rc);
	} else {
		printk("LittleFS mounted at /lfs (64 KB)\n");
	}

	if (usb_init()) {
		printk("USB init failed\n");
		return -1;
	}

	printk("USB ready. Waiting for host to send CNXN...\n");
	
	while (true) {
		k_sleep(K_SECONDS(5));
	}
	return 0;
}
