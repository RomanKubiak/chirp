// NDS (D-Station MIDI Map) - Safe version with minimal MIDI
// Maps controller CCs to D-Station parameters

Log.info("[NDS] loading")
Display.showStatus("NDS loading")

// Lifecycle hooks
Script.onUnload(Fn.new {
    Log.info("[NDS] unloaded")
    Display.showStatus("NDS unloaded")
})

Script.onFocus(Fn.new {
    Log.info("[NDS] focus gained")
    Display.showStatus("NDS READY")
})

Log.info("[NDS] initialized successfully")
