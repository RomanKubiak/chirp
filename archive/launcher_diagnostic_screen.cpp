// Archived diagnostic screen implementation.
//
// This file is intentionally not part of the firmware build.
// It preserves the diagnostic launcher screen code for later reuse.

void Launcher::launcherShowDiagnosticScreen()
{
    launcherMenuVisible_ = false;
    launcherFocusIndex_ = static_cast<int16_t>(launcherScriptCount_);
    lastLauncherStatsMs_ = 0;
    logLauncherDebug("[LAUNCHER] view -> DIAGNOSTIC");
    logLauncherSelection("button-short-click-diagnostic");
    logDiagnosticSnapshot("launcher");
    renderDiagnosticScreen();
}

void Launcher::renderDiagnosticScreen()
{
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
    const int32_t freeRam = approximateFreeRamBytes();
    const uint16_t midiQDepth = gMidiOutQueueDepth;
    const uint16_t midiQHigh = gMidiOutQueueHighWater;
    const uint16_t midiQCap = gMidiOutQueueCapacity;
    const bool sdMounted = (internalFlash.backend() == ChirpFS::Backend::SD);
    const char *storageBackend = internalFlash.backendName();
    const char *storageDiag = internalFlash.storageDiagnostic();
    const int32_t encoderPos = launcherEncoder_.read();

    char line[48] = {0};
    gDirtyTextUi.setCenteredLine(1, "DIAGNOSTIC", true, true);

    snprintf(line, sizeof(line), "CPU %lu  BUS %lu",
             static_cast<unsigned long>(cpuHz / 1000000UL),
             static_cast<unsigned long>(busHz / 1000000UL));
    gDirtyTextUi.setLine(3, 1, line, true, false, false);

    snprintf(line, sizeof(line), "RAM %ldB  WREN %luB",
             static_cast<long>(freeRam),
             static_cast<unsigned long>(wrenBytes));
    gDirtyTextUi.setLine(4, 1, line, true, false, false);

    snprintf(line, sizeof(line), "GC next %lu  heap %lu",
             static_cast<unsigned long>(wrenNextGc),
             static_cast<unsigned long>(config.initialHeapSize));
    gDirtyTextUi.setLine(5, 1, line, true, false, false);

    snprintf(line, sizeof(line), "STOR %s %s", storageBackend, storageDiag);
    gDirtyTextUi.setLine(7, 1, line, true, false, false);

    snprintf(line, sizeof(line), "SD %s  ENC %ld",
             sdMounted ? "yes" : "no",
             static_cast<long>(encoderPos));
    gDirtyTextUi.setLine(8, 1, line, true, false, false);

    snprintf(line, sizeof(line), "SCRIPTS %u  RUN %u",
             static_cast<unsigned>(launcherScriptCount_),
             static_cast<unsigned>(countRunningScripts()));
    gDirtyTextUi.setLine(9, 1, line, true, false, false);

    snprintf(line, sizeof(line), "Q %u/%u H%u", midiQDepth, midiQCap, midiQHigh);
    gDirtyTextUi.setLine(10, 1, line, true, false, false);

    snprintf(line, sizeof(line), "LOOP %lu MAX %lu",
             static_cast<unsigned long>(gDiag.loops),
             static_cast<unsigned long>(gDiag.loopMaxUs));
    gDirtyTextUi.setLine(11, 1, line, true, false, false);

    snprintf(line, sizeof(line), "MIDI %lu BUD %lu",
             static_cast<unsigned long>(gDiag.midiEvents),
             static_cast<unsigned long>(gDiag.midiBudgetHits));
    gDirtyTextUi.setLine(12, 1, line, true, false, false);

    gDirtyTextUi.setCenteredLine(13, "SELECT=MENU", false, false);
    gDirtyTextUi.render();

    char diagLog[256] = {0};
    snprintf(diagLog, sizeof(diagLog),
             "[DIAG] cpu=%luHz bus=%luHz freeRam=%ldB wrenBytes=%lu nextGC=%lu storage=%s sd=%s enc=%ld scripts=%u running=%u q=%u/%u loops=%lu maxLoopUs=%lu midiEv=%lu budget=%lu",
             static_cast<unsigned long>(cpuHz),
             static_cast<unsigned long>(busHz),
             static_cast<long>(freeRam),
             static_cast<unsigned long>(wrenBytes),
             static_cast<unsigned long>(wrenNextGc),
             storageDiag,
             sdMounted ? "yes" : "no",
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
