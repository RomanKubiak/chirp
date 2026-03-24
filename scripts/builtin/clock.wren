foreign class TeensyClockNative {
    foreign static ticksMs()
    foreign static ticksUs()
}

class Clock {
    static ticksMs() {
        return TeensyClockNative.ticksMs()
    }

    static ticksUs() {
        return TeensyClockNative.ticksUs()
    }

    static prefix() {
        return "[t=%(Clock.ticksMs())ms/%(Clock.ticksUs())us]"
    }
}