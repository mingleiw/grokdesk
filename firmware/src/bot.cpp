#include "bot.h"

#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <math.h>

namespace {

constexpr float TAU_F = 6.2831853f;
constexpr float PI_F = 3.1415927f;

// Orb geometry inside the stage. Eye offsets and sizes are fractions of R,
// measured from the Grok Bot mark: two capsules up and right of center,
// tilted so the top leans left.
constexpr float R = 62.0f;
constexpr float CX = BOT_STAGE_W / 2.0f;
constexpr float GROUND = 200.0f;
constexpr float REST_CY = GROUND - R - 6.0f;
constexpr float EYE_AX = 0.431f, EYE_AY = 0.902f;
constexpr float EYE_X[2] = {0.21f, 0.63f};
constexpr float EYE_Y[2] = {-0.42f, -0.49f};
constexpr float EYE_HALF = 0.11f;   // half the straight part of the capsule
constexpr float EYE_R = 0.095f;     // capsule radius before foreshortening
constexpr float EYE_MAX_RR = 0.78f; // eyes never slide closer to the rim than this

constexpr uint16_t ORB = 0x0841;  // #0a0a0c, as in the mark
constexpr uint16_t WHITE = 0xFFFF;
constexpr uint16_t GROK = 0x7E9F;   // #7fd0ff
constexpr uint16_t AMBER = 0xFE60;  // #ffcc00

// Sprite whose framebuffer lives in PSRAM; internal RAM is left for Wi-Fi and TLS.
// Arduino_Canvas::begin() only allocates when _framebuffer is still null.
class Sprite : public Arduino_Canvas {
 public:
  explicit Sprite(Arduino_G *out) : Arduino_Canvas(BOT_STAGE_W, BOT_STAGE_H, out, BOT_STAGE_X, BOT_STAGE_Y) {
    _framebuffer = static_cast<uint16_t *>(
        heap_caps_malloc(BOT_STAGE_W * BOT_STAGE_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
};

Sprite *sprite = nullptr;
BotMood mood = BotMood::Hidden;
uint16_t bg = 0;

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

float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
float ease(float t) {
  t = clamp01(t);
  return t * t * (3 - 2 * t);
}
float wave(uint32_t now, uint32_t period) { return sinf(TAU_F * static_cast<float>(now % period) / period); }

// One 180 ms blink every `period`.
float blinkAt(uint32_t now, uint32_t period) {
  const uint32_t p = now % period;
  return p < 180 ? 1 - fabsf(p / 90.0f - 1) : 0;
}

// Glance from key to key: move for 450 ms, then hold for the rest of `hold`.
Look lookPath(uint32_t now, const Look *keys, int n, uint32_t hold) {
  const int seg = static_cast<int>((now / hold) % n);
  const Look &a = keys[seg];
  const Look &b = keys[(seg + 1) % n];
  const float t = ease(static_cast<float>(now % hold) / 450.0f);
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

uint16_t blend(uint16_t a, uint16_t b, float t) {
  const int ar = a >> 11, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  const int br = b >> 11, bgc = (b >> 5) & 0x3F, bb = b & 0x1F;
  const int r = ar + static_cast<int>((br - ar) * t + 0.5f);
  const int g = ag + static_cast<int>((bgc - ag) * t + 0.5f);
  const int bl = ab + static_cast<int>((bb - ab) * t + 0.5f);
  return static_cast<uint16_t>((r << 11) | (g << 5) | bl);
}

float luma(uint16_t c) {
  return 0.299f * ((c >> 11) << 3) + 0.587f * (((c >> 5) & 0x3F) << 2) + 0.114f * ((c & 0x1F) << 3);
}

void plot(uint16_t *fb, int x, int y, uint16_t color, float coverage) {
  if (coverage <= 0 || x < 0 || y < 0 || x >= BOT_STAGE_W || y >= BOT_STAGE_H) return;
  uint16_t &px = fb[y * BOT_STAGE_W + x];
  px = coverage >= 1 ? color : blend(px, color, coverage);
}

// Anti-aliased filled ellipse.
void ellipse(uint16_t *fb, float cx, float cy, float rx, float ry, uint16_t color) {
  if (rx <= 0 || ry <= 0) return;
  const float m = rx < ry ? rx : ry;
  for (int y = static_cast<int>(cy - ry - 1); y <= static_cast<int>(cy + ry + 1); y++) {
    const float ny = (y + 0.5f - cy) / ry;
    for (int x = static_cast<int>(cx - rx - 1); x <= static_cast<int>(cx + rx + 1); x++) {
      const float nx = (x + 0.5f - cx) / rx;
      const float d = (sqrtf(nx * nx + ny * ny) - 1) * m;
      plot(fb, x, y, color, clamp01(0.5f - d));
    }
  }
}

// Anti-aliased capsule: segment of half-length h along (ax, ay), radius r.
void capsule(uint16_t *fb, float cx, float cy, float ax, float ay, float h, float r, uint16_t color) {
  const float ex = fabsf(ax) * h + r + 1, ey = fabsf(ay) * h + r + 1;
  for (int y = static_cast<int>(cy - ey); y <= static_cast<int>(cy + ey); y++) {
    for (int x = static_cast<int>(cx - ex); x <= static_cast<int>(cx + ex); x++) {
      const float px = x + 0.5f - cx, py = y + 0.5f - cy;
      float t = px * ax + py * ay;
      t = t < -h ? -h : (t > h ? h : t);
      const float qx = px - ax * t, qy = py - ay * t;
      plot(fb, x, y, color, clamp01(0.5f - (sqrtf(qx * qx + qy * qy) - r)));
    }
  }
}

// Upper half of a ring: a happy, closed eye.
void arcUp(uint16_t *fb, float cx, float cy, float rad, float thick, uint16_t color) {
  for (int y = static_cast<int>(cy - rad - thick - 1); y <= static_cast<int>(cy + thick); y++) {
    for (int x = static_cast<int>(cx - rad - thick - 1); x <= static_cast<int>(cx + rad + thick + 1); x++) {
      const float px = x + 0.5f - cx, py = y + 0.5f - cy;
      float d;
      if (py <= 0) {
        d = fabsf(sqrtf(px * px + py * py) - rad) - thick;
      } else {  // round caps at the two ends
        const float ex = px < 0 ? px + rad : px - rad;
        d = sqrtf(ex * ex + py * py) - thick;
      }
      plot(fb, x, y, color, clamp01(0.5f - d));
    }
  }
}

Pose poseFor(uint32_t now) {
  Pose p;
  switch (mood) {
    case BotMood::Calm: {
      static const Look keys[] = {{0, 0}, {-0.7f, 0.15f}, {0, 0}, {0.35f, -0.3f}, {-1, 0.3f}, {0.1f, 0}};
      p.lift = 5 + 5 * wave(now, 2400);
      const Look l = lookPath(now, keys, 6, 2600);
      p.lookX = l.x;
      p.lookY = l.y;
      p.blink = blinkAt(now, 4300);
      break;
    }
    case BotMood::Happy: {
      const float t = static_cast<float>(now % 1300) / 1300.0f;
      p.lift = 30 * sinf(PI_F * t);
      if (p.lift < 5) {  // squash on landing
        const float k = 1 - p.lift / 5;
        p.sx = 1 + 0.07f * k;
        p.sy = 1 - 0.07f * k;
      } else {
        p.sx = 0.98f;
        p.sy = 1.03f;
      }
      p.lookX = 0.1f;
      p.lookY = -0.2f;
      p.happy = true;
      break;
    }
    case BotMood::Thinking:
      p.lift = 4 + 3 * wave(now, 1800);
      p.lookX = 0.7f * wave(now, 1400) - 0.3f;
      p.lookY = -0.3f;
      p.blink = blinkAt(now, 5100);
      break;
    case BotMood::Worried: {
      p.lift = 2 + 2 * wave(now, 3000);
      p.lookX = -1;
      p.lookY = 0.45f;
      const uint32_t s = now % 3600;
      if (s < 360) p.dx = 3 * sinf(TAU_F * s / 90.0f);
      p.blink = blinkAt(now, 3900);
      break;
    }
    case BotMood::Alarmed: {
      p.eyeScale = 1.15f;
      p.lookX = -0.2f;
      const uint32_t s = now % 1700;
      if (s < 320) {
        p.dx = 5 * sinf(TAU_F * s / 80.0f);
        p.lift = 6 * sinf(PI_F * s / 320.0f);
      }
      p.blink = blinkAt(now, 5200);
      break;
    }
    case BotMood::Puzzled: {
      static const Look keys[] = {{-0.8f, -0.5f}, {0.4f, -0.5f}};
      p.lift = 3 + 3 * wave(now, 2600);
      const Look l = lookPath(now, keys, 2, 1500);
      p.lookX = l.x;
      p.lookY = l.y;
      p.blink = blinkAt(now, 4700);
      break;
    }
    case BotMood::Sleepy:
      p.blink = 1;
      p.lookY = 0.3f;
      p.sy = 1 + 0.025f * wave(now, 3200);
      p.sx = 1 - 0.015f * wave(now, 3200);
      break;
    case BotMood::Hidden:
      break;
  }
  return p;
}

void drawEyes(uint16_t *fb, const Pose &p, float cx, float cy) {
  const float lx = p.lookX < -1 ? -1 : (p.lookX > 0.4f ? 0.4f : p.lookX);
  for (int i = 0; i < 2; i++) {
    float ox = EYE_X[i] + lx * 0.33f;
    float oy = EYE_Y[i] + p.lookY * 0.28f;
    float rr = sqrtf(ox * ox + oy * oy);
    if (rr > EYE_MAX_RR) {
      ox *= EYE_MAX_RR / rr;
      oy *= EYE_MAX_RR / rr;
      rr = EYE_MAX_RR;
    }
    // Eyes nearer the rim are seen at an angle, so they read thinner.
    const float fore = powf(1 - rr * rr, 0.25f);
    const float ex = cx + ox * R * p.sx, ey = cy + oy * R * p.sy;
    const float r = EYE_R * R * fore * p.eyeScale;
    const float h = EYE_HALF * R * p.eyeScale;
    if (p.happy) {
      arcUp(fb, ex, ey + r * 0.5f, r * 1.25f, r * 0.42f, WHITE);
    } else if (p.blink > 0.85f) {
      capsule(fb, ex, ey + r * 0.4f, EYE_AY, -EYE_AX, r * 0.9f, r * 0.3f < 1.5f ? 1.5f : r * 0.3f, WHITE);
    } else {
      capsule(fb, ex, ey, EYE_AX, EYE_AY, h * (1 - p.blink), r * (1 - 0.3f * p.blink), WHITE);
    }
  }
}

void drawExtras(uint32_t now, float cx, float top) {
  uint16_t *fb = sprite->getFramebuffer();
  if (mood == BotMood::Thinking) {
    const int lit = static_cast<int>((now / 300) % 3);
    for (int i = 0; i < 3; i++) {
      ellipse(fb, cx - 22 + 22 * i, top - 18, 5, 5, i == lit ? GROK : blend(bg, GROK, 0.35f));
    }
  } else if (mood == BotMood::Puzzled) {
    sprite->setTextSize(4);
    sprite->setTextColor(AMBER);
    sprite->setCursor(static_cast<int16_t>(cx + R * 0.45f + 2 * wave(now, 900)), static_cast<int16_t>(top - 36));
    sprite->print('?');
  } else if (mood == BotMood::Sleepy) {
    for (int k = 0; k < 2; k++) {
      const float t = fmodf(now / 1800.0f + k * 0.5f, 1.0f);
      sprite->setTextSize(t > 0.5f ? 3 : 2);
      sprite->setTextColor(blend(WHITE, bg, t));
      sprite->setCursor(static_cast<int16_t>(cx + R * 0.55f + t * 18), static_cast<int16_t>(top + 6 - t * 44));
      sprite->print('z');
    }
  }
}

}  // namespace

bool botBegin(Arduino_GFX *out) {
  sprite = new Sprite(out);
  if (!sprite->begin(GFX_SKIP_OUTPUT_BEGIN)) {
    Serial.println("bot sprite allocation failed");
    delete sprite;
    sprite = nullptr;
    return false;
  }
  sprite->setTextWrap(false);
  return true;
}

void botSet(BotMood next, uint16_t background) {
  mood = next;
  bg = background;
}

bool botDraw(uint32_t now) {
  if (!sprite || mood == BotMood::Hidden) return false;
  uint16_t *fb = sprite->getFramebuffer();
  const Pose p = poseFor(now);
  const float cx = CX + p.dx;
  const float cy = REST_CY - p.lift;
  const float rx = R * p.sx, ry = R * p.sy;

  sprite->fillScreen(bg);
  const float shrink = 1 - p.lift / 70;
  ellipse(fb, cx, GROUND + 4, R * 0.78f * shrink, 7 * shrink, blend(bg, 0x0000, 0.35f));
  // A black orb disappears on the dark screens, so it gets a thin blue rim there.
  if (luma(bg) < 40) {
    ellipse(fb, cx, cy, rx, ry, blend(bg, GROK, 0.5f));
    ellipse(fb, cx, cy, rx - 3, ry - 3, ORB);
  } else {
    ellipse(fb, cx, cy, rx, ry, ORB);
  }
  drawEyes(fb, p, cx, cy);
  drawExtras(now, cx, cy - ry);
  sprite->flush();
  return true;
}
