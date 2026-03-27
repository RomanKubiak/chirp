#ifndef WREN_FS_PROVIDER_H
#define WREN_FS_PROVIDER_H

#include "chirp_fs.h"
#include "wren_midi_bridge.h"

WrenFsProvider createWrenFsProvider(ChirpFS &flash);

#endif // WREN_FS_PROVIDER_H