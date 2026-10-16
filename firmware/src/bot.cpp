#include "bot.h"

#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <math.h>

#include "board_config.h"

namespace {

constexpr float TAU_F = 6.2831853f;
constexpr float PI_F = 3.1415927f;

// Eye offsets and sizes are fractions of the orb radius, measured from the
// Grok Bot mark: two capsules up and right of center, tilted so the top leans left.
// Poses are written for the status stage (radius 62) and scale with the radius.
constexpr float BASE_R = 62.0f;
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
constexpr uint16_t SPLASH_BG = 0xF79E;  // #f4f4f6, the mark's backdrop

// Sprite whose framebuffer lives in PSRAM; internal RAM is left for Wi-Fi and TLS.
// Arduino_Canvas::begin() only allocates when _framebuffer is still null.
class Sprite : public Arduino_Canvas {
 public:
  Sprite(Arduino_G *out, int16_t w, int16_t h, int16_t x, int16_t y) : Arduino_Canvas(w, h, out, x, y) {
    _framebuffer = static_cast<uint16_t *>(heap_caps_malloc(w * h * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
};

// A sprite plus where the orb sits in it.
struct Stage {
  Sprite *sprite;
  int w, h;
  float r, cx, ground;
  float restCy() const { return ground - r - 6 * r / BASE_R; }
};

Stage stage = {nullptr, BOT_STAGE_W, BOT_STAGE_H, BASE_R, BOT_STAGE_W / 2.0f, 200.0f};
Arduino_GFX *panel = nullptr;
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

void plot(const Stage &st, int x, int y, uint16_t color, float coverage) {
  if (coverage <= 0 || x < 0 || y < 0 || x >= st.w || y >= st.h) return;
  uint16_t &px = st.sprite->getFramebuffer()[y * st.w + x];
  px = coverage >= 1 ? color : blend(px, color, coverage);
}

// Anti-aliased filled ellipse. Each row's interior is filled solid; only the
// two edge runs are shaded per pixel, which keeps the big splash orb cheap.
void ellipse(const Stage &st, float cx, float cy, float rx, float ry, uint16_t color) {
  if (rx <= 0 || ry <= 0) return;
  const float m = rx < ry ? rx : ry;
  uint16_t *fb = st.sprite->getFramebuffer();
  for (int y = static_cast<int>(cy - ry - 1); y <= static_cast<int>(cy + ry + 1); y++) {
    if (y < 0 || y >= st.h) continue;
    const float ny = (y + 0.5f - cy) / ry;
    // Solid run: pixels whose centers sit inside the ellipse shrunk by 3 px.
    const float iny = (y + 0.5f - cy) / (ry - 3.0f);
    const float span = ry > 3.0f && rx > 3.0f && iny * iny < 1 ? (rx - 3.0f) * sqrtf(1 - iny * iny) : 0;
    const int solidL = static_cast<int>(ceilf(cx - span - 0.5f)), solidR = static_cast<int>(floorf(cx + span - 0.5f)) + 1;
    for (int x = static_cast<int>(cx - rx - 1); x <= static_cast<int>(cx + rx + 1); x++) {
      if (x >= solidL && x < solidR) {
        if (x >= 0 && x < st.w) fb[y * st.w + x] = color;
        continue;
      }
      const float nx = (x + 0.5f - cx) / rx;
      const float d = (sqrtf(nx * nx + ny * ny) - 1) * m;
      plot(st, x, y, color, clamp01(0.5f - d));
    }
  }
}

// Anti-aliased capsule: segment of half-length h along (ax, ay), radius r.
void capsule(const Stage &st, float cx, float cy, float ax, float ay, float h, float r, uint16_t color) {
  const float ex = fabsf(ax) * h + r + 1, ey = fabsf(ay) * h + r + 1;
  for (int y = static_cast<int>(cy - ey); y <= static_cast<int>(cy + ey); y++) {
    for (int x = static_cast<int>(cx - ex); x <= static_cast<int>(cx + ex); x++) {
      const float px = x + 0.5f - cx, py = y + 0.5f - cy;
      float t = px * ax + py * ay;
      t = t < -h ? -h : (t > h ? h : t);
      const float qx = px - ax * t, qy = py - ay * t;
      plot(st, x, y, color, clamp01(0.5f - (sqrtf(qx * qx + qy * qy) - r)));
    }
  }
}

// Upper half of a ring: a happy, closed eye.
void arcUp(const Stage &st, float cx, float cy, float rad, float thick, uint16_t color) {
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
      plot(st, x, y, color, clamp01(0.5f - d));
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

// Boot splash, `t` ms in: pop in asleep, wake, look around, hop twice.
Pose splashPose(uint32_t t) {
  static const Look keys[] = {{0, 0}, {-1, 0.25f}, {0.4f, -0.35f}, {0, 0}};
  Pose p;
  if (t < 450) {  // pop in with a little overshoot
    const float k = ease(t / 450.0f);
    const float s = 0.2f + 0.8f * k + 0.12f * sinf(PI_F * k);
    p.sx = p.sy = s;
  }
  if (t < 900) {
    p.blink = 1;
    p.sy *= 1 + 0.03f * wave(t, 900);
  } else if (t < 1100) {
    p.blink = 1 - (t - 900) / 200.0f;
  } else if (t < 3100) {
    const Look l = lookPath(t - 1100, keys, 4, 500);
    p.lookX = l.x;
    p.lookY = l.y;
    if (t > 2900 && t < 3080) p.blink = 1 - fabsf((t - 2900) / 90.0f - 1);
  } else {
    const float h = static_cast<float>((t - 3100) % 600) / 600.0f;
    p.lift = 34 * sinf(PI_F * h);
    if (p.lift < 5) {
      const float k = 1 - p.lift / 5;
      p.sx = 1 + 0.07f * k;
      p.sy = 1 - 0.07f * k;
    }
    p.lookY = -0.2f;
  }
  return p;
}

void drawEyes(const Stage &st, const Pose &p, float cx, float cy) {
  const float R = st.r;
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
    const float r = EYE_R * R * fore * p.eyeScale * p.sx;
    const float h = EYE_HALF * R * p.eyeScale * p.sy;
    if (p.happy) {
      arcUp(st, ex, ey + r * 0.5f, r * 1.25f, r * 0.42f, WHITE);
    } else if (p.blink > 0.85f) {
      capsule(st, ex, ey + r * 0.4f, EYE_AY, -EYE_AX, r * 0.9f, r * 0.3f < 1.5f ? 1.5f : r * 0.3f, WHITE);
    } else {
      capsule(st, ex, ey, EYE_AX, EYE_AY, h * (1 - p.blink), r * (1 - 0.3f * p.blink), WHITE);
    }
  }
}

void drawExtras(const Stage &st, uint32_t now, float cx, float top) {
  Sprite *sprite = st.sprite;
  const float R = st.r;
  if (mood == BotMood::Thinking) {
    const int lit = static_cast<int>((now / 300) % 3);
    for (int i = 0; i < 3; i++) {
      ellipse(st, cx - 22 + 22 * i, top - 18, 5, 5, i == lit ? GROK : blend(bg, GROK, 0.35f));
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

// Shadow, orb and eyes for pose `p` on `background`. Pose offsets scale with the orb.
void drawOrb(const Stage &st, const Pose &p, uint16_t background, float *outCx, float *outTop) {
  const float k = st.r / BASE_R;
  const float cx = st.cx + p.dx * k;
  const float cy = st.restCy() - p.lift * k;
  const float rx = st.r * p.sx, ry = st.r * p.sy;

  st.sprite->fillScreen(background);
  const float shrink = 1 - p.lift / 70;
  ellipse(st, cx, st.ground + 4 * k, st.r * 0.78f * shrink * p.sx, 7 * k * shrink, blend(background, 0x0000, 0.35f));
  // A black orb disappears on the dark screens, so it gets a thin blue rim there.
  if (luma(background) < 40) {
    ellipse(st, cx, cy, rx, ry, blend(background, GROK, 0.5f));
    ellipse(st, cx, cy, rx - 3 * k, ry - 3 * k, ORB);
  } else {
    ellipse(st, cx, cy, rx, ry, ORB);
  }
  drawEyes(st, p, cx, cy);
  *outCx = cx;
  *outTop = cy - ry;
}

}  // namespace

bool botBegin(Arduino_GFX *out) {
  panel = out;
  stage.sprite = new Sprite(out, BOT_STAGE_W, BOT_STAGE_H, BOT_STAGE_X, BOT_STAGE_Y);
  if (!stage.sprite->begin(GFX_SKIP_OUTPUT_BEGIN)) {
    Serial.println("bot sprite allocation failed");
    delete stage.sprite;
    stage.sprite = nullptr;
    return false;
  }
  stage.sprite->setTextWrap(false);
  return true;
}

void botSet(BotMood next, uint16_t background) {
  mood = next;
  bg = background;
}

bool botDraw(uint32_t now) {
  if (!stage.sprite || mood == BotMood::Hidden) return false;
  float cx, top;
  drawOrb(stage, poseFor(now), bg, &cx, &top);
  drawExtras(stage, now, cx, top);
  stage.sprite->flush();
  return true;
}

void botSplash(uint32_t durationMs) {
  if (!panel) return;
  // Big orb centered on the whole panel; the sprite is freed afterwards.
  constexpr int W = 420, H = 460;
  Stage big = {nullptr, W, H, 150.0f, W / 2.0f, 410.0f};
  big.sprite = new Sprite(panel, W, H, (LCD_W - W) / 2, (LCD_H - H) / 2);
  if (!big.sprite->begin(GFX_SKIP_OUTPUT_BEGIN)) {
    Serial.println("splash sprite allocation failed");
    delete big.sprite;
    return;
  }
  panel->fillScreen(SPLASH_BG);
  const uint32_t start = millis();
  for (uint32_t t = 0; t < durationMs; t = millis() - start) {
    float cx, top;
    drawOrb(big, splashPose(t), SPLASH_BG, &cx, &top);
    big.sprite->flush();
    panel->flush();
    delay(10);
  }
  delete big.sprite;
}
