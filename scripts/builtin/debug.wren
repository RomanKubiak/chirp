foreign class DebugNative {
    foreign static debug(message)
    foreign static info(message)
    foreign static warn(message)
    foreign static error(message)
}

class Log {
    static debug(message) {
        return DebugNative.debug("%(message)")
    }

    static info(message) {
        return DebugNative.info("%(message)")
    }

    static warn(message) {
        return DebugNative.warn("%(message)")
    }

    static error(message) {
        return DebugNative.error("%(message)")
    }
}