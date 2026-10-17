#include "office.h"

#include <Arduino_GFX_Library.h>
#include <math.h>
#include <string.h>

#include "board_config.h"
#include "orb.h"

using namespace orb;

namespace {

constexpr uint16_t c565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

constexpr uint16_t WALL = c565(0x1b, 0x22, 0x33);
constexpr uint16_t BASEBOARD = c565(0x12, 0x17, 0x22);
constexpr uint16_t FLOOR = c565(0x2a, 0x2f, 0x3b);
constexpr uint16_t DESK_TOP = c565(0xb0, 0x7a, 0x45);
constexpr uint16_t DESK_EDGE = c565(0xd0, 0x98, 0x5c);
constexpr uint16_t DESK_FRONT = c565(0x7a, 0x52, 0x30);
constexpr uint16_t BEZEL = c565(0x2b, 0x30, 0x3b);
constexpr uint16_t CARD = c565(0x0e, 0x13, 0x1d);
constexpr uint16_t DIM = c565(0x9a, 0xa4, 0xb0);
constexpr uint16_t DARK = c565(0x0b, 0x0d, 0x12);
constexpr uint16_t RED = c565(0xff, 0x5a, 0x5a);
constexpr uint16_t GREEN = c565(0x5d, 0xff, 0x9a);
constexpr uint16_t PURPLE = c565(0xc7, 0x92, 0xea);
constexpr uint16_t HAT = c565(0xf2, 0xb7, 0x05);
constexpr uint16_t HAT_BRIM = c565(0xc8, 0x92, 0x00);
constexpr uint16_t STEEL = c565(0xc9, 0xd1, 0xd9);
constexpr uint16_t WOOD = c565(0x8a, 0x5a, 0x3c);
constexpr uint16_t GLASS = c565(0xcf, 0xe8, 0xff);

// Desk sprite layout, in sprite rows and columns.
constexpr int DESK_Y = 196;     // desk surface; the character sits behind it
constexpr int DESK_FRONT_Y = 206;
constexpr int DESK_BOTTOM = 262;
constexpr int SCREEN_X = 20, SCREEN_Y = 118, SCREEN_W = 92, SCREEN_H = 66;
constexpr float ORB_R = 40.0f;
constexpr float ORB_CX = 150.0f;
constexpr float ORB_GROUND = 204.0f;
constexpr int CARD_Y = 262;
constexpr int TEXT_CHARS = 18;  // size-2 characters per card line
constexpr uint32_t CELEBRATE_MS = 3200;
constexpr uint32_t SELECT_MS = 10000;

enum Role { BUILDER, REVIEWER, TESTER, ROLES };
const char *const ROLE_NAME[ROLES] = {"BUILDER", "REVIEWER", "TESTER"};
// Every agent runs pstack (github.com/cursor/plugins/tree/main/pstack); each event
// names the pstack skill doing the work.
const char *const SKILL = "pstack";

struct Event {
  uint32_t at;
  Role agent;
  AgentStatus status;
  const char *skill;
  const char *text;
};

// SIMULATED: the builder finishes and celebrates, review passes, the tester
// finds a bug, everyone goes back to work, and the second round goes green.
constexpr uint32_t STORY_MS = 44000;
const Event STORY[] = {
    {0, BUILDER, AgentStatus::Working, "/poteto-mode", "Bug fix playbook: reproducing the failure"},
    {0, REVIEWER, AgentStatus::Waiting, "/interrogate", "Waiting for a diff to review"},
    {0, TESTER, AgentStatus::Waiting, "/tdd", "Waiting for a build to test"},
    {3500, BUILDER, AgentStatus::Working, "/poteto-mode", "Root cause found. Writing the fix"},
    {7000, BUILDER, AgentStatus::Done, "/poteto-mode", "Fix verified, PR opened: +1 -1"},
    {7600, REVIEWER, AgentStatus::Working, "/interrogate", "Two models trying to break the diff"},
    {12000, REVIEWER, AgentStatus::Done, "/interrogate", "Verdict: no blocking findings"},
    {12600, TESTER, AgentStatus::Working, "/tdd", "Running 42 tests"},
    {18000, TESTER, AgentStatus::Blocked, "/tdd", "Bug found: test_flip_edges fails (2 != 3)"},
    {20500, BUILDER, AgentStatus::Working, "/poteto-mode", "Binary-searching paired() edge flips"},
    {20500, REVIEWER, AgentStatus::Working, "/blast-radius", "What else breaks if paired() changes?"},
    {21200, TESTER, AgentStatus::Working, "/tdd", "Failing regression test first"},
    {25000, BUILDER, AgentStatus::Working, "/poteto-mode", "Fixing edge flips in paired()"},
    {29000, BUILDER, AgentStatus::Done, "/poteto-mode", "Second fix verified: +6 -2"},
    {31000, REVIEWER, AgentStatus::Done, "/interrogate", "Approved again"},
    {32000, TESTER, AgentStatus::Working, "/tdd", "Re-running 43 tests"},
    {37000, TESTER, AgentStatus::Done, "/tdd", "43/43 passed. Ready to ship"},
};

struct Agent {
  AgentStatus status;
  const char *skill;
  const char *text;
  uint32_t since;  // millis() when this status began
};

Stage desks[ROLES];
uint32_t storyStart = 0;
int selected = -1;
uint32_t selectedAt = 0;

uint16_t statusColor(AgentStatus s) {
  switch (s) {
    case AgentStatus::Working: return GROK;
    case AgentStatus::Blocked: return RED;
    case AgentStatus::Waiting: return AMBER;
    case AgentStatus::Done: return GREEN;
  }
  return DIM;
}

const char *statusWord(AgentStatus s) {
  switch (s) {
    case AgentStatus::Working: return "working";
    case AgentStatus::Blocked: return "blocked";
    case AgentStatus::Waiting: return "waiting";
    case AgentStatus::Done: return "done";
  }
  return "?";
}

Agent agentAt(Role r, uint32_t now) {
  const uint32_t t = (now - storyStart) % STORY_MS;
  Agent a = {AgentStatus::Waiting, "", "", now};
  for (const Event &e : STORY) {
    if (e.agent == r && e.at <= t) a = {e.status, e.skill, e.text, now - (t - e.at)};
  }
  return a;
}

void line(const Stage &st, float x0, float y0, float x1, float y1, float r, uint16_t color) {
  const float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
  if (len <= 0) return;
  capsule(st, (x0 + x1) / 2, (y0 + y1) / 2, dx / len, dy / len, len / 2, r, color);
}

// Upper half of a filled ellipse: the builder's hard hat.
void dome(const Stage &st, float cx, float cy, float rx, float ry, uint16_t color) {
  for (int y = static_cast<int>(cy - ry - 1); y <= static_cast<int>(cy); y++) {
    const float ny = (y + 0.5f - cy) / ry;
    for (int x = static_cast<int>(cx - rx - 1); x <= static_cast<int>(cx + rx + 1); x++) {
      const float nx = (x + 0.5f - cx) / rx;
      plot(st, x, y, color, clamp01(0.5f - (sqrtf(nx * nx + ny * ny) - 1) * ry));
    }
  }
}

// Magnifying glass: steel rim, faint blue lens, wooden handle to the lower right.
void magnifier(const Stage &st, float cx, float cy, float rad) {
  const float t = rad * 0.18f;
  for (int y = static_cast<int>(cy - rad - t - 1); y <= static_cast<int>(cy + rad + t + 1); y++) {
    for (int x = static_cast<int>(cx - rad - t - 1); x <= static_cast<int>(cx + rad + t + 1); x++) {
      const float px = x + 0.5f - cx, py = y + 0.5f - cy;
      const float d = sqrtf(px * px + py * py) - rad;
      plot(st, x, y, GROK, 0.25f * clamp01(-d - t + 0.5f));
      plot(st, x, y, STEEL, clamp01(0.5f - (fabsf(d) - t)));
    }
  }
  const float ax = 0.6f, ay = 0.8f;
  line(st, cx + ax * (rad + t), cy + ay * (rad + t), cx + ax * (rad * 2.1f), cy + ay * (rad * 2.1f), rad * 0.2f, WOOD);
}

void flask(const Stage &st, AgentStatus s, uint32_t now) {
  const float x = 214, bulbY = 180;
  const uint16_t liquid = s == AgentStatus::Blocked ? RED : (s == AgentStatus::Waiting ? blend(GREEN, WALL, 0.5f) : GREEN);
  capsule(st, x, 157, 0, 1, 9, 6, GLASS);
  ellipse(st, x, bulbY, 15, 15, GLASS);
  capsule(st, x, 157, 0, 1, 9, 3.5f, blend(GLASS, WALL, 0.6f));
  ellipse(st, x, bulbY + 1, 12, 12, liquid);
  if (s != AgentStatus::Working && s != AgentStatus::Blocked) return;
  const uint32_t period = s == AgentStatus::Blocked ? 520 : 1000;
  for (int j = 0; j < 3; j++) {
    const float t = static_cast<float>((now + j * period / 3) % period) / period;
    const float bx = x + 3 * sinf(t * 6 + j), by = bulbY - t * 58;
    ellipse(st, bx, by, 3.2f * (1 - 0.5f * t), 3.2f * (1 - 0.5f * t), blend(liquid, WHITE, 0.4f));
  }
}

void monitor(const Stage &st, Role role, const Agent &a, uint32_t now) {
  rect(st, SCREEN_X - 3, SCREEN_Y - 3, SCREEN_W + 6, SCREEN_H + 6, BEZEL);
  rect(st, 60, SCREEN_Y + SCREEN_H + 3, 12, DESK_Y - (SCREEN_Y + SCREEN_H + 3), BEZEL);
  rect(st, 46, DESK_Y - 4, 40, 4, BEZEL);
  const float mx = SCREEN_X + SCREEN_W / 2.0f, my = SCREEN_Y + SCREEN_H / 2.0f;
  switch (a.status) {
    case AgentStatus::Working: {
      rect(st, SCREEN_X, SCREEN_Y, SCREEN_W, SCREEN_H, c565(0x0d, 0x1b, 0x2a));
      if (role == TESTER) {  // progress bar filling, a green tick per test
        const float f = clamp01((now - a.since) / 5000.0f);
        rect(st, SCREEN_X + 8, SCREEN_Y + 40, SCREEN_W - 16, 8, BEZEL);
        rect(st, SCREEN_X + 8, SCREEN_Y + 40, static_cast<int>((SCREEN_W - 16) * f), 8, GREEN);
        for (int k = 0; k < static_cast<int>(f * 10); k++) rect(st, SCREEN_X + 8 + k * 8, SCREEN_Y + 14, 5, 12, GREEN);
        break;
      }
      static const uint8_t widths[] = {58, 34, 70, 46, 22, 64, 40, 52};
      static const uint16_t colors[] = {GROK, GREEN, PURPLE, GROK, AMBER, GREEN, GROK, PURPLE};
      const uint32_t step = role == REVIEWER ? 600 : 220;
      const uint32_t scroll = now / step;
      for (int k = 0; k < 6; k++) {
        const int idx = (k + scroll) % 8;
        int w = widths[idx];
        if (k == 5 && role == BUILDER) w = static_cast<int>(w * static_cast<float>(now % step) / step);
        const int indent = (idx % 3) * 6;
        rect(st, SCREEN_X + 6 + indent, SCREEN_Y + 6 + k * 10, w - indent, 5, colors[idx]);
      }
      if (role == REVIEWER) {  // highlighted line under review
        const int k = static_cast<int>((now / 900) % 6);
        for (int y = SCREEN_Y + 3 + k * 10; y < SCREEN_Y + 14 + k * 10; y++)
          for (int x = SCREEN_X; x < SCREEN_X + SCREEN_W; x++) plot(st, x, y, AMBER, 0.22f);
      }
      if (role == BUILDER && (now / 400) % 2) rect(st, SCREEN_X + 6, SCREEN_Y + 58, 6, 5, WHITE);
      break;
    }
    case AgentStatus::Blocked:
      rect(st, SCREEN_X, SCREEN_Y, SCREEN_W, SCREEN_H, c565(0x4a, 0x0d, 0x12));
      line(st, mx - 16, my - 16, mx + 16, my + 16, 4, RED);
      line(st, mx - 16, my + 16, mx + 16, my - 16, 4, RED);
      break;
    case AgentStatus::Waiting: {
      rect(st, SCREEN_X, SCREEN_Y, SCREEN_W, SCREEN_H, c565(0x14, 0x1a, 0x24));
      const int lit = static_cast<int>((now / 350) % 3);
      for (int j = 0; j < 3; j++) ellipse(st, mx - 16 + 16 * j, my, 4, 4, j == lit ? AMBER : blend(AMBER, WALL, 0.6f));
      break;
    }
    case AgentStatus::Done:
      rect(st, SCREEN_X, SCREEN_Y, SCREEN_W, SCREEN_H, c565(0x0f, 0x33, 0x20));
      line(st, mx - 16, my + 1, mx - 5, my + 12, 4, GREEN);
      line(st, mx - 5, my + 12, mx + 17, my - 12, 4, GREEN);
      break;
  }
}

Pose poseFor(int i, const Agent &a, uint32_t now) {
  Pose p;
  const uint32_t t = now + i * 577;  // desks move out of step
  switch (a.status) {
    case AgentStatus::Working:
      p.lift = 3 + 2 * wave(t, 520);
      p.lookX = -1;
      p.lookY = 0.45f;
      p.blink = blinkAt(t, 3700);
      break;
    case AgentStatus::Blocked: {
      p.eyeScale = 1.15f;
      p.lookX = -0.3f;
      const uint32_t s = t % 1400;
      if (s < 300) p.dx = 5 * sinf(TAU_F * s / 80.0f);
      p.blink = blinkAt(t, 5300);
      break;
    }
    case AgentStatus::Waiting: {
      static const Look keys[] = {{0, 0}, {-0.6f, -0.3f}, {0.3f, -0.4f}, {0, 0.1f}};
      p.lift = 2 + 2 * wave(t, 2600);
      const Look l = lookPath(t, keys, 4, 1700);
      p.lookX = l.x;
      p.lookY = l.y;
      p.blink = blinkAt(t, 3900);
      break;
    }
    case AgentStatus::Done: {
      const uint32_t e = now - a.since;
      p.happy = true;
      if (e < CELEBRATE_MS) {
        const float h = static_cast<float>(e % 640) / 640.0f;
        p.lift = 26 * sinf(PI_F * h);
        if (p.lift < 5) {
          const float k = 1 - p.lift / 5;
          p.sx = 1 + 0.07f * k;
          p.sy = 1 - 0.07f * k;
        }
      } else {
        p.lift = 3 + 3 * wave(t, 2000);
      }
      break;
    }
  }
  return p;
}

uint32_t hash(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  return x ^ (x >> 16);
}

void aboveHead(const Stage &st, const Agent &a, uint32_t now, float cx, float top) {
  Sprite *sp = st.sprite;
  switch (a.status) {
    case AgentStatus::Blocked:
      ellipse(st, cx, top - 24, 14, 14, RED);
      sp->setTextSize(3);
      sp->setTextColor(WHITE);
      sp->setCursor(static_cast<int16_t>(cx - 8), static_cast<int16_t>(top - 35));
      sp->print('!');
      break;
    case AgentStatus::Waiting: {
      const int lit = static_cast<int>((now / 350) % 3);
      for (int j = 0; j < 3; j++) ellipse(st, cx - 16 + 16 * j, top - 18, 4, 4, j == lit ? AMBER : blend(AMBER, WALL, 0.6f));
      break;
    }
    case AgentStatus::Done: {
      const uint32_t e = now - a.since;
      if (e >= CELEBRATE_MS + 800) {
        line(st, cx - 9, top - 20, cx - 3, top - 14, 2.5f, GREEN);
        line(st, cx - 3, top - 14, cx + 9, top - 27, 2.5f, GREEN);
        break;
      }
      static const uint16_t confetti[] = {AMBER, RED, GREEN, GROK, PURPLE, WHITE};
      for (uint32_t j = 0; j < 16; j++) {
        const uint32_t h = hash(j * 2654435761U + a.since);
        const float fx = cx + static_cast<float>(h % 150) - 75;
        const float fy = top - 60 + e * 0.07f + static_cast<float>((h >> 8) % 50);
        const float sway = 6 * sinf(e / 180.0f + j);
        if (fy > DESK_Y - 4) continue;
        rect(st, static_cast<int>(fx + sway), static_cast<int>(fy), 4, 6, confetti[(h >> 16) % 6]);
      }
      break;
    }
    case AgentStatus::Working:
      break;
  }
}

void textCentered(Sprite *sp, const char *s, int y, int size, uint16_t color, int w) {
  sp->setTextSize(size);
  sp->setTextColor(color);
  sp->setCursor(static_cast<int16_t>((w - static_cast<int>(strlen(s)) * 6 * size) / 2), static_cast<int16_t>(y));
  sp->print(s);
}

// Word-wrapped size-2 text, up to `maxLines` lines of TEXT_CHARS.
void textWrapped(Sprite *sp, const char *s, int x, int y, int maxLines, uint16_t color) {
  sp->setTextSize(2);
  sp->setTextColor(color);
  char buf[TEXT_CHARS + 1];
  for (int lineNo = 0; lineNo < maxLines && *s; lineNo++) {
    while (*s == ' ') s++;
    int n = static_cast<int>(strlen(s));
    if (n > TEXT_CHARS) {
      n = TEXT_CHARS;
      while (n > 0 && s[n] != ' ') n--;
      if (n == 0) n = TEXT_CHARS;
    }
    memcpy(buf, s, n);
    buf[n] = '\0';
    sp->setCursor(static_cast<int16_t>(x), static_cast<int16_t>(y + lineNo * 20));
    sp->print(buf);
    s += n;
  }
}

void label(const Stage &st, Role role, const Agent &a, bool open) {
  Sprite *sp = st.sprite;
  const uint16_t col = statusColor(a.status);
  if (!open) {
    textCentered(sp, ROLE_NAME[role], 276, 3, WHITE, st.w);
    const char *word = statusWord(a.status);
    capsule(st, st.w / 2.0f, 324, 1, 0, 50, 15, col);
    textCentered(sp, word, 317, 2, DARK, st.w);
    textCentered(sp, a.skill, 352, 2, DIM, st.w);
    return;
  }
  rect(st, 6, CARD_Y, st.w - 12, 114, col);
  rect(st, 8, CARD_Y + 2, st.w - 16, 110, CARD);
  sp->setTextSize(2);
  sp->setTextColor(col);
  sp->setCursor(14, CARD_Y + 8);
  sp->print(ROLE_NAME[role]);
  sp->setTextColor(DIM);
  sp->setCursor(static_cast<int16_t>(st.w - 14 - static_cast<int>(strlen(SKILL)) * 12), CARD_Y + 8);
  sp->print(SKILL);
  sp->setTextColor(GROK);
  sp->setCursor(14, CARD_Y + 30);
  sp->print(a.skill);
  textWrapped(sp, a.text, 14, CARD_Y + 52, 3, WHITE);
}

void drawDesk(int i, uint32_t now) {
  const Stage &st = desks[i];
  const Role role = static_cast<Role>(i);
  const Agent a = agentAt(role, now);
  rect(st, 0, 0, st.w, OFFICE_WALL_H, WALL);
  rect(st, 0, OFFICE_WALL_H, st.w, 4, BASEBOARD);
  rect(st, 0, OFFICE_WALL_H + 4, st.w, st.h - OFFICE_WALL_H - 4, FLOOR);

  const Pose p = poseFor(i, a, now);
  float cx, top;
  drawOrb(st, p, WALL, &cx, &top, false);
  if (role == BUILDER) {
    const float cy = top + st.r * p.sy;
    dome(st, cx, cy - 0.68f * st.r * p.sy, 0.86f * st.r * p.sx, 0.5f * st.r * p.sy, HAT);
    capsule(st, cx, cy - 0.68f * st.r * p.sy, 1, 0, 0.95f * st.r * p.sx, 0.09f * st.r, HAT_BRIM);
  }

  rect(st, 8, DESK_Y, st.w - 16, DESK_FRONT_Y - DESK_Y, DESK_TOP);
  rect(st, 8, DESK_Y, st.w - 16, 2, DESK_EDGE);
  rect(st, 14, DESK_FRONT_Y, st.w - 28, DESK_BOTTOM - DESK_FRONT_Y, DESK_FRONT);
  rect(st, st.w / 2 - 18, DESK_FRONT_Y + 20, 36, 5, DESK_EDGE);
  monitor(st, role, a, now);
  if (role == TESTER) flask(st, a.status, now);
  if (role == REVIEWER) {
    if (a.status == AgentStatus::Working) {
      magnifier(st, SCREEN_X + SCREEN_W / 2.0f + 30 * wave(now, 1600), SCREEN_Y + SCREEN_H / 2.0f + 10 * wave(now + 400, 1100), 20);
    } else {
      magnifier(st, cx + 0.95f * st.r, top + st.r * 1.15f, 0.4f * st.r);
    }
  }
  aboveHead(st, a, now, cx, top);
  label(st, role, a, selected == i);
  st.sprite->flush();
}

}  // namespace

bool officeBegin(Arduino_GFX *out) {
  for (int i = 0; i < ROLES; i++) {
    desks[i] = {nullptr, OFFICE_DESK_W, OFFICE_H, ORB_R, ORB_CX, ORB_GROUND};
    desks[i].sprite = new Sprite(out, OFFICE_DESK_W, OFFICE_H, OFFICE_LEFT + i * (OFFICE_DESK_W + OFFICE_DESK_GAP), OFFICE_TOP);
    if (!desks[i].sprite->begin(GFX_SKIP_OUTPUT_BEGIN)) {
      Serial.println("office sprite allocation failed");
      for (int j = 0; j <= i; j++) {
        delete desks[j].sprite;
        desks[j].sprite = nullptr;
      }
      return false;
    }
    desks[i].sprite->setTextWrap(false);
  }
  storyStart = millis();
  return true;
}

uint16_t officeWallColor() { return WALL; }
uint16_t officeFloorColor() { return FLOOR; }

bool officeDraw(uint32_t now) {
  if (!desks[0].sprite) return false;
  if (selected >= 0 && now - selectedAt > SELECT_MS) selected = -1;
  for (int i = 0; i < ROLES; i++) drawDesk(i, now);
  return true;
}

bool officeTap(int x, int y, uint32_t now) {
  if (y < OFFICE_TOP || y >= OFFICE_TOP + OFFICE_H) return false;
  for (int i = 0; i < ROLES; i++) {
    const int left = OFFICE_LEFT + i * (OFFICE_DESK_W + OFFICE_DESK_GAP);
    if (x >= left && x < left + OFFICE_DESK_W) {
      selected = selected == i ? -1 : i;
      selectedAt = now;
      return true;
    }
  }
  return false;
}
