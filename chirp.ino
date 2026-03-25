extern "C" {
#include <stdint.h>
#include <sys/types.h>
clock_t _times(void *buf) { return 0; }
}

#include "chirp_config.h"

#include <SPI.h>
#include <ST7735_t3.h>
#include <Encoder.h>
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

// ── Encoder instance (rotary encoder with gray-code debouncing) ───────────────
Encoder launcherEncoder(ENCODER_PIN_CLK, ENCODER_PIN_DT);

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
static bool     gLauncherRunningFlags[kLauncherMaxScripts] = {}; // per-script run state
static int16_t  gLauncherFocusIndex = 0; // item with display focus (scriptCount = MAIN)

static constexpr uint8_t kEncoderPinClk = ENCODER_PIN_CLK;
static constexpr uint8_t kEncoderPinDt = ENCODER_PIN_DT;
static constexpr uint8_t kEncoderPinSw = ENCODER_PIN_SW;

static void logLauncherDebug(const char *message); // forward declaration

static constexpr const char *kLauncherStatePath = "/userdata/launcher_state.json";
static constexpr size_t kBootAutoStartLimit = 1;

// Write /userdata/launcher_state.json
// Format: {"scripts":[{"name":"NDS","running":true,"error":false},...]}
static void launcherSaveState()
{
    String json;
    json.reserve(256);
    json = "{\"scripts\":[";
    for (size_t i = 0; i < gLauncherScriptCount; ++i) {
        if (i > 0) json += ",";
        json += "{\"name\":\"";
        json += gLauncherScripts[i];
        json += "\",\"running\":";
        // Only the focused script (if running) shall be restored on boot.
        // All others are saved as not running to prevent boot loops from multi-script auto-start.
        bool shouldRestore = (static_cast<int16_t>(i) == gLauncherFocusIndex) && gLauncherRunningFlags[i];
        json += shouldRestore ? "true" : "false";
        json += ",\"error\":";
        json += (gLauncherScriptError[i] != 0) ? "true" : "false";
        json += "}";
    }
    json += "]}";

    if (!internalFlash.exists("/userdata")) internalFlash.mkdir("/userdata");
    if (internalFlash.exists(kLauncherStatePath)) internalFlash.remove(kLauncherStatePath);
    File f = internalFlash.open(kLauncherStatePath, FILE_WRITE);
    if (f) {
        f.write(reinterpret_cast<const uint8_t *>(json.c_str()),
                static_cast<size_t>(json.length()));
        f.flush();
        f.close();
        logLauncherDebug("[LAUNCHER] state saved");
    } else {
        logLauncherDebug("[LAUNCHER] state save failed");
    }
}

// Parse launcher_state.json and start scripts that were running.
// Also restores error flags for scripts that had compile errors.
static bool launcherLoadAndAutoStart()
{
    if (!internalFlash.exists(kLauncherStatePath)) return false;
    File f = internalFlash.open(kLauncherStatePath, FILE_READ);
    if (!f) return false;
    String json;
    json.reserve(static_cast<size_t>(f.size()) + 1);
    while (f.available()) json += static_cast<char>(f.read());
    f.close();

    logLauncherDebug("[LAUNCHER] loading saved state");

    // Minimal hand-rolled parser: find each {"name":"X","running":B,"error":B}.
    int pos = 0;
    const int jlen = static_cast<int>(json.length());
    bool anyLoaded = false;
    size_t autoStarted = 0;

    while (pos < jlen) {
        int nameStart = json.indexOf("\"name\":\"", pos);
        if (nameStart < 0) break;
        nameStart += 8;
        int nameEnd = json.indexOf('"', nameStart);
        if (nameEnd < 0) break;
        String name = json.substring(nameStart, nameEnd);

        bool isRunning = false;
        int runKey = json.indexOf("\"running\":", nameEnd);
        if (runKey >= 0) {
            int rv = runKey + 10;
            isRunning = (json.substring(rv, rv + 4) == "true");
        }

        bool hadError = false;
        int errKey = json.indexOf("\"error\":", nameEnd);
        if (errKey >= 0) {
            int ev = errKey + 8;
            hadError = (json.substring(ev, ev + 4) == "true");
        }

        pos = nameEnd + 1;

        for (size_t i = 0; i < gLauncherScriptCount; ++i) {
            if (gLauncherScripts[i] != name) continue;
            char buf[96] = {0};
            if (hadError) {
                gLauncherScriptError[i] = 1;
                snprintf(buf, sizeof(buf), "[LAUNCHER] restored error: %s", name.c_str());
                logLauncherDebug(buf);
                anyLoaded = true;
            } else if (isRunning) {
                if (autoStarted >= kBootAutoStartLimit) {
                    snprintf(buf, sizeof(buf), "[LAUNCHER] boot auto-start skipped: %s", name.c_str());
                    logLauncherDebug(buf);
                    anyLoaded = true;
                    break;
                }

                // Transactional boot restore: persist a safe state before attempting
                // the script start. If this start crashes the MCU, the next boot will
                // not retry it automatically and the launcher will stay up.
                for (size_t clearIndex = 0; clearIndex < gLauncherScriptCount; ++clearIndex)
                    gLauncherRunningFlags[clearIndex] = false;
                launcherSaveState();

                snprintf(buf, sizeof(buf), "[LAUNCHER] auto-start: %s", name.c_str());
                logLauncherDebug(buf);
                if (runStoredWrenScript(name.c_str())) {
                    gLauncherRunningFlags[i] = true;
                    gLauncherScriptError[i]  = 0;
                    launcherSaveState();
                    logSetup("[BOOT] Launcher: auto-started script");
                    autoStarted++;
                } else {
                    gLauncherRunningFlags[i] = false;
                    gLauncherScriptError[i] = 1;
                    launcherSaveState();
                    snprintf(buf, sizeof(buf), "[LAUNCHER] auto-start failed: %s", name.c_str());
                    logLauncherDebug(buf);
                }
                anyLoaded = true;
            }
            break;
        }
    }
    return anyLoaded;
}
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
    unsigned runCount = 0;
    for (size_t i = 0; i < gLauncherScriptCount; ++i) if (gLauncherRunningFlags[i]) ++runCount;
    const char *focusName = (gLauncherFocusIndex >= 0 && static_cast<size_t>(gLauncherFocusIndex) < gLauncherScriptCount)
        ? gLauncherScripts[gLauncherFocusIndex].c_str()
        : "MAIN";
    snprintf(buffer, sizeof(buffer),
             "[LAUNCHER] %s total=%u selected=%u(%s) running=%u focus=%d(%s)",
             tag ? tag : "state",
             static_cast<unsigned>(total),
             static_cast<unsigned>(gLauncherSelectedIndex),
             selectedName,
             runCount,
             static_cast<int>(gLauncherFocusIndex),
             focusName);
    logLauncherDebug(buffer);
}

static int32_t launcherApproxFreeRamBytes()
{
    return -1;
}

static inline bool isScriptRunning(size_t index)
{
    return (index < gLauncherScriptCount) && gLauncherRunningFlags[index];
}
static inline bool isScriptFocused(size_t index)
{
    return gLauncherFocusIndex >= 0 && static_cast<size_t>(gLauncherFocusIndex) == index;
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
    const bool prevActive = (prev < gLauncherScriptCount) && isScriptRunning(prev);
    const bool currActive = (gLauncherSelectedIndex < gLauncherScriptCount) && isScriptRunning(gLauncherSelectedIndex);
    const bool nextActive = (next < gLauncherScriptCount) && isScriptRunning(next);
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

    // When focus is on a running script it owns the display content area.
    // Only update the launcher menu bar; the script drives its own content.
    const size_t focusIdx = (gLauncherFocusIndex >= 0)
        ? static_cast<size_t>(gLauncherFocusIndex)
        : gLauncherScriptCount; // treat negative as MAIN
    if (focusIdx < gLauncherScriptCount && isScriptRunning(focusIdx)) {
        renderLauncherMenuStatus();
        return;
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

    if (focusIdx < gLauncherScriptCount) {
        // Focused item is a script that is not running: show preview or error.
        if (gLauncherScriptError[focusIdx]) {
            const char *err = lastWrenScriptError();
            const char *errScript = lastWrenScriptErrorScriptName();
            if (errScript && strcmp(errScript, gLauncherScripts[focusIdx].c_str()) == 0) {
                char errLine[64] = {0};
                if (err && err[0]) snprintf(errLine, sizeof(errLine), "%.40s", err);
                ChirpDisplay::showLauncherPreview("ERROR", errLine, true);
            } else {
                ChirpDisplay::showLauncherPreview("ERROR", "COMPILE ERROR", true);
            }
        } else {
            ChirpDisplay::showLauncherPreview("NOT RUNNING");
        }
    } else {
        // MAIN or no-script focus: show system stats.
        ChirpDisplay::showSystemStats(line1, line2, line3, line4);
    }
    renderLauncherMenuStatus();
}

// Unload a running script and GC its heap. With stable module names the VM
// does not need to be restarted — reloading the same script later will
// overwrite the module's variable slots in-place.
static void stopScript(size_t idx)
{
    if (!vm) return;
    wrenInterpret(vm, "chirp_runtime", "Script.callUnload()\nMidi.clearListeners()");
    WrenMidiBridge::clearActiveScriptSelection();
    gLauncherRunningFlags[idx] = false;
    wrenCollectGarbage(vm); // old module-level objects are now unreachable; reclaim them
    char buf[96] = {0};
    snprintf(buf, sizeof(buf), "[LAUNCHER] stopped + GC: %s", gLauncherScripts[idx].c_str());
    logLauncherDebug(buf);
    logSetup("[BOOT] Launcher: script stopped");
    launcherSaveState();
}

static void launcherSwitchDisplayContext()
{
    // Short click: grab focus/display ownership for the selected item.
    // Never starts or stops scripts — only changes what is drawn.
    const size_t idx = gLauncherSelectedIndex;
    gLauncherFocusIndex = static_cast<int16_t>(idx);

    if (idx < gLauncherScriptCount) {
        // Give this script display ownership so its Display.* calls pass through.
        WrenMidiBridge::setActiveScriptName(gLauncherScripts[idx].c_str());
        char buf[128] = {0};
        if (isScriptRunning(idx)) {
            // Script is running: ask it to redraw its last state now.
            wrenInterpret(vm, "chirp_runtime", "Script.callFocus()");
            snprintf(buf, sizeof(buf), "[LAUNCHER] focus -> %s (running, redraw triggered)",
                     gLauncherScripts[idx].c_str());
        } else {
            snprintf(buf, sizeof(buf), "[LAUNCHER] focus -> %s (not running)",
                     gLauncherScripts[idx].c_str());
        }
        logLauncherDebug(buf);
    } else {
        // MAIN: no script owns the display.
        WrenMidiBridge::clearActiveScriptSelection();
        logLauncherDebug("[LAUNCHER] focus -> MAIN");
    }
    gLastLauncherStatsMs = 0;
    logLauncherSelection("single-click focus");
    refreshPreviewDisplay();
}

static void launcherToggleScriptRuntime()
{
    // Long press: start or stop the selected script.
    logLauncherSelection("long-press toggle");

    // MAIN entry: no-op.
    if (gLauncherSelectedIndex >= gLauncherScriptCount) {
        logLauncherDebug("[LAUNCHER] long-press on MAIN: no script to toggle");
        return;
    }

    const size_t idx = gLauncherSelectedIndex;
    const String scriptName = gLauncherScripts[idx];

    if (isScriptRunning(idx)) {
        // ── Stop ──
        stopScript(idx);
        // Redirect display focus if this script held it.
        if (isScriptFocused(idx)) {
            gLauncherFocusIndex = static_cast<int16_t>(gLauncherScriptCount); // → MAIN
            WrenMidiBridge::clearActiveScriptSelection();
            logLauncherDebug("[LAUNCHER] focus -> MAIN (stopped script lost focus)");
        } else if (gLauncherFocusIndex >= 0 &&
                   static_cast<size_t>(gLauncherFocusIndex) < gLauncherScriptCount &&
                   isScriptRunning(static_cast<size_t>(gLauncherFocusIndex))) {
            WrenMidiBridge::setActiveScriptName(
                gLauncherScripts[static_cast<size_t>(gLauncherFocusIndex)].c_str());
        }
        gLastLauncherStatsMs = 0;
        refreshPreviewDisplay();
        return;
    }

    // ── Start: run the script and immediately grab focus ──────────────────────
    if (!runStoredWrenScript(scriptName.c_str())) {
        logSetup("[BOOT] Launcher: script start failed");
        gLauncherScriptError[idx] = 1;
        char buf[128] = {0};
        snprintf(buf, sizeof(buf), "[LAUNCHER] start failed: %s", scriptName.c_str());
        logLauncherDebug(buf);
        launcherSaveState();
        gLastLauncherStatsMs = 0;
        refreshPreviewDisplay();
        return;
    }

    gLauncherRunningFlags[idx] = true;
    gLauncherScriptError[idx]  = 0;
    // Long-press start also grabs display focus immediately.
    gLauncherFocusIndex = static_cast<int16_t>(idx);
    WrenMidiBridge::setActiveScriptName(scriptName.c_str());
    char buf[128] = {0};
    snprintf(buf, sizeof(buf), "[LAUNCHER] started + focus: %s", scriptName.c_str());
    logLauncherDebug(buf);
    logSetup("[BOOT] Launcher: script started");
    launcherSaveState();
    gLastLauncherStatsMs = 0;
    refreshPreviewDisplay();
}

static bool launcherHandleNavigation(bool prev, bool next, bool select, bool longPress)
{
    if (!prev && !next && !select && !longPress) return false;

    const size_t total = gLauncherScriptCount + 1;
    if (total == 0) return true;

    // Handle navigation only: update menu selection, no side effects.
    if (prev || next) {
        const size_t previousSelection = gLauncherSelectedIndex;
        if (prev) gLauncherSelectedIndex = (gLauncherSelectedIndex + total - 1) % total;
        if (next) gLauncherSelectedIndex = (gLauncherSelectedIndex + 1) % total;

        if (gLauncherSelectedIndex == gLauncherScriptCount) {
            logLauncherDebug("[LAUNCHER] reached MAIN item");
        }

        char navBuffer[192] = {0};
        snprintf(navBuffer, sizeof(navBuffer),
                 "[ENCODER] nav: prev=%u next=%u before=%u selected=%u total=%u",
                 prev ? 1U : 0U,
                 next ? 1U : 0U,
                 static_cast<unsigned>(previousSelection),
                 static_cast<unsigned>(gLauncherSelectedIndex),
                 static_cast<unsigned>(total));
        logLauncherDebug(navBuffer);

        // Navigation does NOT auto-deactivate running script.
        // Do NOT call any deactivate function here.
        gLastLauncherStatsMs = 0;
        refreshPreviewDisplay();
        logLauncherSelection("nav-only");
        return true;
    }

    // Handle button actions: single click or long press.
    // NOTE: navSelect (select) and longPress are mutually exclusive - each event sets only one.
    if (select || longPress) {
        if (longPress) {
            launcherToggleScriptRuntime();
        } else {
            launcherSwitchDisplayContext();
        }
        logLauncherSelection("button-action");
        return true;
    }

    return false;
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

    return launcherHandleNavigation(prev, next, select, false);
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
    for (size_t i = 0; i < kLauncherMaxScripts; ++i) gLauncherRunningFlags[i] = false;
    gLauncherFocusIndex = static_cast<int16_t>(gLauncherScriptCount); // default focus = MAIN

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

    // Restore and auto-start scripts from last session.
    if (launcherLoadAndAutoStart()) {
        // Give display focus to the first successfully auto-started script.
        bool focusSet = false;
        for (size_t i = 0; i < gLauncherScriptCount; ++i) {
            if (gLauncherRunningFlags[i]) {
                gLauncherFocusIndex = static_cast<int16_t>(i);
                WrenMidiBridge::setActiveScriptName(gLauncherScripts[i].c_str());
                wrenInterpret(vm, "chirp_runtime", "Script.callFocus()");
                focusSet = true;
                break;
            }
        }
        if (!focusSet) {
            // All restored scripts had errors — show MAIN/system stats.
            gLauncherFocusIndex = static_cast<int16_t>(gLauncherScriptCount);
        }
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
    // Encoder library handles CLK and DT pin setup internally
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
    static int32_t lastEncoderPosition = 0;
    static bool lastButtonPressed = false;
    static uint32_t buttonDebounceStartMs = 0;
    static constexpr uint32_t kButtonDebounceMs = 25;
    static uint32_t buttonPressStartMs = 0;
    static constexpr uint32_t kLongPressThresholdMs = 300;
    
    // Button action flags (set during loop, used for navigation)
    bool navPrev = false;
    bool navNext = false;
    bool navSelect = false;
    bool longPress = false;

    gDiag.loops = satAddU32(gDiag.loops, 1);
    logPeriodicDiagnostics();

    // ── Incoming MIDI (bounded by time budget) ────────────────────────────────
    bool midiBudgetExceeded = false;
    uint16_t processed = processMidiInput(kMaxMidiEventsPerLoop, kMidiBudgetUs, midiBudgetExceeded);
    if (processed > 0) hadWork = true;
    if (midiBudgetExceeded) gDiag.midiBudgetHits = satAddU32(gDiag.midiBudgetHits, 1);

    if (drainMidiOutputQueue() > 0) hadWork = true;

    {
        // Read encoder position using Teensy Encoder library (uses hardware interrupts).
        // One mechanical detent = 4 internal counts; only register when delta reaches ±4.
        const int32_t currentPosition = launcherEncoder.read();
        const int32_t positionDelta = currentPosition - lastEncoderPosition;

        if (positionDelta >= 4) {
            // Encoder rotated clockwise: navigate right.
            navNext = true;
            hadWork = true;
            char buffer[96] = {0};
            snprintf(buffer, sizeof(buffer), "[ENCODER] cw position=%ld delta=%ld",
                     static_cast<long>(currentPosition),
                     static_cast<long>(positionDelta));
            logLauncherDebug(buffer);
            lastEncoderPosition += 4;  // Consume one detent
        } else if (positionDelta <= -4) {
            // Encoder rotated counter-clockwise: navigate left.
            navPrev = true;
            hadWork = true;
            char buffer[96] = {0};
            snprintf(buffer, sizeof(buffer), "[ENCODER] ccw position=%ld delta=%ld",
                     static_cast<long>(currentPosition),
                     static_cast<long>(positionDelta));
            logLauncherDebug(buffer);
            lastEncoderPosition -= 4;  // Consume one detent
        }
    }

    {
        const bool pressed = (digitalReadFast(kEncoderPinSw) == LOW);
        const uint32_t nowMs = millis();
        if (pressed != lastButtonPressed) {
            if ((nowMs - buttonDebounceStartMs) >= kButtonDebounceMs) {
                buttonDebounceStartMs = nowMs;
                lastButtonPressed = pressed;
                if (pressed) {
                    buttonPressStartMs = nowMs;
                    logLauncherDebug("[ENCODER] button pressed");
                    hadWork = true;
                } else {
                    // Button released: determine if it was long press.
                    const uint32_t pressDurationMs = nowMs - buttonPressStartMs;
                    if (pressDurationMs >= kLongPressThresholdMs) {
                        longPress = true;
                        char buffer[96] = {0};
                        snprintf(buffer, sizeof(buffer), "[ENCODER] long press (%lums)", 
                                 static_cast<unsigned long>(pressDurationMs));
                        logLauncherDebug(buffer);
                    } else {
                        navSelect = true;
                        logLauncherDebug("[ENCODER] short click");
                    }
                    hadWork = true;
                }
            }
        } else {
            buttonDebounceStartMs = nowMs;
        }
    }

    launcherHandleNavigation(navPrev, navNext, navSelect, longPress);

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

    // Periodically refresh when focus is not on a running script.
    // A running script owns the display and drives it via MIDI events.
    {
        const bool focusedScriptIsRunning =
            (gLauncherFocusIndex >= 0 &&
             static_cast<size_t>(gLauncherFocusIndex) < gLauncherScriptCount &&
             isScriptRunning(static_cast<size_t>(gLauncherFocusIndex)));
        if (!focusedScriptIsRunning) {
            static uint32_t sLastLoopRefreshMs = 0;
            const uint32_t nowLoopMs = millis();
            if (nowLoopMs - sLastLoopRefreshMs >= kSystemStatsRefreshIntervalMs) {
                sLastLoopRefreshMs = nowLoopMs;
                refreshPreviewDisplay();
            }
        }
    }

    yield();

    const uint32_t loopElapsedUs = static_cast<uint32_t>(micros() - loopStartUs);
    gDiag.loopBusyUs = satAddU32(gDiag.loopBusyUs, loopElapsedUs);
    if (loopElapsedUs > gDiag.loopMaxUs) gDiag.loopMaxUs = loopElapsedUs;
}
