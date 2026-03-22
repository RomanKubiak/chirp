#include "wren_host.h"
#include "chirp_config.h"
#include "runtime_log.h"
#include "script_storage.h"
#include "wren_midi_bridge.h"
#include "wren_runtime_script.h"

#include <Arduino.h>

extern WrenVM        *vm;
extern ScriptStorage  scriptStorage;

namespace {
constexpr const char *kBuiltinRuntimePath = "/scripts/builtin/_runtime.wren";
constexpr const char *kLegacyRuntimeName  = "_runtime";
constexpr const char *kLegacyRuntimeName2 = "runtime";
constexpr const char *kUserScriptsDir     = "/scripts/user";
}

// ── Error capture ─────────────────────────────────────────────────────────────
char gCapturedWrenError[192] = {0};

// ── Wren VM callbacks ─────────────────────────────────────────────────────────
void writeFn(WrenVM *wren_vm, const char *text)
{
    (void)wren_vm;
#if !DEBUG_LOGGING
    (void)text;
#else
    if (!text || text[0] == '\0') return;
    size_t i = 0;
    while (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r') i++;
    if (!text[i]) return;

    char stripped[256] = {0};
    size_t len = strnlen(text, sizeof(stripped) - 1);
    memcpy(stripped, text, len);
    while (len > 0 && (stripped[len-1] == '\n' || stripped[len-1] == '\r' ||
                       stripped[len-1] == ' '  || stripped[len-1] == '\t'))
        stripped[--len] = '\0';
    if (len == 0) return;
    if (strstr(stripped, "[MIDI]") && strstr(stripped, "timingClock")) return;

    char buf[256] = {0};
    snprintf(buf, sizeof(buf), "[t=%lums/%luus] [WREN] %s",
             static_cast<unsigned long>(millis()),
             static_cast<unsigned long>(micros()),
             stripped);
    logRuntime(buf);
#endif
}

void errorFn(WrenVM *wren_vm, WrenErrorType type, const char *module, int line, const char *message)
{
    (void)wren_vm;
    char err[180] = {0};
    if (type == WREN_ERROR_COMPILE)
    {
        snprintf(err, sizeof(err), "[WREN COMPILE] %s:%d %s", module, line, message);
        logRuntime(err);
        if (gCapturedWrenError[0] == '\0')
            snprintf(gCapturedWrenError, sizeof(gCapturedWrenError), "%s", err);
        return;
    }
    if (type == WREN_ERROR_RUNTIME)
    {
        snprintf(err, sizeof(err), "[WREN RUNTIME] %s", message);
        logRuntime(err);
        if (gCapturedWrenError[0] == '\0')
            snprintf(gCapturedWrenError, sizeof(gCapturedWrenError), "%s", err);
        return;
    }
#if DEBUG_LOGGING
    snprintf(err, sizeof(err), "[WREN STACK] %s:%d %s", module, line, message);
    logRuntime(err);
#else
    (void)module; (void)line; (void)message;
#endif
}

// ── Wren execution ────────────────────────────────────────────────────────────
WrenInterpretResult interpretWrenWithCapturedError(const char *module, const char *source)
{
    gCapturedWrenError[0] = '\0';
    return wrenInterpret(vm, module, source);
}

// ── Runtime bootstrap ─────────────────────────────────────────────────────────
bool initializeWrenRuntime()
{
    String runtimeSource;
    const char *runtimeText = kEmbeddedWrenRuntime;
    bool fromFlash = false;

    if (scriptStorage.loadFile(kBuiltinRuntimePath, runtimeSource))
    {
        logRuntime("Wren runtime loaded from flash");
        runtimeText = runtimeSource.c_str();
        fromFlash = true;
    }
    else
    {
        logRuntime("Wren runtime missing on flash, using embedded runtime");
        if (scriptStorage.isMounted() &&
            scriptStorage.saveFile(kBuiltinRuntimePath, String(kEmbeddedWrenRuntime)))
            logRuntime("Embedded runtime copied to flash");
    }

    // Legacy cleanup from old flat /scripts naming.
    scriptStorage.removeScript(kLegacyRuntimeName);
    scriptStorage.removeScript(kLegacyRuntimeName2);

    WrenInterpretResult result = interpretWrenWithCapturedError("chirp_runtime", runtimeText);
    if (result != WREN_RESULT_SUCCESS && fromFlash)
    {
        // Flash version failed — overwrite with embedded and retry.
        logRuntime("Wren runtime from flash failed, overwriting with embedded runtime");
        if (scriptStorage.isMounted())
            scriptStorage.saveFile(kBuiltinRuntimePath, String(kEmbeddedWrenRuntime));
        gCapturedWrenError[0] = '\0';
        result = interpretWrenWithCapturedError("chirp_runtime", kEmbeddedWrenRuntime);
    }

    if (result != WREN_RESULT_SUCCESS)
    {
        logRuntime("Wren runtime failed to initialize");
        return false;
    }
    logRuntime("Wren runtime loaded");
    return true;
}

bool executeStoredWrenScriptsOnBoot()
{
    if (!vm || !scriptStorage.isMounted()) return false;

    static constexpr size_t kMaxBootScripts = 32;
    String scriptPaths[kMaxBootScripts];
    const size_t discovered = scriptStorage.listFiles(kUserScriptsDir, scriptPaths, kMaxBootScripts);
    const size_t count = (discovered < kMaxBootScripts) ? discovered : kMaxBootScripts;

    // Insertion sort (alphabetical)
    for (size_t i = 1; i < count; i++)
    {
        String key = scriptPaths[i];
        size_t j = i;
        while (j > 0 && scriptPaths[j-1].compareTo(key) > 0)
        {
            scriptPaths[j] = scriptPaths[j-1];
            j--;
        }
        scriptPaths[j] = key;
    }

    if (count == 0) { logSetup("[BOOT] No /scripts/user/*.wren scripts found"); return true; }

    char info[96] = {0};
    if (discovered > kMaxBootScripts)
        snprintf(info, sizeof(info), "[BOOT] /scripts/user/*.wren: running %lu/%lu (limit=%lu)",
                 static_cast<unsigned long>(count),
                 static_cast<unsigned long>(discovered),
                 static_cast<unsigned long>(kMaxBootScripts));
    else
        snprintf(info, sizeof(info), "[BOOT] /scripts/user/*.wren: %lu",
                 static_cast<unsigned long>(count));
    logSetup(info);

    bool allOk = true;
    for (size_t i = 0; i < count; i++)
    {
        const String &displayPath = scriptPaths[i];
        if (!displayPath.endsWith(".wren")) continue;
        int slash = displayPath.lastIndexOf('/');
        String baseName = (slash >= 0) ? displayPath.substring(slash + 1) : displayPath;
        if (baseName.length() > 0 && baseName[0] == '_') continue; // system script

        char msg[128] = {0};
        snprintf(msg, sizeof(msg), "[BOOT] Running %s", displayPath.c_str());
        logSetup(msg);

        String source;
        if (!scriptStorage.loadFile(displayPath.c_str(), source))
        {
            snprintf(msg, sizeof(msg), "[BOOT] Failed to read %s", displayPath.c_str());
            logSetup(msg); allOk = false; continue;
        }

        WrenInterpretResult result = interpretWrenWithCapturedError("chirp_runtime", source.c_str());
        if (result != WREN_RESULT_SUCCESS)
        {
            snprintf(msg, sizeof(msg), "[BOOT] Wren failed %s", displayPath.c_str());
            logSetup(msg); allOk = false; continue;
        }

        snprintf(msg, sizeof(msg), "[BOOT] Wren ok %s", displayPath.c_str());
        logSetup(msg);
    }
    return allOk;
}
