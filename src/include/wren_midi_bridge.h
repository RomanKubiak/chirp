#ifndef WREN_MIDI_BRIDGE_H
#define WREN_MIDI_BRIDGE_H

#include "midi_types.h"
#include "wren.hpp"
#include <Arduino.h>

using MidiOutputSendFn = bool (*)(const MIDIMessage &event);
using WrenRuntimeLogFn = void (*)(const char *text);

// ── Filesystem provider ───────────────────────────────────────────────────────
// Fill this struct and call WrenMidiBridge::setFsProvider() so Wren scripts can
// read/write/list files on the device filesystem.
struct WrenFsProvider
{
    // Read entire file. Returns false on error; out is left empty.
    bool    (*read)   (const char *path, String &out)           = nullptr;
    // Write data (NUL-terminated string). Returns false on error.
    bool    (*write)  (const char *path, const char *data, size_t len) = nullptr;
    // Remove a file. Returns false if not found or error.
    bool    (*remove) (const char *path)                        = nullptr;
    // Returns true if the path exists (file or dir).
    bool    (*exists) (const char *path)                        = nullptr;
    // Returns file byte count, or -1 if not found.
    int32_t (*size)   (const char *path)                        = nullptr;
    // Fill names[0..maxNames) with entry names (files+dirs) in path.
    // Returns total count found (may exceed maxNames).
    size_t  (*list)   (const char *path, String *names, size_t maxNames) = nullptr;
};

class WrenMidiBridge
{
public:
    static void configure(WrenConfiguration &config);
    static void setOutputSender(MidiOutputSendFn sender);
    static void setRuntimeLogger(WrenRuntimeLogFn logger);
    static void setFsProvider(const WrenFsProvider &provider);
    static bool initialize(WrenVM *vm);
    static void dispatchEvent(WrenVM *vm, const MIDIMessage &event);
    static void shutdown(WrenVM *vm);

    static void clearRegisteredScripts();
    static void registerScriptName(const char *scriptName);
    static bool setActiveScriptName(const char *scriptName);
    static void clearActiveScriptSelection();
    static void beginScriptContext(const char *scriptName);
    static void endScriptContext();
};

#endif // WREN_MIDI_BRIDGE_H