# LED Output: DMA Rework Notes (2026-09-24)

These notes record how the firmware drives the 4 WS2812 strings, what was
measured, and which alternatives were tried and rejected. Read them before
revisiting the output path. The code is in `firmware/src/neo_tree_led_output.cpp`.

## What's in place

- One PIO state machine per string (pio0 SMs 0-3, pins GP2/GP5/GP6/GP7). Each
  has one DMA channel, paced by its SM's TX FIFO DREQ. The SMs and channels are
  claimed properly so the CYW43 WiFi driver's own PIO/DMA allocations can't
  collide with them.
- Per frame, core0 packs the 1000 colors into one half of a double buffer
  (~145µs) and starts DMA. The output then runs without the CPU, with
  interrupts enabled.
- **Strings go out in two phases: strings 1+2 (LEDs 0-599) together, then
  strings 3+4 (LEDs 600-999).** The main loop starts phase 2 once phase 1 has
  drained. There's no busy-waiting.
- A phase is "done" once DMA has finished, the TX stall flags have been
  cleared *after* that point and seen set again, and the FIFOs are empty. The
  next frame starts after a 300µs latch gap.
- Frame rate target is 60 fps (CMake `NEOTREE_FRAME_RATE`).

### Measured on the tree

| | Before (CPU writes) | After |
|---|---|---|
| Frame rate at target 60 | 34.6 fps | **60.0 fps** |
| CPU blocked per frame | ~29ms, interrupts off | ~145µs to pack, interrupts on |
| Output time per frame | ~29ms | 15.0ms |
| Uncapped maximum | — | 65.3 fps (theory 65.4) |
| Uncapped + ~45 WiFi cmds/s + USB | — | 65.1 fps, 0 FIFO faults, 0 dropped commands |

Under heavy command traffic, the main loop can start phase 2 up to ~1.2ms
late. The worst frame measured was 16.3ms, which still fits the 16.7ms
budget at 60 fps, but with little margin.

## What didn't work: all 4 strings in parallel

The first version sent all 4 strings at once: 9ms per frame, ~107 fps max,
106.8 measured. **It glitched visibly on the tree:** random flickering, as if
the data stream were being segmented.
- Strings 3 and 4 (GP6/GP7, the middle and top of the tree) were affected.
  The top string was worst.
- Strings 1 and 2 were clean.
- It glitched at both 60 and 33 fps, so frame rate isn't the cause.

The data and the per-pin bitstreams were ruled out:
- The same packed buffer sent one string at a time was clean, whether by DMA
  (mode 1) or by CPU writes (mode 2, equivalent to the original code).
- In parallel mode, per-frame counters showed **zero TX FIFO underflows and
  zero overflows** on every string. Every SM got a continuous stream, so each
  pin's waveform was bit-for-bit identical to the clean sequential modes.

The only remaining difference was **the four data lines switching at the same
instant**:
- Offsetting each string's DMA start by 300ns (so edges don't coincide)
  visibly **improved** it, but didn't fix it.
- Grouping as {1,2} then {3,4} is fully clean, with or without the offset. So
  the 300ns stagger isn't used by default.

**Most likely cause (unconfirmed):** electrical interaction between the data
lines when they are active at the same time. Two candidate mechanisms:
- Ground bounce on a shared return.
- Crosstalk between bundled wires.

These fit the facts:
- The effect hits the longest runs (the top of the tree) first.
- It depends on timing *between* pins, not on any pin's own signal.
- Separating the lines in time fixes it.

**If this is revisited to get back to 9ms / ~107 fps, try:**
- A series resistor (~33-100Ω) at the Pico on each data line.
- A level shifter to 5V logic.
- A separate ground return per string, or routing the data wires apart.

Then re-test all-parallel with the diagnostics below.

## Diagnostics built in (not persisted - a reboot restores the defaults)

| Command | Bytes | Effect |
|---|---|---|
| `LED_OUTPUT_MODE` (16) | `[16][mode]` | 0 = phased DMA (normal); 1 = DMA one string at a time (blocking); 2 = CPU writes, interrupts off (the original method); 3 = CPU writes, interrupts on |
| `LED_OUTPUT_TUNING` (17) | `[17][fast_slew][drive 0-3][stagger/10ns][fps, 0 = keep][phase map]` | Pin slew/drive strength, start offset between strings in a phase, frame rate target, and phase map. The phase map is 2 bits per string, string *s* at bits 2*s*; default `0b01010000` = {1,2} then {3,4} |

- **Heartbeat (every 5s):** target fps, output mode, pack time, output time
  (max per window), and per-string FIFO underflow/overflow counts.
- **Scripts** (`tools/led_diag/`, PowerShell from the desktop, over WiFi):
  - `string_test.ps1` lights strings 1-4 dim red/green/blue/white. It checks
    pin order and shows glitches.
  - `set_output_mode.ps1 -Mode N`
  - `set_output_tuning.ps1 -StaggerNs 300 -Fps 60 -Phases 0,0,1,1`
  - Test the uncapped rate with `-Fps 255`. Measure it from the heartbeat.

## CPU headroom (measured 2026-09-24)

- **core0** (rendering + command handling) spends ~145µs per frame packing,
  which is under 1% at 60 fps. Command handlers take microseconds each. This
  is where animation rendering will run.
- **core1** (USB serial + WiFi/lwIP IRQs) runs a polling loop. The drop in
  its loop rate is its busy time:

  | Load | core1 busy |
  |---|---|
  | ~87 WiFi group commands/s (2 clients) | 7.4% |
  | ~425 USB serial commands/s | 12.2% |

  Most of the USB cost is the legacy per-byte `printf` echo of every received
  serial byte.

## Issue found during these tests - fixed

A new TCP client connecting **while other clients flooded commands** never
received its HELLO. Instrumented and confirmed:
- `tcp_write` failed with ERR_MEM.
- lwIP's heap peaked at 3,772 of 4,000 bytes.

With `TCP_OVERSIZE = TCP_MSS` (required by `LWIP_NETIF_TX_SINGLE_PBUF`), a
write onto an empty send queue allocates a full ~1.5KB pbuf, so two busy
clients starve a third. The server used to drop failed writes silently. That
would also lose ACKs under load and desync clients.

Fixes:
- `MEM_SIZE` 4000 -> 16384. The peak is now 6.9KB with all 4 slots flooding,
  with no errors, and new clients get their HELLO in 11-26ms.
- The server never drops a reply. On ERR_MEM the frame goes into a per-client
  pending buffer (128B). It's retried in order from lwIP's sent callback and
  a 1s poll. Verified with the old heap: a deferred HELLO arrived 0.8s after
  memory freed.
- If a client's backlog overflows (it isn't reading), the connection is
  closed cleanly so it can reconnect.

The heartbeat line `net diag:` shows deferred and failed writes and overflow
closes. The line `lwip:` shows heap and pool usage and allocation errors
(`LWIP_STATS` is on).
