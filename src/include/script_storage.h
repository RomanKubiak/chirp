#ifndef SCRIPT_STORAGE_H
#define SCRIPT_STORAGE_H

#include <Arduino.h>
#include <LittleFS.h>

class ScriptStorage
{
public:
    static constexpr size_t kDefaultFsBytes = 512 * 1024;

    ScriptStorage(LittleFS_Program &fs,
                  size_t fsSizeBytes = kDefaultFsBytes,
                  const char *scriptsDir = "/scripts");

    bool begin();
    bool isMounted() const;

    bool saveScript(const char *name, const String &source);
    bool loadScript(const char *name, String &sourceOut);
    bool removeScript(const char *name);
    bool scriptExists(const char *name);
    int32_t scriptSize(const char *name);  // returns byte count, -1 if not found
    size_t listScripts(String *namesOut, size_t maxNames); // names without .wren

    bool saveFile(const char *path, const String &source);
    bool loadFile(const char *path, String &sourceOut);
    bool removeFile(const char *path);
    bool fileExists(const char *path);
    int32_t fileSize(const char *path);
    size_t listFiles(const char *dir, String *namesOut, size_t maxNames);
    size_t listManagedFiles(String *namesOut, size_t maxNames);

    String scriptPath(const char *name) const;

private:
    bool ensureDirectory(const char *path);
    bool ensureManagedLayout();
    bool isValidFilePath(const char *path) const;
    String normalizeAbsolutePath(const char *path) const;
    bool isValidScriptName(const char *name) const;

    LittleFS_Program &fs_;
    size_t fsSizeBytes_;
    const char *scriptsDir_;
    bool mounted_;
};

#endif // SCRIPT_STORAGE_H
