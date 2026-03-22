#ifndef MIDI_ROUTER_H
#define MIDI_ROUTER_H

#include <Arduino.h>
#include <MIDI.h>
#include "midi_types.h"
#include "wren.hpp"

// ── Serial MIDI port type alias + extern declarations ─────────────────────────
// Objects are created via MIDI_CREATE_INSTANCE in chirp.ino.
using HardwareSerialMidi = midi::MidiInterface<midi::SerialMIDI<HardwareSerial>>;
extern HardwareSerialMidi MIDI1, MIDI2, MIDI3, MIDI4, MIDI5;

// ── Device manager: round-robin reads across all six MIDI inputs ──────────────
class MidiDeviceManager
{
public:
    bool readNext(MIDIMessage &event);

private:
    uint8_t nextPortIndex_ = 0;

    bool readFromIndex(uint8_t index, MIDIMessage &event);

    template <typename MidiDeviceT>
    static bool readFromDevice(MidiDeviceT &device, byte port, MIDIMessage &event)
    {
        if (!device.read()) return false;
        event.port    = port;
        event.type    = device.getType();
        event.channel = device.getChannel();
        event.data1   = device.getData1();
        event.data2   = device.getData2();
        return true;
    }

    static bool readFromUsb(MIDIMessage &event);
};

extern MidiDeviceManager midiDevices;

// ── Runtime diagnostics counters ──────────────────────────────────────────────
struct RuntimeDiagCounters
{
    uint32_t loops         = 0;
    uint32_t idleLoops     = 0;
    uint32_t midiEvents    = 0;
    uint32_t midiOutQueued = 0;
    uint32_t midiOutSent   = 0;
    uint32_t midiOutDropped = 0;
    uint32_t midiBudgetHits = 0;
    uint32_t controlFrames = 0;
    uint32_t wrenDispatchUs = 0;
    uint32_t controlUs     = 0;
    uint32_t loopBusyUs    = 0;
    uint32_t loopMaxUs     = 0;
};

extern RuntimeDiagCounters gDiag;

// ── MIDI output ring buffer ────────────────────────────────────────────────────
static constexpr uint16_t kMidiOutQueueSize = 256;
static constexpr uint16_t kMidiOutQueueMask = kMidiOutQueueSize - 1;

extern MIDIMessage         gMidiOutQueue[kMidiOutQueueSize];
extern volatile uint16_t   gMidiOutQueueHead;
extern volatile uint16_t   gMidiOutQueueTail;
extern volatile uint16_t   gMidiOutQueueDepth;
extern volatile uint16_t   gMidiOutQueueHighWater;
extern volatile uint16_t   gMidiOutQueueCapacity;

uint32_t satAddU32(uint32_t current, uint32_t delta);
void     setMidiOutQueueStats(uint16_t depth, uint16_t highWater, uint16_t capacity);
bool     enqueueMidiOutput(const MIDIMessage &event);
uint16_t drainMidiOutputQueue();

// ── MIDI staging buffer (live input buffering during hot-reload) ───────────────
void drainMidiInputToBuffer();
void dispatchMidiFromBuffer();

// ── Main-loop MIDI processing helper ─────────────────────────────────────────
// Reads up to maxEvents messages within budgetUs microseconds.
// Returns the count processed; sets budgetExceeded if the time limit was hit.
uint16_t processMidiInput(uint16_t maxEvents, uint32_t budgetUs, bool &budgetExceeded);

#endif // MIDI_ROUTER_H
