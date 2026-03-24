import "script" for Script

foreign class DisplayNative {
    foreign static showInstrument(prev, current, next)
    foreign static showKit(kitName)
    foreign static showParameter(paramName, cc, value)
    foreign static showValue(value)
    foreign static showStatus(statusText)
}

class Display {
    static showInstrument(prev, current, next) {
        if (!Script.canDraw) return
        DisplayNative.showInstrument(prev, current, next)
    }

    static showKit(kitName) {
        if (!Script.canDraw) return
        DisplayNative.showKit(kitName)
    }

    static showParameter(paramName, cc, value) {
        if (!Script.canDraw) return
        DisplayNative.showParameter(paramName, cc, value)
    }

    static showValue(value) {
        if (!Script.canDraw) return
        DisplayNative.showValue(value)
    }

    static showStatus(statusText) {
        if (!Script.canDraw) return
        DisplayNative.showStatus(statusText)
    }
}