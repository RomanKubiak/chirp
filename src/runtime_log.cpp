#include "runtime_log.h"
#include "chirp_config.h"
#include "midi_router.h"
#include "usb_serial_protocol.h"
#include "wren_vm.h"

#include <Arduino.h>
#include <CrashReport.h>

extern WrenVM          *vm;
extern WrenConfiguration config;
extern "C" char        *sbrk(int incr);

// ── Stack pointer (ARM inline asm) ────────────────────────────────────────────
static uintptr_t currentStackPointer()
{
    uintptr_t sp = 0;
    asm volatile("mov %0, sp" : "=r"(sp));
    return sp;
}

static int32_t approximateFreeRamBytes()
{
    char *heapEnd = sbrk(0);
    if (heapEnd == nullptr) return -1;
    const uintptr_t sp   = currentStackPointer();
    const uintptr_t heap = reinterpret_cast<uintptr_t>(heapEnd);
    return (sp > heap) ? static_cast<int32_t>(sp - heap) : -1;
}

// ── Core sinks ────────────────────────────────────────────────────────────────
void logRuntime(const char *text)
{
    sendControlLog(text);
#if DEBUG_RUNTIME_SERIAL
    Serial.println(text);
#endif
}

void logSetup(const char *text)
{
    sendControlLog(text);
    Serial.println(text);
}

// ── Reset-flag formatter ──────────────────────────────────────────────────────
void formatResetFlags(uint32_t flags, char *out, size_t outSize)
{
    if (!out || outSize == 0) return;
    size_t used = 0;
    out[0] = '\0';
    for (uint8_t bit = 0; bit < 32; bit++)
    {
        if (!(flags & (1u << bit))) continue;
        int wrote = snprintf(out + used, outSize - used, "%sB%u", (used == 0) ? "" : ",", bit);
        if (wrote <= 0 || static_cast<size_t>(wrote) >= (outSize - used)) break;
        used += static_cast<size_t>(wrote);
    }
    if (used == 0) snprintf(out, outSize, "none");
}

// ── Boot-time diagnostic ──────────────────────────────────────────────────────
void logCrashReportIfPresent()
{
#if ENABLE_BOOT_DIAG
    logSetup(CrashReport ? "[DIAG crash] present" : "[DIAG crash] none");
#endif
}

void logDiagnosticSnapshot(const char *phase)
{
#if !ENABLE_BOOT_DIAG
    (void)phase;
#else
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
    const uint32_t cycleCount =
#ifdef ARM_DWT_CYCCNT
        static_cast<uint32_t>(ARM_DWT_CYCCNT);
#else
        0;
#endif
    const uint32_t resetFlags =
#ifdef SRC_SRSR
        static_cast<uint32_t>(SRC_SRSR);
#else
        0;
#endif
    char resetBits[96] = {0};
    formatResetFlags(resetFlags, resetBits, sizeof(resetBits));
    const int32_t freeRam    = approximateFreeRamBytes();
    const size_t  wrenBytes  = vm ? vm->bytesAllocated : 0;
    const size_t  wrenNextGc = vm ? vm->nextGC : 0;

    char buf[256] = {0};
    snprintf(buf, sizeof(buf),
             "[DIAG %s] cpuHz=%lu busHz=%lu ms=%lu us=%lu cycles=%lu reset=0x%08lX bits=%s",
             phase,
             static_cast<unsigned long>(cpuHz), static_cast<unsigned long>(busHz),
             static_cast<unsigned long>(millis()), static_cast<unsigned long>(micros()),
             static_cast<unsigned long>(cycleCount),
             static_cast<unsigned long>(resetFlags), resetBits);
    logSetup(buf);

    snprintf(buf, sizeof(buf),
             "[DIAG %s] freeRam~=%ldB wrenBytes=%lu wrenNextGC=%lu wrenCfg(init=%lu min=%lu grow=%d%%)",
             phase,
             static_cast<long>(freeRam),
             static_cast<unsigned long>(wrenBytes), static_cast<unsigned long>(wrenNextGc),
             static_cast<unsigned long>(config.initialHeapSize),
             static_cast<unsigned long>(config.minHeapSize),
             config.heapGrowthPercent);
    logSetup(buf);
#endif
}

// ── Periodic diagnostic (called every loop) ───────────────────────────────────
void logPeriodicDiagnostics()
{
#if ENABLE_PERIODIC_DIAG
    static uint32_t lastDiagMs  = 0;
    static uint32_t lastSampleUs = 0;
    const uint32_t nowMs = millis();
    if ((nowMs - lastDiagMs) < PERIODIC_DIAG_INTERVAL_MS) return;
    lastDiagMs = nowMs;

    const uint32_t nowUs = micros();
    if (lastSampleUs == 0) lastSampleUs = nowUs;
    uint32_t sampleWindowUs = static_cast<uint32_t>(nowUs - lastSampleUs);
    if (sampleWindowUs == 0) sampleWindowUs = 1;
    lastSampleUs = nowUs;

    const RuntimeDiagCounters snapshot = gDiag;
    gDiag = RuntimeDiagCounters{};

    uint32_t loopCpuPctX10 = static_cast<uint32_t>(
        (static_cast<uint64_t>(snapshot.loopBusyUs) * 1000ULL) / sampleWindowUs);
    if (loopCpuPctX10 > 1000) loopCpuPctX10 = 1000;

    uint32_t idlePctX10 = 0;
    if (snapshot.loops > 0)
    {
        idlePctX10 = static_cast<uint32_t>(
            (static_cast<uint64_t>(snapshot.idleLoops) * 1000ULL) / snapshot.loops);
        if (idlePctX10 > 1000) idlePctX10 = 1000;
    }

    auto perSec = [&](uint32_t n) {
        return static_cast<uint32_t>((static_cast<uint64_t>(n) * 1000000ULL) / sampleWindowUs);
    };

    const uint32_t cpuHz =
#ifdef F_CPU_ACTUAL
        static_cast<uint32_t>(F_CPU_ACTUAL);
#else
        static_cast<uint32_t>(F_CPU);
#endif
    const uint32_t cycleCount =
#ifdef ARM_DWT_CYCCNT
        static_cast<uint32_t>(ARM_DWT_CYCCNT);
#else
        0;
#endif
    const int32_t  freeRam   = approximateFreeRamBytes();
    const uintptr_t sp       = currentStackPointer();
    const uintptr_t heap     = reinterpret_cast<uintptr_t>(sbrk(0));
    const size_t   wrenBytes = vm ? vm->bytesAllocated : 0;
    const size_t   wrenNextGc = vm ? vm->nextGC : 0;
    const uint16_t qDepth   = gMidiOutQueueDepth;
    const uint16_t qHigh    = gMidiOutQueueHighWater;
    const uint16_t qCap     = gMidiOutQueueCapacity;

    char buf[320] = {0};
    snprintf(buf, sizeof(buf),
             "[DIAG tick] ms=%lu us=%lu cpuHz=%lu loopCpu=%lu.%lu%% idleLoops=%lu.%lu%%"
             " loops=%lu maxLoopUs=%lu midiEv/s=%lu txQ/s=%lu txSent/s=%lu txDrop/s=%lu"
             " midiBudgetHits=%lu ctrl/s=%lu wrenUs=%lu ctrlUs=%lu outQ=%u/%u cap=%u"
             " cycles=%lu sp=0x%08lX heap=0x%08lX freeRam~=%ldB wrenBytes=%lu nextGC=%lu",
             static_cast<unsigned long>(nowMs),
             static_cast<unsigned long>(nowUs),
             static_cast<unsigned long>(cpuHz),
             loopCpuPctX10 / 10, loopCpuPctX10 % 10,
             idlePctX10 / 10, idlePctX10 % 10,
             static_cast<unsigned long>(snapshot.loops),
             static_cast<unsigned long>(snapshot.loopMaxUs),
             static_cast<unsigned long>(perSec(snapshot.midiEvents)),
             static_cast<unsigned long>(perSec(snapshot.midiOutQueued)),
             static_cast<unsigned long>(perSec(snapshot.midiOutSent)),
             static_cast<unsigned long>(perSec(snapshot.midiOutDropped)),
             static_cast<unsigned long>(snapshot.midiBudgetHits),
             static_cast<unsigned long>(perSec(snapshot.controlFrames)),
             static_cast<unsigned long>(snapshot.wrenDispatchUs),
             static_cast<unsigned long>(snapshot.controlUs),
             static_cast<unsigned int>(qDepth),
             static_cast<unsigned int>(qHigh),
             static_cast<unsigned int>(qCap),
             static_cast<unsigned long>(cycleCount),
             static_cast<unsigned long>(sp),
             static_cast<unsigned long>(heap),
             static_cast<long>(freeRam),
             static_cast<unsigned long>(wrenBytes),
             static_cast<unsigned long>(wrenNextGc));
    logRuntime(buf);
#endif
}

// ── MIDI message tracing ──────────────────────────────────────────────────────
void logMidiMessage(const MIDIMessage &event)
{
#if !DEBUG_LOGGING
    (void)event;
#else
    if (event.type == 0xF8)
    {
        static uint32_t lastClockLogMs = 0;
        const uint32_t now = millis();
        if ((now - lastClockLogMs) < 250) return;
        lastClockLogMs = now;
    }

    const uint32_t ticksMs = millis();
    const uint32_t ticksUs = micros();

    const char *portName = "unknown";
    switch (event.port) {
    case 1: portName = "MIDI1";    break;
    case 2: portName = "MIDI2";    break;
    case 3: portName = "MIDI3";    break;
    case 4: portName = "MIDI4";    break;
    case 5: portName = "MIDI5";    break;
    case 6: portName = "USB-MIDI"; break;
    }

    const char *msgType  = "unknown";
    const char *msgExtra = "";
    switch (event.type) {
    case 0x80: msgType = "Note Off";         break;
    case 0x90: msgType = "Note On";          break;
    case 0xA0: msgType = "Poly Pressure";    break;
    case 0xB0: msgType = "Control Change";   break;
    case 0xC0: msgType = "Program Change";   break;
    case 0xD0: msgType = "Channel Pressure"; break;
    case 0xE0: msgType = "Pitch Bend";       break;
    case 0xF0: msgType = "System Exclusive"; msgExtra = " (SysEx)"; break;
    case 0xF1: msgType = "Time Code";        break;
    case 0xF2: msgType = "Song Position";    break;
    case 0xF3: msgType = "Song Select";      break;
    case 0xF6: msgType = "Tune Request";     break;
    case 0xF8: msgType = "Timing Clock";     break;
    case 0xFA: msgType = "Start";            break;
    case 0xFB: msgType = "Continue";         break;
    case 0xFC: msgType = "Stop";             break;
    case 0xFE: msgType = "Active Sensing";   break;
    case 0xFF: msgType = "Reset";            break;
    }

    char buf[192] = {0};
    if (event.type >= 0x80 && event.type < 0xF0)
    {
        uint8_t ch = (event.channel & 0x0F) + 1;
        switch (event.type & 0xF0)
        {
        case 0x80: case 0x90:
            snprintf(buf, sizeof(buf), "[t=%lums/%luus] [MIDI %s] %s Ch%u Note 0x%02X Vel %u",
                     static_cast<unsigned long>(ticksMs), static_cast<unsigned long>(ticksUs),
                     portName, msgType, ch, event.data1, event.data2); break;
        case 0xA0:
            snprintf(buf, sizeof(buf), "[t=%lums/%luus] [MIDI %s] %s Ch%u Note 0x%02X Pressure %u",
                     static_cast<unsigned long>(ticksMs), static_cast<unsigned long>(ticksUs),
                     portName, msgType, ch, event.data1, event.data2); break;
        case 0xB0:
            snprintf(buf, sizeof(buf), "[t=%lums/%luus] [MIDI %s] %s Ch%u CC 0x%02X = %u",
                     static_cast<unsigned long>(ticksMs), static_cast<unsigned long>(ticksUs),
                     portName, msgType, ch, event.data1, event.data2); break;
        case 0xC0:
            snprintf(buf, sizeof(buf), "[t=%lums/%luus] [MIDI %s] %s Ch%u Program %u",
                     static_cast<unsigned long>(ticksMs), static_cast<unsigned long>(ticksUs),
                     portName, msgType, ch, event.data1); break;
        case 0xD0:
            snprintf(buf, sizeof(buf), "[t=%lums/%luus] [MIDI %s] %s Ch%u Pressure %u",
                     static_cast<unsigned long>(ticksMs), static_cast<unsigned long>(ticksUs),
                     portName, msgType, ch, event.data1); break;
        case 0xE0:
            snprintf(buf, sizeof(buf), "[t=%lums/%luus] [MIDI %s] %s Ch%u Value 0x%04X",
                     static_cast<unsigned long>(ticksMs), static_cast<unsigned long>(ticksUs),
                     portName, msgType, ch,
                     static_cast<unsigned>(event.data1 | (event.data2 << 7))); break;
        default:
            snprintf(buf, sizeof(buf), "[t=%lums/%luus] [MIDI %s] 0x%02X Ch%u Data1=0x%02X Data2=0x%02X",
                     static_cast<unsigned long>(ticksMs), static_cast<unsigned long>(ticksUs),
                     portName, event.type, ch, event.data1, event.data2); break;
        }
    }
    else
    {
        snprintf(buf, sizeof(buf), "[t=%lums/%luus] [MIDI %s] %s%s Data1=0x%02X Data2=0x%02X",
                 static_cast<unsigned long>(ticksMs), static_cast<unsigned long>(ticksUs),
                 portName, msgType, msgExtra, event.data1, event.data2);
    }
    logRuntime(buf);
#endif
}

// ── Debug helpers ─────────────────────────────────────────────────────────────
void debugMidi1RawBytes()
{
#if DEBUG_LOGGING && DEBUG_MIDI1_RAW
    while (Serial1.available() > 0)
    {
        int b = Serial1.peek();
        char buf[32];
        snprintf(buf, sizeof(buf), "[MIDI1-RAW] 0x%02X (%3u)", b, b);
        logRuntime(buf);
        Serial1.read();
    }
#endif
}

void debugMidiReadStatus()
{
#if DEBUG_LOGGING && VERBOSE_MIDI_STATUS
    static uint32_t lastLog = 0;
    const uint32_t now = millis();
    if (now - lastLog < 2000) return;
    lastLog = now;
    bool m1 = MIDI1.read(), m2 = MIDI2.read(),
         m3 = MIDI3.read(), m4 = MIDI4.read(), m5 = MIDI5.read();
    char buf[80];
    snprintf(buf, sizeof(buf), "[MIDI STATUS] M1:%u M2:%u M3:%u M4:%u M5:%u",
             m1?1:0, m2?1:0, m3?1:0, m4?1:0, m5?1:0);
    logRuntime(buf);
#endif
}
