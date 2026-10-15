#pragma once

#include <Arduino.h>

class Arduino_GFX;

// Bring up CH422G (backlight, LCD reset, touch reset), the RGB panel, and GT911.
// Returns false when the framebuffer cannot be allocated.
bool panelBegin();

Arduino_GFX *panelGfx();

// True while a finger is down. False when the controller does not answer.
bool panelTouchDown();
