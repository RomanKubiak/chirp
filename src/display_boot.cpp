#include "display_boot.h"

#include "chirp_display.h"
#include "chirp_config.h"
#include "runtime_log.h"

#include <ST7735_t3.h>

namespace
{
void drawCenteredText(const char *text, uint8_t size)
{
    const char *label = (text != nullptr && text[0] != '\0') ? text : "CHIRP";

    ST7735_t3 &sDisplay = chirpDisplay();

    sDisplay.setFont();
    sDisplay.setTextSize(size);
    sDisplay.setTextColor(ST7735_WHITE, ST7735_BLACK);

    int16_t x1 = 0;
    int16_t y1 = 0;
    uint16_t w = 0;
    uint16_t h = 0;
    sDisplay.getTextBounds(label, 0, 0, &x1, &y1, &w, &h);

    const int16_t x = static_cast<int16_t>((160 - static_cast<int16_t>(w)) / 2) - x1;
    const int16_t y = static_cast<int16_t>((128 - static_cast<int16_t>(h)) / 2) - y1;

    sDisplay.setCursor(x, y);
    sDisplay.print(label);
}
} // namespace

void initializeBootDisplay()
{
#if ENABLE_BOOT_DISPLAY
    initializeChirpDisplay();
    drawCenteredText("CHIRP", 2);
    logSetup("[BOOT] ST7735_t3 ready");
#else
    logSetup("[BOOT] Display disabled");
#endif
}