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
2. [DONE] Transport (UART, interrupt-driven, loopback test)
3. [NEXT] Real PC-to-MCXW71 link over external USB-UART adapter
4. CNXN handshake
5. adb devices
6. adb shell → Zephyr shell
7. adb push/pull
8. reboot/state
9. firmware update / MCUboot
10. auth/security

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

## Milestone 2: UART transport — COMPLETE, VERIFIED ON HARDWARE
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

### Loopback test — PASSED on hardware
Actual confirmed output, PTA17→PTA16 jumper, lpuart0:
```
Sent CNXN packet (16 byte payload)
PASS: transport loopback round-trip OK
Received payload: "host::adb-zephyr"
```
Confirms: interrupt-driven RX into ring buffer, byte-stream parser
reassembly (header then declared-length payload), poll-based TX, and
`adb_encode`/`adb_decode` all work correctly together on real hardware over
lpuart0, independent of the console UART (lpuart1).

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

## Milestone 3: Real PC-to-MCXW71 link over USB-UART adapter — PLANNED, NOT YET IMPLEMENTED

Goal: replace the PTA17→PTA16 loopback jumper with an actual PC on the other
end, talking to lpuart0 through an external USB-to-UART (e.g. FTDI/CP2102)
adapter. This proves real off-board transport before building the CNXN
handshake logic on top of it.

### What changes vs. the loopback milestone
- **Wiring**: USB-UART adapter RX → PTA17 (LPUART0_TX), adapter TX → PTA16
  (LPUART0_RX), adapter GND → board GND. (Cross TX/RX, unlike the loopback
  jumper which was TX→RX on the same UART.)
- **No firmware changes expected** to `transport.c`/`packet.c` — the UART
  peripheral, pins, and baud rate (115200) are unchanged; only what's wired
  to the other end of the wire changes.
- **main.c** needs a variant that doesn't just self-test once and idle, but
  continuously calls `adb_transport_process()` and reacts to whatever the PC
  sends (even before we implement real CNXN handshake logic, we can at least
  log/echo received packets to prove bytes are arriving correctly from a PC).
- **PC side**: for this milestone we will NOT use the real `adb` client yet
  (that requires the full CNXN handshake + USB or TCP transport that real
  `adb` expects). Instead we'll use a simple raw serial test —  likely a
  small Python script using `pyserial` that builds a raw ADB CNXN packet by
  hand (same wire format as `struct adb_packet`) and sends it over the
  USB-UART adapter, then reads back whatever the board sends. This isolates
  "is the physical link + framing correct" from "is the ADB protocol state
  machine correct."

### Open items to confirm before implementing
- Which USB-UART adapter/voltage level (must be 3.3V logic, not 5V, to avoid
  damaging the MCXW71 GPIO).
- Confirm adapter's COM port name on the host PC (e.g. /dev/ttyUSB0), which
  will be separate from the console's /dev/ttyACM0.
- Confirm baud rate stays 115200 for this link (matches board default;
  no reason to change it yet).

### Deliverables for this milestone (next response, once confirmed ready)
- Updated `main.c`: continuous receive loop instead of one-shot self-test.
- A `tools/raw_adb_test.py` (or similar) pyserial script for the PC side to
  send/receive raw ADB-framed packets for manual verification.
- No changes to `packet.c` or `transport.c` unless testing reveals an issue.

## Immediate next step
Confirm the wiring plan above (adapter voltage, port name) and confirm you
want the Python pyserial test script as the PC-side counterpart — then we
implement milestone 3.
