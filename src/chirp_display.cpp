#include "chirp_display.h"
#include "chirp_config.h"
#include <ST7735_t3.h>
#include "fonts/Targa20pt7b.h"
#include "fonts/whitrabt20pt7b.h"
#include <string.h>
#include <stdio.h>

extern ST7735_t3 display;

namespace ChirpDisplay {

// ── Display caching (avoid unnecessary redraws) ───────────────────────────────
static struct {
    bool systemStatsMode;
    bool launcherPreviewMode;
    char prevInst[32];
    char currInst[32];
    char nextInst[32];
    char kitName[16];
    char paramName[32];
    uint8_t lastCC;
    uint8_t lastValue;
    char statusText[64];
    char statsLine1[32];
    char statsLine2[32];
    char statsLine3[32];
    char statsLine4[32];
    char previewTitle[32];
    char previewDetail[64];
    bool previewError;
    bool launcherMenuMode;
    char launcherPrev[32];
    char launcherCurr[32];
    char launcherNext[32];
    bool launcherPrevActive;
    bool launcherCurrActive;
    bool launcherNextActive;
    bool launcherPrevError;
    bool launcherCurrError;
    bool launcherNextError;
} displayState = {};

// ── Colors for ST7735 ─────────────────────────────────────────────────────────
static constexpr uint16_t COLOR_BG       = ST7735_BLACK;
static constexpr uint16_t COLOR_TEXT     = ST7735_WHITE;      // general text: full white
static constexpr uint16_t COLOR_CURRENT  = 0xFFE0;            // selected cursor: bright yellow
static constexpr uint16_t COLOR_RUNNING  = 0x07FF;            // script running: bright cyan
static constexpr uint16_t COLOR_INACTIVE = 0xC618;            // not-running neighbour: light grey (75% white)
static constexpr uint16_t COLOR_ACCENT   = 0x07FF;            // accent/dividers: bright cyan
static constexpr uint16_t COLOR_ERROR    = 0xF800;            // error: full red

// ── Display dimensions (ST7735 1.8") ──────────────────────────────────────────
static constexpr uint16_t SCREEN_WIDTH  = 160;
static constexpr uint16_t SCREEN_HEIGHT = 128;

static bool gUseFrameBuffer = false;
static bool gUseAsyncDma = false;

static int textWidth(const char *text, uint8_t size)
{
    return static_cast<int>(strlen(text)) * static_cast<int>(6 * size);
}

static void copyTrimmed(const char *src, char *dst, size_t dstSize, size_t maxChars)
{
    if (dstSize == 0) return;
    if (src == nullptr || src[0] == '\0')
    {
        strncpy(dst, "--", dstSize - 1);
        dst[dstSize - 1] = '\0';
        return;
    }

    size_t srcLen = strlen(src);
    size_t copyLen = srcLen;
    if (copyLen > maxChars) copyLen = maxChars;
    if (copyLen > (dstSize - 1)) copyLen = dstSize - 1;

    memcpy(dst, src, copyLen);
    dst[copyLen] = '\0';
}

static void drawText(const char *text, int x, int y, uint16_t color, uint8_t size)
{
    display.setFont((const GFXfont *)nullptr);
    display.setTextSize(size);
    display.setTextColor(color);
    display.setCursor(x, y);
    display.print(text);
}

static void drawCurrentLabel(const char *text, int topY, uint16_t color)
{
    if (text == nullptr || text[0] == '\0') {
        drawText("--", (SCREEN_WIDTH - textWidth("--", 1)) / 2, topY + 10, color, 1);
        return;
    }

    int16_t x1 = 0, y1 = 0;
    uint16_t w = 0, h = 0;
    display.setFont(&Targa20pt7b);
    display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);

    // Fallback to default bitmap font if the custom glyphs are too large.
    if (w > (SCREEN_WIDTH - 6) || h > 18) {
        display.setFont((const GFXfont *)nullptr);
        drawText(text, (SCREEN_WIDTH - textWidth(text, 1)) / 2, topY + 10, color, 1);
        return;
    }

    display.setTextColor(color);
    display.setTextSize(1);
    const int16_t x = static_cast<int16_t>((SCREEN_WIDTH - static_cast<int>(w)) / 2) - x1;
    const int16_t baselineY = static_cast<int16_t>(topY - y1);
    display.setCursor(x, baselineY);
    display.print(text);
    display.setFont((const GFXfont *)nullptr);
}

// Draw top/bottom menu text with whitrabt font when it fits, else fallback.
// align: 0=left, 1=center, 2=right
static void drawMenuLabel(const char *text, int anchorX, int topY, int maxWidth, uint16_t color, int align)
{
    if (text == nullptr || text[0] == '\0') text = "--";

    int16_t x1 = 0, y1 = 0;
    uint16_t w = 0, h = 0;
    display.setFont(&whitrabt20pt7b);
    display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);

    // If the custom font won't fit the slot, use default bitmap font.
    if (w > static_cast<uint16_t>(maxWidth) || h > 20)
    {
        display.setFont((const GFXfont *)nullptr);
        int x = anchorX;
        if (align == 1) x = anchorX - (textWidth(text, 1) / 2);
        if (align == 2) x = anchorX - textWidth(text, 1);
        drawText(text, x, topY + 2, color, 1);
        return;
    }

    display.setTextColor(color);
    display.setTextSize(1);
    int x = anchorX;
    if (align == 1) x = anchorX - (static_cast<int>(w) / 2);
    if (align == 2) x = anchorX - static_cast<int>(w);

    const int16_t drawX = static_cast<int16_t>(x) - x1;
    const int16_t baselineY = static_cast<int16_t>(topY) - y1;
    display.setCursor(drawX, baselineY);
    display.print(text);
    display.setFont((const GFXfont *)nullptr);
}

static void waitForDisplayReady()
{
#if ENABLE_DISPLAY_ASYNC_DMA
    if (gUseFrameBuffer && gUseAsyncDma && display.asyncUpdateActive()) {
        display.waitUpdateAsyncComplete();
    }
#endif
}

static void presentDisplay()
{
    if (gUseFrameBuffer) {
#if ENABLE_DISPLAY_ASYNC_DMA
        if (gUseAsyncDma && !display.updateScreenAsync()) {
            gUseAsyncDma = false;
        }
        if (!gUseAsyncDma) {
            display.updateScreen();
        }
#else
        display.updateScreen();
#endif
        return;
    }
    display.updateScreen();
}

// ── Initialize display ─────────────────────────────────────────────────────────
void init()
{
    memset(&displayState, 0, sizeof(displayState));

    // On Teensy 4.x, ST7735_t3 supports optional framebuffer + DMA async flush.
#if ENABLE_DISPLAY_ASYNC_FRAMEBUFFER
    gUseFrameBuffer = display.useFrameBuffer(true) != 0;
    if (gUseFrameBuffer) {
        display.updateChangedAreasOnly(true);
    }
#else
    gUseFrameBuffer = false;
#endif

#if ENABLE_DISPLAY_ASYNC_DMA
    gUseAsyncDma = gUseFrameBuffer;
#else
    gUseAsyncDma = false;
#endif

    clear();
}

void clear()
{
    waitForDisplayReady();
    display.fillScreen(COLOR_BG);
    presentDisplay();
}

void update()
{
    waitForDisplayReady();
    display.fillScreen(COLOR_BG);

    if (displayState.launcherPreviewMode)
    {
        const uint16_t titleColor = displayState.previewError ? 0xF800 : COLOR_TEXT;
        drawCurrentLabel(displayState.previewTitle[0] ? displayState.previewTitle : "Preview", 20, titleColor);
        if (displayState.previewDetail[0]) {
            drawMenuLabel(displayState.previewDetail, SCREEN_WIDTH / 2, 62, SCREEN_WIDTH - 12, COLOR_TEXT, 1);
        }
        if (displayState.launcherMenuMode) {
            uint16_t pc = displayState.launcherPrevError ? COLOR_ERROR : (displayState.launcherPrevActive ? COLOR_RUNNING : COLOR_INACTIVE);
            uint16_t cc = displayState.launcherCurrError ? COLOR_ERROR : (displayState.launcherCurrActive ? COLOR_RUNNING : COLOR_CURRENT);
            uint16_t nc = displayState.launcherNextError ? COLOR_ERROR : (displayState.launcherNextActive ? COLOR_RUNNING : COLOR_INACTIVE);
            drawMenuLabel(displayState.launcherPrev[0] ? displayState.launcherPrev : "--", 2, 104, 48, pc, 0);
            drawMenuLabel(displayState.launcherCurr[0] ? displayState.launcherCurr : "--", SCREEN_WIDTH / 2, 104, 80, cc, 1);
            drawMenuLabel(displayState.launcherNext[0] ? displayState.launcherNext : "--", SCREEN_WIDTH - 2, 104, 48, nc, 2);
        }
        presentDisplay();
        return;
    }

    if (displayState.systemStatsMode)
    {
        drawCurrentLabel("System", 4, COLOR_CURRENT);
        drawMenuLabel(displayState.statsLine1[0] ? displayState.statsLine1 : "-",
                      SCREEN_WIDTH / 2, 42, SCREEN_WIDTH - 8, COLOR_TEXT, 1);
        drawMenuLabel(displayState.statsLine2[0] ? displayState.statsLine2 : "-",
                      SCREEN_WIDTH / 2, 56, SCREEN_WIDTH - 8, COLOR_TEXT, 1);
        drawMenuLabel(displayState.statsLine3[0] ? displayState.statsLine3 : "-",
                      SCREEN_WIDTH / 2, 70, SCREEN_WIDTH - 8, COLOR_TEXT, 1);
        drawMenuLabel(displayState.statsLine4[0] ? displayState.statsLine4 : "-",
                  SCREEN_WIDTH / 2, 84, SCREEN_WIDTH - 8, COLOR_TEXT, 1);
        if (displayState.launcherMenuMode) {
            uint16_t pc = displayState.launcherPrevError ? COLOR_ERROR : (displayState.launcherPrevActive ? COLOR_RUNNING : COLOR_INACTIVE);
            uint16_t cc = displayState.launcherCurrError ? COLOR_ERROR : (displayState.launcherCurrActive ? COLOR_RUNNING : COLOR_CURRENT);
            uint16_t nc = displayState.launcherNextError ? COLOR_ERROR : (displayState.launcherNextActive ? COLOR_RUNNING : COLOR_INACTIVE);
            drawMenuLabel(displayState.launcherPrev[0] ? displayState.launcherPrev : "--", 2, 104, 48, pc, 0);
            drawMenuLabel(displayState.launcherCurr[0] ? displayState.launcherCurr : "--", SCREEN_WIDTH / 2, 104, 80, cc, 1);
            drawMenuLabel(displayState.launcherNext[0] ? displayState.launcherNext : "--", SCREEN_WIDTH - 2, 104, 48, nc, 2);
        } else {
            drawMenuLabel(displayState.statusText[0] ? displayState.statusText : "Ready",
                          SCREEN_WIDTH / 2, 104, SCREEN_WIDTH - 8, COLOR_TEXT, 1);
        }
        presentDisplay();
        return;
    }

    char prevLabel[20] = {0};
    char currLabel[24] = {0};
    char nextLabel[20] = {0};
    char kitLine[24] = {0};
    char ccLine[24] = {0};

    copyTrimmed(displayState.prevInst, prevLabel, sizeof(prevLabel), 8);
    copyTrimmed(displayState.currInst, currLabel, sizeof(currLabel), 14);
    copyTrimmed(displayState.nextInst, nextLabel, sizeof(nextLabel), 8);
    snprintf(kitLine, sizeof(kitLine), "Kit: %s",
             displayState.kitName[0] ? displayState.kitName : "--");
    snprintf(ccLine, sizeof(ccLine), "CC %u: %u",
             static_cast<unsigned>(displayState.lastCC),
             static_cast<unsigned>(displayState.lastValue));

    const int sideNavY = 5;
    const int currentY = 10;
    const int dividerY = 34;
    const int kitY = 42;
    const int paramY = 56;
    const int ccY = 70;
    const int barY = 86;
    const int statusY = 106;

    drawMenuLabel(prevLabel, 2, sideNavY, 48, COLOR_INACTIVE, 0);
    drawMenuLabel(nextLabel, SCREEN_WIDTH - 2, sideNavY, 48, COLOR_INACTIVE, 2);
    drawMenuLabel(currLabel, SCREEN_WIDTH / 2, currentY, 80, COLOR_CURRENT, 1);

    display.drawFastHLine(0, dividerY, SCREEN_WIDTH, COLOR_ACCENT);

    drawText(kitLine, 4, kitY, COLOR_ACCENT, 1);
    drawText("Param:", 4, paramY, COLOR_TEXT, 1);
    drawText(displayState.paramName[0] ? displayState.paramName : "---", 46, paramY, COLOR_TEXT, 1);
    drawText(ccLine, 4, ccY, COLOR_TEXT, 1);

    const int barX = 4;
    const int barWidth = SCREEN_WIDTH - 8;
    const int barHeight = 10;
    const int fillWidth = (static_cast<int>(displayState.lastValue) * barWidth) / 127;
    display.drawRect(barX, barY, barWidth, barHeight, COLOR_TEXT);
    if (fillWidth > 0)
    {
        display.fillRect(barX + 1, barY + 1, fillWidth - 2 < 0 ? 0 : fillWidth - 2, barHeight - 2, COLOR_ACCENT);
    }

    if (displayState.launcherMenuMode) {
        uint16_t pc = displayState.launcherPrevError ? COLOR_ERROR : (displayState.launcherPrevActive ? COLOR_RUNNING : COLOR_INACTIVE);
        uint16_t cc = displayState.launcherCurrError ? COLOR_ERROR : (displayState.launcherCurrActive ? COLOR_RUNNING : COLOR_CURRENT);
        uint16_t nc = displayState.launcherNextError ? COLOR_ERROR : (displayState.launcherNextActive ? COLOR_RUNNING : COLOR_INACTIVE);
        drawMenuLabel(displayState.launcherPrev[0] ? displayState.launcherPrev : "--", 2, statusY, 48, pc, 0);
        drawMenuLabel(displayState.launcherCurr[0] ? displayState.launcherCurr : "--", SCREEN_WIDTH / 2, statusY, 80, cc, 1);
        drawMenuLabel(displayState.launcherNext[0] ? displayState.launcherNext : "--", SCREEN_WIDTH - 2, statusY, 48, nc, 2);
    } else {
        drawMenuLabel(displayState.statusText[0] ? displayState.statusText : "Ready",
                      SCREEN_WIDTH / 2, statusY, SCREEN_WIDTH - 8, COLOR_TEXT, 1);
    }
    presentDisplay();
}

// ── Show instrument navigation ─────────────────────────────────────────────────
void showInstrument(const char *prevName, const char *currentName, const char *nextName)
{
    bool changed = false;
    if (displayState.systemStatsMode) {
        displayState.systemStatsMode = false;
        changed = true;
    }
    
    if (prevName && strcmp(prevName, displayState.prevInst) != 0) {
        strncpy(displayState.prevInst, prevName, sizeof(displayState.prevInst) - 1);
        changed = true;
    }
    
    if (currentName && strcmp(currentName, displayState.currInst) != 0) {
        strncpy(displayState.currInst, currentName, sizeof(displayState.currInst) - 1);
        changed = true;
    }
    
    if (nextName && strcmp(nextName, displayState.nextInst) != 0) {
        strncpy(displayState.nextInst, nextName, sizeof(displayState.nextInst) - 1);
        changed = true;
    }
    
    if (changed) update();
}

void showKit(const char *kitName)
{
    if (displayState.systemStatsMode) {
        displayState.systemStatsMode = false;
    }
    if (kitName && strcmp(kitName, displayState.kitName) != 0) {
        strncpy(displayState.kitName, kitName, sizeof(displayState.kitName) - 1);
        displayState.kitName[sizeof(displayState.kitName) - 1] = '\0';
        update();
    }
}

// ── Show parameter being edited ────────────────────────────────────────────────
void showParameter(const char *paramName, uint8_t cc, uint8_t value)
{
    bool changed = false;
    if (displayState.systemStatsMode) {
        displayState.systemStatsMode = false;
        changed = true;
    }
    
    if (paramName && strcmp(paramName, displayState.paramName) != 0) {
        strncpy(displayState.paramName, paramName, sizeof(displayState.paramName) - 1);
        changed = true;
    }
    
    if (cc != displayState.lastCC) {
        displayState.lastCC = cc;
        changed = true;
    }
    
    if (value != displayState.lastValue) {
        displayState.lastValue = value;
        changed = true;
    }
    
    if (changed) update();
}

void showValue(uint8_t value)
{
    if (displayState.systemStatsMode) {
        displayState.systemStatsMode = false;
    }
    if (value != displayState.lastValue) {
        displayState.lastValue = value;
        update();
    }
}

// ── Show status message ────────────────────────────────────────────────────────
void showStatus(const char *statusText)
{
    if (statusText && strcmp(statusText, displayState.statusText) != 0) {
        strncpy(displayState.statusText, statusText, sizeof(displayState.statusText) - 1);
        update();
    }
}

void showLauncherPreview(const char *title, const char *detail, bool isError)
{
    bool changed = false;
    if (!displayState.launcherPreviewMode) {
        displayState.launcherPreviewMode = true;
        displayState.systemStatsMode = false;
        changed = true;
    }
    if (title == nullptr) title = "Preview";
    if (detail == nullptr) detail = "";
    if (strcmp(title, displayState.previewTitle) != 0) {
        strncpy(displayState.previewTitle, title, sizeof(displayState.previewTitle) - 1);
        displayState.previewTitle[sizeof(displayState.previewTitle) - 1] = '\0';
        changed = true;
    }
    if (strcmp(detail, displayState.previewDetail) != 0) {
        strncpy(displayState.previewDetail, detail, sizeof(displayState.previewDetail) - 1);
        displayState.previewDetail[sizeof(displayState.previewDetail) - 1] = '\0';
        changed = true;
    }
    if (isError != displayState.previewError) {
        displayState.previewError = isError;
        changed = true;
    }
    if (changed) update();
}

void showLauncherMenu(const char *prev, const char *curr, const char *next,
                      bool prevActive, bool currActive, bool nextActive,
                      bool prevError, bool currError, bool nextError)
{
    bool changed = false;
    if (!displayState.launcherMenuMode) {
        displayState.launcherMenuMode = true;
        changed = true;
    }
    if (prev && strcmp(prev, displayState.launcherPrev) != 0) {
        strncpy(displayState.launcherPrev, prev, sizeof(displayState.launcherPrev) - 1);
        displayState.launcherPrev[sizeof(displayState.launcherPrev) - 1] = '\0';
        changed = true;
    }
    if (curr && strcmp(curr, displayState.launcherCurr) != 0) {
        strncpy(displayState.launcherCurr, curr, sizeof(displayState.launcherCurr) - 1);
        displayState.launcherCurr[sizeof(displayState.launcherCurr) - 1] = '\0';
        changed = true;
    }
    if (next && strcmp(next, displayState.launcherNext) != 0) {
        strncpy(displayState.launcherNext, next, sizeof(displayState.launcherNext) - 1);
        displayState.launcherNext[sizeof(displayState.launcherNext) - 1] = '\0';
        changed = true;
    }
    if (prevActive != displayState.launcherPrevActive) {
        displayState.launcherPrevActive = prevActive;
        changed = true;
    }
    if (currActive != displayState.launcherCurrActive) {
        displayState.launcherCurrActive = currActive;
        changed = true;
    }
    if (nextActive != displayState.launcherNextActive) {
        displayState.launcherNextActive = nextActive;
        changed = true;
    }
    if (prevError != displayState.launcherPrevError) {
        displayState.launcherPrevError = prevError;
        changed = true;
    }
    if (currError != displayState.launcherCurrError) {
        displayState.launcherCurrError = currError;
        changed = true;
    }
    if (nextError != displayState.launcherNextError) {
        displayState.launcherNextError = nextError;
        changed = true;
    }
    if (changed) update();
}

void showMidiActivity(uint8_t port, bool active)
{
    // Could flash the status bar or show port activity
    // For now, this is a placeholder for future enhancement
}

bool isAsyncFrameBufferEnabled()
{
    return gUseFrameBuffer && gUseAsyncDma;
}

const char *displayModeName()
{
    if (!gUseFrameBuffer) return "direct";
    if (gUseAsyncDma) return "framebuffer+async";
    return "framebuffer+sync";
}

void showSystemStats(const char *line1, const char *line2, const char *line3, const char *line4)
{
    bool changed = false;
    if (!displayState.systemStatsMode || displayState.launcherPreviewMode) {
        displayState.systemStatsMode = true;
        displayState.launcherPreviewMode = false;  // preview mode and stats mode are mutually exclusive
        changed = true;
    }

    if (line1 && strcmp(line1, displayState.statsLine1) != 0) {
        strncpy(displayState.statsLine1, line1, sizeof(displayState.statsLine1) - 1);
        displayState.statsLine1[sizeof(displayState.statsLine1) - 1] = '\0';
        changed = true;
    }
    if (line2 && strcmp(line2, displayState.statsLine2) != 0) {
        strncpy(displayState.statsLine2, line2, sizeof(displayState.statsLine2) - 1);
        displayState.statsLine2[sizeof(displayState.statsLine2) - 1] = '\0';
        changed = true;
    }
    if (line3 && strcmp(line3, displayState.statsLine3) != 0) {
        strncpy(displayState.statsLine3, line3, sizeof(displayState.statsLine3) - 1);
        displayState.statsLine3[sizeof(displayState.statsLine3) - 1] = '\0';
        changed = true;
    }
    if (line4 && strcmp(line4, displayState.statsLine4) != 0) {
        strncpy(displayState.statsLine4, line4, sizeof(displayState.statsLine4) - 1);
        displayState.statsLine4[sizeof(displayState.statsLine4) - 1] = '\0';
        changed = true;
    }

    if (changed) update();
}

void showDebug(const char *text)
{
    waitForDisplayReady();
    display.setTextSize(1);
    display.setTextColor(COLOR_TEXT);
    display.setCursor(4, 110);
    display.fillRect(0, 110, SCREEN_WIDTH, 18, COLOR_BG);
    display.println(text);
    presentDisplay();
}

}  // namespace ChirpDisplay
