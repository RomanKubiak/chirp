#include "script_storage.h"

ScriptStorage::ScriptStorage(LittleFS_Program &fs, size_t fsSizeBytes, const char *scriptsDir)
    : fs_(fs), fsSizeBytes_(fsSizeBytes), scriptsDir_(scriptsDir), mounted_(false)
{
}

bool ScriptStorage::begin()
{
    if (mounted_)
    {
        return true;
    }

    if (!fs_.begin(fsSizeBytes_))
    {
        return false;
    }

    mounted_ = true;
    return ensureManagedLayout();
}

bool ScriptStorage::isMounted() const
{
    return mounted_;
}

bool ScriptStorage::saveScript(const char *name, const String &source)
{
    if (!begin() || !isValidScriptName(name))
    {
        return false;
    }

    String path = scriptPath(name);
    if (path.length() == 0)
    {
        return false;
    }

    return saveFile(path.c_str(), source);
}

bool ScriptStorage::loadScript(const char *name, String &sourceOut)
{
    sourceOut = "";

    if (!begin() || !isValidScriptName(name))
    {
        return false;
    }

    String path = scriptPath(name);
    if (path.length() == 0)
    {
        return false;
    }

    return loadFile(path.c_str(), sourceOut);
}

bool ScriptStorage::removeScript(const char *name)
{
    if (!begin() || !isValidScriptName(name))
    {
        return false;
    }

    String path = scriptPath(name);
    if (path.length() == 0)
    {
        return false;
    }

    return removeFile(path.c_str());
}

bool ScriptStorage::scriptExists(const char *name)
{
    if (!begin() || !isValidScriptName(name))
    {
        return false;
    }

    String path = scriptPath(name);
    if (path.length() == 0)
    {
        return false;
    }

    return fileExists(path.c_str());
}

int32_t ScriptStorage::scriptSize(const char *name)
{
    if (!begin() || !isValidScriptName(name))
    {
        return -1;
    }

    String path = scriptPath(name);
    if (path.length() == 0)
    {
        return -1;
    }

    return fileSize(path.c_str());
}

size_t ScriptStorage::listScripts(String *namesOut, size_t maxNames)
{
    if (!begin())
    {
        return 0;
    }

    String files[32];
    size_t count = listFiles(scriptsDir_, files, 32);
    size_t out = 0;

    for (size_t i = 0; i < count; ++i)
    {
        String name = files[i];
        int slash = name.lastIndexOf('/');
        if (slash >= 0)
        {
            name = name.substring(slash + 1);
        }

        if (!name.endsWith(".wren"))
        {
            continue;
        }

        name.remove(name.length() - 5, 5);
        if (namesOut != nullptr && out < maxNames)
        {
            namesOut[out] = name;
        }
        ++out;
    }

    return out;
}

bool ScriptStorage::saveFile(const char *path, const String &source)
{
    if (!begin())
    {
        return false;
    }

    String fullPath = normalizeAbsolutePath(path);
    if (fullPath.length() == 0)
    {
        return false;
    }

    int slash = fullPath.lastIndexOf('/');
    if (slash < 0)
    {
        return false;
    }

    String dir = (slash == 0) ? String("/") : fullPath.substring(0, slash);
    if (!ensureDirectory(dir.c_str()))
    {
        return false;
    }

    if (fs_.exists(fullPath.c_str()))
    {
        fs_.remove(fullPath.c_str());
    }

    File file = fs_.open(fullPath.c_str(), FILE_WRITE);
    if (!file)
    {
        return false;
    }

    size_t written = file.print(source);
    file.flush();
    file.close();
    return written == source.length();
}

bool ScriptStorage::loadFile(const char *path, String &sourceOut)
{
    sourceOut = "";

    if (!begin())
    {
        return false;
    }

    String fullPath = normalizeAbsolutePath(path);
    if (fullPath.length() == 0)
    {
        return false;
    }

    File file = fs_.open(fullPath.c_str(), FILE_READ);
    if (!file)
    {
        return false;
    }

    sourceOut.reserve(file.size() + 1);
    while (file.available())
    {
        sourceOut += static_cast<char>(file.read());
    }

    file.close();
    return true;
}

bool ScriptStorage::removeFile(const char *path)
{
    if (!begin())
    {
        return false;
    }

    String fullPath = normalizeAbsolutePath(path);
    if (fullPath.length() == 0 || !fs_.exists(fullPath.c_str()))
    {
        return false;
    }

    return fs_.remove(fullPath.c_str());
}

bool ScriptStorage::fileExists(const char *path)
{
    if (!begin())
    {
        return false;
    }

    String fullPath = normalizeAbsolutePath(path);
    if (fullPath.length() == 0)
    {
        return false;
    }

    return fs_.exists(fullPath.c_str());
}

int32_t ScriptStorage::fileSize(const char *path)
{
    if (!begin())
    {
        return -1;
    }

    String fullPath = normalizeAbsolutePath(path);
    if (fullPath.length() == 0 || !fs_.exists(fullPath.c_str()))
    {
        return -1;
    }

    File file = fs_.open(fullPath.c_str(), FILE_READ);
    if (!file)
    {
        return -1;
    }

    int32_t sz = static_cast<int32_t>(file.size());
    file.close();
    return sz;
}

size_t ScriptStorage::listFiles(const char *dir, String *namesOut, size_t maxNames)
{
    if (!begin())
    {
        return 0;
    }

    String base = normalizeAbsolutePath(dir);
    if (base.length() == 0)
    {
        return 0;
    }

    File d = fs_.open(base.c_str());
    if (!d || !d.isDirectory())
    {
        if (d)
        {
            d.close();
        }
        return 0;
    }

    size_t count = 0;
    while (true)
    {
        File entry = d.openNextFile();
        if (!entry)
        {
            break;
        }

        if (!entry.isDirectory())
        {
            String name = entry.name();
            if (!name.startsWith("/"))
            {
                String prefix = base;
                if (!prefix.endsWith("/"))
                {
                    prefix += "/";
                }
                name = prefix + name;
            }

            if (namesOut != nullptr && count < maxNames)
            {
                namesOut[count] = name;
            }
            ++count;
        }

        entry.close();
    }

    d.close();
    return count;
}

size_t ScriptStorage::listManagedFiles(String *namesOut, size_t maxNames)
{
    if (!begin())
    {
        return 0;
    }

    static constexpr const char *kManagedDirs[] = {
        "/scripts/user",
        "/scripts/builtin",
        "/userdata",
    };

    size_t total = 0;
    for (size_t i = 0; i < (sizeof(kManagedDirs) / sizeof(kManagedDirs[0])); ++i)
    {
        String files[32];
        size_t count = listFiles(kManagedDirs[i], files, 32);
        for (size_t j = 0; j < count; ++j)
        {
            if (namesOut != nullptr && total < maxNames)
            {
                namesOut[total] = files[j];
            }
            ++total;
        }
    }

    if (fileExists("/README.txt"))
    {
        if (namesOut != nullptr && total < maxNames)
        {
            namesOut[total] = "/README.txt";
        }
        ++total;
    }

    return total;
}

String ScriptStorage::scriptPath(const char *name) const
{
    if (!isValidScriptName(name))
    {
        return String();
    }

    String filename(name);
    if (!filename.endsWith(".wren"))
    {
        filename += ".wren";
    }

    String fullPath = scriptsDir_;
    fullPath += "/";
    fullPath += filename;
    return fullPath;
}

bool ScriptStorage::ensureDirectory(const char *path)
{
    String fullPath = normalizeAbsolutePath(path);
    if (fullPath.length() == 0)
    {
        return false;
    }

    if (fs_.exists(fullPath.c_str()))
    {
        File dir = fs_.open(fullPath.c_str());
        bool isDir = dir && dir.isDirectory();
        if (dir)
        {
            dir.close();
        }
        return isDir;
    }

    if (fullPath == "/")
    {
        return true;
    }

    int slash = fullPath.lastIndexOf('/');
    if (slash > 0)
    {
        String parent = fullPath.substring(0, slash);
        if (!ensureDirectory(parent.c_str()))
        {
            return false;
        }
    }

    return fs_.mkdir(fullPath.c_str());
}

bool ScriptStorage::ensureManagedLayout()
{
    static constexpr const char *kRequiredDirs[] = {
        "/scripts",
        "/scripts/user",
        "/scripts/builtin",
        "/userdata",
    };

    for (size_t i = 0; i < (sizeof(kRequiredDirs) / sizeof(kRequiredDirs[0])); ++i)
    {
        if (!ensureDirectory(kRequiredDirs[i]))
        {
            return false;
        }
    }

    return true;
}

bool ScriptStorage::isValidFilePath(const char *path) const
{
    if (path == nullptr || path[0] == '\0')
    {
        return false;
    }

    String value(path);
    if (!value.startsWith("/"))
    {
        return false;
    }

    if (value.indexOf("..") != -1 || value.indexOf('\\') != -1)
    {
        return false;
    }

    return true;
}

String ScriptStorage::normalizeAbsolutePath(const char *path) const
{
    if (!isValidFilePath(path))
    {
        return String();
    }

    String value(path);
    while (value.endsWith("/") && value.length() > 1)
    {
        value.remove(value.length() - 1, 1);
    }
    return value;
}

bool ScriptStorage::isValidScriptName(const char *name) const
{
    if (name == nullptr || name[0] == '\0')
    {
        return false;
    }

    String value(name);
    if (value.indexOf('/') != -1 || value.indexOf('\\') != -1)
    {
        return false;
    }

    if (value.indexOf("..") != -1)
    {
        return false;
    }

    return true;
}
