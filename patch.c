int adb_send_packet_sync(uint32_t cmd, uint32_t arg0, uint32_t arg1, const void *payload, size_t len, k_timeout_t timeout)
{
	uint8_t idx;
	if (k_msgq_get(&tx_free_q, &idx, timeout) != 0) {
		return -ENOMEM;
	}

	struct adb_packet pkt;
	pkt.command = cmd;
	pkt.arg0 = arg0;
	pkt.arg1 = arg1;
	pkt.length = len;
	
	if (len > 0 && payload != NULL) {
		memcpy(pkt.payload, payload, len);
	}

	adb_packet_finalize(&pkt);

	size_t encoded_len;
	if (adb_encode(&pkt, tx_pool[idx], RX_STREAM_MAX, &encoded_len) != 0) {
		k_msgq_put(&tx_free_q, &idx, K_NO_WAIT);
		return -EINVAL;
	}

	tx_inflight_len[idx] = encoded_len;
	k_msgq_put(&tx_pending_q, &idx, K_NO_WAIT);
	k_work_submit(&tx_work);

	return 0;
}
