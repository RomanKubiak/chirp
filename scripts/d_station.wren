import "json" for Json

// ── Load D-Station MIDI map from flash ────────────────────────────────────────
// Builds a flat CC-number → description lookup from cc.tr808, cc.tr909,
// then maps incoming controller knobs to these NDS parameter CCs.

var ccLookup = {}
var parameterCcs = []

// Auto-learn up to 16 incoming controller CC numbers as knob slots 1..16.
var knobCcToSlot = {}
var learnedKnobCcs = []
var warnedTooManyKnobs = false
var knobCachePath = "/userdata/d_station_knob_cc_cache.json"
var knobCacheLoaded = false

var mapText = File.read("/userdata/d_station_midi_map.json")
if (mapText == null) {
    Log.warn("[d_station] MIDI map not found at /userdata/d_station_midi_map.json")
} else {
    var map = Json.parse(mapText)

    // { kit: { instrument: { param: ccNum } } }
    for (kitEntry in map) {
        var kit  = kitEntry.key
        var instruments = kitEntry.value
        for (instrEntry in instruments) {
            var instr  = instrEntry.key
            var params = instrEntry.value
            for (paramEntry in params) {
                var ccNum = Num.fromString(paramEntry.value)
                if (ccNum != null) {
                    ccLookup[ccNum] = "%(kit)/%(instr): %(paramEntry.key)"
                    parameterCcs.add(ccNum)
                }
            }
        }
    }

    // Sort ascending so channel banks are deterministic and easy to reason about.
    for (i in 1...parameterCcs.count) {
        var key = parameterCcs[i]
        var j = i
        while (j > 0 && parameterCcs[j - 1] > key) {
            parameterCcs[j] = parameterCcs[j - 1]
            j = j - 1
        }
        parameterCcs[j] = key
    }

    Log.info("[d_station] Loaded %(parameterCcs.count) NDS parameter CC mappings")
    Log.info("[d_station] 16x16 map active: input port=1 ch=1..16 -> output port=1 ch=10")
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
        Log.info("[d_station] Restored 16 knob CC mappings from flash cache")
    } else {
        Log.warn("[d_station] Ignoring invalid knob cache at %(knobCachePath)")
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
                Log.warn("[d_station] Ignoring CC %(inCc): already learned 16 knob CC numbers")
                warnedTooManyKnobs = true
            }
            return
        }
        slot = learnedKnobCcs.count
        learnedKnobCcs.add(inCc)
        knobCcToSlot[inCc] = slot
        Log.info("[d_station] Learned knob %(slot + 1) -> incoming CC %(inCc)")

        if (!knobCacheLoaded && learnedKnobCcs.count == 16) {
            var cacheJson = Json.stringify(learnedKnobCcs)
            if (File.write(knobCachePath, cacheJson)) {
                knobCacheLoaded = true
                Log.info("[d_station] Cached 16 knob CC mappings to %(knobCachePath)")
            } else {
                Log.warn("[d_station] Failed to write knob cache to %(knobCachePath)")
            }
        }
    }

    // 16 channels x 16 knobs => 256 slots; wrap through available NDS params.
    var matrixIndex = event.channel * 16 + slot
    var outCc = parameterCcs[matrixIndex % parameterCcs.count]
    var outDesc = ccLookup[outCc]

    Midi.cc(1, 9, outCc, event.value) // MIDI channel 10 is zero-based index 9.

    Log.info("[d_station] in p1 ch=%(event.channelNumber) cc=%(inCc) val=%(event.value) knob=%(slot + 1) -> out p1 ch=10 cc=%(outCc) (%(outDesc))")
})

// ── MCU/HUI listener on MIDI port 1 ──────────────────────────────────────────
Midi.onMcuMessage(Fn.new { |msg|
    if (msg.event.port != 1) return
    Log.info("[d_station] %(msg)")
})

Midi.onHuiMessage(Fn.new { |msg|
    if (msg.event.port != 1) return
    Log.info("[d_station] %(msg)")
})

Script.onUnload {
    Midi.clearListeners()
    Log.info("[d_station] unloaded")
}

Log.info("[d_station] d_station.wren loaded")
