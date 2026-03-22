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
    return ensureScriptsDir();
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

    if (fs_.exists(path.c_str()))
    {
        fs_.remove(path.c_str());
    }

    File script = fs_.open(path.c_str(), FILE_WRITE);
    if (!script)
    {
        return false;
    }

    size_t written = script.print(source);
    script.flush();
    script.close();
    return written == source.length();
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

    File script = fs_.open(path.c_str(), FILE_READ);
    if (!script)
    {
        return false;
    }

    sourceOut.reserve(script.size() + 1);
    while (script.available())
    {
        sourceOut += static_cast<char>(script.read());
    }

    script.close();
    return true;
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

    if (!fs_.exists(path.c_str()))
    {
        return false;
    }

    return fs_.remove(path.c_str());
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

    return fs_.exists(path.c_str());
}

size_t ScriptStorage::listScripts(String *namesOut, size_t maxNames)
{
    if (!begin())
    {
        return 0;
    }

    File dir = fs_.open(scriptsDir_);
    if (!dir || !dir.isDirectory())
    {
        if (dir)
        {
            dir.close();
        }
        return 0;
    }

    String prefix = String(scriptsDir_) + "/";
    size_t count = 0;

    while (true)
    {
        File entry = dir.openNextFile();
        if (!entry)
        {
            break;
        }

        if (!entry.isDirectory())
        {
            String name = entry.name();
            if (name.startsWith(prefix))
            {
                name.remove(0, prefix.length());
            }

            if (name.endsWith(".wren"))
            {
                name.remove(name.length() - 5, 5); // strip ".wren"
                if (namesOut != nullptr && count < maxNames)
                {
                    namesOut[count] = name;
                }
                ++count;
            }
        }

        entry.close();
    }

    dir.close();
    return count;
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

int32_t ScriptStorage::scriptSize(const char *name)
{
    if (!begin() || !isValidScriptName(name))
        return -1;

    String path = scriptPath(name);
    if (path.length() == 0 || !fs_.exists(path.c_str()))
        return -1;

    File f = fs_.open(path.c_str(), FILE_READ);
    if (!f)
        return -1;

    int32_t sz = static_cast<int32_t>(f.size());
    f.close();
    return sz;
}

bool ScriptStorage::ensureScriptsDir()
{
    if (fs_.exists(scriptsDir_))
    {
        File dir = fs_.open(scriptsDir_);
        bool isDir = dir && dir.isDirectory();
        if (dir)
        {
            dir.close();
        }
        return isDir;
    }

    return fs_.mkdir(scriptsDir_);
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
