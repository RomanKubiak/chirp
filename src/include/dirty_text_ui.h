#ifndef DIRTY_TEXT_UI_H
#define DIRTY_TEXT_UI_H

#include <Arduino.h>
#include <ST7735_t3.h>

class DirtyTextUi
{
public:
    static constexpr uint8_t kCellWidth = 6;
    static constexpr uint8_t kCellHeight = 8;
    static constexpr uint8_t kCols = 26;
    static constexpr uint8_t kRows = 15;

    void begin(ST7735_t3 *display);
    void clear();
    void setPalette(uint16_t foreground, uint16_t background);
    void drawBorder();
    void setLine(uint8_t row, uint8_t col, const char *text, bool pad = true, bool bold = false, bool inverted = false);
    void setCenteredLine(uint8_t row, const char *text, bool bold = false, bool inverted = false);
    void setCell(uint8_t row, uint8_t col, char ch, bool bold = false, bool inverted = false);
    void render();

private:
    static constexpr uint8_t kTextCols = kCols - 1;

    ST7735_t3 *display_ = nullptr;
    char cells_[kRows][kCols];
    bool dirty_[kRows][kCols];
    bool bold_[kRows][kCols];
    bool inverted_[kRows][kCols];
    uint16_t foregroundColor_ = ST7735_WHITE;
    uint16_t backgroundColor_ = ST7735_BLACK;

    void writeText(uint8_t row, uint8_t col, const char *text, bool pad, bool bold, bool inverted);
    void drawCell(uint8_t row, uint8_t col);
};

extern DirtyTextUi gDirtyTextUi;

#endif // DIRTY_TEXT_UI_H