# Project
TARGET = chirp
SKETCH_DIR = .
SRC = $(SKETCH_DIR)/chirp.ino
BUILD = build
OUTPUT = output
# Teensy board + USB mode
FQBN = teensy:avr:teensy40
ENABLE_LIVE_DEBUG = 0
USB = serialmidi

EXTRA_INCLUDES = -Isrc/wren/include -Isrc/wren/optional -Isrc/wren/vm -Isrc/include

# Set to 1 to enable raw MIDI1 byte logging (shows every byte received).
DEBUG_MIDI1_RAW = 1
# Set to 1 to enable verbose MIDI read status (shows True/False on each loop).
VERBOSE_MIDI_STATUS = 1
# Set to 1 to enable debug logging for MIDI events and Wren print/error logs.
DEBUG_LOGGING = 1
# Set to 1 to mirror runtime logs to Serial in addition to control messages.
DEBUG_RUNTIME_SERIAL = 0
# Set to 1 to emit periodic diagnostics in loop().
ENABLE_PERIODIC_DIAG = 1
# Periodic diagnostics interval in milliseconds.
PERIODIC_DIAG_INTERVAL_MS = 1000
# Set to 1 to emit compact boot diagnostics.
ENABLE_BOOT_DIAG = 1

# Wren heap tuning for Teensy 4.0 RAM constraints.
WREN_INITIAL_HEAP_BYTES = 262144
WREN_MIN_HEAP_BYTES = 131072
WREN_HEAP_GROWTH_PCT = 25

# ST7735 1.8" Display Pin Configuration (ST7735_t3 Teensy library)
DISPLAY_CS = 10
DISPLAY_DC = 6
DISPLAY_RST = 9
DISPLAY_MOSI = 11
DISPLAY_MISO = 12
DISPLAY_CLK = 13
# Set to 0 to disable ST7735_t3 framebuffer/async updates if RAM is tight.
ENABLE_DISPLAY_ASYNC_FRAMEBUFFER = 1
# Set to 0 to force synchronous framebuffer flush (more stable on some boards).
ENABLE_DISPLAY_ASYNC_DMA = 0
# Set to 1 to prefer SD card storage backend on shared SPI bus.
ENABLE_SD_STORAGE_BACKEND = 1
# SD card reader chip-select pin.
SD_CARD_CS = 23
# Max ms to wait for USB serial connection before continuing boot (0 = wait forever).
SERIAL_WAIT_TIMEOUT_MS = 2000
# Rotary encoder pins (CLK, DT, SW button) for launcher navigation.
ENCODER_PIN_CLK = 2
ENCODER_PIN_DT = 3
ENCODER_PIN_SW = 4

EXTRA_CFLAGS = \
	-DDEBUG_MIDI1_RAW=$(DEBUG_MIDI1_RAW) \
	-DVERBOSE_MIDI_STATUS=$(VERBOSE_MIDI_STATUS) \
	-DDEBUG_LOGGING=$(DEBUG_LOGGING) \
	-DDEBUG_RUNTIME_SERIAL=$(DEBUG_RUNTIME_SERIAL) \
	-DENABLE_PERIODIC_DIAG=$(ENABLE_PERIODIC_DIAG) \
	-DPERIODIC_DIAG_INTERVAL_MS=$(PERIODIC_DIAG_INTERVAL_MS) \
	-DENABLE_BOOT_DIAG=$(ENABLE_BOOT_DIAG) \
	-DENABLE_LIVE_DEBUG=$(ENABLE_LIVE_DEBUG) \
	-DWREN_INITIAL_HEAP_BYTES=$(WREN_INITIAL_HEAP_BYTES) \
	-DWREN_MIN_HEAP_BYTES=$(WREN_MIN_HEAP_BYTES) \
	-DWREN_HEAP_GROWTH_PCT=$(WREN_HEAP_GROWTH_PCT) \
	-DDISPLAY_CS=$(DISPLAY_CS) \
	-DDISPLAY_DC=$(DISPLAY_DC) \
	-DDISPLAY_RST=$(DISPLAY_RST) \
	-DDISPLAY_MOSI=$(DISPLAY_MOSI) \
	-DDISPLAY_MISO=$(DISPLAY_MISO) \
	-DDISPLAY_CLK=$(DISPLAY_CLK) \
	-DENABLE_DISPLAY_ASYNC_FRAMEBUFFER=$(ENABLE_DISPLAY_ASYNC_FRAMEBUFFER) \
	-DENABLE_DISPLAY_ASYNC_DMA=$(ENABLE_DISPLAY_ASYNC_DMA) \
	-DENABLE_SD_STORAGE_BACKEND=$(ENABLE_SD_STORAGE_BACKEND) \
	-DSD_CARD_CS=$(SD_CARD_CS) \
	-DSERIAL_WAIT_TIMEOUT_MS=$(SERIAL_WAIT_TIMEOUT_MS) \
	-DENCODER_PIN_CLK=$(ENCODER_PIN_CLK) \
	-DENCODER_PIN_DT=$(ENCODER_PIN_DT) \
	-DENCODER_PIN_SW=$(ENCODER_PIN_SW)
# Arduino CLI
ARDUINO_CLI = arduino-cli

TEENSY_TOOLS = $(HOME)/.arduino15/packages/teensy/tools/teensy-tools/1.60.0
TEENSY_POST = $(TEENSY_TOOLS)/teensy_post_compile
TEENSY_REBOOT = $(TEENSY_TOOLS)/teensy_reboot
TEENSY_LIB_ROOT = $(HOME)/.arduino15/packages/teensy/hardware/avr/1.60.0/libraries
TEENSY_SD_LIB = $(TEENSY_LIB_ROOT)/SD
TEENSY_SDFAT_LIB = $(TEENSY_LIB_ROOT)/SdFat

PORT = /dev/ttyACM0
BOARD = TEENSY40

all: $(BUILD)/$(TARGET).hex

$(BUILD):
	mkdir -p $(BUILD)
	mkdir -p $(OUTPUT)

# Compile using Arduino CLI
$(BUILD)/$(TARGET).hex: $(SRC) | $(BUILD)
	$(ARDUINO_CLI) compile -v \
		--fqbn $(FQBN):usb=$(USB) \
		--build-path $(BUILD) \
		--output-dir $(OUTPUT) \
		--library "$(TEENSY_SD_LIB)" \
		--library "$(TEENSY_SDFAT_LIB)" \
		--build-property "build.extra_flags=$(EXTRA_INCLUDES) $(EXTRA_CFLAGS)" \
		$(SKETCH_DIR)

upload:
	@echo "Press the button on the Teensy, then wait..."
	teensy_loader_cli --mcu=$(BOARD) -wvrs $(OUTPUT)/$(TARGET).ino.hex

CHIRP_FS     = python3 tools/chirp_fs.py -p $(PORT)
SYNC_DIRS    = scripts/ scripts/builtin/ midi_maps/ third_party/wren-json/
DEVICE_WAIT  = 4

# Upload filesystem assets (scripts/ + midi_maps/) to device over USB serial.
# Use --delete to purge stale managed files before copying new assets.
upload-fs:
	$(CHIRP_FS) sync --delete $(SYNC_DIRS)

# Build firmware, flash it, wait for reboot, then sync filesystem assets.
upload-all: $(BUILD)/$(TARGET).hex
	$(MAKE) upload
	@echo "Waiting $(DEVICE_WAIT)s for device to reboot..."
	@sleep $(DEVICE_WAIT)
	$(MAKE) upload-fs

clean:
	rm -rf $(BUILD)
	rm -rf $(OUTPUT)