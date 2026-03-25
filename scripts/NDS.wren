// Runtime dependencies are provided by /scripts/builtin/_runtime.wren:
// - midi.wren    -> Midi
// - display.wren -> Display
// - script.wren  -> Script
// - debug.wren   -> Log
// - file.wren    -> File
// Direct module dependency:
// - json.wren    -> Json
import "json" for Json

// ── Load D-Station MIDI map from flash ────────────────────────────────────────
// Builds a flat CC-number → description lookup from cc.tr808, cc.tr909,
// then maps incoming controller knobs to these NDS parameter CCs.

var ccLookup = {}
var parameterCcs = []
var parameterEntries = []

// Auto-learn up to 16 incoming controller CC numbers as knob slots 1..16.
var knobCcToSlot = {}
var learnedKnobCcs = []
var warnedTooManyKnobs = false
var knobCachePath = "/userdata/NDS_knob_cc_cache.json"
var knobCacheLoaded = false

var currentMatrixIndex = null
var lastDisplayedValue = 0  // tracks last CC value shown, used by onFocus redraw

// Keep incoming name as-is; instrument display names are sourced from JSON full_name.
var formatName = Fn.new { |name|
    if (name == null) return "--"
    return name
}

// "tr808" -> "TR808", "tr909" -> "TR909"
var formatKit = Fn.new { |kit|
    if (kit == "tr808") return "TR808"
    if (kit == "tr909") return "TR909"
    return kit == null ? "--" : kit
}

var shortInstrumentName = Fn.new { |name|
    if (name == null) return "--"
    var full = formatName.call(name)
    if (full.count <= 8) return full
    return full[0...8]
}

var slotLabel = Fn.new { |matrixIndex|
    if (matrixIndex == null) return "--"
    var slot = (matrixIndex % 16) + 1
    var channel = ((matrixIndex / 16).floor) + 1
    return "K%(slot) C%(channel)"
}

var parameterAtMatrix = Fn.new { |matrixIndex|
    if (matrixIndex == null || parameterEntries.count == 0) return null

    var wrapped = matrixIndex % parameterEntries.count
    if (wrapped < 0) wrapped = wrapped + parameterEntries.count
    return parameterEntries[wrapped]
}

var refreshDisplay = Fn.new { |matrixIndex, value|
    if (parameterEntries.count == 0) return

    currentMatrixIndex = matrixIndex
    lastDisplayedValue = value

    var prevEntry = parameterAtMatrix.call(matrixIndex - 1)
    var currEntry = parameterAtMatrix.call(matrixIndex)
    var nextEntry = parameterAtMatrix.call(matrixIndex + 1)
    var prevInstrument = shortInstrumentName.call(prevEntry["instrument_full_name"])
    var currInstrument = shortInstrumentName.call(currEntry["instrument_full_name"])
    var nextInstrument = shortInstrumentName.call(nextEntry["instrument_full_name"])
    var prevSlot = slotLabel.call(matrixIndex - 1)
    var currSlot = slotLabel.call(matrixIndex)
    var nextSlot = slotLabel.call(matrixIndex + 1)

    Display.showInstrument(
        prevInstrument,
        currInstrument,
        nextInstrument
    )

    Display.showKit(formatKit.call(currEntry["kit"]))
    Display.showParameter(currEntry["parameter"], currEntry["cc"], value)
    Display.showStatus("%(prevSlot) < %(currSlot) > %(nextSlot)")
}

var mapText = File.read("/userdata/NDS_midi_map.json")
if (mapText == null) {
    Log.warn("[NDS] MIDI map not found at /userdata/NDS_midi_map.json")
    Display.showStatus("MIDI map missing")
} else {
    var map = Json.parse(mapText)

    // { kit: { instrument: { param: ccNum } } }
    for (kitEntry in map) {
        var kit  = kitEntry.key
        var instruments = kitEntry.value
        for (instrEntry in instruments) {
            var instr  = instrEntry.key
            var params = instrEntry.value
            var instrumentFullName = params["full_name"]
            if (instrumentFullName == null) instrumentFullName = formatName.call(instr)
            for (paramEntry in params) {
                if (paramEntry.key == "full_name") continue
                var ccNum = Num.fromString(paramEntry.value)
                if (ccNum != null) {
                    ccLookup[ccNum] = "%(kit)/%(instr): %(paramEntry.key)"
                    parameterCcs.add(ccNum)
                    parameterEntries.add({
                        "kit": kit,
                        "instrument": instr,
                        "instrument_full_name": instrumentFullName,
                        "parameter": paramEntry.key,
                        "cc": ccNum
                    })
                }
            }
        }
    }

    // Sort ascending so channel banks are deterministic and easy to reason about.
    for (i in 1...parameterCcs.count) {
        var key = parameterCcs[i]
        var entry = parameterEntries[i]
        var j = i
        while (j > 0 && parameterCcs[j - 1] > key) {
            parameterCcs[j] = parameterCcs[j - 1]
            parameterEntries[j] = parameterEntries[j - 1]
            j = j - 1
        }
        parameterCcs[j] = key
        parameterEntries[j] = entry
    }

    Log.info("[NDS] Loaded %(parameterCcs.count) NDS parameter CC mappings")
    Log.info("[NDS] 16x16 map active: input port=1 ch=1..16 -> output port=1 ch=10")
    var firstEntry = parameterEntries[0]
    var secondEntry = parameterAtMatrix.call(1)
    Display.showInstrument("--", shortInstrumentName.call(firstEntry["instrument_full_name"]), shortInstrumentName.call(secondEntry["instrument_full_name"]))
    Display.showKit(formatKit.call(firstEntry["kit"]))
    Display.showParameter(firstEntry["parameter"], firstEntry["cc"], 0)
    Display.showStatus("Awaiting CC input")
}

// Restore cached knob CC assignments if available.
var cacheText = File.read(knobCachePath)
if (cacheText != null) {
    var cache = Json.parse(cacheText)
    var valid = (cache is List && cache.count == 16)

    if (valid) {
        var seen = {}
        for (cc in cache) {
            if (!(cc is Num) || cc < 0 || cc > 127 || seen[cc] != null) {
                valid = false
                break
            }
            seen[cc] = true
        }
    }

    if (valid) {
        for (i in 0...cache.count) {
            var cc = cache[i]
            learnedKnobCcs.add(cc)
            knobCcToSlot[cc] = i
        }
        knobCacheLoaded = true
        Log.info("[NDS] Restored 16 knob CC mappings from flash cache")
    } else {
        Log.warn("[NDS] Ignoring invalid knob cache at %(knobCachePath)")
    }
}

// ── CC listener: 16 knobs x 16 MIDI channels -> NDS CC on ch10 ───────────────
Midi.onControlChange(Fn.new { |event|
    if (event.port != 1) return
    if (parameterCcs.count == 0) return

    var inCc = event.controller
    var slot = knobCcToSlot[inCc]
    if (slot == null) {
        if (learnedKnobCcs.count >= 16) {
            if (!warnedTooManyKnobs) {
                Log.warn("[NDS] Ignoring CC %(inCc): already learned 16 knob CC numbers")
                warnedTooManyKnobs = true
            }
            return
        }
        slot = learnedKnobCcs.count
        learnedKnobCcs.add(inCc)
        knobCcToSlot[inCc] = slot
        Log.info("[NDS] Learned knob %(slot + 1) -> incoming CC %(inCc)")

        if (!knobCacheLoaded && learnedKnobCcs.count == 16) {
            var cacheJson = Json.stringify(learnedKnobCcs)
            if (File.write(knobCachePath, cacheJson)) {
                knobCacheLoaded = true
                Log.info("[NDS] Cached 16 knob CC mappings to %(knobCachePath)")
            } else {
                Log.warn("[NDS] Failed to write knob cache to %(knobCachePath)")
            }
        }
    }

    // 16 channels x 16 knobs => 256 slots; wrap through available NDS params.
    var matrixIndex = event.channel * 16 + slot
    var outCc = parameterCcs[matrixIndex % parameterCcs.count]
    var outMeta = parameterAtMatrix.call(matrixIndex)
    var outInstrument = outMeta["instrument_full_name"]
    var outParameter = outMeta["parameter"]

    Midi.cc(1, 9, outCc, event.value) // MIDI channel 10 is zero-based index 9.

    refreshDisplay.call(matrixIndex, event.value)

    Log.info("[NDS] in p1 ch=%(event.channelNumber) cc=%(inCc) val=%(event.value) knob=%(slot + 1) -> out p1 ch=10 cc=%(outCc) (%(outInstrument): %(outParameter))")
})

// ── MCU/HUI listener on MIDI port 1 ──────────────────────────────────────────
Midi.onMcuMessage(Fn.new { |msg|
    if (msg.event.port != 1) return
    Log.info("[NDS] %(msg)")
})

Midi.onHuiMessage(Fn.new { |msg|
    if (msg.event.port != 1) return
    Log.info("[NDS] %(msg)")
})

Script.onUnload(Fn.new {
    Midi.clearListeners()
    Display.showStatus("NDS unloaded")
    Log.info("[NDS] unloaded")
})

// Redraw the current display state whenever focus returns to this script.
Script.onFocus(Fn.new {
    if (parameterEntries.count == 0) {
        Display.showStatus("NDS: no map loaded")
        return
    }
    var idx = currentMatrixIndex
    if (idx == null) idx = 0
    refreshDisplay.call(idx, lastDisplayedValue)
})

Display.showStatus("NDS loading")
Log.info("[NDS] NDS.wren loaded")
