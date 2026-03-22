# Project
TARGET = merge
SRC = merge.ino
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
		.

upload:
	teensy_loader_cli --mcu=$(BOARD) -srv $(OUTPUT)/$(TARGET).ino.hex

clean:
	rm -rf $(BUILD)
	rm -rf $(OUTPUT)