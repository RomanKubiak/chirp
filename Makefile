# Project
TARGET = chirp
SKETCH_DIR = .
SRC = $(SKETCH_DIR)/chirp.ino
BUILD = build
OUTPUT = output
# Teensy board + USB mode
FQBN = teensy:avr:teensy40
USB = serialmidi

EXTRA_INCLUDES = -Isrc/wren/include -Isrc/wren/optional -Isrc/wren/vm -Isrc/include
TEENSY_HW_LIBS = $(HOME)/.arduino15/packages/teensy/hardware/avr/1.60.0/libraries

# Set to 1 to enable raw MIDI1 byte logging (shows every byte received).
DEBUG_MIDI1_RAW = 0
# Set to 1 to enable verbose MIDI read status (shows True/False on each loop).
VERBOSE_MIDI_STATUS = 0
# Set to 1 to enable debug logging for MIDI events and Wren print/error logs.
DEBUG_LOGGING = 0
# Set to 1 to mirror runtime logs to Serial in addition to control messages.
DEBUG_RUNTIME_SERIAL = 0
# Set to 1 to emit periodic diagnostics in loop().
ENABLE_PERIODIC_DIAG = 0
# Periodic diagnostics interval in milliseconds.
PERIODIC_DIAG_INTERVAL_MS = 1000
# Set to 1 to emit compact boot diagnostics.
ENABLE_BOOT_DIAG = 1

# Wren heap tuning for Teensy 4.0 RAM constraints.
WREN_INITIAL_HEAP_BYTES = 262144
WREN_MIN_HEAP_BYTES = 131072
WREN_HEAP_GROWTH_PCT = 25

# Set to 1 to enable the ST7735 SPI display.
ENABLE_ST7735 = 1
ST7735_ROTATION = 1
ST7735_PIN_CS = 10
ST7735_PIN_DC = 6
ST7735_PIN_RST = 9
ST7735_PIN_BL = -1

EXTRA_CFLAGS = \
	-DDEBUG_MIDI1_RAW=$(DEBUG_MIDI1_RAW) \
	-DVERBOSE_MIDI_STATUS=$(VERBOSE_MIDI_STATUS) \
	-DDEBUG_LOGGING=$(DEBUG_LOGGING) \
	-DDEBUG_RUNTIME_SERIAL=$(DEBUG_RUNTIME_SERIAL) \
	-DENABLE_PERIODIC_DIAG=$(ENABLE_PERIODIC_DIAG) \
	-DPERIODIC_DIAG_INTERVAL_MS=$(PERIODIC_DIAG_INTERVAL_MS) \
	-DENABLE_BOOT_DIAG=$(ENABLE_BOOT_DIAG) \
	-DWREN_INITIAL_HEAP_BYTES=$(WREN_INITIAL_HEAP_BYTES) \
	-DWREN_MIN_HEAP_BYTES=$(WREN_MIN_HEAP_BYTES) \
	-DWREN_HEAP_GROWTH_PCT=$(WREN_HEAP_GROWTH_PCT) \
	-DENABLE_ST7735=$(ENABLE_ST7735) \
	-DST7735_ROTATION=$(ST7735_ROTATION) \
	-DST7735_PIN_CS=$(ST7735_PIN_CS) \
	-DST7735_PIN_DC=$(ST7735_PIN_DC) \
	-DST7735_PIN_RST=$(ST7735_PIN_RST) \
	-DST7735_PIN_BL=$(ST7735_PIN_BL)
# Arduino CLI
ARDUINO_CLI = arduino-cli

TEENSY_TOOLS = $(HOME)/.arduino15/packages/teensy/tools/teensy-tools/1.60.0
TEENSY_POST = $(TEENSY_TOOLS)/teensy_post_compile
TEENSY_REBOOT = $(TEENSY_TOOLS)/teensy_reboot

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
		--library $(TEENSY_HW_LIBS)/ST7735_t3 \
		--build-property "build.extra_flags=$(EXTRA_INCLUDES) $(EXTRA_CFLAGS)" \
		$(SKETCH_DIR)

upload:
	@echo "Press the button on the Teensy, then wait..."
	teensy_loader_cli --mcu=$(BOARD) -wvrs $(OUTPUT)/$(TARGET).ino.hex

CHIRP_FS     = python3 tools/chirp_fs.py -p $(PORT)
SYNC_DIRS    = scripts/ midi_maps/ third_party/wren-json/
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