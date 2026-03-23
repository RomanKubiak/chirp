extern "C" {
#include <stdint.h>
#include <sys/types.h>
clock_t _times(void *buf) { return 0; }
}

#include "wren.hpp"
#include "MIDI.h"
#include "midi_types.h"
#include "script_storage.h"
#include "usb_serial_handler.h"
#include "wren_midi_bridge.h"
#include "wren_runtime_script.h"
#include "wren_vm.h"
#include <cstring>
#include <cstdio>

// Component headers
#include "chirp_config.h"
#include "midi_router.h"
#include "runtime_log.h"
#include "wren_host.h"
#include "usb_frame_handler.h"
#include "chirp_fs.h"
#include "display_lvgl.h"

#if ENABLE_ST7735
#include <ST7735_t3.h>
#endif

// ── Serial MIDI port instances (must live in the sketch) ──────────────────────
MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI1);
MIDI_CREATE_INSTANCE(HardwareSerial, Serial2, MIDI2);
MIDI_CREATE_INSTANCE(HardwareSerial, Serial3, MIDI3);
MIDI_CREATE_INSTANCE(HardwareSerial, Serial4, MIDI4);
MIDI_CREATE_INSTANCE(HardwareSerial, Serial5, MIDI5);

// ── Application-wide globals ──────────────────────────────────────────────────
WrenConfiguration config;
WrenVM *vm;

#if DEBUG_RUNTIME_SERIAL
USBSerialHandler<usb_serial_class> usbHandler(Serial, &Serial);
#else
USBSerialHandler<usb_serial_class> usbHandler(Serial, nullptr);
#endif

ChirpFS internalFlash;
ScriptStorage scriptStorage(internalFlash);

// ── Control log sink (needs direct usbHandler access) ────────────────────────
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

// ── MIDI port configuration ───────────────────────────────────────────────────
static void initializeMidiPorts()
{
    MIDI1.begin(MIDI_CHANNEL_OMNI); MIDI1.turnThruOff();
    MIDI2.begin(MIDI_CHANNEL_OMNI); MIDI2.turnThruOff();
    MIDI3.begin(MIDI_CHANNEL_OMNI); MIDI3.turnThruOff();
    MIDI4.begin(MIDI_CHANNEL_OMNI); MIDI4.turnThruOff();
    MIDI5.begin(MIDI_CHANNEL_OMNI); MIDI5.turnThruOff();
}

// ─────────────────────────────────────────────────────────────────────────────
void setup()
{
    Serial.begin(115200);
    while (!Serial) delay(1);

    logSetup("[BOOT] Setup start");
    logCrashReportIfPresent();

    const bool storageReady = scriptStorage.begin();
    logSetup(storageReady
             ? "[BOOT] Script storage ready"
             : "[BOOT] Script storage unavailable, using embedded scripts");

    wrenInitConfiguration(&config);
    config.initialHeapSize   = WREN_INITIAL_HEAP_BYTES;
    config.minHeapSize       = WREN_MIN_HEAP_BYTES;
    config.heapGrowthPercent = WREN_HEAP_GROWTH_PCT;
    config.writeFn           = &writeFn;
    config.errorFn           = &errorFn;
    WrenMidiBridge::configure(config);
    WrenMidiBridge::setOutputSender(&enqueueMidiOutput);
    WrenMidiBridge::setRuntimeLogger(&logRuntime);

    // ── Wire filesystem provider for Wren File/Config API ─────────────────
    {
        WrenFsProvider fsp;

        fsp.read = [](const char *path, String &out) -> bool {
            File f = internalFlash.open(path, FILE_READ);
            if (!f) return false;
            out.reserve(f.size() + 1);
            while (f.available()) out += static_cast<char>(f.read());
            f.close();
            return true;
        };

        fsp.write = [](const char *path, const char *data, size_t len) -> bool {
            String p(path);
            int slash = p.lastIndexOf('/');
            if (slash > 0) {
                String dir = p.substring(0, slash);
                if (!internalFlash.exists(dir.c_str())) internalFlash.mkdir(dir.c_str());
            }
            if (internalFlash.exists(path)) internalFlash.remove(path);
            File f = internalFlash.open(path, FILE_WRITE);
            if (!f) return false;
            size_t written = f.write(reinterpret_cast<const uint8_t *>(data), len);
            f.flush(); f.close();
            return written == len;
        };

        fsp.remove = [](const char *path) -> bool {
            return internalFlash.exists(path) && internalFlash.remove(path);
        };

        fsp.exists = [](const char *path) -> bool {
            return internalFlash.exists(path);
        };

        fsp.size = [](const char *path) -> int32_t {
            File f = internalFlash.open(path, FILE_READ);
            if (!f) return -1;
            int32_t sz = static_cast<int32_t>(f.size());
            f.close();
            return sz;
        };

        fsp.list = [](const char *path, String *names, size_t maxNames) -> size_t {
            File dir = internalFlash.open(path);
            if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return 0; }
            size_t count = 0;
            while (true) {
                File entry = dir.openNextFile();
                if (!entry) break;
                String name = entry.name();
                int slash = name.lastIndexOf('/');
                if (slash >= 0) name = name.substring(slash + 1);
                entry.close();
                if (names && count < maxNames) names[count] = name;
                count++;
            }
            dir.close();
            return count;
        };

        WrenMidiBridge::setFsProvider(fsp);
    }

    vm = wrenNewVM(&config);

    bool runtimeReady   = initializeWrenRuntime();
    bool bridgeReady    = runtimeReady && WrenMidiBridge::initialize(vm);
    bool scriptsReady   = (runtimeReady && bridgeReady) && executeStoredWrenScriptsOnBoot();

    logSetup(runtimeReady  ? "[BOOT] Wren runtime ready"          : "[BOOT] Wren runtime failed");
    logSetup(bridgeReady   ? "[BOOT] Wren MIDI bridge ready"      : "[BOOT] Wren MIDI bridge unavailable");
    logSetup(scriptsReady  ? "[BOOT] /scripts Wren scripts ready" : "[BOOT] /scripts Wren scripts incomplete");

    displayLvglSetup();

    logDiagnosticSnapshot("post-init");

    initializeMidiPorts();
    setMidiOutQueueStats(0, 0, kMidiOutQueueSize - 1);

    logSetup("[BOOT] Teensy ready");
    logSetup("[BOOT] Setup complete");
    Serial.flush();
    delay(40);
}

// ─────────────────────────────────────────────────────────────────────────────
void loop()
{
    static constexpr uint16_t kMaxMidiEventsPerLoop    = 128;
    static constexpr uint32_t kMidiBudgetUs            = 1200;
    static constexpr uint8_t  kMaxControlFramesPerLoop = 8;

    const uint32_t loopStartUs = micros();
    bool hadWork = false;

    gDiag.loops = satAddU32(gDiag.loops, 1);
    logPeriodicDiagnostics();

    // ── Incoming MIDI (bounded by time budget) ────────────────────────────────
    bool midiBudgetExceeded = false;
    uint16_t processed = processMidiInput(kMaxMidiEventsPerLoop, kMidiBudgetUs, midiBudgetExceeded);
    if (processed > 0) hadWork = true;
    if (midiBudgetExceeded) gDiag.midiBudgetHits = satAddU32(gDiag.midiBudgetHits, 1);

    if (drainMidiOutputQueue() > 0) hadWork = true;

    debugMidi1RawBytes();
    debugMidiReadStatus();

    // ── USB control frames ────────────────────────────────────────────────────
    const uint32_t controlStartUs = micros();
    uint8_t handledFrames = processUsbControlFrames(kMaxControlFramesPerLoop);
    if (handledFrames > 0)
    {
        gDiag.controlFrames = satAddU32(gDiag.controlFrames, handledFrames);
        gDiag.controlUs     = satAddU32(gDiag.controlUs,
                                        static_cast<uint32_t>(micros() - controlStartUs));
        hadWork = true;
    }

    if (!hadWork) gDiag.idleLoops = satAddU32(gDiag.idleLoops, 1);

    displayLvglTask();

    yield();

    const uint32_t loopElapsedUs = static_cast<uint32_t>(micros() - loopStartUs);
    gDiag.loopBusyUs = satAddU32(gDiag.loopBusyUs, loopElapsedUs);
    if (loopElapsedUs > gDiag.loopMaxUs) gDiag.loopMaxUs = loopElapsedUs;
}
