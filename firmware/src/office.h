#pragma once

#include <Arduino.h>

#include "board_config.h"

class Arduino_GFX;

// Agent office: one desk per agent, each drawn in strips through internal RAM.
// The characters follow each agent's status; tapping one opens its latest update.
// For now a scripted story drives the statuses (SIMULATED).
#define OFFICE_TOP 52
#define OFFICE_H 380
#define OFFICE_DESK_W 248
#define OFFICE_DESK_GAP 12
#define OFFICE_LEFT ((LCD_W - 3 * OFFICE_DESK_W - 2 * OFFICE_DESK_GAP) / 2)
#define OFFICE_WALL_H 230  // wall above, floor below, in desk-sprite rows
#define OFFICE_STRIP_H 48  // rows per strip; the strip sprite is 248x48 in internal RAM
#define OFFICE_FRAME_MS 66

enum class AgentStatus { Working, Blocked, Waiting, Done };

// Allocates the strip sprite and starts the story clock.
bool officeBegin(Arduino_GFX *out);

// Wall and floor colors, for the gaps render() paints around the desks.
uint16_t officeWallColor();
uint16_t officeFloorColor();

// Draws all desks for `now` into the panel framebuffer without flushing the cache.
// Each frame repaints the animated part of a desk; its name and status area only
// when that changed, or when `full` (after the screen behind it was repainted).
bool officeDraw(uint32_t now, bool full = false);

// A short tap at panel coordinates. Selects or clears a character; returns true
// when the tap landed on a desk.
bool officeTap(int x, int y, uint32_t now);
