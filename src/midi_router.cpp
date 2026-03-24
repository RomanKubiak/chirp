#include "midi_router.h"
#include "chirp_config.h"
#include "wren_midi_bridge.h"
#include "runtime_log.h"

#include <Arduino.h>

// ── Global definitions ────────────────────────────────────────────────────────
MidiDeviceManager midiDevices;
RuntimeDiagCounters gDiag;

MIDIMessage         gMidiOutQueue[kMidiOutQueueSize];
volatile uint16_t   gMidiOutQueueHead     = 0;
volatile uint16_t   gMidiOutQueueTail     = 0;
volatile uint16_t   gMidiOutQueueDepth    = 0;
volatile uint16_t   gMidiOutQueueHighWater = 0;
volatile uint16_t   gMidiOutQueueCapacity  = 0;

static uint16_t gMidiOutQueueHighWaterLocal = 0;
static MidiPreDispatchHookFn gMidiPreDispatchHook = nullptr;

// ── MIDI input staging buffer (used during hot-reload) ────────────────────────
static constexpr uint16_t kMidiStagingSize = 128;
static MIDIMessage gMidiStagingBuffer[kMidiStagingSize];
static uint16_t    gMidiStagingCount = 0;

// ── Globals defined in chirp.ino, needed here ─────────────────────────────────
extern WrenVM *vm;

// ── MidiDeviceManager ─────────────────────────────────────────────────────────
bool MidiDeviceManager::readNext(MIDIMessage &event)
{
    for (uint8_t i = 0; i < 6; i++)
    {
        uint8_t candidate = static_cast<uint8_t>((nextPortIndex_ + i) % 6);
        if (readFromIndex(candidate, event))
        {
            nextPortIndex_ = static_cast<uint8_t>((candidate + 1) % 6);
            return true;
        }
    }
    return false;
}

bool MidiDeviceManager::readFromIndex(uint8_t index, MIDIMessage &event)
{
    switch (index)
    {
    case 0: return readFromDevice(MIDI1, 1, event);
    case 1: return readFromDevice(MIDI2, 2, event);
    case 2: return readFromDevice(MIDI3, 3, event);
    case 3: return readFromDevice(MIDI4, 4, event);
    case 4: return readFromDevice(MIDI5, 5, event);
    case 5: return readFromUsb(event);
    default: return false;
    }
}

bool MidiDeviceManager::readFromUsb(MIDIMessage &event)
{
    if (!usbMIDI.read()) return false;
    event.port    = 6;
    event.type    = usbMIDI.getType();
    event.channel = usbMIDI.getChannel();
    event.data1   = usbMIDI.getData1();
    event.data2   = usbMIDI.getData2();
    return true;
}

// ── Utility ───────────────────────────────────────────────────────────────────
uint32_t satAddU32(uint32_t current, uint32_t delta)
{
    if ((UINT32_MAX - current) < delta) return UINT32_MAX;
    return current + delta;
}

void setMidiOutQueueStats(uint16_t depth, uint16_t highWater, uint16_t capacity)
{
    gMidiOutQueueDepth    = depth;
    gMidiOutQueueHighWater = highWater;
    gMidiOutQueueCapacity  = capacity;
}

void setMidiPreDispatchHook(MidiPreDispatchHookFn hook)
{
    gMidiPreDispatchHook = hook;
}

static uint16_t midiOutQueueCount(uint16_t head, uint16_t tail)
{
    return static_cast<uint16_t>((head - tail) & kMidiOutQueueMask);
}

// ── MIDI output send (direct to hardware) ─────────────────────────────────────
static bool sendMidiOutputNow(const MIDIMessage &event)
{
    if (event.port < 1 || event.port > 6) return false;
    if (event.type < 0x80 || event.type > 0xEF) return false;

    const byte channel = static_cast<byte>((event.channel & 0x0F) + 1);
    const midi::MidiType msgType = static_cast<midi::MidiType>(event.type);

    switch (event.port)
    {
    case 1: MIDI1.send(msgType, event.data1, event.data2, channel); return true;
    case 2: MIDI2.send(msgType, event.data1, event.data2, channel); return true;
    case 3: MIDI3.send(msgType, event.data1, event.data2, channel); return true;
    case 4: MIDI4.send(msgType, event.data1, event.data2, channel); return true;
    case 5: MIDI5.send(msgType, event.data1, event.data2, channel); return true;
    case 6: usbMIDI.send(event.type, event.data1, event.data2, channel, 0); return true;
    default: return false;
    }
}

// ── MIDI output ring buffer ───────────────────────────────────────────────────
bool enqueueMidiOutput(const MIDIMessage &event)
{
    const uint16_t head = gMidiOutQueueHead;
    const uint16_t next = static_cast<uint16_t>((head + 1) & kMidiOutQueueMask);
    const uint16_t tail = gMidiOutQueueTail;
    if (next == tail)
    {
        gDiag.midiOutDropped = satAddU32(gDiag.midiOutDropped, 1);
        return false;
    }
    gMidiOutQueue[head] = event;
    gMidiOutQueueHead   = next;

    const uint16_t depth = midiOutQueueCount(next, tail);
    if (depth > gMidiOutQueueHighWaterLocal) gMidiOutQueueHighWaterLocal = depth;
    gDiag.midiOutQueued = satAddU32(gDiag.midiOutQueued, 1);
    setMidiOutQueueStats(depth, gMidiOutQueueHighWaterLocal, kMidiOutQueueSize - 1);
    return true;
}

uint16_t drainMidiOutputQueue()
{
    static constexpr uint16_t kMaxMidiOutPerLoop = 64;
    static constexpr uint32_t kMidiOutBudgetUs   = 700;

    uint16_t sent = 0;
    const uint32_t startUs = micros();
    while (sent < kMaxMidiOutPerLoop)
    {
        const uint16_t tail = gMidiOutQueueTail;
        const uint16_t head = gMidiOutQueueHead;
        if (tail == head) break;

        const MIDIMessage event = gMidiOutQueue[tail];
        gMidiOutQueueTail = static_cast<uint16_t>((tail + 1) & kMidiOutQueueMask);

        if (sendMidiOutputNow(event))
            gDiag.midiOutSent    = satAddU32(gDiag.midiOutSent, 1);
        else
            gDiag.midiOutDropped = satAddU32(gDiag.midiOutDropped, 1);

        sent++;
        if (static_cast<uint32_t>(micros() - startUs) >= kMidiOutBudgetUs) break;
    }
    setMidiOutQueueStats(midiOutQueueCount(gMidiOutQueueHead, gMidiOutQueueTail),
                         gMidiOutQueueHighWaterLocal, kMidiOutQueueSize - 1);
    return sent;
}

// ── MIDI staging buffer (hot-reload) ─────────────────────────────────────────
void drainMidiInputToBuffer()
{
    gMidiStagingCount = 0;
    MIDIMessage event;
    while (gMidiStagingCount < kMidiStagingSize && midiDevices.readNext(event))
        gMidiStagingBuffer[gMidiStagingCount++] = event;
}

void dispatchMidiFromBuffer()
{
    for (uint16_t i = 0; i < gMidiStagingCount; i++)
        WrenMidiBridge::dispatchEvent(vm, gMidiStagingBuffer[i]);
    gMidiStagingCount = 0;
}

// ── Main-loop MIDI processing ─────────────────────────────────────────────────
uint16_t processMidiInput(uint16_t maxEvents, uint32_t budgetUs, bool &budgetExceeded)
{
    static constexpr uint16_t kYieldEvery = 16;
    budgetExceeded = false;
    uint16_t processed = 0;
    const uint32_t startUs = micros();
    MIDIMessage event;

    while (processed < maxEvents && midiDevices.readNext(event))
    {
        logMidiMessage(event);

        if (gMidiPreDispatchHook && gMidiPreDispatchHook(event))
        {
            gDiag.midiEvents = satAddU32(gDiag.midiEvents, 1);
            processed++;
            if (static_cast<uint32_t>(micros() - startUs) >= budgetUs)
            {
                budgetExceeded = true;
                break;
            }
            if ((processed % kYieldEvery) == 0) yield();
            continue;
        }

        const uint32_t t0 = micros();
        WrenMidiBridge::dispatchEvent(vm, event);
        gDiag.wrenDispatchUs = satAddU32(gDiag.wrenDispatchUs,
                                          static_cast<uint32_t>(micros() - t0));
        gDiag.midiEvents = satAddU32(gDiag.midiEvents, 1);
        processed++;

        if (static_cast<uint32_t>(micros() - startUs) >= budgetUs)
        {
            budgetExceeded = true;
            break;
        }
        if ((processed % kYieldEvery) == 0) yield();
    }
    return processed;
}
