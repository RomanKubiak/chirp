#include "wren_host.h"
#include "chirp_config.h"
#include "runtime_log.h"
#include "script_storage.h"
#include "wren_midi_bridge.h"
#include "wren_runtime_script.h"
#include "wren_vm.h"

#include <Arduino.h>
#include <cctype>

extern WrenVM        *vm;
extern ScriptStorage  scriptStorage;

namespace {
constexpr const char *kBuiltinRuntimePath = "/scripts/builtin/_runtime.wren";
constexpr const char *kLegacyRuntimeName  = "_runtime";
constexpr const char *kLegacyRuntimeName2 = "runtime";
constexpr const char *kUserScriptsDir     = "/scripts/user";
constexpr size_t kMaxBootScripts = 32;
constexpr size_t kMaxScriptNameLen = 48;
constexpr size_t kMaxScriptModuleLen = 64;

char gBootDisplayScriptName[48] = {0};
char gLastLoadedScriptName[kMaxScriptNameLen] = {0};
char gLastLoadedScriptModule[kMaxScriptModuleLen] = {0};
char gLastScriptError[192] = {0};
char gLastScriptErrorName[kMaxScriptNameLen] = {0};
uint32_t gLastLoadedScriptMemBytes = 0;
uint32_t gScriptLoadSuccessCount = 0;
uint32_t gScriptLoadErrorCount = 0;

void sanitizeModuleToken(const char *src, char *dst, size_t dstSize)
{
    if (dstSize == 0) return;
    size_t out = 0;
    if (src != nullptr)
    {
        for (size_t i = 0; src[i] != '\0' && out < (dstSize - 1); ++i)
        {
            const unsigned char ch = static_cast<unsigned char>(src[i]);
            if (std::isalnum(ch)) dst[out++] = static_cast<char>(std::tolower(ch));
            else dst[out++] = '_';
        }
    }
    if (out == 0 && dstSize > 1)
    {
        dst[out++] = 's';
        if (out < (dstSize - 1)) dst[out++] = 'c';
        if (out < (dstSize - 1)) dst[out++] = 'r';
        if (out < (dstSize - 1)) dst[out++] = 'i';
        if (out < (dstSize - 1)) dst[out++] = 'p';
        if (out < (dstSize - 1)) dst[out++] = 't';
    }
    dst[out] = '\0';
}

String buildIsolatedScriptSource(const char *source)
{
    String wrapped;
    const char *prefix = "import \"chirp_runtime\" for Midi, Display, Script, Log, File, Config, Clock, Debug, Utils, Console\n";
    wrapped.reserve(strlen(prefix) + strlen(source) + 2);
    wrapped += prefix;
    wrapped += source;
    wrapped += "\n";
    return wrapped;
}

void trackScriptLoadSuccess(const char *scriptName, const char *moduleName, uint32_t memBytes)
{
    snprintf(gLastLoadedScriptName, sizeof(gLastLoadedScriptName), "%s", scriptName ? scriptName : "");
    snprintf(gLastLoadedScriptModule, sizeof(gLastLoadedScriptModule), "%s", moduleName ? moduleName : "");
    gLastLoadedScriptMemBytes = memBytes;
    gLastScriptError[0] = '\0';
    gLastScriptErrorName[0] = '\0';
    gScriptLoadSuccessCount++;
}

void trackScriptLoadError(const char *scriptName)
{
    snprintf(gLastScriptErrorName, sizeof(gLastScriptErrorName), "%s", scriptName ? scriptName : "");
    if (gCapturedWrenError[0] != '\0')
        snprintf(gLastScriptError, sizeof(gLastScriptError), "%s", gCapturedWrenError);
    else
        snprintf(gLastScriptError, sizeof(gLastScriptError), "Script execution failed");
    gScriptLoadErrorCount++;
}

void setBootDisplayScriptName(const String &baseName)
{
    if (gBootDisplayScriptName[0] != '\0') return;
    String trimmed = baseName;
    if (trimmed.endsWith(".wren"))
        trimmed = trimmed.substring(0, trimmed.length() - 5);
    if (trimmed.length() == 0) return;
    snprintf(gBootDisplayScriptName, sizeof(gBootDisplayScriptName), "%s", trimmed.c_str());
}
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
    // Always use the embedded runtime: it is always in sync with the firmware.
    // Overwrite the flash copy unconditionally so that after a firmware update
    // the cached file never lags behind and causes "no such method" errors in
    // scripts that rely on newly added Script/Midi API methods.
    if (scriptStorage.isMounted())
    {
        if (scriptStorage.saveFile(kBuiltinRuntimePath, String(kEmbeddedWrenRuntime)))
            logRuntime("Wren runtime (embedded) synced to flash");
        else
            logRuntime("Wren runtime flash sync failed (non-fatal)");
    }

    // Legacy cleanup from old flat /scripts naming.
    scriptStorage.removeScript(kLegacyRuntimeName);
    scriptStorage.removeScript(kLegacyRuntimeName2);

    WrenInterpretResult result = interpretWrenWithCapturedError("chirp_runtime", kEmbeddedWrenRuntime);
    if (result != WREN_RESULT_SUCCESS)
    {
        logRuntime("Wren runtime failed to initialize");
        return false;
    }
    logRuntime("Wren runtime loaded");
    return true;
}

size_t listStoredWrenScripts(String *namesOut, size_t maxNames)
{
    if (!scriptStorage.isMounted()) return 0;

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

    size_t outCount = 0;
    for (size_t i = 0; i < count; i++)
    {
        const String &displayPath = scriptPaths[i];
        if (!displayPath.endsWith(".wren")) continue;
        int slash = displayPath.lastIndexOf('/');
        String baseName = (slash >= 0) ? displayPath.substring(slash + 1) : displayPath;
        if (baseName.length() > 0 && baseName[0] == '_') continue;
        if (baseName.endsWith(".wren"))
            baseName = baseName.substring(0, baseName.length() - 5);
        if (namesOut && outCount < maxNames) namesOut[outCount] = baseName;
        outCount++;
    }
    return outCount;
}

bool prepareStoredWrenScriptsOnBoot()
{
    if (!vm || !scriptStorage.isMounted()) return false;

    gBootDisplayScriptName[0] = '\0';
    WrenMidiBridge::clearRegisteredScripts();
    WrenMidiBridge::clearActiveScriptSelection();

    String names[kMaxBootScripts];
    const size_t discovered = listStoredWrenScripts(names, kMaxBootScripts);
    const size_t count = (discovered < kMaxBootScripts) ? discovered : kMaxBootScripts;

    if (count == 0) { logSetup("[BOOT] No /scripts/user/*.wren scripts found"); return true; }

    char info[96] = {0};
    if (discovered > kMaxBootScripts)
        snprintf(info, sizeof(info), "[BOOT] /scripts/user/*.wren: indexed %lu/%lu (limit=%lu)",
                 static_cast<unsigned long>(count),
                 static_cast<unsigned long>(discovered),
                 static_cast<unsigned long>(kMaxBootScripts));
    else
        snprintf(info, sizeof(info), "[BOOT] /scripts/user/*.wren: %lu",
                 static_cast<unsigned long>(count));
    logSetup(info);

    for (size_t i = 0; i < count; i++)
    {
        const String &baseName = names[i];
        if (baseName.length() == 0) continue;
        setBootDisplayScriptName(baseName + ".wren");
        WrenMidiBridge::registerScriptName(baseName.c_str());
    }

    logSetup("[BOOT] Launcher mode: waiting for script selection");
    return true;
}

bool runStoredWrenScript(const char *name)
{
    if (!vm || !scriptStorage.isMounted() || name == nullptr || name[0] == '\0') return false;

    String path = String(kUserScriptsDir) + "/" + String(name) + ".wren";
    String source;
    if (!scriptStorage.loadFile(path.c_str(), source)) return false;

    // GC between SD load and Wren interpretation: old module objects may still be
    // reachable in the Wren heap; collecting here lowers peak memory during wrapping
    // and compilation of the new script.
    wrenCollectGarbage(vm);

    const bool ok = runWrenUserScriptSource(name, source.c_str());

    char msg[128] = {0};
    if (!ok)
    {
        snprintf(msg, sizeof(msg), "[BOOT] Wren failed /scripts/user/%s.wren", name);
        logSetup(msg);
        return false;
    }

    snprintf(msg, sizeof(msg), "[BOOT] Wren ok /scripts/user/%s.wren", name);
    logSetup(msg);
    return true;
}

bool runWrenUserScriptSource(const char *scriptName, const char *source)
{
    if (!vm || scriptName == nullptr || scriptName[0] == '\0' || source == nullptr) return false;

    // NOTE: Unload of prior script is handled exclusively by the launcher
    // (launcherToggleSelectedScript/launcherDeactivateRunningScript).
    // This avoids double-unload crashes and gives explicit control over cleanup timing.

    uint32_t beforeBytes = vm ? static_cast<uint32_t>(vm->bytesAllocated) : 0;

    char token[32] = {0};
    sanitizeModuleToken(scriptName, token, sizeof(token));
    // Use a stable module name (no epoch suffix) so re-loading the same script
    // reuses the existing module table entry.  Wren overwrites the module's
    // top-level variable slots, making the old objects unreachable so GC can
    // collect them after the reload instead of accumulating stale modules.
    char moduleName[kMaxScriptModuleLen] = {0};
    snprintf(moduleName, sizeof(moduleName), "user_%s", token);

    WrenMidiBridge::setActiveScriptName(scriptName);
    WrenMidiBridge::beginScriptContext(scriptName);
    const String wrappedSource = buildIsolatedScriptSource(source);
    WrenInterpretResult result = interpretWrenWithCapturedError(moduleName, wrappedSource.c_str());
    WrenMidiBridge::endScriptContext();

    uint32_t afterBytes = vm ? static_cast<uint32_t>(vm->bytesAllocated) : beforeBytes;
    uint32_t deltaBytes = (afterBytes >= beforeBytes) ? (afterBytes - beforeBytes) : 0;

    if (result != WREN_RESULT_SUCCESS)
    {
        trackScriptLoadError(scriptName);
        return false;
    }

    trackScriptLoadSuccess(scriptName, moduleName, deltaBytes);
    // GC after a successful load: old slot values (from any prior run of this
    // module) are now unreachable — reclaim them immediately.
    if (vm) wrenCollectGarbage(vm);
    return true;
}

bool executeStoredWrenScriptsOnBoot()
{
    return prepareStoredWrenScriptsOnBoot();
}

const char *bootDisplayScriptName()
{
    return gBootDisplayScriptName;
}

const char *lastLoadedWrenScriptName()
{
    return gLastLoadedScriptName;
}

const char *lastLoadedWrenModuleName()
{
    return gLastLoadedScriptModule;
}

uint32_t lastLoadedWrenScriptBytes()
{
    return gLastLoadedScriptMemBytes;
}

uint32_t totalWrenScriptLoadSuccesses()
{
    return gScriptLoadSuccessCount;
}

uint32_t totalWrenScriptLoadErrors()
{
    return gScriptLoadErrorCount;
}

const char *lastWrenScriptError()
{
    return gLastScriptError;
}

const char *lastWrenScriptErrorScriptName()
{
    return gLastScriptErrorName;
}
