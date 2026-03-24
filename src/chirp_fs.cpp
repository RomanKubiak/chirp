#include "chirp_fs.h"

#include <SPI.h>

#ifndef SD_CARD_CS
#define SD_CARD_CS 23
#endif

#ifndef DISPLAY_CS
#define DISPLAY_CS 10
#endif

bool ChirpFS::begin(size_t fsSizeBytes)
{
    if (backend_ != Backend::None) return true;

#if ENABLE_SD_STORAGE_BACKEND
    if (beginSdCard()) {
        backend_ = Backend::SD;
        snprintf(storageDiag_, sizeof(storageDiag_), "sd: mounted (cs=%u)", static_cast<unsigned>(SD_CARD_CS));
        return true;
    }
#endif

    if (littlefs_.begin(fsSizeBytes)) {
        backend_ = Backend::LittleFS;
        if (storageDiag_[0] == '\0') {
            snprintf(storageDiag_, sizeof(storageDiag_), "littlefs: mounted internal flash");
        } else {
            char prior[96] = {0};
            snprintf(prior, sizeof(prior), "%s", storageDiag_);
            snprintf(storageDiag_, sizeof(storageDiag_), "fallback littlefs (%s)", prior);
        }
        return true;
    }

    if (storageDiag_[0] == '\0') {
        snprintf(storageDiag_, sizeof(storageDiag_), "storage init failed");
    } else {
        char prior[96] = {0};
        snprintf(prior, sizeof(prior), "%s", storageDiag_);
        snprintf(storageDiag_, sizeof(storageDiag_), "storage init failed (%s)", prior);
    }

    return false;
}

bool ChirpFS::beginSdCard()
{
    prepareSharedSpiChipSelects();
    SPI.begin();
    if (!SD.begin(SD_CARD_CS)) {
        snprintf(storageDiag_, sizeof(storageDiag_), "sd init failed (cs=%u)", static_cast<unsigned>(SD_CARD_CS));
        return false;
    }

    File root = SD.open("/");
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        snprintf(storageDiag_, sizeof(storageDiag_), "sd mounted but root invalid");
        return false;
    }
    root.close();
    return true;
}

void ChirpFS::prepareSharedSpiChipSelects() const
{
    pinMode(DISPLAY_CS, OUTPUT);
    digitalWrite(DISPLAY_CS, HIGH);

    pinMode(SD_CARD_CS, OUTPUT);
    digitalWrite(SD_CARD_CS, HIGH);
}

File ChirpFS::open(const char *path, uint8_t mode)
{
    if (backend_ == Backend::SD) return SD.open(path, mode);
    return littlefs_.open(path, mode);
}

bool ChirpFS::exists(const char *path)
{
    if (backend_ == Backend::SD) return SD.exists(path);
    return littlefs_.exists(path);
}

bool ChirpFS::remove(const char *path)
{
    if (backend_ == Backend::SD) return SD.remove(path);
    return littlefs_.remove(path);
}

bool ChirpFS::mkdir(const char *path)
{
    if (backend_ == Backend::SD) return SD.mkdir(path);
    return littlefs_.mkdir(path);
}

uint64_t ChirpFS::totalSize()
{
    if (backend_ == Backend::SD) {
        return SD.totalSize();
    }
    return littlefs_.totalSize();
}

uint64_t ChirpFS::usedSize()
{
    if (backend_ == Backend::SD) {
        return SD.usedSize();
    }
    return littlefs_.usedSize();
}

uint32_t ChirpFS::fsBlockSize() const
{
    if (backend_ == Backend::SD) {
        return SD.sdfs.bytesPerCluster();
    }
    return 0;
}

uint32_t ChirpFS::fsBlockCount() const
{
    if (backend_ == Backend::SD) {
        return SD.sdfs.clusterCount();
    }
    return 0;
}

int32_t ChirpFS::fsBlockCycles() const
{
    if (backend_ == Backend::SD) {
        return -1; // wear-leveling not applicable to FAT
    }
    return 0; // LittleFS_Program does not expose block_cycles publicly
}

const char *ChirpFS::backendName() const
{
    if (backend_ == Backend::SD) return "sd";
    if (backend_ == Backend::LittleFS) return "littlefs";
    return "none";
}
