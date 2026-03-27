import "clock" for Clock
import "debug" for Log
import "file" for File
import "config" for Config
import "script" for Script
import "display" for Display
import "midi" for MidiApi, MidiEvent, McuMessage, HuiMessage, McuDecoder, HuiDecoder

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
            Log.debug(text)
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

var Midi = MidiApi.new()

Console.log("Flash runtime ready")