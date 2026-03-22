#ifndef MERGE_CONFIG_H
#define MERGE_CONFIG_H

// Set to 1 to enable raw MIDI1 byte logging (shows every byte received)
#define DEBUG_MIDI1_RAW 0
// Set to 1 to enable verbose MIDI read status (shows True/False on each loop)
#define VERBOSE_MIDI_STATUS 0
// Set to 1 to enable debug logging for MIDI event logs and Wren print/error logs.
#define DEBUG_LOGGING 0
// Set to 1 to mirror runtime logs to Serial in addition to control messages.
#define DEBUG_RUNTIME_SERIAL 0
// Set to 1 to emit periodic diagnostics in loop().
#define ENABLE_PERIODIC_DIAG 0
#define PERIODIC_DIAG_INTERVAL_MS 1000
// Set to 1 to emit compact boot diagnostics.
#define ENABLE_BOOT_DIAG 1

// Wren heap tuning for Teensy 4.0 RAM constraints.
#define WREN_INITIAL_HEAP_BYTES (256 * 1024)
#define WREN_MIN_HEAP_BYTES     (128 * 1024)
#define WREN_HEAP_GROWTH_PCT    25

#endif // MERGE_CONFIG_H
