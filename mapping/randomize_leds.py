"""
Continuously picks a random number of LEDs (out of the full 1000) and sets
each to a random RGB color, using the COLOR_GROUP_RGB_UPDATE serial command
so a whole batch updates in one message instead of one round-trip per LED.

Run with the venv active:
    python3 randomize_leds.py
Ctrl+C to stop.
"""
import random
import time

import neotree_serial as neoser

TOTAL_LEDS = 1000
GROUP_UPDATE_MSG_TYPE = 2  # COLOR_GROUP_RGB_UPDATE
FRAME_DELAY_S = 0.15
# Was random.randint(1, TOTAL_LEDS) - averaged ~500 LEDs/frame, so nearly the
# whole tree was repainted almost every frame instead of a scattered handful.
# Capped at MAX_GROUP_UPDATE_ENTRIES (12) so each frame is also exactly one
# group-update packet - no chunking needed below.
MAX_LEDS_PER_FRAME = neoser.MAX_GROUP_UPDATE_ENTRIES


def random_frame():
    """One (led_position, r, g, b) tuple per randomly-chosen, distinct LED."""
    count = random.randint(1, MAX_LEDS_PER_FRAME)
    positions = random.sample(range(TOTAL_LEDS), count)
    return [
        (pos, random.randint(0, 255), random.randint(0, 255), random.randint(0, 255))
        for pos in positions
    ]


def send_frame(ser, updates):
    """Chunk into <= neoser.MAX_GROUP_UPDATE_ENTRIES-sized group updates."""
    chunk_size = neoser.MAX_GROUP_UPDATE_ENTRIES
    for i in range(0, len(updates), chunk_size):
        chunk = updates[i:i + chunk_size]
        neoser.write_tree_group_leds(ser, GROUP_UPDATE_MSG_TYPE, chunk)


def main():
    error = neoser.open_neotree_serial('/dev/ttyACM0', baudrate=115200)
    if error:
        print(error)
        return

    print("Randomizing LEDs - Ctrl+C to stop")
    try:
        frame_count = 0
        while True:
            updates = random_frame()
            send_frame(neoser.ser, updates)
            frame_count += 1
            print(f"frame {frame_count}: {len(updates)} LEDs")
            time.sleep(FRAME_DELAY_S)
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        neoser.cleanup_serial()


if __name__ == "__main__":
    main()
