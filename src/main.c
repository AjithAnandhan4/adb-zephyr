/*
 * ADB-Zephyr: raw USB bulk loopback test for FRDM-MCXN236.
 *
 * MILESTONE: USB transport validation (no ADB protocol).
 *
 * What this does:
 *   Any bytes received from the host on EP1 OUT are echoed back byte-for-byte
 *   on EP1 IN. Received data is logged to J10 (MCU-Link UART console).
 *
 * Buffer ownership and lifetime
 * ─────────────────────────────
 * RX buffer (UDC pool):
 *   buf->data is a DMA pointer owned by the UDC subsystem. It is valid ONLY
 *   during the received() callback. usbd_adb.c frees it and re-arms the OUT
 *   endpoint the moment received() returns. We MUST copy before returning.
 *
 * TX buffer (our echo pool):
 *   usbd_adb_write() takes a zero-copy reference — the UDC DMA reads directly
 *   from the caller's buffer. The buffer must not be modified or freed until
 *   sent() fires. If usbd_adb_write() returns any non-zero code, sent() will
 *   NOT fire; the caller must release the buffer immediately.
 *
 * Buffer state machine (invariant: each buffer is in exactly one state)
 * ─────────────────────────────────────────────────────────────────────
 *   FREE → (received picks it) → COPYING → QUEUED → TX_IN_FLIGHT → FREE
 *
 *   FREE:          index is in lb_free_q
 *   COPYING:       memcpy() inside received() callback (transient)
 *   QUEUED:        index is in lb_echo_q, waiting for echo_work_fn
 *   TX_IN_FLIGHT:  usbd_adb_write() accepted it; DMA in progress
 *                  lb_inflight_idx holds the index; buffer absent from queues
 *   FREE (again):  returned to lb_free_q by sent()
 *
 * Concurrent access:
 *   received(), sent(), disconnected() all run on the USBD thread (single
 *   thread). echo_work_fn runs on the system workqueue thread. Shared state:
 *   - lb_free_q, lb_echo_q: Zephyr msgq (internally thread-safe)
 *   - lb_tx_active:         atomic_t (compare-and-swap)
 *   - lb_inflight_idx:      written by echo_work_fn before usbd_adb_write();
 *                            read by sent() after DMA completes.
 *                            Protected by lb_tx_active: echo_work_fn writes
 *                            it then sets lb_tx_active; sent() reads it only
 *                            after lb_tx_active was set; clears lb_tx_active
 *                            before returning. No explicit mutex needed.
 *
 * Disconnect handling:
 *   If TX is in flight, usbd_adb.c fires sent(-ECONNABORTED) BEFORE calling
 *   disconnected(). sent() returns lb_inflight_idx to free_q and clears
 *   lb_tx_active. Then disconnected() drains lb_echo_q (QUEUED buffers only).
 *   If no TX is in flight, disconnected() just drains lb_echo_q and
 *   defensively clears lb_tx_active.
 *   Result: all LB_BUF_COUNT buffers end up in lb_free_q after disconnect.
 *
 * usbd_adb_write() failure handling:
 *   -EBUSY:  cannot happen in this design (lb_tx_active prevents concurrent
 *            calls to usbd_adb_write). Handled defensively with put_front.
 *   -EPERM:  interface not active (disconnect race). Buffer returned to free_q.
 *   other:   unexpected error. Buffer returned to free_q.
 *   In all non-zero cases, sent() will NOT fire; we free immediately.
 *
 * RAM budget (worst case):
 *   3 × ROUND_UP(4096, 4) = 12,288 bytes (USB_STATIC_BUF_DEFINE)
 *   lb_free_q: 3 × 1 byte elements, small overhead
 *   lb_echo_q: 3 × sizeof(lb_item) elements, small overhead
 *   Total extra: ≈12,320 bytes on top of existing ~15,544 bytes ≈ 14.5% RAM
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_adb.h>
#include <zephyr/drivers/usb/usb_buf.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(adb_main, LOG_LEVEL_DBG);

/* ── USB device context ─────────────────────────────────────────────────── */

USBD_DEVICE_DEFINE(adb_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   0x2FE3,   /* VID: Zephyr development (test only) */
		   0xADB1);  /* PID: ADB loopback test */

USBD_DESC_LANG_DEFINE(adb_lang);
USBD_DESC_MANUFACTURER_DEFINE(adb_mfr, "Zephyr Project");
USBD_DESC_PRODUCT_DEFINE(adb_product, "Zephyr ADB");

USBD_DESC_CONFIG_DEFINE(adb_fs_cfg_desc, "FS Configuration");
USBD_DESC_CONFIG_DEFINE(adb_hs_cfg_desc, "HS Configuration");

USBD_CONFIGURATION_DEFINE(adb_fs_config,
			  USB_SCD_SELF_POWERED,
			  250,
			  &adb_fs_cfg_desc);

USBD_CONFIGURATION_DEFINE(adb_hs_config,
			  USB_SCD_SELF_POWERED,
			  250,
			  &adb_hs_cfg_desc);

/* ── Loopback buffer pool ───────────────────────────────────────────────── */

/*
 * Number of independent DMA-safe echo buffers.
 *
 * At FS bulk (64-byte MPS), one TX DMA completes in ~1 USB frame (1 ms).
 * With one buffer TX_IN_FLIGHT we need at least one more for the next
 * received() copy, plus one extra for scheduling jitter. Three is the
 * minimum safe depth.
 */
#define LB_BUF_COUNT 3

/*
 * USB_STATIC_BUF_DEFINE places each buffer in the correct section with
 * the required alignment:
 *   - With CONFIG_DCACHE_LINE_SIZE: cache-line aligned + nocache section
 *   - Without (this target, MCXN236 LPCAC icache only): sizeof(void*)-aligned
 *
 * USB_BUF_ROUND_UP rounds the size up to the alignment granularity.
 * Each buffer can hold up to CONFIG_USBD_ADB_RX_BUF_SIZE bytes (default 4096).
 * IS_UDC_ALIGNED() in usbd_adb_write() will pass for these buffers.
 */
USB_STATIC_BUF_DEFINE(lb_buf_0, CONFIG_USBD_ADB_RX_BUF_SIZE);
USB_STATIC_BUF_DEFINE(lb_buf_1, CONFIG_USBD_ADB_RX_BUF_SIZE);
USB_STATIC_BUF_DEFINE(lb_buf_2, CONFIG_USBD_ADB_RX_BUF_SIZE);

/* Index-to-pointer table. Const pointers; buffers are writable. */
static uint8_t *const lb_pool[LB_BUF_COUNT] = {
	lb_buf_0,
	lb_buf_1,
	lb_buf_2,
};

/*
 * Free queue: holds uint8_t slot indices {0,1,2}.
 * Populated in main() before USB init. Get: received(). Put: sent() / errors.
 * Thread-safe: k_msgq is internally synchronised.
 */
K_MSGQ_DEFINE(lb_free_q, sizeof(uint8_t), LB_BUF_COUNT, sizeof(uint8_t));

/*
 * Echo queue: (slot index, byte count) pairs ready for TX.
 * Put: received(). Get: echo_work_fn().
 * put_front() is used for EBUSY recovery (preserves ordering).
 */
struct lb_item {
	uint8_t  idx;     /* lb_pool slot index */
	uint16_t len;     /* number of valid bytes in lb_pool[idx] */
};
/* Align msgq elements to uint16_t to avoid any padding issues. */
K_MSGQ_DEFINE(lb_echo_q, sizeof(struct lb_item), LB_BUF_COUNT,
	      __alignof__(struct lb_item));

/*
 * TX-active flag (atomic).
 *
 * Cleared (0): no TX in flight; echo_work_fn may dequeue and call
 *              usbd_adb_write().
 * Set    (1): TX in flight; sent() will clear it and re-submit work.
 *
 * atomic_test_and_set_bit() returns the OLD value:
 *   true  → was already set → TX busy, back off
 *   false → was clear, now set → proceed with TX
 */
static ATOMIC_DEFINE(lb_tx_active, 1);

/*
 * Index of the buffer currently TX_IN_FLIGHT.
 * Written by echo_work_fn BEFORE calling usbd_adb_write() (while
 * lb_tx_active is held). Read by sent() AFTER the DMA completes.
 * Ordering guaranteed by lb_tx_active atomic.
 * Valid only while lb_tx_active bit 0 is set.
 */
static uint8_t lb_inflight_idx;

/* Loopback statistics (written from work queue / USBD thread; read from main). */
static uint32_t stat_rx;       /* total OUT completions */
static uint32_t stat_tx_ok;    /* successful TX starts */
static uint32_t stat_tx_err;   /* TX call failures */
static uint32_t stat_dropped;  /* RX dropped (no free buffer) */

/* Forward declaration */
static void echo_work_fn(struct k_work *work);
K_WORK_DEFINE(echo_work, echo_work_fn);

/* ── Echo work handler ──────────────────────────────────────────────────── */

/*
 * echo_work_fn — system workqueue thread context.
 *
 * Safe to call usbd_adb_write() here (not the USBD thread).
 *
 * Design: process exactly one item per invocation. sent() re-submits
 * this work when TX completes, draining lb_echo_q one item per TX cycle.
 * This avoids any concurrent call to usbd_adb_write().
 *
 * Invariant enforcement:
 *   - lb_tx_active set before usbd_adb_write() call.
 *   - If write returns non-zero: buffer returned to lb_free_q immediately
 *     (sent() will NOT fire). lb_tx_active cleared.
 *   - If write returns 0: buffer is TX_IN_FLIGHT. sent() owns it.
 *   - If -EBUSY: impossible under normal conditions (we hold lb_tx_active).
 *     Handled defensively with put_front to preserve ordering, then return.
 */
static void echo_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	struct lb_item item;
	int ret;

	/*
	 * Try to claim the TX slot. If TX is already active (bit was set),
	 * return — sent() will re-submit this work when the transfer finishes.
	 */
	if (atomic_test_and_set_bit(lb_tx_active, 0)) {
		/* TX was already active. */
		return;
	}

	/* TX slot is now ours. Dequeue the next echo item. */
	if (k_msgq_get(&lb_echo_q, &item, K_NO_WAIT) != 0) {
		/* Queue is empty; nothing to do. Release the TX slot. */
		atomic_clear_bit(lb_tx_active, 0);
		return;
	}

	/*
	 * Record inflight index BEFORE calling usbd_adb_write() so that
	 * sent() can identify which buffer to return. lb_tx_active is set,
	 * so sent() cannot run concurrently on this variable.
	 */
	lb_inflight_idx = item.idx;

	LOG_DBG("ECHO TX start: idx=%u len=%u", item.idx, item.len);

	ret = usbd_adb_write(lb_pool[item.idx], item.len);

	if (ret == 0) {
		/*
		 * Transfer enqueued. lb_tx_active remains set.
		 * sent() will: return buffer, clear flag, re-submit work.
		 */
		stat_tx_ok++;
		return;
	}

	/*
	 * usbd_adb_write() failed. sent() will NOT fire for this buffer.
	 * We must return it to lb_free_q immediately before clearing
	 * lb_tx_active (otherwise a concurrent echo_work_fn could dequeue
	 * the next item and read lb_inflight_idx before we set it — but
	 * since there is only one work item and it is not re-entrant, this
	 * cannot happen; still, clearing after the put is cleaner).
	 */
	stat_tx_err++;

	if (ret == -EBUSY) {
		/*
		 * Should not happen: we hold lb_tx_active and usbd_adb_write
		 * uses its own IN_ENGAGED flag. If it does, the buffer is NOT
		 * in flight. Put it back at the front of lb_echo_q to preserve
		 * ordering and retry on the next work invocation.
		 */
		LOG_WRN("Echo TX EBUSY (unexpected) idx=%u — retrying",
			item.idx);
		(void)k_msgq_put_front(&lb_echo_q, &item);
		atomic_clear_bit(lb_tx_active, 0);
		/* Re-submit to retry after yielding. */
		k_work_submit(&echo_work);
		return;
	}

	/* -EPERM (not connected) or other error: discard buffer. */
	LOG_WRN("Echo TX error %d idx=%u — discarding", ret, item.idx);
	(void)k_msgq_put(&lb_free_q, &item.idx, K_NO_WAIT);
	atomic_clear_bit(lb_tx_active, 0);

	/* Try the next queued item (interface may have come back). */
	k_work_submit(&echo_work);
}

/* ── ADB ops (loopback) ─────────────────────────────────────────────────── */

static void adb_connected_cb(struct usbd_class_data *c_data)
{
	LOG_INF("ADB: USB bulk loopback active");
	printk("[ADB] USB connected — echo loopback active\n");
}

static void adb_disconnected_cb(struct usbd_class_data *c_data)
{
	struct lb_item item;

	LOG_INF("ADB: USB interface disabled");
	printk("[ADB] USB disconnected\n");

	/*
	 * If TX was in flight, usbd_adb.c guarantees that sent() fires with
	 * -ECONNABORTED BEFORE calling disable() → disconnected(). By the
	 * time we arrive here, sent() has already:
	 *   1. Returned lb_inflight_idx to lb_free_q.
	 *   2. Cleared lb_tx_active.
	 *   3. Submitted echo_work (which will get -EPERM and return buffers).
	 *
	 * If no TX was in flight, lb_tx_active was already clear.
	 *
	 * Defensive clear: handles the edge case where echo_work_fn set
	 * lb_tx_active but disconnected() races before usbd_adb_write() is
	 * called (window between atomic_test_and_set and usbd_adb_write).
	 * In that case, echo_work_fn will get -EPERM from usbd_adb_write()
	 * and return the buffer via the error path. That is correct.
	 * We still clear here so that the free-queue accounting below is
	 * consistent: we only drain lb_echo_q (QUEUED state), not the
	 * TX_IN_FLIGHT buffer (sent() / echo_work_fn error path owns it).
	 */
	atomic_clear_bit(lb_tx_active, 0);

	/*
	 * Drain QUEUED buffers from lb_echo_q and return them to lb_free_q.
	 * These buffers have data copied but TX has not yet started; they are
	 * not in-flight and sent() will never fire for them.
	 *
	 * We do NOT touch lb_inflight_idx here. If a TX was in flight, sent()
	 * already handled it. If not, lb_inflight_idx is stale/unused.
	 */
	while (k_msgq_get(&lb_echo_q, &item, K_NO_WAIT) == 0) {
		LOG_DBG("Disconnect: returning queued buf idx=%u to free pool",
			item.idx);
		(void)k_msgq_put(&lb_free_q, &item.idx, K_NO_WAIT);
	}
}

static void adb_received_cb(struct usbd_class_data *c_data,
			    const uint8_t *data, size_t len)
{
	/*
	 * USBD thread context. Must not block. Must not call usbd_adb_write().
	 *
	 * data == buf->data: DMA buffer pointer valid ONLY during this call.
	 * usbd_adb.c frees buf and re-arms the OUT endpoint when we return.
	 * MUST copy before returning.
	 *
	 * State transition: FREE → COPYING → QUEUED
	 */
	uint8_t idx;
	struct lb_item item;

	stat_rx++;
	LOG_INF("RX %zu bytes (total=%u)", len, stat_rx);
	LOG_HEXDUMP_DBG(data, MIN(len, 64U), "RX");

	if (len == 0) {
		/* Zero-length OUT: re-arm happens automatically; nothing to echo. */
		return;
	}

	/*
	 * Acquire a free slot. K_NO_WAIT: never block in USBD thread.
	 * If the pool is exhausted (all 3 buffers in QUEUED or TX_IN_FLIGHT),
	 * drop this packet and log. This is a host overrun condition.
	 */
	if (k_msgq_get(&lb_free_q, &idx, K_NO_WAIT) != 0) {
		stat_dropped++;
		LOG_WRN("No free echo buffer — dropping %zu bytes "
			"(total dropped=%u)", len, stat_dropped);
		return;
	}

	/* Copy into the DMA-safe pool buffer (COPYING state, transient). */
	memcpy(lb_pool[idx], data, len);

	/* Move to QUEUED state: enqueue in lb_echo_q. */
	item.idx = idx;
	item.len = (uint16_t)len;

	if (k_msgq_put(&lb_echo_q, &item, K_NO_WAIT) != 0) {
		/*
		 * lb_echo_q full. This cannot happen if lb_echo_q depth ==
		 * LB_BUF_COUNT, because we just dequeued one from lb_free_q
		 * (which has the same count), and each slot is in exactly one
		 * queue at a time. If it does happen it is a programming error.
		 */
		LOG_ERR("BUG: lb_echo_q full after dequeuing from lb_free_q "
			"(idx=%u) — returning buffer", idx);
		(void)k_msgq_put(&lb_free_q, &idx, K_NO_WAIT);
		return;
	}

	/*
	 * Trigger echo. k_work_submit() is @isr_ok and idempotent: if the
	 * item is already queued/running it is a no-op (returns 0 or 2).
	 * We intentionally ignore the return value here.
	 */
	(void)k_work_submit(&echo_work);
}

static void adb_sent_cb(struct usbd_class_data *c_data,
			const uint8_t *buf, size_t len, int err)
{
	/*
	 * USBD thread context. Must not block.
	 *
	 * DMA is complete (success or error). lb_inflight_idx identifies the
	 * buffer that was TX_IN_FLIGHT.
	 *
	 * State transition: TX_IN_FLIGHT → FREE, then trigger echo_work for
	 * the next QUEUED item.
	 *
	 * Ordering: return buffer to lb_free_q FIRST, then clear lb_tx_active,
	 * then submit work. This ensures echo_work_fn never calls
	 * usbd_adb_write() with a buffer that is still in the pool's accounting
	 * as "unavailable".
	 *
	 * Verify the buf pointer matches the inflight buffer (defensive check).
	 */
	if (buf != lb_pool[lb_inflight_idx]) {
		LOG_ERR("sent(): buf %p does not match inflight lb_pool[%u]=%p",
			(const void *)buf,
			lb_inflight_idx,
			(const void *)lb_pool[lb_inflight_idx]);
		/*
		 * Something is very wrong. Clear lb_tx_active to unblock the
		 * pipeline. Do not return lb_inflight_idx to avoid double-free.
		 */
		atomic_clear_bit(lb_tx_active, 0);
		(void)k_work_submit(&echo_work);
		return;
	}

	if (err == 0) {
		LOG_DBG("TX complete: idx=%u len=%zu", lb_inflight_idx, len);
	} else if (err == -ECONNABORTED) {
		LOG_INF("TX aborted (disconnect): idx=%u", lb_inflight_idx);
	} else {
		LOG_WRN("TX error %d: idx=%u", err, lb_inflight_idx);
	}

	/* Return buffer: TX_IN_FLIGHT → FREE */
	(void)k_msgq_put(&lb_free_q, &lb_inflight_idx, K_NO_WAIT);

	/* Release TX slot, then trigger work for any remaining QUEUED items. */
	atomic_clear_bit(lb_tx_active, 0);
	(void)k_work_submit(&echo_work);
}

static const struct usbd_adb_ops adb_loopback_ops = {
	.connected    = adb_connected_cb,
	.disconnected = adb_disconnected_cb,
	.received     = adb_received_cb,
	.sent         = adb_sent_cb,
};

/* ── USBD message callback ──────────────────────────────────────────────── */

static void usbd_msg_cb(struct usbd_context *const ctx,
			const struct usbd_msg *msg)
{
	LOG_INF("USBD: %s", usbd_msg_type_string(msg->type));

	if (usbd_can_detect_vbus(ctx)) {
		if (msg->type == USBD_MSG_VBUS_READY) {
			if (usbd_enable(ctx)) {
				LOG_ERR("Failed to enable USB");
			}
		}
		if (msg->type == USBD_MSG_VBUS_REMOVED) {
			if (usbd_disable(ctx)) {
				LOG_ERR("Failed to disable USB");
			}
		}
	}
}

/* ── USB stack init ─────────────────────────────────────────────────────── */

static int usb_init(void)
{
	struct usbd_context *ctx = &adb_usbd;
	int ret;

	ret = usbd_add_descriptor(ctx, &adb_lang);
	if (ret) {
		LOG_ERR("Failed to add lang descriptor: %d", ret);
		return ret;
	}
	ret = usbd_add_descriptor(ctx, &adb_mfr);
	if (ret) {
		LOG_ERR("Failed to add mfr descriptor: %d", ret);
		return ret;
	}
	ret = usbd_add_descriptor(ctx, &adb_product);
	if (ret) {
		LOG_ERR("Failed to add product descriptor: %d", ret);
		return ret;
	}

	/* Register loopback ops BEFORE usbd_enable(). */
	ret = usbd_adb_register(&adb_loopback_ops);
	if (ret) {
		LOG_ERR("Failed to register ADB ops: %d", ret);
		return ret;
	}

	ret = usbd_add_configuration(ctx, USBD_SPEED_FS, &adb_fs_config);
	if (ret) {
		LOG_ERR("Failed to add FS config: %d", ret);
		return ret;
	}
	ret = usbd_register_class(ctx, "adb_0", USBD_SPEED_FS, 1);
	if (ret) {
		LOG_ERR("Failed to register ADB class (FS): %d", ret);
		return ret;
	}

	if (usbd_caps_speed(ctx) == USBD_SPEED_HS) {
		ret = usbd_add_configuration(ctx, USBD_SPEED_HS,
					     &adb_hs_config);
		if (ret) {
			LOG_ERR("Failed to add HS config: %d", ret);
			return ret;
		}
		ret = usbd_register_class(ctx, "adb_0", USBD_SPEED_HS, 1);
		if (ret) {
			LOG_ERR("Failed to register ADB class (HS): %d", ret);
			return ret;
		}
	}

	usbd_device_set_code_triple(ctx, USBD_SPEED_FS, 0, 0, 0);
	if (usbd_caps_speed(ctx) == USBD_SPEED_HS) {
		usbd_device_set_code_triple(ctx, USBD_SPEED_HS, 0, 0, 0);
	}

	ret = usbd_init(ctx);
	if (ret) {
		LOG_ERR("Failed to init USBD: %d", ret);
		return ret;
	}

	ret = usbd_msg_register_cb(ctx, usbd_msg_cb);
	if (ret) {
		LOG_ERR("Failed to register USBD msg cb: %d", ret);
		return ret;
	}

	if (!usbd_can_detect_vbus(ctx)) {
		ret = usbd_enable(ctx);
		if (ret) {
			LOG_ERR("Failed to enable USBD: %d", ret);
			return ret;
		}
	}

	LOG_INF("USB ADB loopback ready (VID=0x2FE3 PID=0xADB1)");
	return 0;
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void)
{
	/*
	 * Pre-populate lb_free_q with all slot indices.
	 * Must happen before USB init so received() can immediately dequeue.
	 */
	for (uint8_t i = 0; i < LB_BUF_COUNT; i++) {
		(void)k_msgq_put(&lb_free_q, &i, K_NO_WAIT);
	}

	printk("=== ADB-Zephyr USB bulk loopback test ===\n");
	printk("J10 = debug console  J11 = USB device\n");
	printk("Pool: %d buffers × %d bytes = %d bytes\n\n",
	       LB_BUF_COUNT,
	       CONFIG_USBD_ADB_RX_BUF_SIZE,
	       LB_BUF_COUNT * CONFIG_USBD_ADB_RX_BUF_SIZE);

	if (usb_init()) {
		printk("FAIL: USB init failed\n");
		return -1;
	}

	printk("USB ready. On Linux host:\n");
	printk("  cd adb-zephyr/tools && make && ./adb_loopback_test\n\n");

	while (true) {
		k_sleep(K_SECONDS(10));
		printk("[stat] rx=%u tx_ok=%u dropped=%u tx_err=%u\n",
		       stat_rx, stat_tx_ok, stat_dropped, stat_tx_err);
	}

	return 0;
}
