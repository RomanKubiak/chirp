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
#include <cctype>
#include <string>

extern "C" void _reboot_Teensyduino_(void);

extern "C" char *sbrk(int incr);
extern WrenVM *vm;
extern ScriptStorage scriptStorage;
extern ChirpFS internalFlash;
extern RuntimeDiagCounters gDiag;
extern WrenConfiguration config;

Launcher gLauncher;

namespace {
uintptr_t currentStackPointer()
{
    uintptr_t sp = 0;
    asm volatile("mov %0, sp" : "=r"(sp));
    return sp;
}

int32_t approximateFreeRamBytes()
{
    char *heapEnd = sbrk(0);
    if (heapEnd == nullptr) return -1;
    const uintptr_t sp = currentStackPointer();
    const uintptr_t heap = reinterpret_cast<uintptr_t>(heapEnd);
    return (sp > heap) ? static_cast<int32_t>(sp - heap) : -1;
}

void runScriptCallbackWithContext(const String &scriptName, const char *source)
{
    if (source == nullptr || source[0] == '\0') return;
    WrenMidiBridge::beginScriptContext(scriptName.c_str());
    wrenInterpret(vm, "chirp_runtime", source);
    WrenMidiBridge::endScriptContext();
}

std::string trimCopy(const std::string &text)
{
    size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) begin++;
    size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) end--;
    return text.substr(begin, end - begin);
}

bool parseBoolValue(const std::string &value)
{
    return value == "1" || value == "true" || value == "TRUE" || value == "yes" || value == "on";
}

bool parseUnsignedValue(const std::string &value, size_t &out)
{
    if (value.empty()) return false;
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') return false;
    out = static_cast<size_t>(parsed);
    return true;
}
} // namespace

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
            navPrev = true;
            hadWork = true;
            char buffer[96] = {0};
            snprintf(buffer, sizeof(buffer), "[ENCODER] cw position=%ld delta=%ld",
                     static_cast<long>(currentPosition),
                     static_cast<long>(positionDelta));
            logLauncherDebug(buffer);
            lastEncoderPosition_ += 4;
        } else if (positionDelta <= -4) {
            navNext = true;
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
                    buttonLongPressHandled_ = false;
                    logLauncherDebug("[ENCODER] button pressed");
                    hadWork = true;
                } else {
                    if (!buttonLongPressHandled_) {
                        logLauncherDebug("[ENCODER] short click");
                        launcherHandleNavigation(false, false, true, false);
                    }
                    buttonLongPressHandled_ = false;
                    hadWork = true;
                }
            }
        } else {
            buttonDebounceStartMs_ = nowMs;
            if (pressed && !buttonLongPressHandled_ &&
                (nowMs - buttonPressStartMs_) >= kLongPressThresholdMs) {
                buttonLongPressHandled_ = true;
                const uint32_t pressDurationMs = nowMs - buttonPressStartMs_;
                char buffer[96] = {0};
                snprintf(buffer, sizeof(buffer), "[ENCODER] long press (%lums)",
                         static_cast<unsigned long>(pressDurationMs));
                logLauncherDebug(buffer);
                launcherHandleNavigation(false, false, false, true);
                hadWork = true;
            }
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
    launcherMenuVisible_ = true;
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
    snprintf(buffer, sizeof(buffer), "[LAUNCHER] init listed=%u selected=%u bootScript=%.24s",
             static_cast<unsigned>(launcherScriptCount_),
             static_cast<unsigned>(launcherSelectedIndex_),
             bootScriptName() ? bootScriptName() : "");
    logLauncherDebug(buffer);
    for (size_t i = 0; i < launcherScriptCount_; ++i) {
        snprintf(buffer, sizeof(buffer), "[LAUNCHER] script[%u]=%.24s",
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
            runScriptCallbackWithContext(launcherScripts_[launcherFocusIndex_], "Script.callFocus()");
        }
    }

    refreshLauncherStatus();
}

void Launcher::renderLauncherScreen()
{
    if (!launcherMenuVisible_) return;

    gDirtyTextUi.setPalette(ST7735_WHITE, ST7735_BLACK);
    gDirtyTextUi.drawBorder();

    for (uint8_t row = 1; row + 1 < DirtyTextUi::kRows; ++row) {
        gDirtyTextUi.setLine(row, 1, "", true);
    }

    static constexpr size_t kScriptWindowRows = 8;
    static constexpr uint8_t kSpecialStartRow = 10;

    size_t scriptStart = 0;
    if (launcherScriptCount_ > kScriptWindowRows) {
        if (launcherSelectedIndex_ < launcherScriptCount_) {
            scriptStart = (launcherSelectedIndex_ >= (kScriptWindowRows - 1))
                              ? (launcherSelectedIndex_ - (kScriptWindowRows - 1))
                              : 0;
            const size_t maxStart = launcherScriptCount_ - kScriptWindowRows;
            if (scriptStart > maxStart) scriptStart = maxStart;
        } else {
            scriptStart = launcherScriptCount_ - kScriptWindowRows;
        }
    }

    const size_t scriptVisibleCount = (launcherScriptCount_ > scriptStart)
                                          ? ((launcherScriptCount_ - scriptStart) < kScriptWindowRows
                                                 ? (launcherScriptCount_ - scriptStart)
                                                 : kScriptWindowRows)
                                          : 0;

    for (size_t row = 0; row < scriptVisibleCount; ++row) {
        const size_t index = scriptStart + row;
        const bool selected = (index == launcherSelectedIndex_);
        const bool running = isScriptRunning(index);
        char line[48] = {0};
        snprintf(line, sizeof(line), "%c%c %.32s",
                 selected ? '>' : ' ',
                 running ? '*' : ' ',
                 launcherScripts_[index].c_str());
        gDirtyTextUi.setLine(static_cast<uint8_t>(1 + row), 1, line, true, false, false);
    }

    char line[48] = {0};
    snprintf(line, sizeof(line), "%c  REBOOT", (launcherSelectedIndex_ == launcherScriptCount_) ? '>' : ' ');
    gDirtyTextUi.setLine(kSpecialStartRow, 1, line, true, false, false);

    snprintf(line, sizeof(line), "%c  DIAGNOSTIC", (launcherSelectedIndex_ == launcherScriptCount_ + 1) ? '>' : ' ');
    gDirtyTextUi.setLine(static_cast<uint8_t>(kSpecialStartRow + 1), 1, line, true, false, false);

    snprintf(line, sizeof(line), "%c  CONFIG", (launcherSelectedIndex_ == launcherScriptCount_ + 2) ? '>' : ' ');
    gDirtyTextUi.setLine(static_cast<uint8_t>(kSpecialStartRow + 2), 1, line, true, false, false);

    gDirtyTextUi.render();
}

void Launcher::launcherShowDiagnosticScreen()
{
    launcherMenuVisible_ = false;
    launcherFocusIndex_ = static_cast<int16_t>(launcherScriptCount_);
    lastLauncherStatsMs_ = 0;
    logLauncherDebug("[LAUNCHER] view -> DIAGNOSTIC");
    logLauncherSelection("button-short-click-diagnostic");
    renderDiagnosticScreen();
}

void Launcher::launcherShowConfigScreen()
{
    launcherMenuVisible_ = false;
    launcherFocusIndex_ = static_cast<int16_t>(launcherScriptCount_ + 2);
    lastLauncherStatsMs_ = 0;
    logLauncherDebug("[LAUNCHER] view -> CONFIG");
    logLauncherSelection("button-short-click-config");
    renderConfigScreen();
}

void Launcher::renderDiagnosticScreen()
{
    if (launcherMenuVisible_) return;

    gDirtyTextUi.setPalette(ST7735_WHITE, ST7735_BLACK);
    gDirtyTextUi.drawBorder();

    for (uint8_t row = 1; row + 1 < DirtyTextUi::kRows; ++row) {
        gDirtyTextUi.setLine(row, 1, "", true);
    }

    const uint32_t cpuHz =
#ifdef F_CPU_ACTUAL
        static_cast<uint32_t>(F_CPU_ACTUAL);
#else
        static_cast<uint32_t>(F_CPU);
#endif
    const uint32_t busHz =
#ifdef F_BUS_ACTUAL
        static_cast<uint32_t>(F_BUS_ACTUAL);
#else
        0;
#endif
    const uint32_t wrenBytes = vm ? static_cast<uint32_t>(vm->bytesAllocated) : 0;
    const uint32_t wrenNextGc = vm ? static_cast<uint32_t>(vm->nextGC) : 0;
    const int32_t freeRam = launcherApproxFreeRamBytes();
    const uint16_t midiQDepth = gMidiOutQueueDepth;
    const uint16_t midiQHigh = gMidiOutQueueHighWater;
    const uint16_t midiQCap = gMidiOutQueueCapacity;
    const char *storageBackend = internalFlash.backendName();
    const char *storageDiag = internalFlash.storageDiagnostic();
    const int32_t encoderPos = launcherEncoder_.read();

    char line[96] = {0};
    gDirtyTextUi.setCenteredLine(1, "DIAGNOSTIC", true, true);

    snprintf(line, sizeof(line), "CPU %lu  BUS %lu",
             static_cast<unsigned long>(cpuHz / 1000000UL),
             static_cast<unsigned long>(busHz / 1000000UL));
    gDirtyTextUi.setLine(3, 1, line, true, false, false);

    snprintf(line, sizeof(line), "RAM %ldB  WREN %luB",
             static_cast<long>(freeRam),
             static_cast<unsigned long>(wrenBytes));
    gDirtyTextUi.setLine(4, 1, line, true, false, false);

    snprintf(line, sizeof(line), "GC %lu/%lu",
             static_cast<unsigned long>(wrenNextGc),
             static_cast<unsigned long>(config.initialHeapSize));
    gDirtyTextUi.setLine(5, 1, line, true, false, false);

    char storageBackendShort[13] = {0};
    char storageDiagShort[17] = {0};
    if (storageBackend != nullptr) {
        strncpy(storageBackendShort, storageBackend, sizeof(storageBackendShort) - 1);
    }
    if (storageDiag != nullptr) {
        strncpy(storageDiagShort, storageDiag, sizeof(storageDiagShort) - 1);
    }
    strcpy(line, "STOR ");
    strncat(line, storageBackendShort, sizeof(line) - strlen(line) - 1);
    strncat(line, " ", sizeof(line) - strlen(line) - 1);
    strncat(line, storageDiagShort, sizeof(line) - strlen(line) - 1);
    gDirtyTextUi.setLine(7, 1, line, true, false, false);

    snprintf(line, sizeof(line), "ENC %ld  SCRIPTS %u",
             static_cast<long>(encoderPos),
             static_cast<unsigned>(launcherScriptCount_));
    gDirtyTextUi.setLine(8, 1, line, true, false, false);

    snprintf(line, sizeof(line), "RUN %u  Q %u/%u",
             static_cast<unsigned>(countRunningScripts()),
             midiQDepth,
             midiQCap);
    gDirtyTextUi.setLine(9, 1, line, true, false, false);

    snprintf(line, sizeof(line), "LOOP %lu MAX %lu",
             static_cast<unsigned long>(gDiag.loops),
             static_cast<unsigned long>(gDiag.loopMaxUs));
    gDirtyTextUi.setLine(10, 1, line, true, false, false);

    snprintf(line, sizeof(line), "MIDI %lu BUD %lu",
             static_cast<unsigned long>(gDiag.midiEvents),
             static_cast<unsigned long>(gDiag.midiBudgetHits));
    gDirtyTextUi.setLine(11, 1, line, true, false, false);

    snprintf(line, sizeof(line), "QH %u", midiQHigh);
    gDirtyTextUi.setLine(12, 1, line, true, false, false);

    gDirtyTextUi.setCenteredLine(13, "SELECT=MENU", false, false);
    gDirtyTextUi.render();

    char diagLog[192] = {0};
    snprintf(diagLog, sizeof(diagLog),
             "[DIAG] cpu=%luHz bus=%luHz freeRam=%ldB wrenBytes=%lu nextGC=%lu storage=%.24s enc=%ld scripts=%u running=%u q=%u/%u loops=%lu maxLoopUs=%lu midiEv=%lu budget=%lu",
             static_cast<unsigned long>(cpuHz),
             static_cast<unsigned long>(busHz),
             static_cast<long>(freeRam),
             static_cast<unsigned long>(wrenBytes),
             static_cast<unsigned long>(wrenNextGc),
             storageDiag,
             static_cast<long>(encoderPos),
             static_cast<unsigned>(launcherScriptCount_),
             static_cast<unsigned>(countRunningScripts()),
             static_cast<unsigned>(midiQDepth),
             static_cast<unsigned>(midiQCap),
             static_cast<unsigned long>(gDiag.loops),
             static_cast<unsigned long>(gDiag.loopMaxUs),
             static_cast<unsigned long>(gDiag.midiEvents),
             static_cast<unsigned long>(gDiag.midiBudgetHits));
    logLauncherDebug(diagLog);
}

void Launcher::renderConfigScreen()
{
    if (launcherMenuVisible_) return;

    gDirtyTextUi.setPalette(ST7735_WHITE, ST7735_BLACK);
    gDirtyTextUi.drawBorder();

    for (uint8_t row = 1; row + 1 < DirtyTextUi::kRows; ++row) {
        gDirtyTextUi.setLine(row, 1, "", true);
    }

    char line[64] = {0};
    gDirtyTextUi.setCenteredLine(1, "CONFIG", true, true);

    snprintf(line, sizeof(line), "DEFAULT BUILTIN FONT");
    gDirtyTextUi.setLine(3, 1, line, true, false, false);

    snprintf(line, sizeof(line), "NO FONT OPTIONS ACTIVE");
    gDirtyTextUi.setLine(4, 1, line, true, false, false);

    snprintf(line, sizeof(line), "SELECT = BACK TO MENU");
    gDirtyTextUi.setLine(5, 1, line, true, false, false);

    gDirtyTextUi.render();
}

void Launcher::launcherSaveState()
{
    String iniText;
    iniText.reserve(1024);
    iniText = "[launcher]\n";
    iniText += "selected=";
    iniText += String(static_cast<unsigned long>(launcherSelectedIndex_));
    iniText += "\n\n";
    for (size_t i = 0; i < launcherScriptCount_; ++i) {
        iniText += "[script.";
        iniText += String(static_cast<unsigned long>(i));
        iniText += "]\n";
        iniText += "name=";
        iniText += launcherScripts_[i];
        iniText += "\n";
        iniText += "running=";
        iniText += launcherRunningFlags_[i] ? "true" : "false";
        iniText += "\n";
        iniText += "error=";
        iniText += (launcherScriptError_[i] != 0) ? "true" : "false";
        iniText += "\n\n";
    }

    if (!internalFlash.exists("/userdata")) internalFlash.mkdir("/userdata");
    if (internalFlash.exists(kLauncherStatePath)) internalFlash.remove(kLauncherStatePath);
    File f = internalFlash.open(kLauncherStatePath, FILE_WRITE);
    if (f) {
        f.write(reinterpret_cast<const uint8_t *>(iniText.c_str()), static_cast<size_t>(iniText.length()));
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
    String iniText;
    iniText.reserve(static_cast<size_t>(f.size()) + 1);
    while (f.available()) iniText += static_cast<char>(f.read());
    f.close();

    logLauncherDebug("[LAUNCHER] loading saved state");

    size_t loadedSelected = launcherSelectedIndex_;
    bool hasLoadedSelected = false;

    struct SavedScriptState {
        String name;
        bool running = false;
        bool error = false;
    } savedScripts[kLauncherMaxScripts];

    std::string currentSection;
    const char *p = iniText.c_str();

    if (static_cast<unsigned char>(p[0]) == 0xEF &&
        static_cast<unsigned char>(p[1]) == 0xBB &&
        static_cast<unsigned char>(p[2]) == 0xBF)
    {
        p += 3;
    }

    while (*p) {
        const char *lineEnd = p;
        while (*lineEnd && *lineEnd != '\n' && *lineEnd != '\r') lineEnd++;

        std::string line = trimCopy(std::string(p, static_cast<size_t>(lineEnd - p)));
        if (!line.empty() && line[0] != '#' && line[0] != ';') {
            if (line.front() == '[' && line.back() == ']') {
                currentSection = trimCopy(line.substr(1, line.size() - 2));
            } else {
                const size_t eqPos = line.find('=');
                if (eqPos != std::string::npos) {
                    std::string key = trimCopy(line.substr(0, eqPos));
                    std::string value = trimCopy(line.substr(eqPos + 1));
                    if (!value.empty() && ((value.front() == '"' && value.back() == '"') ||
                                           (value.front() == '\'' && value.back() == '\''))) {
                        value = value.substr(1, value.size() - 2);
                    }

                    if (currentSection == "launcher") {
                        if (key == "selected") {
                            hasLoadedSelected = parseUnsignedValue(value, loadedSelected);
                        }
                    } else if (currentSection.rfind("script.", 0) == 0) {
                        const std::string suffix = currentSection.substr(7);
                        char *end = nullptr;
                        const unsigned long scriptIndex = std::strtoul(suffix.c_str(), &end, 10);
                        if (end != suffix.c_str() && *end == '\0' && scriptIndex < kLauncherMaxScripts) {
                            if (key == "name") {
                                savedScripts[scriptIndex].name = value.c_str();
                            } else if (key == "running") {
                                savedScripts[scriptIndex].running = parseBoolValue(value);
                            } else if (key == "error") {
                                savedScripts[scriptIndex].error = parseBoolValue(value);
                            }
                        }
                    }
                }
            }
        }

        if (*lineEnd == '\r' && lineEnd[1] == '\n') lineEnd++;
        p = (*lineEnd == '\0') ? lineEnd : lineEnd + 1;
    }

    if (hasLoadedSelected && loadedSelected < launcherMenuItemCount()) {
        launcherSelectedIndex_ = loadedSelected;
    }

    bool anyLoaded = false;
    size_t autoStarted = 0;

    for (size_t savedIndex = 0; savedIndex < kLauncherMaxScripts; ++savedIndex) {
        const String &name = savedScripts[savedIndex].name;
        if (name.length() == 0) continue;

        for (size_t i = 0; i < launcherScriptCount_; ++i) {
            if (launcherScripts_[i] != name) continue;

            launcherScriptError_[i] = savedScripts[savedIndex].error ? 1 : 0;
            if (savedScripts[savedIndex].error) {
                char buf[96] = {0};
                snprintf(buf, sizeof(buf), "[LAUNCHER] restored error: %.24s", name.c_str());
                logLauncherDebug(buf);
                anyLoaded = true;
            }

            if (!savedScripts[savedIndex].running) {
                launcherRunningFlags_[i] = false;
                anyLoaded = true;
                break;
            }

            if (autoStarted >= kBootAutoStartLimit) {
                char buf[96] = {0};
                snprintf(buf, sizeof(buf), "[LAUNCHER] boot auto-start skipped: %.24s", name.c_str());
                logLauncherDebug(buf);
                anyLoaded = true;
                break;
            }

            for (size_t clearIndex = 0; clearIndex < launcherScriptCount_; ++clearIndex) {
                launcherRunningFlags_[clearIndex] = false;
            }

            char buf[96] = {0};
            snprintf(buf, sizeof(buf), "[LAUNCHER] auto-start: %.24s", name.c_str());
            logLauncherDebug(buf);
            if (runStoredWrenScript(name.c_str())) {
                launcherRunningFlags_[i] = true;
                launcherScriptError_[i] = 0;
                logSetup("[BOOT] Launcher: auto-started script");
                autoStarted++;
            } else {
                launcherRunningFlags_[i] = false;
                launcherScriptError_[i] = 1;
                snprintf(buf, sizeof(buf), "[LAUNCHER] auto-start failed: %.24s", name.c_str());
                logLauncherDebug(buf);
            }
            anyLoaded = true;
            break;
        }
    }

    if (anyLoaded) launcherSaveState();
    return anyLoaded;
}

void Launcher::refreshLauncherStatus()
{
    if (!launcherMenuVisible_) return;

    if (!focusedScriptIsRunning()) {
        const char *activeScript = lastLoadedWrenScriptName();
        char buffer[128] = {0};
        snprintf(buffer, sizeof(buffer), "[LAUNCHER] idle focus=%u active=%.24s",
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

    runScriptCallbackWithContext(launcherScripts_[idx], "Script.callUnload()\nMidi.clearListeners()");
    launcherRunningFlags_[idx] = false;
    wrenCollectGarbage(vm);

    char buf[96] = {0};
    snprintf(buf, sizeof(buf), "[LAUNCHER] stopped + GC: %.24s", launcherScripts_[idx].c_str());
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
        snprintf(buf, sizeof(buf), "[LAUNCHER] focus -> %.24s", launcherScripts_[idx].c_str());
        logLauncherDebug(buf);
    } else {
        logLauncherDebug("[LAUNCHER] focus -> MAIN");
    }
    launcherMenuVisible_ = false;
    lastLauncherStatsMs_ = 0;
    logLauncherSelection("single-click focus");

    if (idx < launcherScriptCount_ && isScriptRunning(idx)) {
        runScriptCallbackWithContext(launcherScripts_[idx], "Script.callFocus()");
    }
}

void Launcher::launcherShowMenu()
{
    launcherMenuVisible_ = true;
    logLauncherDebug("[LAUNCHER] view -> MAIN MENU");
    logLauncherSelection("button-short-click-menu");
    renderLauncherScreen();
}

void Launcher::launcherShowScriptOutput()
{
    if (isDiagnosticIndex(launcherSelectedIndex_)) {
        launcherShowDiagnosticScreen();
        return;
    }

    if (isRebootIndex(launcherSelectedIndex_)) {
        logLauncherDebug("[LAUNCHER] short click -> reboot");
        logLauncherSelection("button-short-click-reboot");
        launcherRequestReboot(false);
        return;
    }

    if (isConfigIndex(launcherSelectedIndex_)) {
        launcherShowConfigScreen();
        return;
    }

    if (launcherSelectedIndex_ >= launcherScriptCount_) {
        logLauncherDebug("[LAUNCHER] short click on MAIN ignored");
        return;
    }

    if (!isScriptRunning(launcherSelectedIndex_)) {
        logLauncherDebug("[LAUNCHER] short click on stopped script ignored");
        return;
    }

    launcherSwitchFocusContext();
}

void Launcher::launcherToggleScriptRuntime()
{
    logLauncherSelection("long-press toggle");

    if (isDiagnosticIndex(launcherSelectedIndex_)) {
        logLauncherDebug("[LAUNCHER] long-press on DIAGNOSTIC ignored");
        return;
    }

    if (isRebootIndex(launcherSelectedIndex_) || isConfigIndex(launcherSelectedIndex_)) {
        logLauncherDebug("[LAUNCHER] long-press on non-script item ignored");
        return;
    }

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
        launcherMenuVisible_ = true;
        lastLauncherStatsMs_ = 0;
        return;
    }

    if (!runStoredWrenScript(scriptName.c_str())) {
        logSetup("[BOOT] Launcher: script start failed");
        launcherScriptError_[idx] = 1;
        char buf[128] = {0};
        snprintf(buf, sizeof(buf), "[LAUNCHER] start failed: %.24s", scriptName.c_str());
        logLauncherDebug(buf);
        launcherSaveState();
        lastLauncherStatsMs_ = 0;
        return;
    }

    launcherRunningFlags_[idx] = true;
    launcherScriptError_[idx] = 0;
    launcherFocusIndex_ = static_cast<int16_t>(idx);
    char buf[128] = {0};
    snprintf(buf, sizeof(buf), "[LAUNCHER] started + focus: %.24s", scriptName.c_str());
    logLauncherDebug(buf);
    logSetup("[BOOT] Launcher: script started");
    launcherSaveState();
    lastLauncherStatsMs_ = 0;
    runScriptCallbackWithContext(scriptName, "Script.callFocus()");
}

bool Launcher::launcherHandleNavigation(bool prev, bool next, bool select, bool longPress)
{
    if (!prev && !next && !select && !longPress) return false;

    if (!launcherMenuVisible_ && launcherFocusIndex_ >= 0 &&
        static_cast<size_t>(launcherFocusIndex_) == launcherScriptCount_ + 2)
    {
        if (select || longPress || prev || next) {
            launcherShowMenu();
            return true;
        }
    }

    const size_t total = launcherMenuItemCount();
    if (total == 0) return true;

    if (prev || next) {
        const size_t previousSelection = launcherSelectedIndex_;
        if (prev) launcherSelectedIndex_ = (launcherSelectedIndex_ + total - 1) % total;
        if (next) launcherSelectedIndex_ = (launcherSelectedIndex_ + 1) % total;

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

    if (select) {
        if (launcherMenuVisible_) {
            logLauncherDebug(isDiagnosticIndex(launcherSelectedIndex_)
                                 ? "[LAUNCHER] short click -> diagnostic"
                                 : "[LAUNCHER] short click -> script output");
            logLauncherSelection(isDiagnosticIndex(launcherSelectedIndex_)
                                     ? "button-short-click-diagnostic"
                                     : "button-short-click-output");
            launcherShowScriptOutput();
        } else {
            launcherShowMenu();
        }
        return true;
    }

    if (longPress) {
        launcherToggleScriptRuntime();
        return true;
    }

    return false;
}

unsigned Launcher::countRunningScripts() const
{
    unsigned running = 0;
    for (size_t i = 0; i < launcherScriptCount_; ++i) {
        if (launcherRunningFlags_[i]) ++running;
    }
    return running;
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

size_t Launcher::launcherMenuItemCount() const
{
    return launcherScriptCount_ + 3;
}

bool Launcher::isDiagnosticIndex(size_t index) const
{
    return index == launcherScriptCount_ + 1;
}

bool Launcher::isRebootIndex(size_t index) const
{
    return index == launcherScriptCount_;
}

bool Launcher::isConfigIndex(size_t index) const
{
    return index == launcherScriptCount_ + 2;
}

String Launcher::launcherEntryName(size_t index) const
{
    if (index < launcherScriptCount_) return launcherScripts_[index];
    if (isRebootIndex(index)) return "REBOOT";
    if (isDiagnosticIndex(index)) return "Diagnostic";
    if (isConfigIndex(index)) return "CONFIG";
    return "MAIN";
}

int32_t Launcher::launcherApproxFreeRamBytes() const
{
    return approximateFreeRamBytes();
}

void Launcher::logLauncherDebug(const char *message) const
{
    if (message == nullptr || message[0] == '\0') return;
    logSetup(message);
}

void Launcher::logLauncherSelection(const char *tag) const
{
    char buffer[192] = {0};
    const size_t total = launcherMenuItemCount();
    const String selectedName = launcherEntryName(launcherSelectedIndex_);
    unsigned runCount = countRunningScripts();
    String focusName = "MAIN";
    char selectedNameShort[17] = {0};
    char focusNameShort[17] = {0};
    if (launcherFocusIndex_ >= 0) {
        const size_t focusIndex = static_cast<size_t>(launcherFocusIndex_);
        if (focusIndex < launcherScriptCount_) {
            focusName = launcherScripts_[focusIndex];
        } else if (isRebootIndex(focusIndex)) {
            focusName = "REBOOT";
        } else if (isDiagnosticIndex(focusIndex)) {
            focusName = "Diagnostic";
        } else if (isConfigIndex(focusIndex)) {
            focusName = "CONFIG";
        }
    }
    strncpy(selectedNameShort, selectedName.c_str(), sizeof(selectedNameShort) - 1);
    strncpy(focusNameShort, focusName.c_str(), sizeof(focusNameShort) - 1);
    snprintf(buffer, sizeof(buffer),
             "[LAUNCHER] %s total=%u selected=%u(%s) running=%u focus=%d(%s)",
             tag ? tag : "state",
             static_cast<unsigned>(total),
             static_cast<unsigned>(launcherSelectedIndex_),
             selectedNameShort,
             runCount,
             static_cast<int>(launcherFocusIndex_),
             focusNameShort);
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

void Launcher::launcherRequestReboot(bool bootloader)
{
    logLauncherDebug(bootloader ? "[LAUNCHER] reboot -> BOOTLOADER" : "[LAUNCHER] reboot -> REBOOT");
    logSetup(bootloader ? "[BOOT] Launcher: bootloader requested" : "[BOOT] Launcher: reboot requested");
    delay(50);
    if (bootloader) {
        _reboot_Teensyduino_();
    } else {
        SCB_AIRCR = 0x05FA0004;
    }
    while (true) {}
}