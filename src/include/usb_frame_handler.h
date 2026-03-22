#ifndef USB_FRAME_HANDLER_H
#define USB_FRAME_HANDLER_H

#include <stdint.h>

// Process up to maxFrames incoming USB control frames.
// Returns the number of frames handled.
uint8_t processUsbControlFrames(uint8_t maxFrames);

#endif // USB_FRAME_HANDLER_H
