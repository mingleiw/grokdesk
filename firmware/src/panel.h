#pragma once

#include <Arduino.h>

class Arduino_GFX;

// Bring up CH422G (backlight, LCD reset, touch reset), the RGB panel, and GT911.
// Returns false when the framebuffer cannot be allocated.
bool panelBegin();

Arduino_GFX *panelGfx();

// True while a finger is down. Keeps the last answer between controller updates.
// False when the controller does not answer.
bool panelTouchDown();

// Panel coordinates of the most recent touch point.
void panelTouchPoint(int *x, int *y);
