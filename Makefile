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

EXTRA_INCLUDES = -Isrc/wren/src/include -Isrc/wren/src/optional -Isrc/wren/src/vm -Isrc/include

# Set to 1 to enable raw MIDI1 byte logging (shows every byte received).
DEBUG_MIDI1_RAW = 0
# Set to 1 to enable verbose MIDI read status (shows True/False on each loop).
VERBOSE_MIDI_STATUS = 0
# Set to 1 to enable debug logging for MIDI events and Wren print/error logs.
DEBUG_LOGGING = 1
# Set to 1 to mirror runtime logs to Serial in addition to control messages.
DEBUG_RUNTIME_SERIAL = 0
# Set to 1 to emit periodic diagnostics in loop().
ENABLE_PERIODIC_DIAG = 0
# Periodic diagnostics interval in milliseconds.
PERIODIC_DIAG_INTERVAL_MS = 1000
# Set to 1 to emit compact boot diagnostics.
ENABLE_BOOT_DIAG = 1
# Set to 1 to emit detailed trace logs around script stop/start and Wren reload.
TRACE_SCRIPT_RELOAD = 0

# Wren heap tuning for Teensy 4.0 RAM constraints.
WREN_INITIAL_HEAP_BYTES = 262144
WREN_MIN_HEAP_BYTES = 131072
WREN_HEAP_GROWTH_PCT = 25

# Shared SPI bus pin configuration.
SHARED_SPI_CS = 10
SHARED_SPI_DC = 6
SHARED_SPI_RST = 9
SHARED_SPI_MOSI = 11
SHARED_SPI_MISO = 12
SHARED_SPI_CLK = 13
# Set to 1 to prefer SD card storage backend on shared SPI bus.
ENABLE_SD_STORAGE_BACKEND = 1
# SD card reader chip-select pin.
SD_CARD_CS = 23
# Max ms to wait for USB serial connection before continuing boot (0 = wait forever).
SERIAL_WAIT_TIMEOUT_MS = 2000
# Rotary encoder pins (CLK, DT, SW button) for launcher navigation.
ENCODER_PIN_CLK = 5
ENCODER_PIN_DT = 4
ENCODER_PIN_SW = 3

EXTRA_CFLAGS = \
	-DDEBUG_MIDI1_RAW=$(DEBUG_MIDI1_RAW) \
	-DVERBOSE_MIDI_STATUS=$(VERBOSE_MIDI_STATUS) \
	-DDEBUG_LOGGING=$(DEBUG_LOGGING) \
	-DDEBUG_RUNTIME_SERIAL=$(DEBUG_RUNTIME_SERIAL) \
	-DENABLE_PERIODIC_DIAG=$(ENABLE_PERIODIC_DIAG) \
	-DPERIODIC_DIAG_INTERVAL_MS=$(PERIODIC_DIAG_INTERVAL_MS) \
	-DENABLE_BOOT_DIAG=$(ENABLE_BOOT_DIAG) \
	-DTRACE_SCRIPT_RELOAD=$(TRACE_SCRIPT_RELOAD) \
	-DENABLE_LIVE_DEBUG=$(ENABLE_LIVE_DEBUG) \
	-DWREN_INITIAL_HEAP_BYTES=$(WREN_INITIAL_HEAP_BYTES) \
	-DWREN_MIN_HEAP_BYTES=$(WREN_MIN_HEAP_BYTES) \
	-DWREN_HEAP_GROWTH_PCT=$(WREN_HEAP_GROWTH_PCT) \
	-DSHARED_SPI_CS=$(SHARED_SPI_CS) \
	-DSHARED_SPI_DC=$(SHARED_SPI_DC) \
	-DSHARED_SPI_RST=$(SHARED_SPI_RST) \
	-DSHARED_SPI_MOSI=$(SHARED_SPI_MOSI) \
	-DSHARED_SPI_MISO=$(SHARED_SPI_MISO) \
	-DSHARED_SPI_CLK=$(SHARED_SPI_CLK) \
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
TEENSY_ST7735_LIB = $(TEENSY_LIB_ROOT)/ST7735_t3

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
		--library "$(TEENSY_ST7735_LIB)" \
		--build-property "build.extra_flags=$(EXTRA_INCLUDES) $(EXTRA_CFLAGS)" \
		$(SKETCH_DIR)

upload:
	@echo "Press the button on the Teensy, then wait..."
	teensy_loader_cli --mcu=$(BOARD) -wvrs $(OUTPUT)/$(TARGET).ino.hex

CHIRP_FS     = python3 tools/chirp_fs.py -p $(PORT)
SYNC_DIRS    = scripts/ scripts/builtin/ midi_maps/
DEVICE_WAIT  = 8

# Upload filesystem assets (scripts/ + midi_maps/) to device over USB serial.
# Use --delete to purge stale managed files before copying new assets.
upload-fs:
	$(CHIRP_FS) sync --delete $(SYNC_DIRS)

# Build firmware, flash it, wait for reboot, then sync filesystem assets.
upload-all: $(BUILD)/$(TARGET).hex
	#$(MAKE) bootloader
	#@echo "Waiting $(DEVICE_WAIT)s for bootloader enumeration..."
	#@sleep $(DEVICE_WAIT)
	$(MAKE) upload
	@echo "Waiting for $(PORT) to reappear..."
	@deadline=$$(($$(date +%s) + $(DEVICE_WAIT))); \
	while [ ! -e "$(PORT)" ] && [ $$(date +%s) -lt $$deadline ]; do sleep 1; done; \
	if [ ! -e "$(PORT)" ]; then echo "$(PORT) did not reappear in time"; exit 1; fi
	@for attempt in 1 2 3; do \
		if $(CHIRP_FS) sync --wait $(DEVICE_WAIT) --delete $(SYNC_DIRS); then exit 0; fi; \
		if [ $$attempt -lt 3 ]; then echo "Sync failed, retrying..."; sleep 2; fi; \
	done; \
	exit 1

# Request device to enter HalfKay bootloader mode (facilitates reflashing in debug loop).
bootloader:
	@echo "Requesting bootloader entry on $(PORT)..."
	$(CHIRP_FS) bootloader

clean:
	rm -rf $(BUILD)
	rm -rf $(OUTPUT)