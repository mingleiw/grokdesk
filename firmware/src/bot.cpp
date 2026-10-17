#include "bot.h"

#include <Arduino_GFX_Library.h>
#include <math.h>

#include "board_config.h"
#include "orb.h"

using namespace orb;

namespace {

constexpr uint16_t SPLASH_BG = 0xF79E;  // #f4f4f6, the mark's backdrop

Stage stage = {nullptr, BOT_STAGE_W, BOT_STAGE_H, BASE_R, BOT_STAGE_W / 2.0f, 200.0f};
Arduino_GFX *panel = nullptr;
BotMood mood = BotMood::Hidden;
uint16_t bg = 0;

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
    cursor(st, cx + R * 0.45f + 2 * wave(now, 900), top - 36);
    sprite->print('?');
  } else if (mood == BotMood::Sleepy) {
    for (int k = 0; k < 2; k++) {
      const float t = fmodf(now / 1800.0f + k * 0.5f, 1.0f);
      sprite->setTextSize(t > 0.5f ? 3 : 2);
      sprite->setTextColor(blend(WHITE, bg, t));
      cursor(st, cx + R * 0.55f + t * 18, top + 6 - t * 44);
      sprite->print('z');
    }
  }
}

}  // namespace

bool botBegin(Arduino_GFX *out) {
  panel = out;
  stage.sprite = new Sprite(out, BOT_STAGE_W, BOT_STRIP_H, BOT_STAGE_X, BOT_STAGE_Y, true);
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
  const Pose p = poseFor(now);
  drawStrips(stage, BOT_STRIP_H, panel, BOT_STAGE_X, BOT_STAGE_Y, BOT_STAGE_H, [&] {
    float cx, top;
    drawOrb(stage, p, bg, &cx, &top);
    drawExtras(stage, now, cx, top);
  });
  return true;
}

void botSplash(uint32_t durationMs) {
  if (!panel) return;
  // Big orb centered on the whole panel; the sprite is freed afterwards.
  constexpr int W = 420, H = 460, ROWS = 40;
  Stage big = {nullptr, W, ROWS, 150.0f, W / 2.0f, 410.0f};
  big.sprite = new Sprite(panel, W, ROWS, (LCD_W - W) / 2, (LCD_H - H) / 2, true);
  if (!big.sprite->begin(GFX_SKIP_OUTPUT_BEGIN)) {
    Serial.println("splash sprite allocation failed");
    delete big.sprite;
    return;
  }
  panel->fillScreen(SPLASH_BG);
  const uint32_t start = millis();
  for (uint32_t t = 0; t < durationMs; t = millis() - start) {
    const Pose p = splashPose(t);
    drawStrips(big, ROWS, panel, (LCD_W - W) / 2, (LCD_H - H) / 2, H, [&] {
      float cx, top;
      drawOrb(big, p, SPLASH_BG, &cx, &top);
    });
    delay(10);
  }
  delete big.sprite;
}
