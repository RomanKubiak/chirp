foreign class FileNative {
    foreign static read(path)
    foreign static write(path, content)
    foreign static remove(path)
    foreign static exists(path)
    foreign static size(path)
    foreign static list(path)
}

class File {
    static read(path)          { FileNative.read(path) }
    static write(path, content){ FileNative.write(path, "%(content)") }
    static remove(path)        { FileNative.remove(path) }
    static exists(path)        { FileNative.exists(path) }
    static size(path)          { FileNative.size(path) }
    static list(path)          { FileNative.list(path) }
}