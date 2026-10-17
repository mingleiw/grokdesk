#include "orb.h"

#include <esp_heap_caps.h>
#include <math.h>

namespace orb {

namespace {

// Eye offsets and sizes are fractions of the orb radius, measured from the
// Grok Bot mark: two capsules up and right of center, tilted so the top leans left.
constexpr float EYE_AX = 0.431f, EYE_AY = 0.902f;
constexpr float EYE_X[2] = {0.21f, 0.63f};
constexpr float EYE_Y[2] = {-0.42f, -0.49f};
constexpr float EYE_HALF = 0.11f;   // half the straight part of the capsule
constexpr float EYE_R = 0.095f;     // capsule radius before foreshortening
constexpr float EYE_MAX_RR = 0.78f; // eyes never slide closer to the rim than this

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

}  // namespace

Sprite::Sprite(Arduino_G *out, int16_t w, int16_t h, int16_t x, int16_t y, bool internal)
    : Arduino_Canvas(w, h, out, x, y) {
  _framebuffer = static_cast<uint16_t *>(
      heap_caps_malloc(w * h * 2, (internal ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM) | MALLOC_CAP_8BIT));
}

float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
float ease(float t) {
  t = clamp01(t);
  return t * t * (3 - 2 * t);
}
float wave(uint32_t now, uint32_t period) { return sinf(TAU_F * static_cast<float>(now % period) / period); }

float blinkAt(uint32_t now, uint32_t period) {
  const uint32_t p = now % period;
  return p < 180 ? 1 - fabsf(p / 90.0f - 1) : 0;
}

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

// First and last picture rows of [ya, yb] that fall inside the sprite.
static inline int rowFrom(const Stage &st, int ya) { return ya < st.y0 ? st.y0 : ya; }
static inline int rowTo(const Stage &st, int yb) { return yb > st.y0 + st.h - 1 ? st.y0 + st.h - 1 : yb; }

void plot(const Stage &st, int x, int y, uint16_t color, float coverage) {
  y -= st.y0;
  if (coverage <= 0 || x < 0 || y < 0 || x >= st.w || y >= st.h) return;
  uint16_t &px = st.sprite->getFramebuffer()[y * st.w + x];
  px = coverage >= 1 ? color : blend(px, color, coverage);
}

void cursor(const Stage &st, float x, float y) {
  // Truncate before shifting, so a strip places text on the same row as one full sprite.
  st.sprite->setCursor(static_cast<int16_t>(x), static_cast<int16_t>(static_cast<int>(y) - st.y0));
}

void rect(const Stage &st, int x, int y, int w, int h, uint16_t color) {
  uint16_t *fb = st.sprite->getFramebuffer();
  for (int yy = rowFrom(st, y < 0 ? 0 : y); yy <= rowTo(st, y + h - 1); yy++) {
    for (int xx = x < 0 ? 0 : x; xx < x + w && xx < st.w; xx++) fb[(yy - st.y0) * st.w + xx] = color;
  }
}

// Anti-aliased filled ellipse. Each row's interior is filled solid; only the
// two edge runs are shaded per pixel, which keeps the big splash orb cheap.
void ellipse(const Stage &st, float cx, float cy, float rx, float ry, uint16_t color) {
  if (rx <= 0 || ry <= 0) return;
  const float m = rx < ry ? rx : ry;
  uint16_t *fb = st.sprite->getFramebuffer();
  for (int y = rowFrom(st, static_cast<int>(cy - ry - 1)); y <= rowTo(st, static_cast<int>(cy + ry + 1)); y++) {
    if (y < 0) continue;
    const float ny = (y + 0.5f - cy) / ry;
    // Solid run: pixels whose centers sit inside the ellipse shrunk by 3 px.
    const float iny = (y + 0.5f - cy) / (ry - 3.0f);
    const float span = ry > 3.0f && rx > 3.0f && iny * iny < 1 ? (rx - 3.0f) * sqrtf(1 - iny * iny) : 0;
    const int solidL = static_cast<int>(ceilf(cx - span - 0.5f)), solidR = static_cast<int>(floorf(cx + span - 0.5f)) + 1;
    for (int x = static_cast<int>(cx - rx - 1); x <= static_cast<int>(cx + rx + 1); x++) {
      if (x >= solidL && x < solidR) {
        if (x >= 0 && x < st.w) fb[(y - st.y0) * st.w + x] = color;
        continue;
      }
      const float nx = (x + 0.5f - cx) / rx;
      const float d = (sqrtf(nx * nx + ny * ny) - 1) * m;
      plot(st, x, y, color, clamp01(0.5f - d));
    }
  }
}

void capsule(const Stage &st, float cx, float cy, float ax, float ay, float h, float r, uint16_t color) {
  const float ex = fabsf(ax) * h + r + 1, ey = fabsf(ay) * h + r + 1;
  for (int y = rowFrom(st, static_cast<int>(cy - ey)); y <= rowTo(st, static_cast<int>(cy + ey)); y++) {
    for (int x = static_cast<int>(cx - ex); x <= static_cast<int>(cx + ex); x++) {
      const float px = x + 0.5f - cx, py = y + 0.5f - cy;
      float t = px * ax + py * ay;
      t = t < -h ? -h : (t > h ? h : t);
      const float qx = px - ax * t, qy = py - ay * t;
      plot(st, x, y, color, clamp01(0.5f - (sqrtf(qx * qx + qy * qy) - r)));
    }
  }
}

void arcUp(const Stage &st, float cx, float cy, float rad, float thick, uint16_t color) {
  for (int y = rowFrom(st, static_cast<int>(cy - rad - thick - 1)); y <= rowTo(st, static_cast<int>(cy + thick)); y++) {
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

void drawOrb(const Stage &st, const Pose &p, uint16_t background, float *outCx, float *outTop, bool clear,
             uint16_t body) {
  const float k = st.r / BASE_R;
  const float cx = st.cx + p.dx * k;
  const float cy = st.restCy() - p.lift * k;
  const float rx = st.r * p.sx, ry = st.r * p.sy;

  if (clear) st.sprite->fillScreen(background);
  const float shrink = 1 - p.lift / 70;
  ellipse(st, cx, st.ground + 4 * k, st.r * 0.78f * shrink * p.sx, 7 * k * shrink, blend(background, 0x0000, 0.35f));
  // A black orb disappears on the dark screens, so it gets a thin blue rim there.
  if (luma(background) < 40) {
    ellipse(st, cx, cy, rx, ry, blend(background, GROK, 0.5f));
    ellipse(st, cx, cy, rx - 3 * k, ry - 3 * k, body);
  } else {
    ellipse(st, cx, cy, rx, ry, body);
  }
  drawEyes(st, p, cx, cy);
  *outCx = cx;
  *outTop = cy - ry;
}

}  // namespace orb
