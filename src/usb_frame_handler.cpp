#include "usb_frame_handler.h"
#include "usb_serial_handler.h"
#include "usb_serial_protocol.h"
#include "script_storage.h"
#include "wren_host.h"
#include "midi_router.h"
#include "runtime_log.h"
#include "wren.hpp"

#include "chirp_fs.h"
#include <Arduino.h>
#include <cstring>

// Extern references to globals defined in chirp.ino
extern USBSerialHandler<usb_serial_class> usbHandler;
extern ScriptStorage                      scriptStorage;
extern WrenVM                            *vm;
extern ChirpFS                            internalFlash;

// ── File-scope I/O buffers (avoid stack pressure) ────────────────────────────
static uint8_t    fsBuf[FRAME_MAX_PAYLOAD];
static ChirpFrame incomingFrame;
static const char kRebootToken[] = "RBT!";
static constexpr uint8_t kWriteChunkMarker = 0xFF;
static String writeChunkPath;
static File writeChunkFile;

// ── Helpers ───────────────────────────────────────────────────────────────────
static void fsStatus(uint8_t type, uint8_t seq, uint8_t status, bool flushNow = false)
{
    usbHandler.send(type, seq, &status, 1, flushNow);
}

// ── Protocol frame handler ────────────────────────────────────────────────────
static void handleFrame(const ChirpFrame &frame)
{
    switch (frame.type)
    {
    case MSG_LOG_TEXT:
    {
        uint16_t len = (frame.payloadLen < FRAME_MAX_PAYLOAD)
                       ? frame.payloadLen : FRAME_MAX_PAYLOAD;
        char text[FRAME_MAX_PAYLOAD + 1];
        memcpy(text, frame.payload, len);
        text[len] = '\0';
#if DEBUG_RUNTIME_SERIAL
        Serial.print("[USB LOG] ");
        Serial.println(text);
#endif
        break;
    }

    case MSG_PING:
        usbHandler.send(MSG_PONG, frame.seq);
        logRuntime("[USB] PING received, sent PONG");
        break;

    case MSG_REBOOT_REQ:
    {
        if (frame.payloadLen != 4 || memcmp(frame.payload, kRebootToken, 4) != 0)
        {
            fsStatus(MSG_REBOOT_RESP, frame.seq, STATUS_INVALID, true);
            logRuntime("[USB] Ignored reboot request (invalid token)");
            break;
        }
        fsStatus(MSG_REBOOT_RESP, frame.seq, STATUS_OK, true);
        logRuntime("[USB] Reboot requested");
        delay(50);
        SCB_AIRCR = 0x05FA0004;
        while (true) {}
        break;
    }

    case MSG_FS_LIST_REQ:
    {
        String names[64];
        size_t count = scriptStorage.listManagedFiles(names, 64);
        uint16_t idx = 0;
        fsBuf[idx++] = static_cast<uint8_t>(count & 0xFF);
        fsBuf[idx++] = static_cast<uint8_t>(count >> 8);
        for (size_t i = 0; i < count; i++)
        {
            uint16_t avail = (idx + 2 <= FRAME_MAX_PAYLOAD)
                             ? static_cast<uint16_t>(FRAME_MAX_PAYLOAD - idx - 1) : 0;
            if (avail == 0) break;
            uint16_t nl16 = static_cast<uint16_t>(names[i].length());
            uint8_t  nl   = static_cast<uint8_t>(nl16 < avail ? nl16 : avail);
            fsBuf[idx++]  = nl;
            memcpy(fsBuf + idx, names[i].c_str(), nl);
            idx += nl;
        }
        usbHandler.send(MSG_FS_LIST_RESP, frame.seq, fsBuf, idx);
        break;
    }

    case MSG_FS_READ_REQ:
    {
        if (frame.payloadLen < 2) { fsStatus(MSG_FS_READ_RESP, frame.seq, STATUS_INVALID); break; }
        uint8_t nameLen = frame.payload[0];
        if (nameLen == 0 || static_cast<uint16_t>(1 + nameLen) > frame.payloadLen)
            { fsStatus(MSG_FS_READ_RESP, frame.seq, STATUS_INVALID); break; }
        char path[256] = {0};
        memcpy(path, frame.payload + 1, nameLen);
        String source;
        if (!scriptStorage.loadFile(path, source))
            { fsStatus(MSG_FS_READ_RESP, frame.seq, STATUS_NOT_FOUND); break; }
        if (source.length() + 1 > FRAME_MAX_PAYLOAD)
            { fsStatus(MSG_FS_READ_RESP, frame.seq, STATUS_TOO_LARGE); break; }
        fsBuf[0] = STATUS_OK;
        memcpy(fsBuf + 1, source.c_str(), source.length());
        usbHandler.send(MSG_FS_READ_RESP, frame.seq, fsBuf,
                        static_cast<uint16_t>(1 + source.length()));
        break;
    }

    case MSG_FS_WRITE_REQ:
    {
        if (frame.payloadLen < 2) { fsStatus(MSG_FS_WRITE_RESP, frame.seq, STATUS_INVALID); break; }
        uint8_t nameLen = frame.payload[0];
        if (nameLen == 0 || static_cast<uint16_t>(1 + nameLen) > frame.payloadLen)
            { fsStatus(MSG_FS_WRITE_RESP, frame.seq, STATUS_INVALID); break; }
        char path[256] = {0};
        memcpy(path, frame.payload + 1, nameLen);
        uint16_t dataOffset = 1 + nameLen;
        bool isChunked = (dataOffset + 2 <= frame.payloadLen) &&
                         (frame.payload[dataOffset] == kWriteChunkMarker);

        if (isChunked)
        {
            const uint8_t flags = frame.payload[dataOffset + 1];
            const bool append   = (flags & 0x01) != 0;
            const bool finalize = (flags & 0x02) != 0;
            dataOffset += 2;
            uint16_t dataLen = frame.payloadLen - dataOffset;

            String incomingPath(path);
            if (!append)
            {
                if (writeChunkFile)
                {
                    writeChunkFile.close();
                }

                writeChunkPath = incomingPath;

                // Start a fresh destination file and ensure parent directories exist.
                if (!scriptStorage.saveFile(path, String("")))
                {
                    fsStatus(MSG_FS_WRITE_RESP, frame.seq, STATUS_ERROR);
                    writeChunkPath = "";
                    break;
                }

                writeChunkFile = internalFlash.open(path, FILE_WRITE);
                if (!writeChunkFile)
                {
                    fsStatus(MSG_FS_WRITE_RESP, frame.seq, STATUS_ERROR);
                    writeChunkPath = "";
                    break;
                }
            }
            else if (writeChunkPath != incomingPath || !writeChunkFile)
            {
                fsStatus(MSG_FS_WRITE_RESP, frame.seq, STATUS_INVALID);
                if (writeChunkFile)
                {
                    writeChunkFile.close();
                }
                writeChunkPath = "";
                break;
            }

            size_t written = writeChunkFile.write(frame.payload + dataOffset, dataLen);
            if (written != dataLen)
            {
                writeChunkFile.close();
                writeChunkPath = "";
                fsStatus(MSG_FS_WRITE_RESP, frame.seq, STATUS_ERROR);
                break;
            }

            if (finalize)
            {
                writeChunkFile.flush();
                writeChunkFile.close();
                writeChunkPath = "";
                fsStatus(MSG_FS_WRITE_RESP, frame.seq, STATUS_OK);
            }
            else
            {
                fsStatus(MSG_FS_WRITE_RESP, frame.seq, STATUS_OK);
            }
            break;
        }

        uint16_t dataLen = frame.payloadLen - dataOffset;
        memcpy(fsBuf, frame.payload + dataOffset, dataLen);
        fsBuf[dataLen] = '\0';
        String source(reinterpret_cast<char *>(fsBuf));
        fsStatus(MSG_FS_WRITE_RESP, frame.seq,
                 scriptStorage.saveFile(path, source) ? STATUS_OK : STATUS_ERROR);
        break;
    }

    case MSG_FS_DELETE_REQ:
    {
        if (frame.payloadLen < 2) { fsStatus(MSG_FS_DELETE_RESP, frame.seq, STATUS_INVALID); break; }
        uint8_t nameLen = frame.payload[0];
        if (nameLen == 0 || static_cast<uint16_t>(1 + nameLen) > frame.payloadLen)
            { fsStatus(MSG_FS_DELETE_RESP, frame.seq, STATUS_INVALID); break; }
        char path[256] = {0};
        memcpy(path, frame.payload + 1, nameLen);
        fsStatus(MSG_FS_DELETE_RESP, frame.seq,
                 scriptStorage.removeFile(path) ? STATUS_OK : STATUS_NOT_FOUND);
        break;
    }

    case MSG_FS_STAT_REQ:
    {
        if (frame.payloadLen < 2) { fsStatus(MSG_FS_STAT_RESP, frame.seq, STATUS_INVALID); break; }
        uint8_t nameLen = frame.payload[0];
        if (nameLen == 0 || static_cast<uint16_t>(1 + nameLen) > frame.payloadLen)
            { fsStatus(MSG_FS_STAT_RESP, frame.seq, STATUS_INVALID); break; }
        char path[256] = {0};
        memcpy(path, frame.payload + 1, nameLen);
        int32_t sz = scriptStorage.fileSize(path);
        if (sz < 0) { fsStatus(MSG_FS_STAT_RESP, frame.seq, STATUS_NOT_FOUND); break; }
        uint32_t usz = static_cast<uint32_t>(sz);
        fsBuf[0] = STATUS_OK;
        fsBuf[1] = static_cast<uint8_t>(usz & 0xFF);
        fsBuf[2] = static_cast<uint8_t>((usz >> 8)  & 0xFF);
        fsBuf[3] = static_cast<uint8_t>((usz >> 16) & 0xFF);
        fsBuf[4] = static_cast<uint8_t>((usz >> 24) & 0xFF);
        usbHandler.send(MSG_FS_STAT_RESP, frame.seq, fsBuf, 5);
        break;
    }

    case MSG_FS_RUN_REQ:
    {
        if (frame.payloadLen < 2) { fsStatus(MSG_FS_RUN_RESP, frame.seq, STATUS_INVALID); break; }
        uint8_t nameLen = frame.payload[0];
        if (nameLen == 0 || static_cast<uint16_t>(1 + nameLen) > frame.payloadLen)
            { fsStatus(MSG_FS_RUN_RESP, frame.seq, STATUS_INVALID); break; }
        char path[256] = {0};
        memcpy(path, frame.payload + 1, nameLen);
        String source;
        if (!scriptStorage.loadFile(path, source))
            { fsStatus(MSG_FS_RUN_RESP, frame.seq, STATUS_NOT_FOUND); break; }

        // ── Hot-reload sequence ──────────────────────────────────────────────
        // 1. Drain MIDI input that arrived before the reload starts.
        drainMidiInputToBuffer();

        // 2. Call the user's unload hook then wipe all MIDI listeners.
        wrenInterpret(vm, "chirp_runtime", "Script.callUnload()\nMidi.clearListeners()");

        // 3. Interpret the new script.
        WrenInterpretResult result = interpretWrenWithCapturedError("chirp_runtime", source.c_str());

        // 4. Replay any MIDI that arrived during interpretation.
        dispatchMidiFromBuffer();

        if (result != WREN_RESULT_SUCCESS)
        {
            fsBuf[0] = STATUS_ERROR;
            const char *errMsg = (gCapturedWrenError[0] != '\0')
                                 ? gCapturedWrenError
                                 : "Script execution failed";
            uint16_t msgLen  = static_cast<uint16_t>(strlen(errMsg));
            uint16_t copyLen = (msgLen < (FRAME_MAX_PAYLOAD - 1)) ? msgLen : (FRAME_MAX_PAYLOAD - 1);
            memcpy(fsBuf + 1, errMsg, copyLen);
            usbHandler.send(MSG_FS_RUN_RESP, frame.seq, fsBuf, static_cast<uint16_t>(1 + copyLen));
        }
        else
        {
            fsBuf[0] = STATUS_OK;
            usbHandler.send(MSG_FS_RUN_RESP, frame.seq, fsBuf, 1);
        }
        break;
    }

    case MSG_FS_SPACE_REQ:
    {
        const uint32_t total       = static_cast<uint32_t>(internalFlash.totalSize());
        const uint32_t used        = static_cast<uint32_t>(internalFlash.usedSize());
        const uint32_t blockSize   = internalFlash.fsBlockSize();
        const uint32_t blockCount  = internalFlash.fsBlockCount();
        const int32_t  blockCycles = internalFlash.fsBlockCycles();
        uint8_t *p = fsBuf;
        *p++ = STATUS_OK;
        // total_bytes  u32 LE
        *p++ = static_cast<uint8_t>(total & 0xFF);
        *p++ = static_cast<uint8_t>((total >> 8)  & 0xFF);
        *p++ = static_cast<uint8_t>((total >> 16) & 0xFF);
        *p++ = static_cast<uint8_t>((total >> 24) & 0xFF);
        // used_bytes   u32 LE
        *p++ = static_cast<uint8_t>(used & 0xFF);
        *p++ = static_cast<uint8_t>((used >> 8)  & 0xFF);
        *p++ = static_cast<uint8_t>((used >> 16) & 0xFF);
        *p++ = static_cast<uint8_t>((used >> 24) & 0xFF);
        // block_size   u32 LE
        *p++ = static_cast<uint8_t>(blockSize & 0xFF);
        *p++ = static_cast<uint8_t>((blockSize >> 8)  & 0xFF);
        *p++ = static_cast<uint8_t>((blockSize >> 16) & 0xFF);
        *p++ = static_cast<uint8_t>((blockSize >> 24) & 0xFF);
        // block_count  u32 LE
        *p++ = static_cast<uint8_t>(blockCount & 0xFF);
        *p++ = static_cast<uint8_t>((blockCount >> 8)  & 0xFF);
        *p++ = static_cast<uint8_t>((blockCount >> 16) & 0xFF);
        *p++ = static_cast<uint8_t>((blockCount >> 24) & 0xFF);
        // block_cycles i32 LE  (-1 = wear-leveling disabled)
        *p++ = static_cast<uint8_t>(static_cast<uint32_t>(blockCycles) & 0xFF);
        *p++ = static_cast<uint8_t>((static_cast<uint32_t>(blockCycles) >> 8)  & 0xFF);
        *p++ = static_cast<uint8_t>((static_cast<uint32_t>(blockCycles) >> 16) & 0xFF);
        *p++ = static_cast<uint8_t>((static_cast<uint32_t>(blockCycles) >> 24) & 0xFF);
        usbHandler.send(MSG_FS_SPACE_RESP, frame.seq, fsBuf, static_cast<uint16_t>(p - fsBuf));
        break;
    }

    default:
    {
        char info[64] = {0};
        snprintf(info, sizeof(info), "[USB] Unhandled type=0x%02X len=%u",
                 frame.type, frame.payloadLen);
        logRuntime(info);
        break;
    }
    }
}

// ── Public entry point ────────────────────────────────────────────────────────
uint8_t processUsbControlFrames(uint8_t maxFrames)
{
    uint8_t handled = 0;
    while (handled < maxFrames && usbHandler.update())
    {
        if (usbHandler.getFrame(incomingFrame))
        {
            handleFrame(incomingFrame);
            handled++;
        }
        else
        {
            break;
        }
    }
    return handled;
}
