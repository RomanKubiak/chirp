import "file" for File

foreign class ConfigNative {
    foreign static parse(text)
}

class Config {
    construct init(m) { _m = m }

    static parse(text) { Config.init(ConfigNative.parse(text == null ? "" : text)) }
    static load(path)  { Config.parse(File.read(path)) }

    [key]               { _m[key] }
    [key]=(val)         { _m[key] = val }
    get(key, fallback)  { _m.containsKey(key) ? _m[key] : fallback }
    num(key, fallback)  {
        if (!_m.containsKey(key)) return fallback
        var n = Num.fromString(_m[key])
        return n == null ? fallback : n
    }
    has(key)   { _m.containsKey(key) }
    keys       { _m.keys.toList }
    toMap      { _m }
}