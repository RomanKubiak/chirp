import "clock" for Clock
import "script" for Script, ScriptNative

foreign class MidiNative {
    foreign static noteOffType()
    foreign static noteOnType()
    foreign static controlChangeType()
    foreign static programChangeType()
    foreign static channelPressureType()
    foreign static pitchBendType()
    foreign static portName(port)
    foreign static typeName(type)
    foreign static send(port, type, channel, data1, data2)
}

class MidiEvent {
    construct new(port, type, channel, data1, data2, ticksMs, ticksUs) {
        _port = port
        _type = type
        _channel = channel
        _data1 = data1
        _data2 = data2
        _ticksMs = ticksMs
        _ticksUs = ticksUs
    }

    port { _port }
    type { _type }
    channel { _channel }
    channelNumber { _channel + 1 }
    data1 { _data1 }
    data2 { _data2 }
    ticksMs { _ticksMs }
    ticksUs { _ticksUs }
    note { _data1 }
    velocity { _data2 }
    controller { _data1 }
    value { _data2 }
    program { _data1 }
    portName { Midi.portName(_port) }
    typeName { Midi.typeName(_type) }
    isNoteOn { _type == Midi.noteOnType && _data2 > 0 }
    isNoteOff { _type == Midi.noteOffType || (_type == Midi.noteOnType && _data2 == 0) }
    isControlChange { _type == Midi.controlChangeType }
    isProgramChange { _type == Midi.programChangeType }
    isChannelPressure { _type == Midi.channelPressureType }
    isPitchBend { _type == Midi.pitchBendType }
}

class McuMessage {
    construct new(event, name, state, detail) {
        _event  = event
        _name   = name
        _state  = state
        _detail = detail
    }
    event    { _event }
    name     { _name }
    state    { _state }
    detail   { _detail }
    portName { _event.portName }
    toString {
        var s = "mcu:%(portName) %(_name)"
        if (_state  != null) s = s + " %(_state)"
        if (_detail != null) s = s + " (%(_detail))"
        return s
    }
}

class HuiMessage {
    construct new(event, name, state) {
        _event = event
        _name  = name
        _state = state
    }
    event    { _event }
    name     { _name }
    state    { _state }
    portName { _event.portName }
    toString { "hui:%(portName) %(_name) %(_state)" }
}

class McuDecoder {
    static decode(event) {
        if (event.isNoteOn || event.isNoteOff) {
            var n = McuDecoder.noteName(event.note)
            if (n == null) return null
            return McuMessage.new(event, n, event.isNoteOn ? "press" : "release", null)
        }
        if (event.isControlChange) {
            var cc = event.controller
            if (cc >= 16 && cc <= 23) {
                var v = event.value
                return McuMessage.new(event, "vpot_%(cc - 15)",
                    (v & 0x40) != 0 ? "ccw" : "cw", "%(v & 0x3F)")
            }
            if (cc == 60) {
                var v = event.value
                return McuMessage.new(event, "jog",
                    (v & 0x40) != 0 ? "ccw" : "cw", "%(v & 0x3F)")
            }
        }
        if (event.isPitchBend) {
            var strip = event.channel < 8 ? "ch%(event.channel + 1)" : "master"
            var raw   = event.data1 | (event.data2 << 7)
            return McuMessage.new(event, "fader_%(strip)", null, "%(raw)")
        }
        return null
    }

    static noteName(n) {
        if (n <  8)  return "rec_rdy_%(n + 1)"
        if (n < 16)  return "solo_%(n - 7)"
        if (n < 24)  return "mute_%(n - 15)"
        if (n < 32)  return "select_%(n - 23)"
        if (n < 40)  return "vpot_sel_%(n - 31)"
        if (n >= 54 && n <= 61)   return "f%(n - 53)"
        if (n >= 104 && n <= 111) return "fader_touch_%(n - 103)"
        if (n == 40)  return "assign_track"
        if (n == 41)  return "assign_send"
        if (n == 42)  return "assign_pan"
        if (n == 43)  return "assign_plugin"
        if (n == 44)  return "assign_eq"
        if (n == 45)  return "assign_instr"
        if (n == 46)  return "bank_left"
        if (n == 47)  return "bank_right"
        if (n == 48)  return "ch_bank_left"
        if (n == 49)  return "ch_bank_right"
        if (n == 50)  return "flip"
        if (n == 51)  return "global_view"
        if (n == 52)  return "name_value"
        if (n == 53)  return "smpte_beats"
        if (n == 70)  return "shift"
        if (n == 71)  return "option"
        if (n == 72)  return "ctrl"
        if (n == 73)  return "alt"
        if (n == 74)  return "auto_read"
        if (n == 75)  return "auto_write"
        if (n == 76)  return "trim"
        if (n == 77)  return "auto_touch"
        if (n == 78)  return "auto_latch"
        if (n == 79)  return "group"
        if (n == 80)  return "save"
        if (n == 81)  return "undo"
        if (n == 82)  return "cancel"
        if (n == 83)  return "enter"
        if (n == 84)  return "marker"
        if (n == 85)  return "nudge"
        if (n == 86)  return "cycle"
        if (n == 87)  return "drop"
        if (n == 88)  return "replace"
        if (n == 89)  return "click"
        if (n == 90)  return "solo_mode"
        if (n == 91)  return "rewind"
        if (n == 92)  return "ffwd"
        if (n == 93)  return "stop"
        if (n == 94)  return "play"
        if (n == 95)  return "record"
        if (n == 96)  return "cursor_up"
        if (n == 97)  return "cursor_down"
        if (n == 98)  return "cursor_left"
        if (n == 99)  return "cursor_right"
        if (n == 100) return "zoom"
        if (n == 101) return "scrub"
        if (n == 102) return "user_a"
        if (n == 103) return "user_b"
        if (n == 112) return "fader_touch_master"
        return null
    }
}

class HuiDecoder {
    static decode(event) {
        if (!event.isControlChange || event.channel != 0) return null
        if (event.controller == 0) {
            __zone = event.value
            return null
        }
        if (event.controller == 32 && __zone != null) {
            var zone    = __zone
            __zone      = null
            var port    = event.value & 0x0F
            var pressed = (event.value & 0x40) != 0
            return HuiDecoder.describe(event, zone, port, pressed)
        }
        return null
    }

    static describe(event, zone, port, pressed) {
        var st = pressed ? "press" : "release"
        if (zone < 8) {
            var p = ["fader_lo","select","mute","solo","auto","vpot_sel","insert","rec_rdy"]
            var pname = port < p.count ? p[port] : "port_" + port.toString
            return HuiMessage.new(event, "ch%(zone + 1)_%(pname)", st)
        }
        if (zone == 12) {
            var t = ["roto_lo","roto_hi","roto_led","","","stop","play","ffwd","rewind","record","return"]
            var tname = (port < t.count && t[port] != "") ? t[port] : "port_" + port.toString
            return HuiMessage.new(event, "transport_%(tname)", st)
        }
        if (zone == 13) {
            var e = ["save","undo","","","edit_mode","cut","copy","paste","delete","insert","numpad","numpad_dot"]
            var ename = (port < e.count && e[port] != "") ? e[port] : "port_" + port.toString
            return HuiMessage.new(event, "edit_%(ename)", st)
        }
        return HuiMessage.new(event, "zone_%(zone)_port_%(port)", st)
    }
}

class MidiApi {
    construct new() {
        _eventListeners = []
        _noteOnListeners = []
        _noteOffListeners = []
        _controlChangeListeners = []
        _programChangeListeners = []
        _mcuListeners = []
        _huiListeners = []
    }

    listen(callback) {
        var wrapped = Script.wrapListener(callback)
        _eventListeners.add(wrapped)
        return wrapped
    }

    onEvent(callback) {
        return listen(callback)
    }

    onNoteOn(callback) {
        var wrapped = Script.wrapListener(callback)
        _noteOnListeners.add(wrapped)
        return wrapped
    }

    onNoteOff(callback) {
        var wrapped = Script.wrapListener(callback)
        _noteOffListeners.add(wrapped)
        return wrapped
    }

    onControlChange(callback) {
        var wrapped = Script.wrapListener(callback)
        _controlChangeListeners.add(wrapped)
        return wrapped
    }

    onProgramChange(callback) {
        var wrapped = Script.wrapListener(callback)
        _programChangeListeners.add(wrapped)
        return wrapped
    }

    onMcuMessage(callback) {
        var wrapped = Script.wrapListener(callback)
        _mcuListeners.add(wrapped)
        return wrapped
    }

    onHuiMessage(callback) {
        var wrapped = Script.wrapListener(callback)
        _huiListeners.add(wrapped)
        return wrapped
    }

    clearListeners() {
        // Only clear listeners that belong to the currently active script.
        // This prevents clearing listeners from other running scripts.
        var activeScript = ScriptNative.loadingName()
        _eventListeners = _eventListeners.where { |l| l["owner"] != activeScript }.toList
        _noteOnListeners = _noteOnListeners.where { |l| l["owner"] != activeScript }.toList
        _noteOffListeners = _noteOffListeners.where { |l| l["owner"] != activeScript }.toList
        _controlChangeListeners = _controlChangeListeners.where { |l| l["owner"] != activeScript }.toList
        _programChangeListeners = _programChangeListeners.where { |l| l["owner"] != activeScript }.toList
        _mcuListeners = _mcuListeners.where { |l| l["owner"] != activeScript }.toList
        _huiListeners = _huiListeners.where { |l| l["owner"] != activeScript }.toList
    }

    noteOffType { MidiNative.noteOffType() }
    noteOnType { MidiNative.noteOnType() }
    controlChangeType { MidiNative.controlChangeType() }
    programChangeType { MidiNative.programChangeType() }
    channelPressureType { MidiNative.channelPressureType() }
    pitchBendType { MidiNative.pitchBendType() }

    portName(port) {
        return MidiNative.portName(port)
    }

    typeName(type) {
        return MidiNative.typeName(type)
    }

    send(port, type, channel, data1, data2) {
        return MidiNative.send(port, type, channel, data1, data2)
    }

    noteOn(port, channel, note, velocity) {
        return send(port, noteOnType, channel, note, velocity)
    }

    noteOff(port, channel, note, velocity) {
        return send(port, noteOffType, channel, note, velocity)
    }

    controlChange(port, channel, controller, value) {
        return send(port, controlChangeType, channel, controller, value)
    }

    cc(port, channel, controller, value) {
        return controlChange(port, channel, controller, value)
    }

    programChange(port, channel, program) {
        return send(port, programChangeType, channel, program, 0)
    }

    channelPressure(port, channel, pressure) {
        return send(port, channelPressureType, channel, pressure, 0)
    }

    pitchBend(port, channel, value) {
        var clamped = value
        if (clamped < 0) clamped = 0
        if (clamped > 16383) clamped = 16383
        var lsb = clamped & 0x7F
        var msb = (clamped >> 7) & 0x7F
        return send(port, pitchBendType, channel, lsb, msb)
    }

    dispatchFromNative(port, type, channel, data1, data2) {
        var event = MidiEvent.new(port, type, channel, data1, data2, Clock.ticksMs(), Clock.ticksUs())
        emitListeners(_eventListeners, event)
        if (event.isNoteOn) emitListeners(_noteOnListeners, event)
        if (event.isNoteOff) emitListeners(_noteOffListeners, event)
        if (event.isControlChange) emitListeners(_controlChangeListeners, event)
        if (event.isProgramChange) emitListeners(_programChangeListeners, event)
        if (_mcuListeners.count > 0) {
            var m = McuDecoder.decode(event)
            if (m != null) {
                emitListeners(_mcuListeners, m)
            }
        }
        if (_huiListeners.count > 0) {
            var h = HuiDecoder.decode(event)
            if (h != null) emitListeners(_huiListeners, h)
        }
    }

    emitListeners(listeners, event) {
        for (listener in listeners) {
            var owner = listener["owner"]
            var fn = listener["fn"]
            ScriptNative.enterContext(owner)
            fn.call(event)
            ScriptNative.leaveContext()
        }
    }
}