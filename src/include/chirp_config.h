#ifndef CHIRP_CONFIG_H
#define CHIRP_CONFIG_H

// Defaults are provided here; override from build flags (e.g. Makefile EXTRA_CFLAGS).
#ifndef DEBUG_MIDI1_RAW
#define DEBUG_MIDI1_RAW 0
#endif

#ifndef VERBOSE_MIDI_STATUS
#define VERBOSE_MIDI_STATUS 0
#endif

#ifndef DEBUG_LOGGING
#define DEBUG_LOGGING 0
#endif

#ifndef DEBUG_RUNTIME_SERIAL
#define DEBUG_RUNTIME_SERIAL 0
#endif

#ifndef ENABLE_PERIODIC_DIAG
#define ENABLE_PERIODIC_DIAG 0
#endif

#ifndef PERIODIC_DIAG_INTERVAL_MS
#define PERIODIC_DIAG_INTERVAL_MS 1000
#endif

#ifndef ENABLE_BOOT_DIAG
#define ENABLE_BOOT_DIAG 1
#endif

// Wren heap tuning defaults for Teensy 4.0 RAM constraints.
#ifndef WREN_INITIAL_HEAP_BYTES
#define WREN_INITIAL_HEAP_BYTES (256 * 1024)
#endif

#ifndef WREN_MIN_HEAP_BYTES
#define WREN_MIN_HEAP_BYTES (128 * 1024)
#endif

#ifndef WREN_HEAP_GROWTH_PCT
#define WREN_HEAP_GROWTH_PCT 25
#endif

#endif // CHIRP_CONFIG_H
