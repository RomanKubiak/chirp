#ifndef RUNTIME_LOG_H
#define RUNTIME_LOG_H

#include <stdint.h>
#include "midi_types.h"

// ── Core log sinks ─────────────────────────────────────────────────────────────
// sendControlLog is defined in merge.ino (needs direct access to usbHandler).
void sendControlLog(const char *text, bool flushNow = false);
void logRuntime(const char *text);
void logSetup(const char *text);

// ── Diagnostic helpers ─────────────────────────────────────────────────────────
void formatResetFlags(uint32_t flags, char *out, size_t outSize);
void logCrashReportIfPresent();
void logDiagnosticSnapshot(const char *phase);
void logPeriodicDiagnostics();

// ── MIDI event tracing (debug-gated) ─────────────────────────────────────────
void logMidiMessage(const MIDIMessage &event);
void debugMidi1RawBytes();
void debugMidiReadStatus();

#endif // RUNTIME_LOG_H
