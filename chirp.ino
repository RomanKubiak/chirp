extern "C" {
#include <stdint.h>
#include <sys/types.h>
clock_t _times(void *buf) { return 0; }
}

#include "chirp_config.h"

#include <SPI.h>
#include <ST7735_t3.h>
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
#include <sys/types.h>

#if ENABLE_LIVE_DEBUG
#include <TeensyDebug.h>
#pragma GCC optimize ("O0")
#endif

// Component headers
#include "midi_router.h"
#include "runtime_log.h"
#include "wren_host.h"
#include "usb_frame_handler.h"
#include "chirp_display.h"
#include "chirp_fs.h"

// ── Serial MIDI port instances (must live in the sketch) ──────────────────────
MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI1);
MIDI_CREATE_INSTANCE(HardwareSerial, Serial2, MIDI2);
MIDI_CREATE_INSTANCE(HardwareSerial, Serial3, MIDI3);
MIDI_CREATE_INSTANCE(HardwareSerial, Serial4, MIDI4);
MIDI_CREATE_INSTANCE(HardwareSerial, Serial5, MIDI5);

// ── Display instance (ST7735) ────────────────────────────────────────────────
ST7735_t3 display(DISPLAY_CS, DISPLAY_DC, DISPLAY_RST);

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

// ── Launcher state (preview + toggle) ───────────────────────────────────────
static constexpr size_t kLauncherMaxScripts = 32;
static constexpr uint32_t kSystemStatsRefreshIntervalMs = 1000;
static constexpr uint32_t kSystemScreenPageIntervalMs = 5000;
String gLauncherScripts[kLauncherMaxScripts];
int16_t gLauncherScriptError[kLauncherMaxScripts] = {0};
size_t gLauncherScriptCount = 0;
size_t gLauncherSelectedIndex = 0;
uint32_t gLastLauncherStatsMs = 0;
uint32_t gLastMidiRateSampleUs = 0;
uint32_t gLastMidiRateEvents = 0;
uint32_t gLastMidiEventsPerSec = 0;
static int16_t gLauncherRunningIndex = -1;

static constexpr uint8_t kEncoderPinClk = ENCODER_PIN_CLK;
static constexpr uint8_t kEncoderPinDt = ENCODER_PIN_DT;
static constexpr uint8_t kEncoderPinSw = ENCODER_PIN_SW;

static void logLauncherDebug(const char *message)
{
    if (message == nullptr || message[0] == '\0') return;
    logSetup(message);
}

static void logLauncherSelection(const char *tag)
{
    char buffer[192] = {0};
    const size_t total = gLauncherScriptCount + 1;
    const char *selectedName = (gLauncherSelectedIndex < gLauncherScriptCount)
        ? gLauncherScripts[gLauncherSelectedIndex].c_str()
        : "MAIN";
    const char *runningName = (gLauncherRunningIndex >= 0 && static_cast<size_t>(gLauncherRunningIndex) < gLauncherScriptCount)
        ? gLauncherScripts[gLauncherRunningIndex].c_str()
        : "none";
    snprintf(buffer, sizeof(buffer),
             "[LAUNCHER] %s total=%u selected=%u(%s) running=%d(%s)",
             tag ? tag : "state",
             static_cast<unsigned>(total),
             static_cast<unsigned>(gLauncherSelectedIndex),
             selectedName,
             static_cast<int>(gLauncherRunningIndex),
             runningName);
    logLauncherDebug(buffer);
}

static int32_t launcherApproxFreeRamBytes()
{
    return -1;
}

static inline bool isScriptActive(size_t index)
{
    return gLauncherRunningIndex >= 0 &&
           static_cast<size_t>(gLauncherRunningIndex) == index;
}

static String launcherEntryName(size_t index)
{
    if (index < gLauncherScriptCount) return gLauncherScripts[index];
    return "MAIN";
}

static void renderLauncherMenuStatus()
{
    const size_t total = gLauncherScriptCount + 1;
    if (total == 0) return;

    const size_t prev = (gLauncherSelectedIndex + total - 1) % total;
    const size_t next = (gLauncherSelectedIndex + 1) % total;
    const String prevName = launcherEntryName(prev);
    const String currName = launcherEntryName(gLauncherSelectedIndex);
    const String nextName = launcherEntryName(next);

    // MAIN entry (index == scriptCount) has no active or error state.
    const bool prevActive = (prev < gLauncherScriptCount) && isScriptActive(prev);
    const bool currActive = (gLauncherSelectedIndex < gLauncherScriptCount) && isScriptActive(gLauncherSelectedIndex);
    const bool nextActive = (next < gLauncherScriptCount) && isScriptActive(next);
    const bool prevError = (prev < gLauncherScriptCount) && gLauncherScriptError[prev];
    const bool currError = (gLauncherSelectedIndex < gLauncherScriptCount) && gLauncherScriptError[gLauncherSelectedIndex];
    const bool nextError = (next < gLauncherScriptCount) && gLauncherScriptError[next];

    ChirpDisplay::showLauncherMenu(
        prevName.c_str(), currName.c_str(), nextName.c_str(),
        prevActive, currActive, nextActive,
        prevError, currError, nextError);

    char buffer[192] = {0};
    snprintf(buffer, sizeof(buffer),
             "[LAUNCHER] menu prev=%u(%s) curr=%u(%s) next=%u(%s)",
             static_cast<unsigned>(prev), prevName.c_str(),
             static_cast<unsigned>(gLauncherSelectedIndex), currName.c_str(),
             static_cast<unsigned>(next), nextName.c_str());
    logLauncherDebug(buffer);
}

static void refreshPreviewDisplay()
{
    const uint32_t now = millis();
    const bool activeScriptVisible = (gLauncherRunningIndex >= 0);
    if (activeScriptVisible) {
        if ((now - gLastLauncherStatsMs) < kSystemStatsRefreshIntervalMs) return;
        gLastLauncherStatsMs = now;
    }
    const uint32_t nowUs = micros();

    const int32_t freeRam = launcherApproxFreeRamBytes();
    const uint32_t midiEvents = gDiag.midiEvents;

    if (gLastMidiRateSampleUs == 0) {
        gLastMidiRateSampleUs = nowUs;
        gLastMidiRateEvents = midiEvents;
        gLastMidiEventsPerSec = 0;
    } else {
        uint32_t sampleWindowUs = static_cast<uint32_t>(nowUs - gLastMidiRateSampleUs);
        if (sampleWindowUs == 0) sampleWindowUs = 1;
        if (midiEvents >= gLastMidiRateEvents) {
            const uint32_t deltaEvents = midiEvents - gLastMidiRateEvents;
            gLastMidiEventsPerSec = static_cast<uint32_t>(
                (static_cast<uint64_t>(deltaEvents) * 1000000ULL) / sampleWindowUs);
        } else {
            // Counters were reset or wrapped; re-baseline on this sample.
            gLastMidiEventsPerSec = 0;
        }
        gLastMidiRateSampleUs = nowUs;
        gLastMidiRateEvents = midiEvents;
    }

    const unsigned long wrenBytes = vm ? static_cast<unsigned long>(vm->bytesAllocated) : 0;
    const unsigned long nextGc = vm ? static_cast<unsigned long>(vm->nextGC) : 0;
    const unsigned long nowMs = static_cast<unsigned long>(now);
    const unsigned long totalSeconds = nowMs / 1000UL;
    const unsigned long hh = (totalSeconds / 3600UL) % 100UL;
    const unsigned long mm = (totalSeconds / 60UL) % 60UL;
    const unsigned long ss = totalSeconds % 60UL;
    const unsigned long mmmm = nowMs % 10000UL;
    const char *displayMode = ChirpDisplay::displayModeName();
    const char *displayModeShort = "dir";
    if (strcmp(displayMode, "framebuffer+sync") == 0) displayModeShort = "fb+s";
    else if (strcmp(displayMode, "framebuffer+async") == 0) displayModeShort = "fb+a";
    const char *activeScript = lastLoadedWrenScriptName();
    const char *activeModule = lastLoadedWrenModuleName();
    const char *lastError = lastWrenScriptError();
    const char *lastErrorScript = lastWrenScriptErrorScriptName();
    const uint32_t scriptMemBytes = lastLoadedWrenScriptBytes();
    const uint32_t scriptLoadsOk = totalWrenScriptLoadSuccesses();
    const uint32_t scriptLoadsErr = totalWrenScriptLoadErrors();
    const uint32_t page = (now / kSystemScreenPageIntervalMs) % 2;

    char line1[32] = {0};
    char line2[32] = {0};
    char line3[32] = {0};
    char line4[32] = {0};

    if (page == 0) {
        snprintf(line1, sizeof(line1), "RAM:%ldK WREN:%luK",
                 static_cast<long>(freeRam / 1024),
                 static_cast<unsigned long>(wrenBytes / 1024));
        snprintf(line2, sizeof(line2), "GC:%luK mEv/s:%lu",
                 static_cast<unsigned long>(nextGc / 1024),
                 static_cast<unsigned long>(gLastMidiEventsPerSec));
        snprintf(line3, sizeof(line3), "DSP:%s Q:%u/%u",
                 displayModeShort,
                 static_cast<unsigned>(gMidiOutQueueDepth),
                 static_cast<unsigned>(gMidiOutQueueCapacity));
        snprintf(line4, sizeof(line4), "%02lu:%02lu:%02lu:%04lu", hh, mm, ss, mmmm);
    } else {
        snprintf(line1, sizeof(line1), "SCR:%s (%lu)",
                 (activeScript && activeScript[0]) ? activeScript : "none",
                 static_cast<unsigned long>(gLauncherScriptCount));
        snprintf(line2, sizeof(line2), "MOD:%s", (activeModule && activeModule[0]) ? activeModule : "-");
        snprintf(line3, sizeof(line3), "MEM:%luK ok:%lu err:%lu",
                 static_cast<unsigned long>(scriptMemBytes / 1024),
                 static_cast<unsigned long>(scriptLoadsOk),
                 static_cast<unsigned long>(scriptLoadsErr));
        if (lastError && lastError[0]) {
            (void)lastErrorScript;
            snprintf(line4, sizeof(line4), "ERR:%s", lastError);
        } else {
            snprintf(line4, sizeof(line4), "ERR:none");
        }
    }

    if (gLauncherRunningIndex >= 0) {
        // Active script: show system stats with launcher menu
        ChirpDisplay::showSystemStats(line1, line2, line3, line4);
    } else if (gLauncherSelectedIndex < gLauncherScriptCount) {
        // Preview inactive script: show NOT RUNNING or error
        if (gLauncherScriptError[gLauncherSelectedIndex]) {
            const char *err = lastWrenScriptError();
            const char *errScript = lastWrenScriptErrorScriptName();
            // Show error if it matches this script
            if (errScript && strcmp(errScript, gLauncherScripts[gLauncherSelectedIndex].c_str()) == 0) {
                char errLine[64] = {0};
                if (err && err[0]) {
                    snprintf(errLine, sizeof(errLine), "%.40s", err);
                }
                ChirpDisplay::showLauncherPreview("ERROR", errLine, true);
            } else {
                ChirpDisplay::showLauncherPreview("ERROR", "COMPILE ERROR", true);
            }
        } else {
            ChirpDisplay::showLauncherPreview("NOT RUNNING");
        }
    } else {
        // MAIN entry: show system stats
        ChirpDisplay::showSystemStats(line1, line2, line3, line4);
    }
    renderLauncherMenuStatus();
}

static void launcherToggleSelectedScript()
{
    logLauncherSelection("toggle-enter");
    // MAIN entry: deactivate any running script.
    if (gLauncherSelectedIndex >= gLauncherScriptCount) {
        if (gLauncherRunningIndex >= 0) {
            wrenInterpret(vm, "chirp_runtime", "Script.callUnload()\nMidi.clearListeners()");
            WrenMidiBridge::clearActiveScriptSelection();
            gLauncherRunningIndex = -1;
            logSetup("[BOOT] Launcher: script deactivated");
            logLauncherDebug("[LAUNCHER] MAIN selected, deactivated running script");
        }
        refreshPreviewDisplay();
        return;
    }

    // Toggle off: if this script is currently active, deactivate it.
    if (isScriptActive(gLauncherSelectedIndex)) {
        wrenInterpret(vm, "chirp_runtime", "Script.callUnload()\nMidi.clearListeners()");
        WrenMidiBridge::clearActiveScriptSelection();
        gLauncherRunningIndex = -1;
        logSetup("[BOOT] Launcher: script deactivated");
        logLauncherDebug("[LAUNCHER] toggled active script off");
        refreshPreviewDisplay();
        return;
    }

    // Toggle on: activate the selected script.
    const String scriptName = gLauncherScripts[gLauncherSelectedIndex];
    
    // Unload any currently running script before loading the new one.
    // runWrenUserScriptSource() now relies on the launcher to handle this.
    if (gLauncherRunningIndex >= 0) {
        wrenInterpret(vm, "chirp_runtime", "Script.callUnload()\nMidi.clearListeners()");
        WrenMidiBridge::clearActiveScriptSelection();
        gLauncherRunningIndex = -1;
    }

    if (!runStoredWrenScript(scriptName.c_str())) {
        logSetup("[BOOT] Launcher: script start failed");
        gLauncherRunningIndex = -1;
        gLauncherScriptError[gLauncherSelectedIndex] = 1;  // Mark as error
        char buffer[128] = {0};
        snprintf(buffer, sizeof(buffer), "[LAUNCHER] script start failed: %s", scriptName.c_str());
        logLauncherDebug(buffer);
        refreshPreviewDisplay();
        return;
    }

    gLauncherRunningIndex = static_cast<int16_t>(gLauncherSelectedIndex);
    gLauncherScriptError[gLauncherSelectedIndex] = 0;  // Clear error on successful load
    char buffer[128] = {0};
    snprintf(buffer, sizeof(buffer), "[LAUNCHER] script activated: %s", scriptName.c_str());
    logLauncherDebug(buffer);
    renderLauncherMenuStatus();
}

static void launcherDeactivateRunningScript()
{
    if (gLauncherRunningIndex < 0) return;

    logLauncherSelection("deactivate-running");
    wrenInterpret(vm, "chirp_runtime", "Script.callUnload()\nMidi.clearListeners()");
    WrenMidiBridge::clearActiveScriptSelection();
    gLauncherRunningIndex = -1;
}

static bool launcherHandleNavigation(bool prev, bool next, bool select)
{
    if (!prev && !next && !select) return false;

    const size_t total = gLauncherScriptCount + 1;
    if (total == 0) return true;

    const size_t previousSelection = gLauncherSelectedIndex;

    char navBuffer[192] = {0};
    snprintf(navBuffer, sizeof(navBuffer),
             "[ENCODER] prev=%u next=%u select=%u before=%u total=%u",
             prev ? 1U : 0U,
             next ? 1U : 0U,
             select ? 1U : 0U,
             static_cast<unsigned>(previousSelection),
             static_cast<unsigned>(total));
    logLauncherDebug(navBuffer);

    if (prev) gLauncherSelectedIndex = (gLauncherSelectedIndex + total - 1) % total;
    if (next) gLauncherSelectedIndex = (gLauncherSelectedIndex + 1) % total;

    if ((prev || next) && gLauncherSelectedIndex == gLauncherScriptCount) {
        logLauncherDebug("[LAUNCHER] reached MAIN item");
    }

    if ((prev || next) &&
        gLauncherRunningIndex >= 0 &&
        static_cast<size_t>(gLauncherRunningIndex) == previousSelection &&
        gLauncherSelectedIndex != previousSelection) {
        launcherDeactivateRunningScript();
    }

    if (select) {
        launcherToggleSelectedScript();
    } else {
        gLastLauncherStatsMs = 0;
        logLauncherDebug("[LAUNCHER] forced preview refresh from navigation");
        refreshPreviewDisplay();
    }

    logLauncherSelection("nav-exit");
    return true;
}

static bool launcherHandleMidiControl(const MIDIMessage &event)
{
    if (event.port != 1) return false;

    bool prev = false;
    bool next = false;
    bool select = false;

    if (event.type == 0x90 && event.data2 > 0) {
        if (event.data1 == 98) prev = true;       // MCU cursor_left
        if (event.data1 == 99) next = true;       // MCU cursor_right
        if (event.data1 == 83 || event.data1 == 94) select = true; // Enter or Play
    }

    if (event.type == 0xB0 && event.data1 == 60) {
        if (event.data2 & 0x40) prev = true;      // jog ccw
        else next = true;                         // jog cw
    }

    return launcherHandleNavigation(prev, next, select);
}

static void initializeLauncherState()
{
    gLauncherScriptCount = 0;
    const size_t listed = listStoredWrenScripts(gLauncherScripts, kLauncherMaxScripts);
    gLauncherScriptCount = (listed < kLauncherMaxScripts) ? listed : kLauncherMaxScripts;
    
    // Clear error flags
    for (size_t i = 0; i < kLauncherMaxScripts; i++) {
        gLauncherScriptError[i] = 0;
    }
    
    gLauncherSelectedIndex = 0;
    gLauncherRunningIndex = -1;

    // On launcher boot, no script should own drawing until one is selected.
    WrenMidiBridge::clearActiveScriptSelection();
    char buffer[192] = {0};
    snprintf(buffer, sizeof(buffer), "[LAUNCHER] init listed=%u selected=%u bootScript=%s",
             static_cast<unsigned>(gLauncherScriptCount),
             static_cast<unsigned>(gLauncherSelectedIndex),
             bootDisplayScriptName() ? bootDisplayScriptName() : "");
    logLauncherDebug(buffer);
    for (size_t i = 0; i < gLauncherScriptCount; ++i) {
        snprintf(buffer, sizeof(buffer), "[LAUNCHER] script[%u]=%s",
                 static_cast<unsigned>(i), gLauncherScripts[i].c_str());
        logLauncherDebug(buffer);
    }
    refreshPreviewDisplay();
}

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

// ── Display initialization ────────────────────────────────────────────────────
static void initializeDisplay()
{
    SPI.begin();
    
    // Initialize ST7735 1.8" display (Teensy optimized with DMA)
    display.initR(INITR_BLACKTAB);
    
    // Initialize ChirpDisplay UI framework
    display.setRotation(1);
    ChirpDisplay::init();
    ChirpDisplay::showStatus("Launcher");

    {
        char displayModeLine[64] = {0};
        snprintf(displayModeLine, sizeof(displayModeLine), "[BOOT] Display mode: %s", ChirpDisplay::displayModeName());
        logSetup(displayModeLine);
    }
    
    logSetup("[BOOT] Display initialized");
}

// ─────────────────────────────────────────────────────────────────────────────
void setup()
{
#if ENABLE_LIVE_DEBUG
    debug.begin(SerialUSB1);
#endif
    Serial.begin(115200);
    {
        const uint32_t serialDeadlineMs = millis() + SERIAL_WAIT_TIMEOUT_MS;
        while (!Serial && millis() < serialDeadlineMs) delay(1);
    }

    logSetup("[BOOT] Setup start");
    logCrashReportIfPresent();

    const bool storageReady = scriptStorage.begin();
    if (storageReady) {
        char storageLine[64] = {0};
        snprintf(storageLine, sizeof(storageLine), "[BOOT] Script storage ready (%s)", internalFlash.backendName());
        logSetup(storageLine);
        char storageDiag[128] = {0};
        snprintf(storageDiag, sizeof(storageDiag), "[BOOT] Storage detail: %s", internalFlash.storageDiagnostic());
        logSetup(storageDiag);
    } else {
        logSetup("[BOOT] Script storage unavailable, using embedded scripts");
        char storageDiag[128] = {0};
        snprintf(storageDiag, sizeof(storageDiag), "[BOOT] Storage detail: %s", internalFlash.storageDiagnostic());
        logSetup(storageDiag);
    }

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
    bool scriptsReady   = (runtimeReady && bridgeReady) && prepareStoredWrenScriptsOnBoot();

    logSetup(runtimeReady  ? "[BOOT] Wren runtime ready"          : "[BOOT] Wren runtime failed");
    logSetup(bridgeReady   ? "[BOOT] Wren MIDI bridge ready"      : "[BOOT] Wren MIDI bridge unavailable");
    logSetup(scriptsReady  ? "[BOOT] /scripts Wren scripts ready" : "[BOOT] /scripts Wren scripts incomplete");

    logDiagnosticSnapshot("post-init");

    initializeDisplay();
    pinMode(kEncoderPinClk, INPUT_PULLUP);
    pinMode(kEncoderPinDt, INPUT_PULLUP);
    pinMode(kEncoderPinSw, INPUT_PULLUP);
    initializeMidiPorts();
    setMidiOutQueueStats(0, 0, kMidiOutQueueSize - 1);
    setMidiPreDispatchHook(launcherHandleMidiControl);
    initializeLauncherState();

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
    static uint8_t lastEncoderState = 0;
    static bool encoderStateInitialized = false;
    static bool lastButtonPressed = false;
    static uint32_t buttonDebounceStartMs = 0;
    static constexpr uint32_t kButtonDebounceMs = 25;

    if (!encoderStateInitialized) {
        const uint8_t clk = digitalReadFast(kEncoderPinClk) ? 1 : 0;
        const uint8_t dt = digitalReadFast(kEncoderPinDt) ? 1 : 0;
        lastEncoderState = static_cast<uint8_t>((clk << 1) | dt);
        encoderStateInitialized = true;
    }

    gDiag.loops = satAddU32(gDiag.loops, 1);
    logPeriodicDiagnostics();

    // ── Incoming MIDI (bounded by time budget) ────────────────────────────────
    bool midiBudgetExceeded = false;
    uint16_t processed = processMidiInput(kMaxMidiEventsPerLoop, kMidiBudgetUs, midiBudgetExceeded);
    if (processed > 0) hadWork = true;
    if (midiBudgetExceeded) gDiag.midiBudgetHits = satAddU32(gDiag.midiBudgetHits, 1);

    if (drainMidiOutputQueue() > 0) hadWork = true;

    bool navPrev = false;
    bool navNext = false;
    {
        const uint8_t clk = digitalReadFast(kEncoderPinClk) ? 1 : 0;
        const uint8_t dt = digitalReadFast(kEncoderPinDt) ? 1 : 0;
        const uint8_t state = static_cast<uint8_t>((clk << 1) | dt);
        const uint8_t transition = static_cast<uint8_t>((lastEncoderState << 2) | state);

        // Only fire on arrival at the detent resting state (both pins HIGH = 0b11).
        // A mechanical encoder makes 4 gray-code transitions per click; restricting
        // to the final landing edge gives exactly one event per physical detent.
        //   CW : ...→ 01 → 11  (transition 0b0111)
        //   CCW: ...→ 10 → 11  (transition 0b1011)
        if (state == 0b11) {
            if (transition == 0b0111) {
                navNext = true;
                hadWork = true;
                char buffer[96] = {0};
                snprintf(buffer, sizeof(buffer), "[ENCODER] cw transition=%u state=%u",
                         static_cast<unsigned>(transition),
                         static_cast<unsigned>(state));
                logLauncherDebug(buffer);
            } else if (transition == 0b1011) {
                navPrev = true;
                hadWork = true;
                char buffer[96] = {0};
                snprintf(buffer, sizeof(buffer), "[ENCODER] ccw transition=%u state=%u",
                         static_cast<unsigned>(transition),
                         static_cast<unsigned>(state));
                logLauncherDebug(buffer);
            }
        }

        if (state != lastEncoderState) {
            char buffer[96] = {0};
            snprintf(buffer, sizeof(buffer), "[ENCODER] raw last=%u state=%u transition=%u",
                     static_cast<unsigned>(lastEncoderState),
                     static_cast<unsigned>(state),
                     static_cast<unsigned>(transition));
            logLauncherDebug(buffer);
        }

        lastEncoderState = state;
    }

    bool navSelect = false;
    {
        const bool pressed = (digitalReadFast(kEncoderPinSw) == LOW);
        const uint32_t nowMs = millis();
        if (pressed != lastButtonPressed) {
            if ((nowMs - buttonDebounceStartMs) >= kButtonDebounceMs) {
                buttonDebounceStartMs = nowMs;
                lastButtonPressed = pressed;
                if (pressed) {
                    navSelect = true;
                    hadWork = true;
                    logLauncherDebug("[ENCODER] button pressed");
                }
            }
        } else {
            buttonDebounceStartMs = nowMs;
        }
    }

    launcherHandleNavigation(navPrev, navNext, navSelect);

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

    // Only keep refreshing while an active script owns the display.
    if (gLauncherRunningIndex >= 0) {
        refreshPreviewDisplay();
    }

    yield();

    const uint32_t loopElapsedUs = static_cast<uint32_t>(micros() - loopStartUs);
    gDiag.loopBusyUs = satAddU32(gDiag.loopBusyUs, loopElapsedUs);
    if (loopElapsedUs > gDiag.loopMaxUs) gDiag.loopMaxUs = loopElapsedUs;
}
