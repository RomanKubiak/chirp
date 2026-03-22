#ifndef USB_SERIAL_PROTOCOL_H
#define USB_SERIAL_PROTOCOL_H

#include <stdint.h>
#include <string.h>

// ── Frame format ──────────────────────────────────────────────────────────────
// [0xAA][0x55][TYPE:u8][SEQ:u8][LEN:u16-LE][PAYLOAD:LEN bytes][CRC16:u16-LE]
// CRC16-CCITT covers TYPE through last byte of PAYLOAD.
// Overhead: 8 bytes.  Payload cap: FRAME_MAX_PAYLOAD bytes.
// ─────────────────────────────────────────────────────────────────────────────

static constexpr uint8_t  FRAME_SYNC0       = 0xAA;
static constexpr uint8_t  FRAME_SYNC1       = 0x55;
static constexpr uint16_t FRAME_OVERHEAD    = 8;
static constexpr uint16_t FRAME_MAX_PAYLOAD = 4096;

// ── Message types ─────────────────────────────────────────────────────────────
enum ChirpMessageType : uint8_t
{
    MSG_LOG_TEXT         = 0x01,
    MSG_MIDI_EVENT       = 0x10,
    MSG_INTERNAL_CONTROL = 0x11,
    MSG_REBOOT_REQ       = 0x70,
    MSG_REBOOT_RESP      = 0x71,
    MSG_PING             = 0x7E,
    MSG_PONG             = 0x7F,

    // Filesystem commands
    MSG_FS_LIST_REQ      = 0x20,
    MSG_FS_LIST_RESP     = 0x21,
    MSG_FS_READ_REQ      = 0x22,
    MSG_FS_READ_RESP     = 0x23,
    MSG_FS_WRITE_REQ     = 0x24,
    MSG_FS_WRITE_RESP    = 0x25,
    MSG_FS_DELETE_REQ    = 0x26,
    MSG_FS_DELETE_RESP   = 0x27,
    MSG_FS_STAT_REQ      = 0x28,
    MSG_FS_STAT_RESP     = 0x29,
    MSG_FS_RUN_REQ       = 0x2A,
    MSG_FS_RUN_RESP      = 0x2B,
    MSG_FS_SPACE_REQ     = 0x2C,
    MSG_FS_SPACE_RESP    = 0x2D,
};

// ── Status codes ─────────────────────────────────────────────────────────────
enum ChirpStatus : uint8_t
{
    STATUS_OK        = 0x00,
    STATUS_ERROR     = 0x01,
    STATUS_NOT_FOUND = 0x02,
    STATUS_TOO_LARGE = 0x03,
    STATUS_INVALID   = 0x04,
};

// ── CRC16-CCITT (poly 0x1021, init 0xFFFF, no reflection) ────────────────────
class CRC16
{
public:
    static uint16_t calculate(const uint8_t *data, uint16_t length,
                              uint16_t init = 0xFFFF)
    {
        uint16_t crc = init;
        for (uint16_t i = 0; i < length; i++)
        {
            crc ^= static_cast<uint16_t>(data[i]) << 8;
            for (uint8_t j = 0; j < 8; j++)
                crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
        }
        return crc;
    }
};

// ── Parsed/built frame ────────────────────────────────────────────────────────
struct ChirpFrame
{
    uint8_t  type;
    uint8_t  seq;
    uint16_t payloadLen;
    uint8_t  payload[FRAME_MAX_PAYLOAD];
};

// ── Frame encoder ─────────────────────────────────────────────────────────────
class ChirpProtocol
{
public:
    // Encode a frame into outBuf. Returns total bytes written, or 0 on error.
    // CRC covers: type(1) + seq(1) + len_lo(1) + len_hi(1) + payload(payloadLen)
    static uint16_t encode(uint8_t *outBuf, uint16_t outSize,
                           uint8_t type, uint8_t seq,
                           const uint8_t *payload, uint16_t payloadLen)
    {
        uint16_t total = FRAME_OVERHEAD + payloadLen;
        if (total > outSize || payloadLen > FRAME_MAX_PAYLOAD)
            return 0;

        outBuf[0] = FRAME_SYNC0;
        outBuf[1] = FRAME_SYNC1;
        outBuf[2] = type;
        outBuf[3] = seq;
        outBuf[4] = static_cast<uint8_t>(payloadLen & 0xFF);
        outBuf[5] = static_cast<uint8_t>(payloadLen >> 8);

        if (payload && payloadLen > 0)
            memcpy(outBuf + 6, payload, payloadLen);

        uint16_t crc = CRC16::calculate(outBuf + 2, 4 + payloadLen);
        outBuf[6 + payloadLen]     = static_cast<uint8_t>(crc & 0xFF);
        outBuf[6 + payloadLen + 1] = static_cast<uint8_t>(crc >> 8);
        return total;
    }
};



#endif // USB_SERIAL_PROTOCOL_H
