# ADB on Zephyr — Project State

## Environment
- Board: FRDM-MCXW71 (MCXW716C)
- Zephyr: v4.4.0-14283-g62acbd571c72 (4.4.99)
- SDK: Zephyr SDK 1.0.1
- Workspace: ~/zephyrproject/zephyr
- App: ~/zephyrproject/adb-zephyr

## UART assignment
- LPUART1 = console/debug, WORKING, /dev/ttyACM0 @ 115200 (zephyr,console = &lpuart1)
- LPUART0 = dedicated ADB transport (separate from console, per design decision)
  - RX = PTA16, TX = PTA17
  - Already `status = "okay"` in board default DTS, pinctrl already defined
    (`pinmux_lpuart0` in frdm_mcxw71-pinctrl.dtsi) — no overlay needed
  - current-speed already 115200 by board default
- Driver style: interrupt-driven (CONFIG_UART_INTERRUPT_DRIVEN=y), not async/DMA

## Milestone roadmap
1. [DONE] ADB packet framing (encode/decode, checksum, magic)
2. [IN PROGRESS] Transport (UART, interrupt-driven, loopback test)
3. CNXN handshake
4. adb devices
5. adb shell → Zephyr shell
6. adb push/pull
7. reboot/state
8. firmware update / MCUboot
9. auth/security

## Milestone 1: Packet layer — DONE, VERIFIED ON HARDWARE
Root cause of original failure: 4120-byte buffer on the call stack overflowed
the board's small main stack → silent RAM corruption, no fault message, no
console output. Fix: all packet-sized objects (`struct adb_packet`, wire
buffers) are `static`, never stack-local or pass/return-by-value. Encoding
uses explicit little-endian byte packing (`put_le32`/`get_le32`), not
struct-copy (which relies on undefined/implementation-defined padding and
endianness behavior).

Verified hardware output:
```
=== ADB packet layer test ===
Encoded CNXN packet: 40 bytes
Decoded header: command=0x4e584e43 arg0=0x01000000 arg1=0x00001000 length=16 checksum=0x00000628 magic=0xb1a7b1bc
PASS: CNXN packet round-trip OK
Payload (16 bytes): "host::adb-zephyr"
```

## Milestone 2: UART transport — IN PROGRESS
Design: interrupt-driven RX on lpuart0 into a ring buffer, drained by
`adb_transport_process()` into a static byte-stream parser that reassembles
full ADB packets (header first, then declared-length payload) and calls a
user RX callback. TX is poll-based for now (simple, sufficient pre-throughput
testing; can move to interrupt-driven TX later if push/pull needs it).

Test setup: loopback jumper PTA17 (LPUART0_TX) → PTA16 (LPUART0_RX), no host
PC or USB-serial adapter needed yet. Board sends a CNXN packet to itself over
lpuart0 and confirms it decodes back correctly.

### Build error hit and fixed
```
error: invalid use of void expression
    if (!uart_irq_update(dev))
```
Cause: in Zephyr 4.4.99, `uart_irq_update()` returns `void`, not `int`
(this differs from older Zephyr versions/API docs assumptions). Fix: call it
unconditionally, without wrapping in a return-value check:

```c
static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	uart_irq_update(dev);

	if (uart_irq_rx_ready(dev)) {
		uint8_t byte;

		while (uart_fifo_read(dev, &byte, 1) == 1) {
			(void)ring_buf_put(&rx_ringbuf, &byte, 1);
		}
		k_sem_give(&rx_sem);
	}
}
```
No other logic changed. Awaiting build confirmation on hardware.

### Loopback test — not yet confirmed on hardware
Expected output once jumpered and flashed:
```
=== ADB UART transport loopback test ===
Requires jumper: PTA17 (LPUART0_TX) -> PTA16 (LPUART0_RX)
Sent CNXN packet (16 byte payload) over lpuart0
PASS: transport loopback round-trip OK
Received payload: "host::adb-zephyr"
```

## Files currently created/modified
```
adb-zephyr/
├── CMakeLists.txt          (adds src/transport.c)
├── prj.conf                (adds CONFIG_UART_INTERRUPT_DRIVEN=y)
├── app.overlay             (unchanged — no overlay needed for lpuart0)
├── include/
│   ├── adb.h               (packet header/API — from milestone 1)
│   └── transport.h         (transport API — new in milestone 2)
└── src/
    ├── main.c              (now: transport loopback test, replaces
    │                         milestone-1 packet-only test)
    ├── packet.c             (unchanged from milestone 1)
    └── transport.c          (new in milestone 2; fixed uart_irq_update() call)
```

## Key rules established so far
- Never put an `ADB_MAX_PAYLOAD`-sized (or `struct adb_packet`-sized) object
  on the stack — static/global storage only, always passed by pointer.
- LPUART1 stays dedicated to console/shell/debug; LPUART0 stays dedicated to
  ADB traffic. Don't merge these.
- RX is interrupt-driven into a ring buffer; parsing happens outside the ISR
  in `adb_transport_process()`.

## Immediate next step
Confirm clean build with the `uart_irq_update()` fix, then flash + verify
the PTA17→PTA16 loopback test passes on hardware (paste console output).

## Next milestone after that
CNXN handshake state machine (milestone 3): react to a received CNXN by
sending our own CNXN back with matching version/maxdata, track connection
state, reject/ignore packets before handshake completes.
