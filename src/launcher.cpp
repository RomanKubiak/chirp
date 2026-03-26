#include "launcher.h"

#include "chirp_display.h"
#include "chirp_config.h"
#include "chirp_fs.h"
#include "dirty_text_ui.h"
#include "midi_router.h"
#include "runtime_log.h"
#include "script_storage.h"
#include "usb_frame_handler.h"
#include "wren_host.h"
#include "wren_midi_bridge.h"
#include "wren_vm.h"
#include "wren.hpp"

#include <cstdio>
#include <cstring>

extern WrenVM *vm;
extern ScriptStorage scriptStorage;
extern ChirpFS internalFlash;
extern RuntimeDiagCounters gDiag;

Launcher gLauncher;

Launcher::Launcher()
    : launcherEncoder_(ENCODER_PIN_CLK, ENCODER_PIN_DT)
{
}

void queueLauncherNavigation(bool prev, bool next, bool select, bool longPress)
{
    gLauncher.queueNavigation(prev, next, select, longPress);
}

bool launcherMidiPreDispatchHook(const MIDIMessage &event)
{
    return gLauncher.handleMidiControl(event);
}

void Launcher::queueNavigation(bool prev, bool next, bool select, bool longPress)
{
    if (prev) pendingNavPrev_ = true;
    if (next) pendingNavNext_ = true;
    if (select) pendingNavSelect_ = true;
    if (longPress) pendingNavLongPress_ = true;
}

void Launcher::setup()
{
    pinMode(ENCODER_PIN_SW, INPUT_PULLUP);
    setMidiOutQueueStats(0, 0, kMidiOutQueueSize - 1);
    setMidiPreDispatchHook(&launcherMidiPreDispatchHook);
    initializeChirpDisplay();
    gDirtyTextUi.begin(&chirpDisplay());
    initializeLauncherState();
    renderLauncherScreen();
}

void Launcher::loop()
{
    static constexpr uint16_t kMaxMidiEventsPerLoop = 128;
    static constexpr uint32_t kMidiBudgetUs = 1200;
    static constexpr uint8_t  kMaxControlFramesPerLoop = 8;

    const uint32_t loopStartUs = micros();
    bool hadWork = false;

    gDiag.loops = satAddU32(gDiag.loops, 1);
    logPeriodicDiagnostics();

    bool midiBudgetExceeded = false;
    uint16_t processed = processMidiInput(kMaxMidiEventsPerLoop, kMidiBudgetUs, midiBudgetExceeded);
    if (processed > 0) hadWork = true;
    if (midiBudgetExceeded) gDiag.midiBudgetHits = satAddU32(gDiag.midiBudgetHits, 1);

    if (drainMidiOutputQueue() > 0) hadWork = true;

    {
        const int32_t currentPosition = launcherEncoder_.read();
        const int32_t positionDelta = currentPosition - lastEncoderPosition_;

        bool navPrev = false;
        bool navNext = false;

        if (positionDelta >= 4) {
            navNext = true;
            hadWork = true;
            char buffer[96] = {0};
            snprintf(buffer, sizeof(buffer), "[ENCODER] cw position=%ld delta=%ld",
                     static_cast<long>(currentPosition),
                     static_cast<long>(positionDelta));
            logLauncherDebug(buffer);
            lastEncoderPosition_ += 4;
        } else if (positionDelta <= -4) {
            navPrev = true;
            hadWork = true;
            char buffer[96] = {0};
            snprintf(buffer, sizeof(buffer), "[ENCODER] ccw position=%ld delta=%ld",
                     static_cast<long>(currentPosition),
                     static_cast<long>(positionDelta));
            logLauncherDebug(buffer);
            lastEncoderPosition_ -= 4;
        }

        if (navPrev || navNext) {
            launcherHandleNavigation(navPrev, navNext, false, false);
        }
    }

    {
        const bool pressed = (digitalReadFast(ENCODER_PIN_SW) == LOW);
        const uint32_t nowMs = millis();
        if (pressed != lastButtonPressed_) {
            if ((nowMs - buttonDebounceStartMs_) >= kButtonDebounceMs) {
                buttonDebounceStartMs_ = nowMs;
                lastButtonPressed_ = pressed;
                if (pressed) {
                    buttonPressStartMs_ = nowMs;
                    logLauncherDebug("[ENCODER] button pressed");
                    hadWork = true;
                } else {
                    const uint32_t pressDurationMs = nowMs - buttonPressStartMs_;
                    if (pressDurationMs >= kLongPressThresholdMs) {
                        char buffer[96] = {0};
                        snprintf(buffer, sizeof(buffer), "[ENCODER] long press (%lums)",
                                 static_cast<unsigned long>(pressDurationMs));
                        logLauncherDebug(buffer);
                        launcherHandleNavigation(false, false, false, true);
                    } else {
                        logLauncherDebug("[ENCODER] short click");
                        launcherHandleNavigation(false, false, true, false);
                    }
                    hadWork = true;
                }
            }
        } else {
            buttonDebounceStartMs_ = nowMs;
        }
    }

    debugMidi1RawBytes();
    debugMidiReadStatus();

    const uint32_t controlStartUs = micros();
    uint8_t handledFrames = processUsbControlFrames(kMaxControlFramesPerLoop);
    if (handledFrames > 0)
    {
        gDiag.controlFrames = satAddU32(gDiag.controlFrames, handledFrames);
        gDiag.controlUs = satAddU32(gDiag.controlUs,
                                    static_cast<uint32_t>(micros() - controlStartUs));
        hadWork = true;
    }

    processPendingUsbNavigation();

    if (!hadWork) gDiag.idleLoops = satAddU32(gDiag.idleLoops, 1);

    if (!focusedScriptIsRunning()) {
        if (millis() - lastLoopRefreshMs_ >= kSystemStatsRefreshIntervalMs) {
            lastLoopRefreshMs_ = millis();
            refreshLauncherStatus();
        }
    }

    if (millis() - lastUiFrameMs_ >= 120) {
        lastUiFrameMs_ = millis();
        renderLauncherScreen();
    }

    yield();

    const uint32_t loopElapsedUs = static_cast<uint32_t>(micros() - loopStartUs);
    gDiag.loopBusyUs = satAddU32(gDiag.loopBusyUs, loopElapsedUs);
    if (loopElapsedUs > gDiag.loopMaxUs) gDiag.loopMaxUs = loopElapsedUs;
}

bool Launcher::handleMidiControl(const MIDIMessage &event)
{
    if (event.port != 1) return false;

    bool prev = false;
    bool next = false;
    bool select = false;

    if (event.type == 0x90 && event.data2 > 0) {
        if (event.data1 == 98) prev = true;
        if (event.data1 == 99) next = true;
        if (event.data1 == 83 || event.data1 == 94) select = true;
    }

    if (event.type == 0xB0 && event.data1 == 60) {
        if (event.data2 & 0x40) prev = true;
        else next = true;
    }

    return launcherHandleNavigation(prev, next, select, false);
}

void Launcher::initializeLauncherState()
{
    launcherScriptCount_ = 0;
    const size_t listed = listStoredWrenScripts(launcherScripts_, kLauncherMaxScripts);
    launcherScriptCount_ = (listed < kLauncherMaxScripts) ? listed : kLauncherMaxScripts;

    for (size_t i = 0; i < kLauncherMaxScripts; i++) {
        launcherScriptError_[i] = 0;
        launcherRunningFlags_[i] = false;
    }

    launcherSelectedIndex_ = 0;
    launcherFocusIndex_ = static_cast<int16_t>(launcherScriptCount_);
    lastLauncherStatsMs_ = 0;
    lastMidiRateSampleUs_ = 0;
    lastMidiRateEvents_ = 0;
    lastMidiEventsPerSec_ = 0;
    lastUiFrameMs_ = 0;
    pendingNavPrev_ = false;
    pendingNavNext_ = false;
    pendingNavSelect_ = false;
    pendingNavLongPress_ = false;
    lastEncoderPosition_ = 0;
    lastButtonPressed_ = false;
    buttonDebounceStartMs_ = 0;
    buttonPressStartMs_ = 0;
    lastLoopRefreshMs_ = 0;

    char buffer[192] = {0};
    snprintf(buffer, sizeof(buffer), "[LAUNCHER] init listed=%u selected=%u bootScript=%s",
             static_cast<unsigned>(launcherScriptCount_),
             static_cast<unsigned>(launcherSelectedIndex_),
             bootScriptName() ? bootScriptName() : "");
    logLauncherDebug(buffer);
    for (size_t i = 0; i < launcherScriptCount_; ++i) {
        snprintf(buffer, sizeof(buffer), "[LAUNCHER] script[%u]=%s",
                 static_cast<unsigned>(i), launcherScripts_[i].c_str());
        logLauncherDebug(buffer);
    }

    if (launcherLoadAndAutoStart()) {
        bool focusSet = false;
        bool focusNeedsRedraw = false;
        for (size_t i = 0; i < launcherScriptCount_; ++i) {
            if (launcherRunningFlags_[i]) {
                launcherFocusIndex_ = static_cast<int16_t>(i);
                focusNeedsRedraw = true;
                focusSet = true;
                break;
            }
        }
        if (!focusSet) {
            launcherFocusIndex_ = static_cast<int16_t>(launcherScriptCount_);
        }

        if (focusNeedsRedraw) {
            wrenInterpret(vm, "chirp_runtime", "Script.callFocus()");
        }
    }

    refreshLauncherStatus();
}

void Launcher::renderLauncherScreen()
{
    gDirtyTextUi.setPalette(ST7735_WHITE, ST7735_BLACK);
    gDirtyTextUi.drawBorder();

    gDirtyTextUi.setCenteredLine(1, "CHIRP MONO UI", false, true);

    char line[32] = {0};
    String selectedNameValue = launcherEntryName(launcherSelectedIndex_);
    String focusNameValue = launcherEntryName(
        (launcherFocusIndex_ >= 0 && static_cast<size_t>(launcherFocusIndex_) <= launcherScriptCount_)
            ? static_cast<size_t>(launcherFocusIndex_)
            : launcherScriptCount_);
    const char *selectedName = selectedNameValue.c_str();
    const char *focusName = focusNameValue.c_str();

    const uint32_t frameMs = millis();
    const uint32_t frameTick = frameMs / 120U;
    const uint32_t simA = (frameTick * 7U + 13U) % 10000U;
    const uint32_t simB = (frameTick * 19U + 91U) % 10000U;
    const uint32_t simC = (frameTick * 31U + 7U) % 10000U;
    const uint32_t loopCount = gDiag.loops;
    const uint32_t midiCount = gDiag.midiEvents;
    const uint32_t busyUs = gDiag.loopBusyUs;
    const uint32_t maxUs = gDiag.loopMaxUs;
    const uint32_t controlFrames = gDiag.controlFrames;
    uint32_t runningCount = 0;
    for (size_t i = 0; i < launcherScriptCount_; ++i) {
        if (launcherRunningFlags_[i]) ++runningCount;
    }
    const char spinner = "|/-\\"[frameTick & 3U];

    snprintf(line, sizeof(line), "S:%-9.9s F:%-9.9s", selectedName ? selectedName : "", focusName ? focusName : "MAIN");
    gDirtyTextUi.setLine(2, 1, line);

    snprintf(line, sizeof(line), "A:%04u B:%04u C:%04u",
             static_cast<unsigned>(simA),
             static_cast<unsigned>(simB),
             static_cast<unsigned>(simC));
    gDirtyTextUi.setLine(3, 1, line);

    snprintf(line, sizeof(line), "L:%05u M:%05u", static_cast<unsigned>(loopCount % 100000U), static_cast<unsigned>(midiCount % 100000U));
    gDirtyTextUi.setLine(4, 1, line);

    snprintf(line, sizeof(line), "B:%05u X:%05u C:%05u",
             static_cast<unsigned>(busyUs % 100000U),
             static_cast<unsigned>(maxUs % 100000U),
             static_cast<unsigned>(controlFrames % 100000U));
    gDirtyTextUi.setLine(5, 1, line);

    snprintf(line, sizeof(line), "R:%u/%u %c", static_cast<unsigned>(runningCount), static_cast<unsigned>(launcherScriptCount_), spinner);
    gDirtyTextUi.setLine(6, 1, line);

    gDirtyTextUi.setLine(7, 1, "-----------------------");

    const size_t listRows = 5;
    size_t startIndex = 0;
    if (launcherScriptCount_ > listRows) {
        if (launcherSelectedIndex_ >= listRows / 2) {
            startIndex = launcherSelectedIndex_ - (listRows / 2);
        }
        if (startIndex + listRows > launcherScriptCount_) {
            startIndex = launcherScriptCount_ - listRows;
        }
    }

    for (size_t row = 0; row < listRows; ++row) {
        const size_t index = startIndex + row;
        if (index >= launcherScriptCount_) {
            gDirtyTextUi.setLine(static_cast<uint8_t>(8 + row), 1, "", true);
            continue;
        }

        const char selector = (index == launcherSelectedIndex_) ? '>' : ' ';
        const char focusMark = (index == static_cast<size_t>(launcherFocusIndex_)) ? '*' : ' ';
        const char runningMark = launcherRunningFlags_[index] ? 'R' : ' ';
        const char errorMark = (launcherScriptError_[index] != 0) ? '!' : ' ';

        snprintf(line, sizeof(line), "%c%c%c%c %-13.13s",
                 selector,
                 focusMark,
                 runningMark,
                 errorMark,
                 launcherScripts_[index].c_str());
        const bool isSelected = (index == launcherSelectedIndex_);
        gDirtyTextUi.setLine(static_cast<uint8_t>(8 + row), 1, line, true, isSelected);
    }

    gDirtyTextUi.setLine(13, 1, "ENC nav BTN focus HOLD", true);
    gDirtyTextUi.render();
}

void Launcher::launcherSaveState()
{
    String json;
    json.reserve(256);
    json = "{\"scripts\":";
    json += "[";
    for (size_t i = 0; i < launcherScriptCount_; ++i) {
        if (i > 0) json += ",";
        json += "{\"name\":\"";
        json += launcherScripts_[i];
        json += "\",\"running\":";
        const bool shouldRestore = (static_cast<int16_t>(i) == launcherFocusIndex_) && launcherRunningFlags_[i];
        json += shouldRestore ? "true" : "false";
        json += ",\"error\":";
        json += (launcherScriptError_[i] != 0) ? "true" : "false";
        json += "}";
    }
    json += "]}";

    if (!internalFlash.exists("/userdata")) internalFlash.mkdir("/userdata");
    if (internalFlash.exists(kLauncherStatePath)) internalFlash.remove(kLauncherStatePath);
    File f = internalFlash.open(kLauncherStatePath, FILE_WRITE);
    if (f) {
        f.write(reinterpret_cast<const uint8_t *>(json.c_str()), static_cast<size_t>(json.length()));
        f.flush();
        f.close();
        logLauncherDebug("[LAUNCHER] state saved");
    } else {
        logLauncherDebug("[LAUNCHER] state save failed");
    }
}

bool Launcher::launcherLoadAndAutoStart()
{
    if (!internalFlash.exists(kLauncherStatePath)) return false;
    File f = internalFlash.open(kLauncherStatePath, FILE_READ);
    if (!f) return false;
    String json;
    json.reserve(static_cast<size_t>(f.size()) + 1);
    while (f.available()) json += static_cast<char>(f.read());
    f.close();

    logLauncherDebug("[LAUNCHER] loading saved state");

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

        for (size_t i = 0; i < launcherScriptCount_; ++i) {
            if (launcherScripts_[i] != name) continue;
            char buf[96] = {0};
            if (hadError) {
                launcherScriptError_[i] = 1;
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

                for (size_t clearIndex = 0; clearIndex < launcherScriptCount_; ++clearIndex)
                    launcherRunningFlags_[clearIndex] = false;
                launcherSaveState();

                snprintf(buf, sizeof(buf), "[LAUNCHER] auto-start: %s", name.c_str());
                logLauncherDebug(buf);
                if (runStoredWrenScript(name.c_str())) {
                    launcherRunningFlags_[i] = true;
                    launcherScriptError_[i] = 0;
                    launcherSaveState();
                    logSetup("[BOOT] Launcher: auto-started script");
                    autoStarted++;
                } else {
                    launcherRunningFlags_[i] = false;
                    launcherScriptError_[i] = 1;
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

void Launcher::refreshLauncherStatus()
{
    if (!focusedScriptIsRunning()) {
        const char *activeScript = lastLoadedWrenScriptName();
        char buffer[128] = {0};
        snprintf(buffer, sizeof(buffer), "[LAUNCHER] idle focus=%u active=%s",
                 static_cast<unsigned>(launcherSelectedIndex_),
                 (activeScript && activeScript[0]) ? activeScript : "none");
        logLauncherDebug(buffer);
    }

    renderLauncherScreen();
}

void Launcher::stopScript(size_t idx)
{
    if (!vm) return;
    if (idx >= launcherScriptCount_) return;

    wrenInterpret(vm, "chirp_runtime", "Script.callUnload()\nMidi.clearListeners()");
    launcherRunningFlags_[idx] = false;
    wrenCollectGarbage(vm);

    char buf[96] = {0};
    snprintf(buf, sizeof(buf), "[LAUNCHER] stopped + GC: %s", launcherScripts_[idx].c_str());
    logLauncherDebug(buf);
    logSetup("[BOOT] Launcher: script stopped");
    launcherSaveState();
}

void Launcher::launcherSwitchFocusContext()
{
    const size_t idx = launcherSelectedIndex_;
    launcherFocusIndex_ = static_cast<int16_t>(idx);
    if (idx < launcherScriptCount_) {
        char buf[128] = {0};
        snprintf(buf, sizeof(buf), "[LAUNCHER] focus -> %s", launcherScripts_[idx].c_str());
        logLauncherDebug(buf);
    } else {
        logLauncherDebug("[LAUNCHER] focus -> MAIN");
    }
    lastLauncherStatsMs_ = 0;
    logLauncherSelection("single-click focus");
}

void Launcher::launcherToggleScriptRuntime()
{
    logLauncherSelection("long-press toggle");

    if (launcherSelectedIndex_ >= launcherScriptCount_) {
        logLauncherDebug("[LAUNCHER] long-press on MAIN: no script to toggle");
        return;
    }

    const size_t idx = launcherSelectedIndex_;
    const String scriptName = launcherScripts_[idx];

    if (isScriptRunning(idx)) {
        stopScript(idx);
        if (isScriptFocused(idx)) {
            launcherFocusIndex_ = static_cast<int16_t>(launcherScriptCount_);
            logLauncherDebug("[LAUNCHER] focus -> MAIN (stopped script lost focus)");
        }
        lastLauncherStatsMs_ = 0;
        return;
    }

    if (!runStoredWrenScript(scriptName.c_str())) {
        logSetup("[BOOT] Launcher: script start failed");
        launcherScriptError_[idx] = 1;
        char buf[128] = {0};
        snprintf(buf, sizeof(buf), "[LAUNCHER] start failed: %s", scriptName.c_str());
        logLauncherDebug(buf);
        launcherSaveState();
        lastLauncherStatsMs_ = 0;
        return;
    }

    launcherRunningFlags_[idx] = true;
    launcherScriptError_[idx] = 0;
    launcherFocusIndex_ = static_cast<int16_t>(idx);
    char buf[128] = {0};
    snprintf(buf, sizeof(buf), "[LAUNCHER] started + focus: %s", scriptName.c_str());
    logLauncherDebug(buf);
    logSetup("[BOOT] Launcher: script started");
    launcherSaveState();
    lastLauncherStatsMs_ = 0;
    wrenInterpret(vm, "chirp_runtime", "Script.callFocus()");
}

bool Launcher::launcherHandleNavigation(bool prev, bool next, bool select, bool longPress)
{
    if (!prev && !next && !select && !longPress) return false;

    const size_t total = launcherScriptCount_ + 1;
    if (total == 0) return true;

    if (prev || next) {
        const size_t previousSelection = launcherSelectedIndex_;
        if (prev) launcherSelectedIndex_ = (launcherSelectedIndex_ + total - 1) % total;
        if (next) launcherSelectedIndex_ = (launcherSelectedIndex_ + 1) % total;

        if (launcherSelectedIndex_ == launcherScriptCount_) {
            logLauncherDebug("[LAUNCHER] reached MAIN item");
        }

        char navBuffer[192] = {0};
        snprintf(navBuffer, sizeof(navBuffer),
                 "[ENCODER] nav: prev=%u next=%u before=%u selected=%u total=%u",
                 prev ? 1U : 0U,
                 next ? 1U : 0U,
                 static_cast<unsigned>(previousSelection),
                 static_cast<unsigned>(launcherSelectedIndex_),
                 static_cast<unsigned>(total));
        logLauncherDebug(navBuffer);

        lastLauncherStatsMs_ = 0;
        logLauncherSelection("nav-only");
        renderLauncherScreen();
        return true;
    }

    if (select || longPress) {
        if (longPress) {
            launcherToggleScriptRuntime();
        } else {
            launcherSwitchFocusContext();
        }
        logLauncherSelection("button-action");
        renderLauncherScreen();
        return true;
    }

    return false;
}

bool Launcher::isScriptRunning(size_t index) const
{
    return (index < launcherScriptCount_) && launcherRunningFlags_[index];
}

bool Launcher::isScriptFocused(size_t index) const
{
    return launcherFocusIndex_ >= 0 && static_cast<size_t>(launcherFocusIndex_) == index;
}

bool Launcher::focusedScriptIsRunning() const
{
    return launcherFocusIndex_ >= 0 &&
           static_cast<size_t>(launcherFocusIndex_) < launcherScriptCount_ &&
           isScriptRunning(static_cast<size_t>(launcherFocusIndex_));
}

String Launcher::launcherEntryName(size_t index) const
{
    if (index < launcherScriptCount_) return launcherScripts_[index];
    return "MAIN";
}

int32_t Launcher::launcherApproxFreeRamBytes() const
{
    return -1;
}

void Launcher::logLauncherDebug(const char *message) const
{
    if (message == nullptr || message[0] == '\0') return;
    logSetup(message);
}

void Launcher::logLauncherSelection(const char *tag) const
{
    char buffer[192] = {0};
    const size_t total = launcherScriptCount_ + 1;
    const char *selectedName = (launcherSelectedIndex_ < launcherScriptCount_)
        ? launcherScripts_[launcherSelectedIndex_].c_str()
        : "MAIN";
    unsigned runCount = 0;
    for (size_t i = 0; i < launcherScriptCount_; ++i) if (launcherRunningFlags_[i]) ++runCount;
    const char *focusName = (launcherFocusIndex_ >= 0 && static_cast<size_t>(launcherFocusIndex_) < launcherScriptCount_)
        ? launcherScripts_[static_cast<size_t>(launcherFocusIndex_)].c_str()
        : "MAIN";
    snprintf(buffer, sizeof(buffer),
             "[LAUNCHER] %s total=%u selected=%u(%s) running=%u focus=%d(%s)",
             tag ? tag : "state",
             static_cast<unsigned>(total),
             static_cast<unsigned>(launcherSelectedIndex_),
             selectedName,
             runCount,
             static_cast<int>(launcherFocusIndex_),
             focusName);
    logLauncherDebug(buffer);
}

void Launcher::processPendingUsbNavigation()
{
    bool usbPrev = false;
    bool usbNext = false;
    bool usbSel = false;
    bool usbLong = false;

    if (pendingNavPrev_)      { pendingNavPrev_ = false; usbPrev = true; }
    if (pendingNavNext_)      { pendingNavNext_ = false; usbNext = true; }
    if (pendingNavSelect_)    { pendingNavSelect_ = false; usbSel = true; }
    if (pendingNavLongPress_) { pendingNavLongPress_ = false; usbLong = true; }

    if (usbPrev || usbNext || usbSel || usbLong) {
        char buffer[96] = {0};
        snprintf(buffer, sizeof(buffer),
                 "[USB CTRL] prev=%u next=%u select=%u long=%u",
                 usbPrev ? 1U : 0U,
                 usbNext ? 1U : 0U,
                 usbSel ? 1U : 0U,
                 usbLong ? 1U : 0U);
        logLauncherDebug(buffer);
        launcherHandleNavigation(usbPrev, usbNext, usbSel, usbLong);
        logLauncherSelection("usb-control");
    }
}