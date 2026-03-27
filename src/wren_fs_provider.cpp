#include "wren_fs_provider.h"

#include <Arduino.h>

namespace {
ChirpFS *gFlash = nullptr;

bool providerRead(const char *path, String &out)
{
    if (!gFlash) return false;
    File f = gFlash->open(path, FILE_READ);
    if (!f) return false;
    out.reserve(f.size() + 1);
    while (f.available()) out += static_cast<char>(f.read());
    f.close();
    return true;
}

bool providerWrite(const char *path, const char *data, size_t len)
{
    if (!gFlash) return false;
    String p(path);
    int slash = p.lastIndexOf('/');
    if (slash > 0) {
        String dir = p.substring(0, slash);
        if (!gFlash->exists(dir.c_str())) gFlash->mkdir(dir.c_str());
    }
    if (gFlash->exists(path)) gFlash->remove(path);
    File f = gFlash->open(path, FILE_WRITE);
    if (!f) return false;
    size_t written = f.write(reinterpret_cast<const uint8_t *>(data), len);
    f.flush();
    f.close();
    return written == len;
}

bool providerRemove(const char *path)
{
    return gFlash && gFlash->exists(path) && gFlash->remove(path);
}

bool providerExists(const char *path)
{
    return gFlash && gFlash->exists(path);
}

int32_t providerSize(const char *path)
{
    if (!gFlash) return -1;
    File f = gFlash->open(path, FILE_READ);
    if (!f) return -1;
    int32_t sz = static_cast<int32_t>(f.size());
    f.close();
    return sz;
}

size_t providerList(const char *path, String *names, size_t maxNames)
{
    if (!gFlash) return 0;
    File dir = gFlash->open(path);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return 0;
    }

    size_t count = 0;
    while (true) {
        File entry = dir.openNextFile();
        if (!entry) break;
        String name = entry.name();
        int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        entry.close();
        if (names && count < maxNames) names[count] = name;
        count++;
    }

    dir.close();
    return count;
}
} // namespace

WrenFsProvider createWrenFsProvider(ChirpFS &flash)
{
    gFlash = &flash;

    WrenFsProvider provider;
    provider.read = &providerRead;
    provider.write = &providerWrite;
    provider.remove = &providerRemove;
    provider.exists = &providerExists;
    provider.size = &providerSize;
    provider.list = &providerList;
    return provider;
}