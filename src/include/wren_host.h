#ifndef WREN_HOST_H
#define WREN_HOST_H

#include "wren.hpp"

// ── Error capture buffer (written by errorFn, read by handleFrame) ────────────
extern char gCapturedWrenError[192];

// ── Wren VM callbacks (registered in merge.ino setup via config) ──────────────
void writeFn(WrenVM *vm, const char *text);
void errorFn(WrenVM *vm, WrenErrorType type, const char *module, int line, const char *message);

// ── Wren execution helpers ─────────────────────────────────────────────────────
// Clears gCapturedWrenError, runs the source, and returns the interpret result.
WrenInterpretResult interpretWrenWithCapturedError(const char *module, const char *source);

// ── Wren bootstrap ────────────────────────────────────────────────────────────
bool initializeWrenRuntime();
bool executeStoredWrenScriptsOnBoot();

#endif // WREN_HOST_H
