#pragma once

#include <Arduino.h>

class Arduino_GFX;

// Grok Bot: a black orb with two slanted eyes, animated in a stage on the right
// of the status screens. Text on those screens stays left of BOT_STAGE_X.
#define BOT_STAGE_X 608
#define BOT_STAGE_Y 120
#define BOT_STAGE_W 184
#define BOT_STAGE_H 232
#define BOT_STRIP_H 58  // the stage is drawn in 4 strips through internal RAM
#define BOT_FRAME_MS 40

enum class BotMood {
  Hidden,    // portal, connecting: no stage
  Calm,      // healthy: slow bob, looks around, blinks
  Happy,     // resolved: hops, squinting eyes
  Thinking,  // analyzing: eyes scan, dots pulse above
  Worried,   // failed with explanation: looks at the text, small shakes
  Alarmed,   // failed, no explanation: wide eyes, frequent shakes
  Puzzled,   // unknown state: "?" above
  Sleepy,    // offline: eyes closed, "z" drifting up
};

// Allocates the sprite in PSRAM. Returns false when it cannot.
bool botBegin(Arduino_GFX *out);

// Mood and the screen color behind the stage. Call from render().
void botSet(BotMood mood, uint16_t bg);

// Draws the frame for `now` into the panel framebuffer without flushing the cache.
// Returns false when the bot is hidden or not allocated.
bool botDraw(uint32_t now);

// Boot screen: only a big Grok Bot on the light backdrop of the mark. It pops in
// asleep, wakes, looks around and hops. Blocks for `durationMs`.
void botSplash(uint32_t durationMs);
