#pragma once

// Drawing shared by Grok Bot (bot.cpp) and the agent office (office.cpp):
// PSRAM sprites, anti-aliased shapes, and the orb with its two slanted eyes.

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

namespace orb {

constexpr float TAU_F = 6.2831853f;
constexpr float PI_F = 3.1415927f;

// Poses are written for an orb of radius 62 and scale with the radius.
constexpr float BASE_R = 62.0f;

constexpr uint16_t BODY = 0x0841;  // #0a0a0c, as in the mark
constexpr uint16_t WHITE = 0xFFFF;
constexpr uint16_t GROK = 0x7E9F;   // #7fd0ff
constexpr uint16_t AMBER = 0xFE60;  // #ffcc00

// Sprite whose framebuffer lives in PSRAM, or with `internal` in internal RAM.
// Arduino_Canvas::begin() only allocates when _framebuffer is still null.
class Sprite : public Arduino_Canvas {
 public:
  Sprite(Arduino_G *out, int16_t w, int16_t h, int16_t x, int16_t y, bool internal = false);
};

// A sprite plus where the orb sits in it. Coordinates are in the whole picture;
// the sprite holds rows y0 .. y0 + h - 1 of it, so a small sprite in fast internal
// RAM can draw a tall picture one strip at a time.
struct Stage {
  Sprite *sprite;
  int w, h;
  float r, cx, ground;
  int y0 = 0;
  float restCy() const { return ground - r - 6 * r / BASE_R; }
};

struct Pose {
  float dx = 0, lift = 0, sx = 1, sy = 1;
  float lookX = 0, lookY = 0;
  float blink = 0;  // 0 open, 1 closed
  bool happy = false;
  float eyeScale = 1;
};

struct Look {
  float x, y;
};

float clamp01(float v);
float ease(float t);
float wave(uint32_t now, uint32_t period);
// One 180 ms blink every `period`.
float blinkAt(uint32_t now, uint32_t period);
// Glance from key to key: move for 450 ms, then hold for the rest of `hold`.
Look lookPath(uint32_t now, const Look *keys, int n, uint32_t hold);

uint16_t blend(uint16_t a, uint16_t b, float t);
float luma(uint16_t c);

void plot(const Stage &st, int x, int y, uint16_t color, float coverage);
// Text cursor at picture coordinates.
void cursor(const Stage &st, float x, float y);
void rect(const Stage &st, int x, int y, int w, int h, uint16_t color);
void ellipse(const Stage &st, float cx, float cy, float rx, float ry, uint16_t color);
// Segment of half-length h along (ax, ay), radius r.
void capsule(const Stage &st, float cx, float cy, float ax, float ay, float h, float r, uint16_t color);
// Upper half of a ring: a happy, closed eye.
void arcUp(const Stage &st, float cx, float cy, float rad, float thick, uint16_t color);

// Shadow, orb and eyes for pose `p`. With `clear`, the sprite is first filled with
// `background`. Pose offsets scale with the orb. Returns the orb's center x and top y.
void drawOrb(const Stage &st, const Pose &p, uint16_t background, float *outCx, float *outTop, bool clear = true,
             uint16_t body = BODY);

// Paints a picture `height` rows tall through the sprite one strip of `rows` rows at
// a time, copying each strip to (x, y + strip top) on `out`. `draw` paints the whole
// picture every time; the primitives skip rows outside the current strip. Drawing in
// a small internal-RAM sprite keeps PSRAM, which the panel scans out of constantly,
// down to the one write of each finished pixel.
template <class Draw>
void drawStrips(Stage &st, int rows, Arduino_GFX *out, int x, int y, int height, Draw draw) {
  for (int top = 0; top < height; top += rows) {
    st.y0 = top;
    st.h = height - top < rows ? height - top : rows;
    draw();
    out->draw16bitRGBBitmap(x, y + top, st.sprite->getFramebuffer(), st.w, st.h);
  }
}

}  // namespace orb
