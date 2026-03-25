// Generic clocked arpeggiator script for Chirp.
// Runtime dependencies are provided by /scripts/builtin/_runtime.wren:
// - midi.wren    -> Midi
// - display.wren -> Display
// - script.wren  -> Script
// - debug.wren   -> Log
// - External MIDI clock driven (0xF8)
// - Listens only on MIDI port 2
// - Arpeggiates held notes from lowest to highest
// - Configurable rate/gate via CC
// - Uses display to show ARP configuration and live note state

var cfg = {
    "in_port": 2,
    "out_port": 1,
    "in_channel": 0,
    "out_channel": 0,
    "clock_port": 2,
    "rate_clocks": 6,           // 3=1/32, 6=1/16, 12=1/8, 24=1/4
    "gate_clocks": 3,           // 1..24
    "thru_notes": false,

    // Optional realtime config CCs on in_channel
    "cc_rate": 22,
    "cc_gate": 23,
    "cc_clear": 25
}

var heldNotes = []

var clockCounter = 0
var stepIndex = 0
var running = true
var currentNote = -1
var currentVelocity = 100
var gateRemaining = 0
var rateValues = [3, 6, 12, 24]

var contains = Fn.new { |list, value|
    for (v in list) {
        if (v == value) return true
    }
    return false
}

var removeValue = Fn.new { |list, value|
    for (i in 0...list.count) {
        if (list[i] == value) {
            list.removeAt(i)
            return true
        }
    }
    return false
}

var sortedCopy = Fn.new { |src|
    var out = []
    for (v in src) out.add(v)
    for (i in 1...out.count) {
        var key = out[i]
        var j = i
        while (j > 0 && out[j - 1] > key) {
            out[j] = out[j - 1]
            j = j - 1
        }
        out[j] = key
    }
    return out
}

var rateLabel = Fn.new {
    if (cfg["rate_clocks"] == 3) return "1/32"
    if (cfg["rate_clocks"] == 6) return "1/16"
    if (cfg["rate_clocks"] == 12) return "1/8"
    if (cfg["rate_clocks"] == 24) return "1/4"
    return "CLK"
}

var notePool = Fn.new {
    if (heldNotes.count == 0) return []
    return sortedCopy.call(heldNotes)
}

var heldNotesLabel = Fn.new {
    var pool = notePool.call()
    if (pool.count == 0) return "HELD:-"

    var text = "HELD:"
    for (i in 0...pool.count) {
        if (i > 0) text = text + ","
        text = text + pool[i]
        if (text.count >= 30) break
    }
    return text
}

var resetOutputNote = Fn.new {
    if (currentNote >= 0) {
        Midi.noteOff(cfg["out_port"], cfg["out_channel"], currentNote, 0)
        currentNote = -1
    }
    gateRemaining = 0
}

var renderDisplay = Fn.new { |status|
    var pool = notePool.call()
    var noteShown = currentNote >= 0 ? currentNote : 0

    Display.showInstrument("ARP", "LOW-HI", "P2")
    Display.showKit("R%(rateLabel.call()) G%(cfg["gate_clocks"])")
    Display.showParameter("NOTE", noteShown, currentVelocity)

    Display.showStatus("%(heldNotesLabel.call()) %(status)")
}

var sequenceNoteAt = Fn.new { |pool, step|
    var total = pool.count
    if (total <= 0) return null
    return pool[step % total]
}

var triggerStep = Fn.new {
    var pool = notePool.call()
    if (pool.count == 0) {
        resetOutputNote.call()
        renderDisplay.call("IDLE")
        return
    }

    var note = sequenceNoteAt.call(pool, stepIndex)
    stepIndex = stepIndex + 1

    if (note == null) return

    resetOutputNote.call()
    Midi.noteOn(cfg["out_port"], cfg["out_channel"], note, currentVelocity)
    currentNote = note
    gateRemaining = cfg["gate_clocks"]
    renderDisplay.call("RUN")
}

var applyControlChange = Fn.new { |event|
    var cc = event.controller
    var value = event.value

    if (cc == cfg["cc_rate"]) {
        var idx = ((value * rateValues.count) / 128).floor
        if (idx >= rateValues.count) idx = rateValues.count - 1
        cfg["rate_clocks"] = rateValues[idx]
        clockCounter = 0
        stepIndex = 0
        renderDisplay.call("CFG")
        return
    }

    if (cc == cfg["cc_gate"]) {
        cfg["gate_clocks"] = 1 + ((value * 24) / 128).floor
        if (cfg["gate_clocks"] < 1) cfg["gate_clocks"] = 1
        if (cfg["gate_clocks"] > 24) cfg["gate_clocks"] = 24
        renderDisplay.call("CFG")
        return
    }

    if (cc == cfg["cc_clear"] && value >= 64) {
        heldNotes = []
        clockCounter = 0
        stepIndex = 0
        resetOutputNote.call()
        renderDisplay.call("CLR")
    }
}

Midi.onEvent(Fn.new { |event|
    if (event.port != 2) return

    if (event.type == 0xFA) { // Start
        running = true
        clockCounter = 0
        stepIndex = 0
        renderDisplay.call("START")
        return
    }

    if (event.type == 0xFC) { // Stop
        running = false
        resetOutputNote.call()
        renderDisplay.call("STOP")
        return
    }

    if (event.type == 0xF8) { // Clock
        if (!running) return

        if (gateRemaining > 0) {
            gateRemaining = gateRemaining - 1
            if (gateRemaining <= 0) resetOutputNote.call()
        }

        clockCounter = clockCounter + 1
        if ((clockCounter % cfg["rate_clocks"]) == 0) {
            triggerStep.call()
        }
        return
    }

    if (event.port != cfg["in_port"]) return
    if (event.channel != cfg["in_channel"]) return

    if (event.isControlChange) {
        applyControlChange.call(event)
        return
    }

    if (event.isNoteOn) {
        currentVelocity = event.velocity

        if (!contains.call(heldNotes, event.note)) heldNotes.add(event.note)

        if (cfg["thru_notes"]) {
            Midi.noteOn(cfg["out_port"], cfg["out_channel"], event.note, event.velocity)
        }

        renderDisplay.call("NOTE")
        return
    }

    if (event.isNoteOff) {
        removeValue.call(heldNotes, event.note)

        if (cfg["thru_notes"]) {
            Midi.noteOff(cfg["out_port"], cfg["out_channel"], event.note, event.velocity)
        }

        renderDisplay.call("NOTE")
    }
})

Script.onUnload(Fn.new {
    resetOutputNote.call()
    Midi.clearListeners()
    Display.showStatus("ARP unloaded")
    Log.info("[ARP] unloaded")
})

Script.onFocus(Fn.new {
    Display.showStatus("ARP active")
    Log.info("[ARP] focus gained")
})

renderDisplay.call("READY")
Log.info("[ARP] ARP.wren loaded")
