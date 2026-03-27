extern "C" {
#include <stdint.h>
#include <sys/types.h>
clock_t _times(void *buf) { return 0; }
}

#include "chirp_config.h"

#include "wren.hpp"
#include "MIDI.h"
#include "midi_types.h"
#include "chirp_boot.h"
#include "script_storage.h"
#include "usb_serial_handler.h"
#include "launcher.h"
#include "wren_vm.h"
#include <cstring>
#include <cstdio>

#if ENABLE_LIVE_DEBUG
#include <TeensyDebug.h>
#pragma GCC optimize ("O0")
#endif

// Component headers
#include "runtime_log.h"
#include "chirp_fs.h"

// ── Serial MIDI port instances (must live in the sketch) ──────────────────────
MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI1);
MIDI_CREATE_INSTANCE(HardwareSerial, Serial2, MIDI2);
MIDI_CREATE_INSTANCE(HardwareSerial, Serial3, MIDI3);
MIDI_CREATE_INSTANCE(HardwareSerial, Serial4, MIDI4);
MIDI_CREATE_INSTANCE(HardwareSerial, Serial5, MIDI5);

#if DEBUG_RUNTIME_SERIAL
USBSerialHandler<usb_serial_class> usbHandler(Serial, &Serial);
#else
USBSerialHandler<usb_serial_class> usbHandler(Serial, nullptr);
#endif

// ── Application-wide globals ──────────────────────────────────────────────────
WrenConfiguration config;
WrenVM *vm;

ChirpFS internalFlash;
ScriptStorage scriptStorage(internalFlash);

void sendControlLog(const char *text, bool flushNow)
{
    if (text == nullptr || text[0] == '\0') return;
    size_t len = strlen(text);
    if (len > FRAME_MAX_PAYLOAD) len = FRAME_MAX_PAYLOAD;
    usbHandler.send(MSG_LOG_TEXT, 0,
                    reinterpret_cast<const uint8_t *>(text),
                    static_cast<uint16_t>(len),
                    flushNow);
}

void setup()
{
    initializeChirpBoot();
}

void loop()
{
    gLauncher.loop();
}
