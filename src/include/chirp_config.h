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

#ifndef ENABLE_LIVE_DEBUG
#define ENABLE_LIVE_DEBUG 0
#endif

// Set to 1 to emit detailed script reload trace logs around stop/start,
// module reset, GC, and Wren interpretation boundaries. Intended for
// diagnosing crashes during repeated script load/unload cycles.
#ifndef TRACE_SCRIPT_RELOAD
#define TRACE_SCRIPT_RELOAD 0
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

// ST7735 1.8" Display pin configuration (from Makefile, used by ST7735_t3 library)
#ifndef DISPLAY_CS
#define DISPLAY_CS 10
#endif

#ifndef DISPLAY_DC
#define DISPLAY_DC 6
#endif

#ifndef DISPLAY_RST
#define DISPLAY_RST 9
#endif

#ifndef DISPLAY_MOSI
#define DISPLAY_MOSI 11
#endif

#ifndef DISPLAY_MISO
#define DISPLAY_MISO 12
#endif

#ifndef DISPLAY_CLK
#define DISPLAY_CLK 13
#endif

// Set to 1 to enable ST7735_t3 framebuffer + async DMA updates.
// Set to 0 to reduce RAM usage and use direct drawing only.
#ifndef ENABLE_DISPLAY_ASYNC_FRAMEBUFFER
#define ENABLE_DISPLAY_ASYNC_FRAMEBUFFER 1
#endif

// Set to 1 to enable async DMA flush when framebuffer is enabled.
// Set to 0 to use synchronous flush (more stable on some setups).
#ifndef ENABLE_DISPLAY_ASYNC_DMA
#define ENABLE_DISPLAY_ASYNC_DMA 0
#endif

// Storage backend: prefer SD on shared SPI (CS pin below), then fallback to LittleFS.
#ifndef ENABLE_SD_STORAGE_BACKEND
#define ENABLE_SD_STORAGE_BACKEND 1
#endif

#ifndef SD_CARD_CS
#define SD_CARD_CS 23
#endif

// Maximum ms to wait for USB serial before continuing boot (0 = wait forever).
#ifndef SERIAL_WAIT_TIMEOUT_MS
#define SERIAL_WAIT_TIMEOUT_MS 2000
#endif

// Rotary encoder module pins used for launcher navigation.
#ifndef ENCODER_PIN_CLK
#define ENCODER_PIN_CLK 2
#endif

#ifndef ENCODER_PIN_DT
#define ENCODER_PIN_DT 3
#endif

#ifndef ENCODER_PIN_SW
#define ENCODER_PIN_SW 4
#endif

#endif // CHIRP_CONFIG_H
