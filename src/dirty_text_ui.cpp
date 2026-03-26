#include "dirty_text_ui.h"

#include "chirp_display.h"

#include <cstring>

DirtyTextUi gDirtyTextUi;

void DirtyTextUi::begin(ST7735_t3 *display)
{
    display_ = display;
    if (display_ == nullptr) return;

    display_->setFont();
    display_->setTextSize(1);
    display_->setTextWrap(false);
    display_->setTextColor(foregroundColor_, backgroundColor_);

    clear();
}

void DirtyTextUi::clear()
{
    if (display_ == nullptr) return;

    for (uint8_t row = 0; row < kRows; ++row) {
        for (uint8_t col = 0; col < kCols; ++col) {
            cells_[row][col] = ' ';
            dirty_[row][col] = true;
            bold_[row][col] = false;
            inverted_[row][col] = false;
        }
    }

    display_->fillScreen(backgroundColor_);
}

void DirtyTextUi::setPalette(uint16_t foreground, uint16_t background)
{
    foregroundColor_ = foreground;
    backgroundColor_ = background;
    if (display_ != nullptr) {
        display_->setTextColor(foregroundColor_, backgroundColor_);
    }
}

void DirtyTextUi::drawBorder()
{
    if (kCols < 2 || kRows < 2) return;

    setCell(0, 0, '+');
    setCell(0, kCols - 1, '+');
    setCell(kRows - 1, 0, '+');
    setCell(kRows - 1, kCols - 1, '+');

    for (uint8_t col = 1; col + 1 < kCols; ++col) {
        setCell(0, col, '-');
        setCell(kRows - 1, col, '-');
    }

    for (uint8_t row = 1; row + 1 < kRows; ++row) {
        setCell(row, 0, '|');
        setCell(row, kCols - 1, '|');
    }
}

void DirtyTextUi::setLine(uint8_t row, uint8_t col, const char *text, bool pad, bool bold, bool inverted)
{
    writeText(row, col, text, pad, bold, inverted);
}

void DirtyTextUi::setCenteredLine(uint8_t row, const char *text, bool bold, bool inverted)
{
    if (text == nullptr) text = "";
    const size_t length = strlen(text);
    const uint8_t startCol = (length >= kTextCols) ? 0 : static_cast<uint8_t>((kTextCols - length) / 2);
    writeText(row, startCol, text, false, bold, inverted);
}

void DirtyTextUi::setCell(uint8_t row, uint8_t col, char ch, bool bold, bool inverted)
{
    if (row >= kRows || col >= kCols) return;
    if (cells_[row][col] == ch && bold_[row][col] == bold && inverted_[row][col] == inverted) return;

    cells_[row][col] = ch;
    bold_[row][col] = bold;
    inverted_[row][col] = inverted;
    dirty_[row][col] = true;
}

void DirtyTextUi::render()
{
    if (display_ == nullptr) return;

    for (uint8_t row = 0; row < kRows; ++row) {
        for (uint8_t col = 0; col < kCols; ++col) {
            if (!dirty_[row][col]) continue;
            drawCell(row, col);
            dirty_[row][col] = false;
        }
    }
}

void DirtyTextUi::writeText(uint8_t row, uint8_t col, const char *text, bool pad, bool bold, bool inverted)
{
    if (row >= kRows || col >= kTextCols) return;
    if (text == nullptr) text = "";

    uint8_t x = col;
    const char *cursor = text;
    while (*cursor != '\0' && x < kTextCols) {
        setCell(row, x, *cursor++, bold, inverted);
        ++x;
    }

    if (pad) {
        while (x < kTextCols) {
            setCell(row, x, ' ', bold, inverted);
            ++x;
        }
    }
}

void DirtyTextUi::drawCell(uint8_t row, uint8_t col)
{
    if (display_ == nullptr) return;

    const int16_t x = static_cast<int16_t>(col * kCellWidth);
    const int16_t y = static_cast<int16_t>(row * kCellHeight + (row / 2));
    const char ch = cells_[row][col];
    const bool inverted = inverted_[row][col];
    const uint16_t cellForeground = inverted ? backgroundColor_ : foregroundColor_;
    const uint16_t cellBackground = inverted ? foregroundColor_ : backgroundColor_;

    display_->fillRect(x, y, kCellWidth, kCellHeight, cellBackground);
    if (ch == ' ') return;

    display_->setTextColor(cellForeground, cellBackground);
    display_->setCursor(x, y);
    display_->print(ch);
    if (bold_[row][col]) {
        display_->setCursor(x + 1, y);
        display_->print(ch);
    }
    display_->setTextColor(foregroundColor_, backgroundColor_);
}