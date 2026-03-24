#ifndef CHIRP_DISPLAY_H
#define CHIRP_DISPLAY_H

#include <stdint.h>

// ── Display UI Framework for Chirp ────────────────────────────────────────────
// Provides Wren-callable display functions for script-driven UI

namespace ChirpDisplay {

// ── Core display operations ───────────────────────────────────────────────────
void init();
void clear();
void update();

// ── Navigation display (main view) ─────────────────────────────────────────────
// Shows: [< PREV] Current Instrument [NEXT >]
//        Current Parameter
//        Value indicator

void showInstrument(const char *prevName, const char *currentName, const char *nextName);
void showKit(const char *kitName);
void showParameter(const char *paramName, uint8_t cc, uint8_t value);
void showValue(uint8_t value);

// ── Status display ─────────────────────────────────────────────────────────────
void showStatus(const char *statusText);
void showLauncherMenu(const char *prev, const char *curr, const char *next,
                      bool prevActive, bool currActive, bool nextActive,
                      bool prevError = false, bool currError = false, bool nextError = false);
void showLauncherPreview(const char *title, const char *detail = nullptr, bool isError = false);
void showMidiActivity(uint8_t port, bool active);
bool isAsyncFrameBufferEnabled();
const char *displayModeName();
void showSystemStats(const char *line1, const char *line2, const char *line3, const char *line4);

// ── Utility ────────────────────────────────────────────────────────────────────
void showDebug(const char *text);

}  // namespace ChirpDisplay

#endif  // CHIRP_DISPLAY_H
