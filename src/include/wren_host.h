#ifndef WREN_HOST_H
#define WREN_HOST_H

#include <Arduino.h>
#include "wren.hpp"

// ── Error capture buffer (written by errorFn, read by handleFrame) ────────────
extern char gCapturedWrenError[192];

// ── Wren VM callbacks (registered in chirp.ino setup via config) ──────────────
void writeFn(WrenVM *vm, const char *text);
void errorFn(WrenVM *vm, WrenErrorType type, const char *module, int line, const char *message);

// ── Wren execution helpers ─────────────────────────────────────────────────────
// Clears gCapturedWrenError, runs the source, and returns the interpret result.
WrenInterpretResult interpretWrenWithCapturedError(const char *module, const char *source);

// ── Wren bootstrap ────────────────────────────────────────────────────────────
bool initializeWrenRuntime();
bool executeStoredWrenScriptsOnBoot();
bool prepareStoredWrenScriptsOnBoot();
size_t listStoredWrenScripts(String *namesOut, size_t maxNames);
bool runStoredWrenScript(const char *name);
bool runWrenUserScriptSource(const char *scriptName, const char *source);
const char *bootScriptName();

// ── Runtime script load telemetry ────────────────────────────────────────────
const char *lastLoadedWrenScriptName();
const char *lastLoadedWrenModuleName();
uint32_t lastLoadedWrenScriptBytes();
uint32_t totalWrenScriptLoadSuccesses();
uint32_t totalWrenScriptLoadErrors();
const char *lastWrenScriptError();
const char *lastWrenScriptErrorScriptName();

#endif // WREN_HOST_H
