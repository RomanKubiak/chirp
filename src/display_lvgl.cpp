#include "display_lvgl.h"

#include "chirp_config.h"
#include "runtime_log.h"

#include <Arduino.h>
#include <ST7735_t3.h>

#if ENABLE_ST7735

namespace
{
static ST7735_t3 sDisplay(ST7735_PIN_CS, ST7735_PIN_DC, ST7735_PIN_RST);
static bool sReady = false;
}

#endif // ENABLE_ST7735

void displayLvglSetup()
{
#if !ENABLE_ST7735
    logSetup("[BOOT] Display disabled");
#else
#if ST7735_PIN_BL >= 0
    pinMode(ST7735_PIN_BL, OUTPUT);
    digitalWrite(ST7735_PIN_BL, HIGH);
#endif

    sDisplay.initR(INITR_BLACKTAB);
    sDisplay.setRotation(ST7735_ROTATION);
    sDisplay.fillScreen(ST7735_BLACK);

    sDisplay.setTextColor(ST7735_WHITE);
    sDisplay.setTextSize(1);
    sDisplay.setCursor(4, 4);
    sDisplay.print("chirp");

    sReady = true;
    logSetup("[BOOT] ST7735 ready");
#endif
}

void displayLvglTask() {}

bool displayLvglReady()
{
#if ENABLE_ST7735
    return sReady;
#else
    return false;
#endif
}
