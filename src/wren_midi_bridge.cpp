#include "wren_midi_bridge.h"

#include <Arduino.h>
#include <cctype>
#include <cstring>
#include <string>

#ifndef DEBUG_WREN_BRIDGE_SERIAL
#define DEBUG_WREN_BRIDGE_SERIAL 0
#endif

#if DEBUG_WREN_BRIDGE_SERIAL
#define WREN_BRIDGE_SERIAL_PRINTLN(msg) Serial.println(msg)
#else
#define WREN_BRIDGE_SERIAL_PRINTLN(msg) do {} while (0)
#endif

namespace
{
constexpr const char *kWrenRuntimeModule = "merge_runtime";

const char kEmbeddedWrenMidiRuntime[] = R"WREN(

foreign class MidiNative {
    foreign static noteOffType()
    foreign static noteOnType()
    foreign static controlChangeType()
    foreign static programChangeType()
    foreign static channelPressureType()
    foreign static pitchBendType()
    foreign static portName(port)
    foreign static typeName(type)
    foreign static send(port, type, channel, data1, data2)
}

foreign class TeensyClockNative {
    foreign static ticksMs()
    foreign static ticksUs()
}

foreign class DebugNative {
    foreign static debug(message)
    foreign static info(message)
    foreign static warn(message)
    foreign static error(message)
}

foreign class FileNative {
    foreign static read(path)
    foreign static write(path, content)
    foreign static remove(path)
    foreign static exists(path)
    foreign static size(path)
    foreign static list(path)
}

foreign class ConfigNative {
    foreign static parse(text)
}

class Clock {
    static ticksMs() {
        return TeensyClockNative.ticksMs()
    }

    static ticksUs() {
        return TeensyClockNative.ticksUs()
    }

    static prefix() {
        return "[t=%(Clock.ticksMs())ms/%(Clock.ticksUs())us]"
    }
}

class Log {
    static debug(message) {
        return DebugNative.debug("%(message)")
    }

    static info(message) {
        return DebugNative.info("%(message)")
    }

    static warn(message) {
        return DebugNative.warn("%(message)")
    }

    static error(message) {
        return DebugNative.error("%(message)")
    }
}

// ── File I/O ───────────────────────────────────────────────────────────────
// Paths must start with /scripts/ or /data/ and may not contain "..".
//
//   var src = File.read("/data/synth.cfg")   // String or null on error
//   File.write("/data/out.txt", "hello")     // Bool
//   File.exists("/data/synth.cfg")           // Bool
//   File.size("/data/synth.cfg")             // Num (bytes) or -1
//   File.remove("/data/synth.cfg")           // Bool
//   var names = File.list("/data")           // List of entry names

class File {
    static read(path)          { FileNative.read(path) }
    static write(path, content){ FileNative.write(path, "%(content)") }
    static remove(path)        { FileNative.remove(path) }
    static exists(path)        { FileNative.exists(path) }
    static size(path)          { FileNative.size(path) }
    static list(path)          { FileNative.list(path) }
}

// ── Config file parser ─────────────────────────────────────────────────────
// Parses strict INI-style text into a flat Map.
// Supports [section] headers and stores entries as "section.key".
// Lines starting with # or ; are comments. Inline comments are supported.
// Quoted values support escapes: \n, \r, \t, \\, \", \\'.
//
//   var cfg = Config.load("/data/jx3p.cfg")
//   Log.info(cfg["CC_CUTOFF"])              // "74"
//   Log.info(cfg.get("NAME", "unknown"))    // "Roland JX-3P"
//   Log.info(cfg.num("CC_CUTOFF", 0))       // 74  (as Num)

class Config {
    construct init(m) { _m = m }

    static parse(text) { Config.init(ConfigNative.parse(text == null ? "" : text)) }
    static load(path)  { Config.parse(File.read(path)) }

    [key]               { _m[key] }
    [key]=(val)         { _m[key] = val }
    get(key, fallback)    { _m.containsKey(key) ? _m[key] : fallback }
    num(key, fallback)    {
        if (!_m.containsKey(key)) return fallback
        var n = Num.fromString(_m[key])
        return n == null ? fallback : n
    }
    has(key)   { _m.containsKey(key) }
    keys       { _m.keys.toList }
    toMap      { _m }
}

class MidiEvent {
    construct new(port, type, channel, data1, data2, ticksMs, ticksUs) {
        _port = port
        _type = type
        _channel = channel
        _data1 = data1
        _data2 = data2
        _ticksMs = ticksMs
        _ticksUs = ticksUs
    }

    port { _port }
    type { _type }
    channel { _channel }
    channelNumber { _channel + 1 }
    data1 { _data1 }
    data2 { _data2 }
    ticksMs { _ticksMs }
    ticksUs { _ticksUs }
    note { _data1 }
    velocity { _data2 }
    controller { _data1 }
    value { _data2 }
    program { _data1 }
    portName { Midi.portName(_port) }
    typeName { Midi.typeName(_type) }
    isNoteOn { _type == Midi.noteOnType && _data2 > 0 }
    isNoteOff { _type == Midi.noteOffType || (_type == Midi.noteOnType && _data2 == 0) }
    isControlChange { _type == Midi.controlChangeType }
    isProgramChange { _type == Midi.programChangeType }
    isChannelPressure { _type == Midi.channelPressureType }
    isPitchBend { _type == Midi.pitchBendType }
}

class MidiApi {
    construct new() {
        _eventListeners = []
        _noteOnListeners = []
        _noteOffListeners = []
        _controlChangeListeners = []
        _programChangeListeners = []
    }

    listen(callback) {
        _eventListeners.add(callback)
        return callback
    }

    onEvent(callback) {
        return listen(callback)
    }

    onNoteOn(callback) {
        _noteOnListeners.add(callback)
        return callback
    }

    onNoteOff(callback) {
        _noteOffListeners.add(callback)
        return callback
    }

    onControlChange(callback) {
        _controlChangeListeners.add(callback)
        return callback
    }

    onProgramChange(callback) {
        _programChangeListeners.add(callback)
        return callback
    }

    clearListeners() {
        _eventListeners = []
        _noteOnListeners = []
        _noteOffListeners = []
        _controlChangeListeners = []
        _programChangeListeners = []
    }

    noteOffType { MidiNative.noteOffType() }
    noteOnType { MidiNative.noteOnType() }
    controlChangeType { MidiNative.controlChangeType() }
    programChangeType { MidiNative.programChangeType() }
    channelPressureType { MidiNative.channelPressureType() }
    pitchBendType { MidiNative.pitchBendType() }

    portName(port) {
        return MidiNative.portName(port)
    }

    typeName(type) {
        return MidiNative.typeName(type)
    }

    send(port, type, channel, data1, data2) {
        return MidiNative.send(port, type, channel, data1, data2)
    }

    noteOn(port, channel, note, velocity) {
        return send(port, noteOnType, channel, note, velocity)
    }

    noteOff(port, channel, note, velocity) {
        return send(port, noteOffType, channel, note, velocity)
    }

    controlChange(port, channel, controller, value) {
        return send(port, controlChangeType, channel, controller, value)
    }

    cc(port, channel, controller, value) {
        return controlChange(port, channel, controller, value)
    }

    programChange(port, channel, program) {
        return send(port, programChangeType, channel, program, 0)
    }

    channelPressure(port, channel, pressure) {
        return send(port, channelPressureType, channel, pressure, 0)
    }

    pitchBend(port, channel, value) {
        var clamped = value
        if (clamped < 0) clamped = 0
        if (clamped > 16383) clamped = 16383
        var lsb = clamped & 0x7F
        var msb = (clamped >> 7) & 0x7F
        return send(port, pitchBendType, channel, lsb, msb)
    }

    dispatchFromNative(port, type, channel, data1, data2) {
        var event = MidiEvent.new(port, type, channel, data1, data2, Clock.ticksMs(), Clock.ticksUs())
        emitListeners(_eventListeners, event)
        if (event.isNoteOn) emitListeners(_noteOnListeners, event)
        if (event.isNoteOff) emitListeners(_noteOffListeners, event)
        if (event.isControlChange) emitListeners(_controlChangeListeners, event)
        if (event.isProgramChange) emitListeners(_programChangeListeners, event)
    }

    emitListeners(listeners, event) {
        for (listener in listeners) {
            listener.call(event)
        }
    }
}

var Midi = MidiApi.new()

// ── Script lifecycle hooks ─────────────────────────────────────────────────
// Scripts can register an optional teardown callback:
//
//   Script.onUnload {
//     Midi.clearListeners()
//     Log.info("unloaded")
//   }
//
// The firmware calls Script.callUnload() before hot-reloading a script,
// then clears all Midi listeners automatically so the new script starts clean.

class Script {
    static onUnload(fn) { __fn = fn }
    static callUnload() {
        if (__fn is Fn) __fn.call()
        __fn = null
    }
}
Script.onUnload(null)
)WREN";

const char kEmbeddedWrenMidiExample[] = R"WREN(
System.print("Embedded MIDI example registering listeners")

Midi.onEvent(Fn.new { |event|
    if (event.typeName == "timingClock") return
    System.print("[MIDI] %(event.portName) %(event.typeName) ch=%(event.channelNumber) data1=%(event.data1) data2=%(event.data2)")
})

Midi.onNoteOn(Fn.new { |event|
    System.print("[NOTE ON] %(event.portName) ch=%(event.channelNumber) note=%(event.note) vel=%(event.velocity)")
})

Midi.onControlChange(Fn.new { |event|
    System.print("[CC] %(event.portName) ch=%(event.channelNumber) cc=%(event.controller) value=%(event.value)")
})

System.print("Embedded MIDI example ready")
)WREN";

WrenHandle *gMidiClassHandle = nullptr;
WrenHandle *gDispatchHandle = nullptr;
MidiOutputSendFn gOutputSendFn = nullptr;
WrenRuntimeLogFn gRuntimeLogFn = nullptr;
WrenFsProvider   gFsProvider   = {};

void releaseHandle(WrenVM *vm, WrenHandle *&handle)
{
    if (handle != nullptr)
    {
        wrenReleaseHandle(vm, handle);
        handle = nullptr;
    }
}

void midiNativeAllocate(WrenVM *vm)
{
    wrenSetSlotNewForeign(vm, 0, 0, 0);
}

const char *portNameFor(uint8_t port)
{
    switch (port)
    {
    case 1: return "MIDI1";
    case 2: return "MIDI2";
    case 3: return "MIDI3";
    case 4: return "MIDI4";
    case 5: return "MIDI5";
    case 6: return "USB-MIDI";
    default: return "unknown";
    }
}

const char *typeNameFor(uint8_t type)
{
    switch (type)
    {
    case 0x80: return "noteOff";
    case 0x90: return "noteOn";
    case 0xA0: return "polyPressure";
    case 0xB0: return "controlChange";
    case 0xC0: return "programChange";
    case 0xD0: return "channelPressure";
    case 0xE0: return "pitchBend";
    case 0xF0: return "systemExclusive";
    case 0xF1: return "timeCode";
    case 0xF2: return "songPosition";
    case 0xF3: return "songSelect";
    case 0xF6: return "tuneRequest";
    case 0xF8: return "timingClock";
    case 0xFA: return "start";
    case 0xFB: return "continue";
    case 0xFC: return "stop";
    case 0xFE: return "activeSensing";
    case 0xFF: return "reset";
    default: return "unknown";
    }
}

void setSlotTypeConstant(WrenVM *vm, int slot, uint8_t value)
{
    wrenSetSlotDouble(vm, slot, static_cast<double>(value));
}

void midiNativeNoteOffType(WrenVM *vm)
{
    setSlotTypeConstant(vm, 0, static_cast<uint8_t>(MIDIMessageType::NOTE_OFF));
}

void midiNativeNoteOnType(WrenVM *vm)
{
    setSlotTypeConstant(vm, 0, static_cast<uint8_t>(MIDIMessageType::NOTE_ON));
}

void midiNativeControlChangeType(WrenVM *vm)
{
    setSlotTypeConstant(vm, 0, static_cast<uint8_t>(MIDIMessageType::CONTROL_CHANGE));
}

void midiNativeProgramChangeType(WrenVM *vm)
{
    setSlotTypeConstant(vm, 0, static_cast<uint8_t>(MIDIMessageType::PROGRAM_CHANGE));
}

void midiNativeChannelPressureType(WrenVM *vm)
{
    setSlotTypeConstant(vm, 0, static_cast<uint8_t>(MIDIMessageType::CHANNEL_PRESSURE));
}

void midiNativePitchBendType(WrenVM *vm)
{
    setSlotTypeConstant(vm, 0, static_cast<uint8_t>(MIDIMessageType::PITCH_BEND));
}

void teensyClockNativeTicksMs(WrenVM *vm)
{
    wrenSetSlotDouble(vm, 0, static_cast<double>(millis()));
}

void teensyClockNativeTicksUs(WrenVM *vm)
{
    wrenSetSlotDouble(vm, 0, static_cast<double>(micros()));
}

void midiNativePortName(WrenVM *vm)
{
    uint8_t port = static_cast<uint8_t>(wrenGetSlotDouble(vm, 1));
    wrenSetSlotString(vm, 0, portNameFor(port));
}

void midiNativeTypeName(WrenVM *vm)
{
    uint8_t type = static_cast<uint8_t>(wrenGetSlotDouble(vm, 1));
    wrenSetSlotString(vm, 0, typeNameFor(type));
}

void midiNativeSend(WrenVM *vm)
{
    if (gOutputSendFn == nullptr)
    {
        wrenSetSlotBool(vm, 0, false);
        return;
    }

    MIDIMessage event;
    event.port = static_cast<uint8_t>(wrenGetSlotDouble(vm, 1));
    event.type = static_cast<uint8_t>(wrenGetSlotDouble(vm, 2));
    event.channel = static_cast<uint8_t>(wrenGetSlotDouble(vm, 3));
    event.data1 = static_cast<uint8_t>(wrenGetSlotDouble(vm, 4));
    event.data2 = static_cast<uint8_t>(wrenGetSlotDouble(vm, 5));

    // Allow ports 1-6 and all byte-sized MIDI fields from Wren.
    if (event.port < 1 || event.port > 6)
    {
        wrenSetSlotBool(vm, 0, false);
        return;
    }

    const bool queued = gOutputSendFn(event);
    wrenSetSlotBool(vm, 0, queued);
}

void logFromWren(WrenVM *vm, const char *level)
{
    const char *message = "";
    if (wrenGetSlotType(vm, 1) == WREN_TYPE_STRING)
    {
        message = wrenGetSlotString(vm, 1);
    }

    if (gRuntimeLogFn != nullptr)
    {
        char buf[220] = {0};
        snprintf(buf, sizeof(buf), "[WREN %s] %s", level, message);
        gRuntimeLogFn(buf);
    }

    wrenSetSlotBool(vm, 0, true);
}

void debugNativeDebug(WrenVM *vm)
{
    logFromWren(vm, "DEBUG");
}

void debugNativeInfo(WrenVM *vm)
{
    logFromWren(vm, "INFO");
}

void debugNativeWarn(WrenVM *vm)
{
    logFromWren(vm, "WARN");
}

void debugNativeError(WrenVM *vm)
{
    logFromWren(vm, "ERROR");
}

// ── Path validation ────────────────────────────────────────────────────────
// Accept only /scripts/* and /data/* to prevent filesystem escapes.
static bool isValidFsPath(const char *path)
{
    if (path == nullptr || path[0] != '/') return false;
    if (strstr(path, "..") != nullptr)     return false;
    return (std::strncmp(path, "/scripts/", 9) == 0 ||
            std::strncmp(path, "/data/",    6) == 0 ||
            std::strcmp(path, "/data")         == 0 ||
            std::strcmp(path, "/scripts")      == 0);
}

// ── FileNative handlers ────────────────────────────────────────────────────
void fileNativeRead(WrenVM *vm)
{
    if (wrenGetSlotType(vm, 1) != WREN_TYPE_STRING ||
        gFsProvider.read == nullptr)
    {
        wrenSetSlotNull(vm, 0); return;
    }
    const char *path = wrenGetSlotString(vm, 1);
    if (!isValidFsPath(path)) { wrenSetSlotNull(vm, 0); return; }
    String out;
    if (!gFsProvider.read(path, out)) { wrenSetSlotNull(vm, 0); return; }
    wrenSetSlotString(vm, 0, out.c_str());
}

void fileNativeWrite(WrenVM *vm)
{
    if (wrenGetSlotType(vm, 1) != WREN_TYPE_STRING ||
        wrenGetSlotType(vm, 2) != WREN_TYPE_STRING ||
        gFsProvider.write == nullptr)
    {
        wrenSetSlotBool(vm, 0, false); return;
    }
    const char *path    = wrenGetSlotString(vm, 1);
    const char *content = wrenGetSlotString(vm, 2);
    if (!isValidFsPath(path)) { wrenSetSlotBool(vm, 0, false); return; }
    wrenSetSlotBool(vm, 0, gFsProvider.write(path, content, strlen(content)));
}

void fileNativeRemove(WrenVM *vm)
{
    if (wrenGetSlotType(vm, 1) != WREN_TYPE_STRING ||
        gFsProvider.remove == nullptr)
    {
        wrenSetSlotBool(vm, 0, false); return;
    }
    const char *path = wrenGetSlotString(vm, 1);
    if (!isValidFsPath(path)) { wrenSetSlotBool(vm, 0, false); return; }
    wrenSetSlotBool(vm, 0, gFsProvider.remove(path));
}

void fileNativeExists(WrenVM *vm)
{
    if (wrenGetSlotType(vm, 1) != WREN_TYPE_STRING ||
        gFsProvider.exists == nullptr)
    {
        wrenSetSlotBool(vm, 0, false); return;
    }
    const char *path = wrenGetSlotString(vm, 1);
    if (!isValidFsPath(path)) { wrenSetSlotBool(vm, 0, false); return; }
    wrenSetSlotBool(vm, 0, gFsProvider.exists(path));
}

void fileNativeSize(WrenVM *vm)
{
    if (wrenGetSlotType(vm, 1) != WREN_TYPE_STRING ||
        gFsProvider.size == nullptr)
    {
        wrenSetSlotDouble(vm, 0, -1.0); return;
    }
    const char *path = wrenGetSlotString(vm, 1);
    if (!isValidFsPath(path)) { wrenSetSlotDouble(vm, 0, -1.0); return; }
    wrenSetSlotDouble(vm, 0, static_cast<double>(gFsProvider.size(path)));
}

void fileNativeList(WrenVM *vm)
{
    wrenSetSlotNewList(vm, 0);
    if (wrenGetSlotType(vm, 1) != WREN_TYPE_STRING ||
        gFsProvider.list == nullptr) return;
    const char *path = wrenGetSlotString(vm, 1);
    if (!isValidFsPath(path) && std::strcmp(path, "/data") != 0 &&
        std::strcmp(path, "/scripts") != 0) return;

    static constexpr size_t kMaxEntries = 64;
    String names[kMaxEntries];
    size_t count = gFsProvider.list(path, names, kMaxEntries);
    wrenEnsureSlots(vm, 2);
    size_t limit = count < kMaxEntries ? count : kMaxEntries;
    for (size_t i = 0; i < limit; i++)
    {
        wrenSetSlotString(vm, 1, names[i].c_str());
        wrenInsertInList(vm, 0, -1, 1);
    }
}

// ── ConfigNative handler ───────────────────────────────────────────────────
// Strict INI parser with support for:
// - section headers: [section.subsection]
// - key/value pairs: key = value
// - line comments: # ... and ; ...
// - inline comments outside quotes
// - quoted values with basic escapes: \n \r \t \\ \" \'
//
// Parsed keys are flattened as: section.key
// Example:
//   [midi]
//   rx_channel = 10
// becomes:
//   "midi.rx_channel" -> "10"
void configNativeParse(WrenVM *vm)
{
    auto isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };

    auto trim = [&](std::string &s) {
        size_t b = 0;
        while (b < s.size() && isSpace(s[b])) b++;
        size_t e = s.size();
        while (e > b && isSpace(s[e - 1])) e--;
        s = s.substr(b, e - b);
    };

    auto abortParse = [&](unsigned long lineNo, const char *reason) {
        char err[192] = {0};
        snprintf(err, sizeof(err), "Config parse error line %lu: %s", lineNo, reason);
        wrenSetSlotString(vm, 3, err);
        wrenAbortFiber(vm, 3);
    };

    auto unescape = [](const std::string &src) {
        std::string out;
        out.reserve(src.size());
        for (size_t i = 0; i < src.size(); i++)
        {
            if (src[i] != '\\' || i + 1 >= src.size())
            {
                out.push_back(src[i]);
                continue;
            }

            char n = src[++i];
            switch (n)
            {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case '\\': out.push_back('\\'); break;
            case '"': out.push_back('"'); break;
            case '\'': out.push_back('\''); break;
            default:
                out.push_back(n);
                break;
            }
        }
        return out;
    };

    wrenEnsureSlots(vm, 4); // slot 0=map, 1=key, 2=value, 3=error
    wrenSetSlotNewMap(vm, 0);
    if (wrenGetSlotType(vm, 1) != WREN_TYPE_STRING)
    {
        wrenSetSlotString(vm, 3, "Config.parse expects a string");
        wrenAbortFiber(vm, 3);
        return;
    }

    const char *text = wrenGetSlotString(vm, 1);
    if (text == nullptr) return;

    std::string currentSection;
    const char *p = text;
    unsigned long lineNo = 1;

    // Skip UTF-8 BOM if present.
    if (static_cast<unsigned char>(p[0]) == 0xEF &&
        static_cast<unsigned char>(p[1]) == 0xBB &&
        static_cast<unsigned char>(p[2]) == 0xBF)
    {
        p += 3;
    }

    while (*p)
    {
        const char *lineEnd = p;
        while (*lineEnd && *lineEnd != '\n' && *lineEnd != '\r') lineEnd++;

        std::string line(p, static_cast<size_t>(lineEnd - p));
        trim(line);

        if (!line.empty() && line[0] != '#' && line[0] != ';')
        {
            if (line[0] == '[')
            {
                size_t close = line.find(']');
                if (close == std::string::npos)
                {
                    abortParse(lineNo, "missing closing ']' in section header");
                    return;
                }

                std::string trailing = line.substr(close + 1);
                trim(trailing);
                if (!trailing.empty() && trailing[0] != '#' && trailing[0] != ';')
                {
                    abortParse(lineNo, "unexpected characters after section header");
                    return;
                }

                currentSection = line.substr(1, close - 1);
                trim(currentSection);
                if (currentSection.empty())
                {
                    abortParse(lineNo, "empty section name");
                    return;
                }
            }
            else
            {
                // Find '=' not inside quotes.
                bool inSingle = false;
                bool inDouble = false;
                size_t eqPos = std::string::npos;
                for (size_t i = 0; i < line.size(); i++)
                {
                    const char c = line[i];
                    const bool escaped = (i > 0 && line[i - 1] == '\\');
                    if (c == '\'' && !inDouble && !escaped) inSingle = !inSingle;
                    else if (c == '"' && !inSingle && !escaped) inDouble = !inDouble;
                    else if (c == '=' && !inSingle && !inDouble)
                    {
                        eqPos = i;
                        break;
                    }
                }

                if (eqPos == std::string::npos)
                {
                    abortParse(lineNo, "expected '=' in key/value assignment");
                    return;
                }

                std::string key = line.substr(0, eqPos);
                std::string value = line.substr(eqPos + 1);
                trim(key);
                trim(value);

                if (key.empty())
                {
                    abortParse(lineNo, "empty key name");
                    return;
                }

                // Remove inline comments from unquoted values.
                if (!value.empty() && value[0] != '"' && value[0] != '\'')
                {
                    size_t commentPos = std::string::npos;
                    for (size_t i = 0; i < value.size(); i++)
                    {
                        if ((value[i] == '#' || value[i] == ';') &&
                            (i == 0 || std::isspace(static_cast<unsigned char>(value[i - 1]))))
                        {
                            commentPos = i;
                            break;
                        }
                    }
                    if (commentPos != std::string::npos)
                    {
                        value = value.substr(0, commentPos);
                        trim(value);
                    }
                }
                else if (!value.empty())
                {
                    // Quoted value: find closing quote and allow trailing comment.
                    const char q = value[0];
                    size_t close = std::string::npos;
                    for (size_t i = 1; i < value.size(); i++)
                    {
                        if (value[i] == q && value[i - 1] != '\\')
                        {
                            close = i;
                            break;
                        }
                    }
                    if (close == std::string::npos)
                    {
                        abortParse(lineNo, "unterminated quoted value");
                        return;
                    }

                    std::string trailing = value.substr(close + 1);
                    trim(trailing);
                    if (!trailing.empty() && trailing[0] != '#' && trailing[0] != ';')
                    {
                        abortParse(lineNo, "unexpected characters after quoted value");
                        return;
                    }

                    value = value.substr(1, close - 1);
                    value = unescape(value);
                }

                const std::string fullKey = currentSection.empty()
                    ? key
                    : (currentSection + "." + key);

                wrenSetSlotString(vm, 1, fullKey.c_str());
                wrenSetSlotString(vm, 2, value.c_str());
                wrenSetMapValue(vm, 0, 1, 2);
            }
        }

        p = lineEnd;
        if (*p == '\r')
        {
            p++;
            if (*p == '\n') p++;
            lineNo++;
        }
        else if (*p == '\n')
        {
            p++;
            lineNo++;
        }
        else if (*p == '\0')
        {
            break;
        }
    }
}

WrenForeignMethodFn bindMidiForeignMethod(WrenVM *vm,
                                          const char *module,
                                          const char *className,
                                          bool isStatic,
                                          const char *signature)
{
    (void)vm;

    if (std::strcmp(module, kWrenRuntimeModule) != 0)
    {
        return nullptr;
    }

    if (!isStatic)
    {
        return nullptr;
    }

    if (std::strcmp(className, "MidiNative") == 0)
    {
        if (std::strcmp(signature, "noteOffType()") == 0) return midiNativeNoteOffType;
        if (std::strcmp(signature, "noteOnType()") == 0) return midiNativeNoteOnType;
        if (std::strcmp(signature, "controlChangeType()") == 0) return midiNativeControlChangeType;
        if (std::strcmp(signature, "programChangeType()") == 0) return midiNativeProgramChangeType;
        if (std::strcmp(signature, "channelPressureType()") == 0) return midiNativeChannelPressureType;
        if (std::strcmp(signature, "pitchBendType()") == 0) return midiNativePitchBendType;
        if (std::strcmp(signature, "portName(_)") == 0) return midiNativePortName;
        if (std::strcmp(signature, "typeName(_)") == 0) return midiNativeTypeName;
        if (std::strcmp(signature, "send(_,_,_,_,_)") == 0) return midiNativeSend;
    }

    if (std::strcmp(className, "DebugNative") == 0)
    {
        if (std::strcmp(signature, "debug(_)") == 0) return debugNativeDebug;
        if (std::strcmp(signature, "info(_)") == 0) return debugNativeInfo;
        if (std::strcmp(signature, "warn(_)") == 0) return debugNativeWarn;
        if (std::strcmp(signature, "error(_)") == 0) return debugNativeError;
    }

    if (std::strcmp(className, "FileNative") == 0)
    {
        if (std::strcmp(signature, "read(_)") == 0)       return fileNativeRead;
        if (std::strcmp(signature, "write(_,_)") == 0)    return fileNativeWrite;
        if (std::strcmp(signature, "remove(_)") == 0)     return fileNativeRemove;
        if (std::strcmp(signature, "exists(_)") == 0)     return fileNativeExists;
        if (std::strcmp(signature, "size(_)") == 0)       return fileNativeSize;
        if (std::strcmp(signature, "list(_)") == 0)       return fileNativeList;
    }

    if (std::strcmp(className, "ConfigNative") == 0)
    {
        if (std::strcmp(signature, "parse(_)") == 0) return configNativeParse;
    }

    if (std::strcmp(className, "TeensyClockNative") == 0)
    {
        if (std::strcmp(signature, "ticksMs()") == 0) return teensyClockNativeTicksMs;
        if (std::strcmp(signature, "ticksUs()") == 0) return teensyClockNativeTicksUs;
    }

    return nullptr;
}

WrenForeignClassMethods bindMidiForeignClass(WrenVM *vm,
                                             const char *module,
                                             const char *className)
{
    (void)vm;

    WrenForeignClassMethods methods = {nullptr, nullptr};
    if (std::strcmp(module, kWrenRuntimeModule) == 0 &&
        (std::strcmp(className, "MidiNative") == 0 ||
         std::strcmp(className, "TeensyClockNative") == 0 ||
         std::strcmp(className, "DebugNative") == 0 ||
         std::strcmp(className, "FileNative") == 0 ||
         std::strcmp(className, "ConfigNative") == 0))
    {
        methods.allocate = midiNativeAllocate;
    }
    return methods;
}

bool initializeEmbeddedExample(WrenVM *vm)
{
    WrenInterpretResult result = wrenInterpret(vm, kWrenRuntimeModule, kEmbeddedWrenMidiExample);
    if (result != WREN_RESULT_SUCCESS)
    {
        WREN_BRIDGE_SERIAL_PRINTLN("Wren MIDI example failed to initialize");
        return false;
    }

    WREN_BRIDGE_SERIAL_PRINTLN("Wren MIDI example ready");
    return true;
}

bool captureDispatchHandles(WrenVM *vm)
{
    if (!wrenHasVariable(vm, kWrenRuntimeModule, "Midi"))
    {
        WREN_BRIDGE_SERIAL_PRINTLN("Wren MIDI bridge missing Midi class");
        return false;
    }

    wrenEnsureSlots(vm, 1);
    wrenGetVariable(vm, kWrenRuntimeModule, "Midi", 0);
    gMidiClassHandle = wrenGetSlotHandle(vm, 0);
    gDispatchHandle = wrenMakeCallHandle(vm, "dispatchFromNative(_,_,_,_,_)");
    return gMidiClassHandle != nullptr && gDispatchHandle != nullptr;
}
}

void WrenMidiBridge::configure(WrenConfiguration &config)
{
    config.bindForeignMethodFn = bindMidiForeignMethod;
    config.bindForeignClassFn = bindMidiForeignClass;
}

void WrenMidiBridge::setOutputSender(MidiOutputSendFn sender)
{
    gOutputSendFn = sender;
}

void WrenMidiBridge::setRuntimeLogger(WrenRuntimeLogFn logger)
{
    gRuntimeLogFn = logger;
}

void WrenMidiBridge::setFsProvider(const WrenFsProvider &provider)
{
    gFsProvider = provider;
}

bool WrenMidiBridge::initialize(WrenVM *vm)
{
    shutdown(vm);

    WrenInterpretResult result = wrenInterpret(vm, kWrenRuntimeModule, kEmbeddedWrenMidiRuntime);
    if (result != WREN_RESULT_SUCCESS)
    {
        WREN_BRIDGE_SERIAL_PRINTLN("Wren MIDI runtime failed to initialize");
        return false;
    }

    if (!captureDispatchHandles(vm))
    {
        shutdown(vm);
        return false;
    }

    if (!initializeEmbeddedExample(vm))
    {
        shutdown(vm);
        return false;
    }

    WREN_BRIDGE_SERIAL_PRINTLN("Wren MIDI bridge ready");
    return true;
}

void WrenMidiBridge::dispatchEvent(WrenVM *vm, const MIDIMessage &event)
{
    if (vm == nullptr || gMidiClassHandle == nullptr || gDispatchHandle == nullptr)
    {
        return;
    }

    wrenEnsureSlots(vm, 6);
    wrenSetSlotHandle(vm, 0, gMidiClassHandle);
    wrenSetSlotDouble(vm, 1, static_cast<double>(event.port));
    wrenSetSlotDouble(vm, 2, static_cast<double>(event.type));
    wrenSetSlotDouble(vm, 3, static_cast<double>(event.channel));
    wrenSetSlotDouble(vm, 4, static_cast<double>(event.data1));
    wrenSetSlotDouble(vm, 5, static_cast<double>(event.data2));

    WrenInterpretResult result = wrenCall(vm, gDispatchHandle);
    if (result != WREN_RESULT_SUCCESS)
    {
        WREN_BRIDGE_SERIAL_PRINTLN("Wren MIDI dispatch failed");
    }
}

void WrenMidiBridge::shutdown(WrenVM *vm)
{
    if (vm == nullptr)
    {
        gMidiClassHandle = nullptr;
        gDispatchHandle = nullptr;
        return;
    }

    releaseHandle(vm, gMidiClassHandle);
    releaseHandle(vm, gDispatchHandle);
}