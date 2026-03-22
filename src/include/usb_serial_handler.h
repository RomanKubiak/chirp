#ifndef USB_SERIAL_HANDLER_H
#define USB_SERIAL_HANDLER_H

#include "usb_serial_protocol.h"
#include <Arduino.h>

// ── Receive state machine ─────────────────────────────────────────────────────
enum class RxState : uint8_t
{
    SYNC0,
    SYNC1,
    HEADER,   // 4 bytes: type, seq, len_lo, len_hi
    PAYLOAD,
    CRC,      // 2 bytes
    COMPLETE,
};

// ── USBSerialHandler ──────────────────────────────────────────────────────────
// Framing: [0xAA][0x55][TYPE][SEQ][LEN_LO][LEN_HI][PAYLOAD...][CRC_LO][CRC_HI]
// CRC16-CCITT over TYPE..last payload byte.
template <typename SerialPortT>
class USBSerialHandler
{
public:
    static constexpr uint32_t RX_TIMEOUT_MS = 2000;

    explicit USBSerialHandler(SerialPortT &s, Print *dbg = nullptr)
        : serial_(s), debug_(dbg), state_(RxState::SYNC0),
          hdrIdx_(0), payloadIdx_(0), crcIdx_(0), lastActivity_(0)
    {
        memset(&frame_, 0, sizeof(frame_));
    }

    // Call frequently from loop(). Returns true when a valid frame is ready.
    bool update()
    {
        while (serial_.available())
        {
            uint8_t b = static_cast<uint8_t>(serial_.read());
            lastActivity_ = millis();

            switch (state_)
            {
            case RxState::SYNC0:
                if (b == FRAME_SYNC0) state_ = RxState::SYNC1;
                break;

            case RxState::SYNC1:
                if (b == FRAME_SYNC1) { hdrIdx_ = 0; state_ = RxState::HEADER; }
                else                  { state_ = RxState::SYNC0; }
                break;

            case RxState::HEADER:
                hdrBuf_[hdrIdx_++] = b;
                if (hdrIdx_ == 4)
                {
                    frame_.type       = hdrBuf_[0];
                    frame_.seq        = hdrBuf_[1];
                    frame_.payloadLen = static_cast<uint16_t>(hdrBuf_[2])
                                      | (static_cast<uint16_t>(hdrBuf_[3]) << 8);
                    if (frame_.payloadLen > FRAME_MAX_PAYLOAD)
                    {
                        if (debug_) debug_->println("[PROTO] payload too large");
                        reset(); break;
                    }
                    payloadIdx_ = 0;
                    crcIdx_     = 0;
                    state_ = (frame_.payloadLen > 0) ? RxState::PAYLOAD : RxState::CRC;
                }
                break;

            case RxState::PAYLOAD:
                frame_.payload[payloadIdx_++] = b;
                if (payloadIdx_ >= frame_.payloadLen)
                    { crcIdx_ = 0; state_ = RxState::CRC; }
                break;

            case RxState::CRC:
                crcBuf_[crcIdx_++] = b;
                if (crcIdx_ == 2)
                {
                    uint16_t got = static_cast<uint16_t>(crcBuf_[0])
                                 | (static_cast<uint16_t>(crcBuf_[1]) << 8);
                    // CRC over type+seq+len_lo+len_hi+payload
                    uint16_t expected = CRC16::calculate(hdrBuf_, 4);
                    if (frame_.payloadLen > 0)
                        expected = CRC16::calculate(
                            frame_.payload, frame_.payloadLen, expected);
                    if (got == expected)
                        { state_ = RxState::COMPLETE; return true; }
                    if (debug_) debug_->println("[PROTO] CRC mismatch");
                    reset();
                }
                break;

            case RxState::COMPLETE:
                break; // caller must drain via getFrame() first
            }
        }

        // Timeout incomplete reception
        if (state_ != RxState::SYNC0 && state_ != RxState::COMPLETE)
        {
            if (millis() - lastActivity_ > RX_TIMEOUT_MS)
            {
                if (debug_) debug_->println("[PROTO] RX timeout");
                reset();
            }
        }
        return false;
    }

    // Copy the completed frame out and reset. Returns false if none ready.
    bool getFrame(ChirpFrame &out)
    {
        if (state_ != RxState::COMPLETE) return false;
        memcpy(&out, &frame_, sizeof(ChirpFrame));
        reset();
        return true;
    }

    bool isFrameAvailable() const { return state_ == RxState::COMPLETE; }

    // Encode and send a frame. payload may be nullptr when payloadLen == 0.
    bool send(uint8_t type, uint8_t seq,
              const uint8_t *payload = nullptr, uint16_t payloadLen = 0,
              bool flushNow = false)
    {
        uint16_t wrote = ChirpProtocol::encode(
            sendBuf_, sizeof(sendBuf_), type, seq, payload, payloadLen);
        if (wrote == 0) return false;
        bool ok = (serial_.write(sendBuf_, wrote) == wrote);
        if (flushNow)
        {
            serial_.flush();
        }
        return ok;
    }

    bool sendText(uint8_t type, uint8_t seq, const char *text, bool flushNow = false)
    {
        if (!text) return false;
        uint16_t len = static_cast<uint16_t>(strnlen(text, FRAME_MAX_PAYLOAD));
        return send(type, seq, reinterpret_cast<const uint8_t *>(text), len, flushNow);
    }

    bool sendLog(const char *text, bool flushNow = false)
    {
        return sendText(MSG_LOG_TEXT, 0, text, flushNow);
    }

    void reset()
    {
        state_      = RxState::SYNC0;
        hdrIdx_     = 0;
        payloadIdx_ = 0;
        crcIdx_     = 0;
    }

private:
    SerialPortT &serial_;
    Print       *debug_;
    RxState      state_;
    uint8_t      hdrBuf_[4];
    uint8_t      hdrIdx_;
    uint16_t     payloadIdx_;
    uint8_t      crcBuf_[2];
    uint8_t      crcIdx_;
    uint32_t     lastActivity_;

    ChirpFrame   frame_;
    uint8_t      sendBuf_[FRAME_OVERHEAD + FRAME_MAX_PAYLOAD];
};

#endif // USB_SERIAL_HANDLER_H

