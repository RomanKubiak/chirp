#include "chirp_display.h"

#include "chirp_config.h"

namespace
{
ST7735_t3 sDisplay(SHARED_SPI_CS, SHARED_SPI_DC, SHARED_SPI_RST);
bool sDisplayReady = false;
} // namespace

ST7735_t3 &chirpDisplay()
{
    return sDisplay;
}

void initializeChirpDisplay()
{
#if ENABLE_BOOT_DISPLAY
    if (sDisplayReady) return;

    sDisplay.initR(INITR_BLACKTAB);
    sDisplay.setRotation(BOOT_DISPLAY_ROTATION);
    sDisplay.setTextWrap(false);
    sDisplay.fillScreen(ST7735_BLACK);
    sDisplayReady = true;
#endif
}