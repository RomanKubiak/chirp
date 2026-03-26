#ifndef LAUNCHER_H
#define LAUNCHER_H

#include <Arduino.h>
#include <Encoder.h>
#include "midi_types.h"

class Launcher
{
public:
    Launcher();

    void setup();
    void loop();

    void queueNavigation(bool prev, bool next, bool select, bool longPress);
    bool handleMidiControl(const MIDIMessage &event);

private:
    static constexpr size_t   kLauncherMaxScripts = 32;
    static constexpr uint32_t  kSystemStatsRefreshIntervalMs = 1000;
    static constexpr uint32_t  kSystemScreenPageIntervalMs = 5000;
    static constexpr uint32_t  kButtonDebounceMs = 25;
    static constexpr uint32_t  kLongPressThresholdMs = 300;
    static constexpr size_t    kBootAutoStartLimit = 1;
    static constexpr const char *kLauncherStatePath = "/userdata/launcher_state.json";

    Encoder launcherEncoder_;
    String  launcherScripts_[kLauncherMaxScripts];
    int16_t launcherScriptError_[kLauncherMaxScripts];
    bool    launcherRunningFlags_[kLauncherMaxScripts];

    size_t   launcherScriptCount_ = 0;
    size_t   launcherSelectedIndex_ = 0;
    int16_t  launcherFocusIndex_ = 0;
    uint32_t lastLauncherStatsMs_ = 0;
    uint32_t lastMidiRateSampleUs_ = 0;
    uint32_t lastMidiRateEvents_ = 0;
    uint32_t lastMidiEventsPerSec_ = 0;
    uint32_t lastUiFrameMs_ = 0;

    bool pendingNavPrev_ = false;
    bool pendingNavNext_ = false;
    bool pendingNavSelect_ = false;
    bool pendingNavLongPress_ = false;

    int32_t  lastEncoderPosition_ = 0;
    bool     lastButtonPressed_ = false;
    uint32_t buttonDebounceStartMs_ = 0;
    uint32_t buttonPressStartMs_ = 0;
    uint32_t lastLoopRefreshMs_ = 0;

    void initializeLauncherState();
    void launcherSaveState();
    bool launcherLoadAndAutoStart();

    void refreshLauncherStatus();
    void renderLauncherScreen();
    void stopScript(size_t idx);
    void launcherSwitchFocusContext();
    void launcherToggleScriptRuntime();
    bool launcherHandleNavigation(bool prev, bool next, bool select, bool longPress);

    bool isScriptRunning(size_t index) const;
    bool isScriptFocused(size_t index) const;
    bool focusedScriptIsRunning() const;
    String launcherEntryName(size_t index) const;
    int32_t launcherApproxFreeRamBytes() const;
    void logLauncherDebug(const char *message) const;
    void logLauncherSelection(const char *tag) const;
    void processPendingUsbNavigation();
};

extern Launcher gLauncher;
bool launcherMidiPreDispatchHook(const MIDIMessage &event);
void queueLauncherNavigation(bool prev, bool next, bool select, bool longPress);

#endif // LAUNCHER_H