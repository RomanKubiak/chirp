# Project
TARGET = chirp
SKETCH_DIR = chirp
SRC = $(SKETCH_DIR)/chirp.ino
BUILD = build
OUTPUT = output
# Teensy board + USB mode
FQBN = teensy:avr:teensy40
USB = serialmidi

EXTRA_INCLUDES = -Isrc/wren/include -Isrc/wren/optional -Isrc/wren/vm -Isrc/include

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
		--build-property "build.extra_flags=$(EXTRA_INCLUDES)" \
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