#ifndef CHIRP_FS_H
#define CHIRP_FS_H

#include <Arduino.h>
#include <LittleFS.h>
#include <SD.h>

#include "chirp_config.h"

// ── ChirpFS ───────────────────────────────────────────────────────────────────
// Storage backend wrapper that tries SD card first (shared SPI bus) and
// transparently falls back to internal LittleFS program flash.
// ─────────────────────────────────────────────────────────────────────────────
class ChirpFS
{
public:
    enum class Backend : uint8_t {
        None,
        SD,
        LittleFS,
    };

    bool begin(size_t fsSizeBytes);

    File open(const char *path, uint8_t mode = FILE_READ);
    bool exists(const char *path);
    bool remove(const char *path);
    bool mkdir(const char *path);

    uint64_t totalSize();
    uint64_t usedSize();

    uint32_t fsBlockSize() const;
    uint32_t fsBlockCount() const;
    int32_t fsBlockCycles() const;

    Backend backend() const { return backend_; }
    const char *backendName() const;
    const char *storageDiagnostic() const { return storageDiag_; }

private:
    bool beginSdCard();
    void prepareSharedSpiChipSelects() const;

    Backend backend_ = Backend::None;
    LittleFS_Program littlefs_;
    char storageDiag_[96] = "storage not initialized";
};

#endif // CHIRP_FS_H
