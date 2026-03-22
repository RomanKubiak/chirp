#include "wren_runtime_script.h"

const char kEmbeddedWrenRuntime[] = R"WREN(
var debugEnabled = true

class Console {
    static print(text) {
        System.print("%(text)")
    }

    static log(text) {
        System.print("[LOG] %(text)")
    }

    static warn(text) {
        System.print("[WARN] %(text)")
    }
}

class Debug {
    static enable() {
        debugEnabled = true
    }

    static disable() {
        debugEnabled = false
    }

    static log(text) {
        if (debugEnabled) {
            System.print("[DEBUG] %(text)")
        }
    }
}

class Utils {
    static clamp(value, min, max) {
        if (value < min) return min
        if (value > max) return max
        return value
    }

    static lerp(a, b, t) {
        return a + ((b - a) * t)
    }

    static midiByte(value) {
        return clamp(value, 0, 127)
    }
}

Console.log("Embedded runtime ready")
)WREN";
