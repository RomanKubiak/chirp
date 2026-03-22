#ifndef CHIRP_FS_H
#define CHIRP_FS_H

#include <LittleFS.h>

// ── ChirpFS ───────────────────────────────────────────────────────────────────
// Thin subclass of LittleFS_Program that exposes the protected lfs_config
// fields needed for diagnostics (block geometry, wear-leveling threshold).
//
// Designed to be a drop-in replacement for LittleFS_Program so that a future
// swap to LittleFS_SPI (external SD via SPI) only requires changing the base
// class here, with no changes at call sites.
// ─────────────────────────────────────────────────────────────────────────────
class ChirpFS : public LittleFS_Program
{
public:
    // Erase block size in bytes (e.g. 4096 for internal program flash).
    uint32_t fsBlockSize()  const { return configured ? static_cast<uint32_t>(config.block_size)  : 0; }

    // Total number of erase blocks.
    uint32_t fsBlockCount() const { return configured ? static_cast<uint32_t>(config.block_count) : 0; }

    // Wear-leveling cycle threshold.
    // Positive = max erase cycles before metadata is moved to a fresh block.
    // -1 = block-level wear-leveling disabled.
    int32_t  fsBlockCycles() const { return configured ? static_cast<int32_t>(config.block_cycles) : 0; }
};

#endif // CHIRP_FS_H
