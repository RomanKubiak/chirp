#include "chirp_boot.h"

#include "chirp_config.h"

#include "display_boot.h"

#include "MIDI.h"
#include "launcher.h"
#include "midi_router.h"
#include "runtime_log.h"
#include "script_storage.h"
#include "wren_fs_provider.h"
#include "wren_host.h"
#include "wren_midi_bridge.h"
#include "wren_runtime_script.h"
#include "wren_vm.h"

#include <cstdio>

extern WrenConfiguration config;
extern WrenVM *vm;
extern ChirpFS internalFlash;
extern ScriptStorage scriptStorage;

namespace {
void initializeMidiPorts()
{
    MIDI1.begin(MIDI_CHANNEL_OMNI); MIDI1.turnThruOff();
    MIDI2.begin(MIDI_CHANNEL_OMNI); MIDI2.turnThruOff();
    MIDI3.begin(MIDI_CHANNEL_OMNI); MIDI3.turnThruOff();
    MIDI4.begin(MIDI_CHANNEL_OMNI); MIDI4.turnThruOff();
    MIDI5.begin(MIDI_CHANNEL_OMNI); MIDI5.turnThruOff();
}
} // namespace

void initializeChirpBoot()
{
#if ENABLE_LIVE_DEBUG
    debug.begin(SerialUSB1);
#endif
    Serial.begin(115200);
    {
        const uint32_t serialDeadlineMs = millis() + SERIAL_WAIT_TIMEOUT_MS;
        while (!Serial && millis() < serialDeadlineMs) delay(1);
    }

    initializeBootDisplay();

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

    WrenFsProvider fsProvider = createWrenFsProvider(internalFlash);
    WrenMidiBridge::setFsProvider(fsProvider);

    vm = wrenNewVM(&config);

    bool runtimeReady = initializeWrenRuntime();
    bool bridgeReady = runtimeReady && WrenMidiBridge::initialize(vm);
    bool scriptsReady = (runtimeReady && bridgeReady) && prepareStoredWrenScriptsOnBoot();

    logSetup(runtimeReady  ? "[BOOT] Wren runtime ready"          : "[BOOT] Wren runtime failed");
    logSetup(bridgeReady   ? "[BOOT] Wren MIDI bridge ready"      : "[BOOT] Wren MIDI bridge unavailable");
    logSetup(scriptsReady  ? "[BOOT] /scripts Wren scripts ready" : "[BOOT] /scripts Wren scripts incomplete");

    logDiagnosticSnapshot("post-init");

    initializeMidiPorts();
    gLauncher.setup();

    logSetup("[BOOT] Teensy ready");
    logSetup("[BOOT] Setup complete");
    Serial.flush();
    delay(40);
}