import "script" for Script

foreign class DisplayNative {
    foreign static cols()
    foreign static rows()
    foreign static setPalette(foreground, background)
    foreign static clear()
    foreign static drawBorder()
    foreign static setLine(row, col, text, pad, bold, inverted)
    foreign static setCenteredLine(row, text, bold, inverted)
    foreign static setCell(row, col, ch, bold, inverted)
    foreign static render()
}

class Display {
    static cols() { DisplayNative.cols() }
    static rows() { DisplayNative.rows() }
    static setPalette(foreground, background) { DisplayNative.setPalette(foreground, background) }
    static clear() { DisplayNative.clear() }
    static drawBorder() { DisplayNative.drawBorder() }
    static blank() { Display.blank(Script.loadingName()) }
    static blank(title) {
        Display.clear()
        Display.drawBorder()
        Display.setCenteredLine(Display.rows() / 2, title, true, false)
        Display.render()
    }
    static setLine(row, col, text, pad, bold, inverted) {
        DisplayNative.setLine(row, col, text, pad, bold, inverted)
    }
    static setCenteredLine(row, text, bold, inverted) {
        DisplayNative.setCenteredLine(row, text, bold, inverted)
    }
    static setCell(row, col, ch, bold, inverted) {
        DisplayNative.setCell(row, col, ch, bold, inverted)
    }
    static render() { DisplayNative.render() }
}
