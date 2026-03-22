// init.wren — safe for hot-reload

Script.onUnload {
    Log.info("init.wren unloading")
    // Any other cleanup (state reset, etc.)
}

Midi.onNoteOn(Fn.new { |event|
    Log.info("noteOn ch=%(event.channelNumber) note=%(event.note)")
    Midi.noteOn(1, event.channel, event.note, event.velocity)
})

Log.info("init.wren loaded")