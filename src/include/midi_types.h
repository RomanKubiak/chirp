#ifndef MIDI_TYPES_H
#define MIDI_TYPES_H

#include <cstring>
#include <stdint.h>

// MIDI Message Types (standard MIDI status bytes)
enum class MIDIMessageType : uint8_t
{
    NOTE_OFF = 0x80,
    NOTE_ON = 0x90,
    POLY_KEY_PRESSURE = 0xA0,
    CONTROL_CHANGE = 0xB0,
    PROGRAM_CHANGE = 0xC0,
    CHANNEL_PRESSURE = 0xD0,
    PITCH_BEND = 0xE0,
    SYSTEM_EXCLUSIVE = 0xF0,
    UNKNOWN = 0xFF
};

// Core MIDI Message structure
struct MIDIMessage
{
    uint8_t port;       // Port number (1-5)
    uint8_t type;       // MIDI message type
    uint8_t channel;    // MIDI channel (0-15, where 0=channel 1)
    uint8_t data1;      // First data byte (note, controller, etc)
    uint8_t data2;      // Second data byte (velocity, value, etc)
    
    MIDIMessage() : port(0), type(0), channel(0), data1(0), data2(0) {}
    MIDIMessage(uint8_t p, uint8_t t, uint8_t ch, uint8_t d1, uint8_t d2)
        : port(p), type(t), channel(ch), data1(d1), data2(d2) {}
};

// Specialized MIDI message types for convenience
struct NoteMessage : MIDIMessage
{
    uint8_t note() const { return data1; }
    uint8_t velocity() const { return data2; }
    bool isNoteOn() const { return type == (uint8_t)MIDIMessageType::NOTE_ON && velocity() > 0; }
    bool isNoteOff() const { return type == (uint8_t)MIDIMessageType::NOTE_OFF || 
                                    (type == (uint8_t)MIDIMessageType::NOTE_ON && velocity() == 0); }
};

struct ControlChangeMessage : MIDIMessage
{
    uint8_t controller() const { return data1; }
    uint8_t value() const { return data2; }
};

struct ProgramChangeMessage : MIDIMessage
{
    uint8_t program() const { return data1; }
};

// SYSEX Message - variable length for internal control
// Maximum SYSEX payload size (adjust based on available memory)
#define SYSEX_MAX_LENGTH 256

struct SYSEXMessage
{
    uint8_t port;                      // Port number (1-5)
    uint8_t length;                    // Actual data length
    uint8_t data[SYSEX_MAX_LENGTH];   // SYSEX payload (excluding F0 and F7)
    
    SYSEXMessage() : port(0), length(0) {}
    
    SYSEXMessage(uint8_t p, const uint8_t *payload, uint8_t len)
        : port(p), length(len > SYSEX_MAX_LENGTH ? SYSEX_MAX_LENGTH : len)
    {
        if (len > 0)
        {
            memcpy(data, payload, length);
        }
    }
    
    // Get a specific byte from the payload
    uint8_t getByte(uint8_t index) const
    {
        return (index < length) ? data[index] : 0;
    }
    
    // Set a specific byte in the payload
    void setByte(uint8_t index, uint8_t value)
    {
        if (index < length)
        {
            data[index] = value;
        }
    }
};

#endif // MIDI_TYPES_H
