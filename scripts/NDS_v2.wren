// NDS (D-Station MIDI Map) - headless CC mapping + MIDI logging

var mapPath = "/userdata/NDS_midi_map.ini"
var instrumentOrder = [
    "tr808.BD", "tr808.SD", "tr808.LT", "tr808.MT", "tr808.HT", "tr808.RS",
    "tr808.HC", "tr808.CB", "tr808.CH", "tr808.OH", "tr808.CC", "tr808.Lc",
    "tr808.Mc", "tr808.Cc", "tr808.MA", "tr808.CL",
    "tr909.BD", "tr909.SD", "tr909.LT", "tr909.MT", "tr909.HT", "tr909.RS",
    "tr909.HC", "tr909.CH", "tr909.OH", "tr909.CY", "tr909.RC"
]
var instrumentShortByKey = {}
var instrumentDisplayByKey = {}
var instrumentIndexByKey = {}
var ccLookup = {}
var parameterCcs = []
var learnedKnobCcs = []
var knobCcToSlot = {}
var warnedTooManyKnobs = false
var currentStatus = "loading map"
var currentInstrumentLine = "instrument: -"
var currentParameterLine = "parameter: -"
var currentValueLine = "VAL - [              ]"
var currentEventLine = "waiting for MIDI"
var currentRouteLine = "no mapped output yet"
var currentInstrumentIndex = 0

var makeValueBar = Fn.new { |value, width|
    var clamped = value
    if (clamped < 0) clamped = 0
    if (clamped > 127) clamped = 127

    var filled = ((clamped * width) / 127).floor
    if (filled < 0) filled = 0
    if (filled > width) filled = width

    return ("#" * filled) + (" " * (width - filled))
}

var caseAdjustLabel = Fn.new { |text, uppercase|
    var out = ""
    for (i in 0...text.count) {
        var ch = text.byteAt(i)
        if (uppercase) {
            if (ch >= 97 && ch <= 122) ch = ch - 32
        } else {
            if (ch >= 65 && ch <= 90) ch = ch + 32
        }
        out = out + String.fromByte(ch)
    }
    return out
}

var buildUpperMapTokens = Fn.new { |focusIndex|
    if (instrumentOrder.count == 0) return ["NO", "INSTRUMENTS", false, false, false, false, false]

    var total = instrumentOrder.count
    var labels = []

    for (offset in [-3, -2, -1, 0, 1, 2, 3]) {
        var index = focusIndex + offset
        while (index < 0) index = index + total
        while (index >= total) index = index - total

        var key = instrumentOrder[index]
        var label = instrumentDisplayByKey[key]
        if (offset == 0) {
            label = "[%(label)]"
        }
        labels.add(label)
    }

    return labels
}

var drawUpperMap = Fn.new { |focusIndex|
    DisplayNative.setLine(1, 1, "                        ", true, false, false)

    var labels = buildUpperMapTokens.call(focusIndex)

    var totalWidth = 0
    for (i in 0...labels.count) {
        totalWidth = totalWidth + labels[i].count
        if (i > 0) totalWidth = totalWidth + 1
    }

    var usableWidth = DisplayNative.cols() - 2
    var startCol = 1 + ((usableWidth - totalWidth) / 2).floor
    if (startCol < 1) startCol = 1

    var col = startCol
    for (i in 0...labels.count) {
        if (i > 0) {
            DisplayNative.setCell(1, col, " ", false, false)
            col = col + 1
        }

        var label = labels[i]
        for (j in 0...label.count) {
            DisplayNative.setCell(1, col, label[j], false, false)
            col = col + 1
        }
    }
}

var refreshUpperMap = Fn.new {
    drawUpperMap.call(currentInstrumentIndex)
}

var drawStatus = Fn.new {
    DisplayNative.clear()
    DisplayNative.drawBorder()
    drawUpperMap.call(currentInstrumentIndex)
    DisplayNative.setCenteredLine(3, currentStatus, true, false)
    DisplayNative.setCenteredLine(5, currentInstrumentLine, false, false)
    DisplayNative.setCenteredLine(6, currentParameterLine, false, false)
    DisplayNative.setCenteredLine(7, currentValueLine, false, false)
    DisplayNative.setLine(9, 2, "MAP %(parameterCcs.count) CC", true, false, false)
    DisplayNative.setLine(10, 2, "KNOBS %(learnedKnobCcs.count)/16", true, false, false)
    DisplayNative.setLine(11, 2, currentEventLine, true, false, false)
    DisplayNative.setLine(12, 2, currentRouteLine, true, false, false)
    DisplayNative.render()
}

for (key in instrumentOrder) {
    var parts = key.split(".")
    var family = parts[0] == "tr808" ? "808" : "909"
    instrumentShortByKey[key] = parts[1]
    var label = caseAdjustLabel.call(parts[1], parts[0] == "tr808")
    instrumentDisplayByKey[key] = "%(family):%(label)"
}

for (i in 0...instrumentOrder.count) {
    instrumentIndexByKey[instrumentOrder[i]] = i
}

currentInstrumentLine = "INST %(instrumentDisplayByKey[instrumentOrder[currentInstrumentIndex]])"
currentParameterLine = "PARAM -"
currentRouteLine = "ROUTE %(instrumentDisplayByKey[instrumentOrder[currentInstrumentIndex]]) -"

var updateInstrumentState = Fn.new { |outDesc|
    var parts = outDesc.split(".")
    if (parts.count >= 3) {
        var instrumentKey = "%(parts[0]).%(parts[1])"
        var parameterName = parts[parts.count - 1]
        var instrumentIndex = instrumentIndexByKey[instrumentKey]

        if (instrumentIndex != null) {
            currentInstrumentIndex = instrumentIndex
        }

        currentInstrumentLine = "INST %(instrumentDisplayByKey[instrumentKey])"
        currentParameterLine = "PARAM %(parameterName)"
        currentRouteLine = "ROUTE %(instrumentDisplayByKey[instrumentKey]) %(parameterName)"
        refreshUpperMap.call()
    }
}

var updateValueLine = Fn.new { |value|
    currentValueLine = "VAL %(value) [%(makeValueBar.call(value, 14))]"
}

Log.info("[NDS] loading")
refreshUpperMap.call()
drawStatus.call()

var mapText = File.read(mapPath)
if (mapText == null) {
    Log.warn("[NDS] MIDI map not found at %(mapPath)")
} else {
    var map = Config.parse(mapText).toMap

    // Build a flat CC-number -> description lookup from the INI sections.
    for (entry in map) {
        var ccNum = Num.fromString(entry.value)
        if (ccNum == null) continue
        ccLookup[ccNum] = entry.key
        parameterCcs.add(ccNum)
    }

    // Keep output ordering deterministic and easy to inspect.
    for (i in 1...parameterCcs.count) {
        var key = parameterCcs[i]
        var j = i
        while (j > 0 && parameterCcs[j - 1] > key) {
            parameterCcs[j] = parameterCcs[j - 1]
            j = j - 1
        }
        parameterCcs[j] = key
    }

    Log.info("[NDS] map loaded %(parameterCcs.count) CC mappings")
    currentStatus = "cc map ready"
    refreshUpperMap.call()
    drawStatus.call()
}

Midi.onControlChange(Fn.new { |event|
    if (event.port != 1) return
    if (parameterCcs.count == 0) return

    var inCc = event.controller
    var slot = knobCcToSlot[inCc]
    if (slot == null) {
        if (learnedKnobCcs.count >= 16) {
            if (!warnedTooManyKnobs) {
                Log.warn("[NDS] ignoring CC %(inCc): already learned 16 knob CC numbers")
                warnedTooManyKnobs = true
            }
            return
        }

        slot = learnedKnobCcs.count
        learnedKnobCcs.add(inCc)
        knobCcToSlot[inCc] = slot
        Log.info("[NDS] learned knob %(slot + 1) -> incoming CC %(inCc)")
    }

    var matrixIndex = event.channel * 16 + slot
    var outCc = parameterCcs[matrixIndex % parameterCcs.count]
    var outDesc = ccLookup[outCc]

    Midi.cc(1, 9, outCc, event.value)

    updateInstrumentState.call(outDesc)
    updateValueLine.call(event.value)
    currentEventLine = "CC ch%(event.channelNumber) cc%(inCc) v%(event.value) k%(slot + 1)"
    drawStatus.call()

    Log.info("[NDS] in p%(event.port + 1) ch=%(event.channelNumber) cc=%(inCc) val=%(event.value) knob=%(slot + 1) -> out p1 ch=10 cc=%(outCc) (%(outDesc))")
})

Midi.onEvent(Fn.new { |event|
    if (event.isNoteOn) {
        Log.info("[NDS] note on %(event.note) vel=%(event.velocity) ch=%(event.channel)")
        currentEventLine = "ON ch%(event.channelNumber) n%(event.note) v%(event.velocity)"
        drawStatus.call()
        return
    }

    if (event.isNoteOff) {
        Log.info("[NDS] note off %(event.note) vel=%(event.velocity) ch=%(event.channel)")
        currentEventLine = "OFF ch%(event.channelNumber) n%(event.note) v%(event.velocity)"
        drawStatus.call()
        return
    }

    if (event.isControlChange) return

    Log.info("[NDS] midi type=%(event.type) port=%(event.port) ch=%(event.channel)")
})

Log.info("[NDS] loaded")

// Lifecycle hooks
Script.onUnload(Fn.new {
    Midi.clearListeners()
    Log.info("[NDS] unloaded")
})

Script.onFocus(Fn.new {
    Log.info("[NDS] focus gained")
    refreshUpperMap.call()
    drawStatus.call()
})

Log.info("[NDS] initialized successfully")
