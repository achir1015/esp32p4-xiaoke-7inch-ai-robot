// ============================================================================
//  ESP32-P4 七吋 AI 語音視覺聊天機器人「小柯」（OpenAI 版）
//  硬體：Guition JC1060P470C（ESP32-P4 + ESP32-C6、7 吋 1024x600 觸控、ES8311、OV02C10 鏡頭）
//  流程：直接說話（聲控自動偵測，或按住螢幕）→ OpenAI 語音轉文字 → GPT 回答
//        （說「拍照/你看到什麼」會附上鏡頭畫面）→ OpenAI TTS 播放
//  螢幕：日期、24 小時制時間、農民曆 + 機器人表情（眨眼、東張西望、聆聽/思考/說話/想睡）
//  移植自 ESP32-S3 版小柯（AI_Voice_Robot）；開發環境 Arduino IDE 2.x + esp32 by Espressif 3.3.x
// ============================================================================
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <algorithm>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <time.h>
#include <vector>
#include <initializer_list>
#include "qrcodegen.h"
#include "mbedtls/base64.h"
#if __has_include("config.h")
#include "config.h"          // 自己的設定（不上傳 GitHub）
#else
#include "config.example.h"  // 範本：Wi-Fi 與 API Key 可開機後用網頁設定
#endif
#include "cjk_font.h"
#include "lunar_table.h"
#include "web_page.h"
#include "p4_hw.h"
#include "almanac.h"
#include "hal/brownout_hal.h"
#include "mp3dec.h"
#include "song_types.h"

// ------------------------------------------------------------------ 顏色 ----
constexpr uint16_t RGB(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
#define C_BLACK   0x0000
#define C_WHITE   0xFFFF
#define C_RED     0xF800
#define C_GREEN   0x07E0
#define C_BLUE    0x001F
#define C_CYAN    0x07FF
#define C_YELLOW  0xFFE0
#define C_ORANGE  0xFD20
#define C_GRAY    0x8410
#define C_BG      0x0000                // 背景不畫底色（黑）
#define C_BAR_LINE RGB(40, 52, 72)      // 分隔線
#define C_HELMET   RGB(232, 236, 242)   // 頭盔：白
#define C_VISOR    RGB(16, 30, 50)      // 面罩：深藍
#define C_GLOW     RGB(64, 232, 255)    // 發光表情：青色
#define C_GLOW_DIM RGB(30, 110, 135)
#define C_RED2     RGB(255, 70, 100)    // 生氣
#define C_PINK     RGB(255, 110, 150)   // 愛心／腮紅
#define C_PANEL    RGB(14, 22, 38)      // 農民曆卡片底色

#define SCREEN_W 512                    // 版面以 512x300 邏輯座標排版，畫到 1024x600 時放大 2 倍
#define SCREEN_H 300
#define BAR_H    44                     // 上方日期時間列高度
#define LINE_H   18                     // 中文字行高

// ------------------------------------------------------------------ 畫布 ----
// 整個畫面先畫在 PSRAM 的 1024x600 畫布（hiBuf），再用 DMA2D 一次送到螢幕，不會閃爍
uint16_t *hiBuf = nullptr;

static inline void hspan(int x0, int x1, int y, uint16_t c) {
  if ((unsigned)y >= LCD_H) return;
  if (x0 < 0) x0 = 0;
  if (x1 >= LCD_W) x1 = LCD_W - 1;
  uint16_t *p = hiBuf + y * LCD_W;
  for (int x = x0; x <= x1; x++) p[x] = c;
}
static void fillRectHi(int x, int y, int w, int h, uint16_t c) {
  for (int yy = y; yy < y + h; yy++) hspan(x, x + w - 1, yy, c);
}

// 文字與版面用的 2 倍放大畫布（Adafruit_GFX 介面，座標 512x300）
class Canvas2x : public Adafruit_GFX {
 public:
  Canvas2x() : Adafruit_GFX(SCREEN_W, SCREEN_H) {}
  void drawPixel(int16_t x, int16_t y, uint16_t c) override {
    if ((uint16_t)x >= SCREEN_W || (uint16_t)y >= SCREEN_H) return;
    uint16_t *b = hiBuf + (y * 2) * LCD_W + x * 2;
    b[0] = b[1] = b[LCD_W] = b[LCD_W + 1] = c;
  }
  void writePixel(int16_t x, int16_t y, uint16_t c) override { drawPixel(x, y, c); }
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) override { fillRectHi(x * 2, y * 2, w * 2, h * 2, c); }
  void writeFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) override { fillRect(x, y, w, h, c); }
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t c) override { fillRect(x, y, w, 1, c); }
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t c) override { fillRect(x, y, 1, h, c); }
  void writeFastHLine(int16_t x, int16_t y, int16_t w, uint16_t c) override { fillRect(x, y, w, 1, c); }
  void writeFastVLine(int16_t x, int16_t y, int16_t h, uint16_t c) override { fillRect(x, y, 1, h, c); }
  void fillScreen(uint16_t c) override {
    uint32_t v = c | (uint32_t)c << 16, *p = (uint32_t *)hiBuf;
    for (int i = 0; i < LCD_W * LCD_H / 2; i++) p[i] = v;
  }
};
Canvas2x *cv = nullptr;

// ---- 1024x600 原生解析度的繪圖（機器人用，邊緣才平滑）----
static uint16_t blend565(uint16_t a, uint16_t b, float t) {         // t = 0 → a，1 → b
  if (t <= 0) return a;
  if (t >= 1) return b;
  int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
  return ((int)(ar + (br - ar) * t) << 11) | ((int)(ag + (bg - ag) * t) << 5) | (int)(ab + (bb - ab) * t);
}
// 圓角矩形（上下漸層）
static void fillRRGrad(int x, int y, int w, int h, int r, uint16_t cTop, uint16_t cBot) {
  r = min(r, min(w, h) / 2);
  for (int yy = 0; yy < h; yy++) {
    int inset = 0;
    float dy = yy < r ? r - yy - 0.5f : yy >= h - r ? yy - (h - r) + 0.5f : -1;
    if (dy >= 0) inset = r - (int)sqrtf(max(0.0f, r * r - dy * dy));
    hspan(x + inset, x + w - 1 - inset, y + yy, h > 1 ? blend565(cTop, cBot, (float)yy / (h - 1)) : cTop);
  }
}
static void fillRR(int x, int y, int w, int h, int r, uint16_t c) { fillRRGrad(x, y, w, h, r, c, c); }
static void fillEllipse(int cx, int cy, int rx, int ry, uint16_t c) {
  if (rx < 1 || ry < 1) return;
  for (int dy = -ry; dy <= ry; dy++) {
    int dx = (int)(rx * sqrtf(max(0.0f, 1.0f - (float)(dy * dy) / (ry * ry))));
    hspan(cx - dx, cx + dx, cy + dy, c);
  }
}
static void fillCircleHi(int cx, int cy, int r, uint16_t c) { fillEllipse(cx, cy, r, r, c); }
// 粗線（兩端圓頭）
static void strokeLine(float x0, float y0, float x1, float y1, int th, uint16_t c) {
  float len = hypotf(x1 - x0, y1 - y0);
  int n = max(1, (int)(len / max(1.0f, th / 3.0f)));
  for (int i = 0; i <= n; i++) fillCircleHi(x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, th / 2, c);
}
// 粗弧線：角度以度為單位，0 = 右，90 = 下（螢幕座標）
static void strokeArc(int cx, int cy, int r, float a0, float a1, int th, uint16_t c) {
  float step = max(1.0f, th / 3.0f) / max(1, r) * 57.3f;
  for (float a = a0; a <= a1 + 0.01f; a += step)
    fillCircleHi(cx + r * cosf(a / 57.3f), cy + r * sinf(a / 57.3f), th / 2, c);
}
static void fillHeart(int x, int y, int r, uint16_t c) {
  int h = r * 55 / 100;
  fillCircleHi(x - r / 2, y, h, c);
  fillCircleHi(x + r / 2, y, h, c);
  for (int yy = 0; yy <= r * 6 / 5 - r / 6; yy++) {                 // 下方三角形
    int half = (r + 1) * (r * 6 / 5 - r / 6 - yy) / max(1, r * 6 / 5 - r / 6);
    hspan(x - half, x + half, y + r / 6 + yy, c);
  }
}

// 16px 繁中點陣字（GNU Unifont，見 cjk_font.h），y 為基線，透明背景
class CjkText {
 public:
  Adafruit_GFX *g = nullptr;
  void setTarget(Adafruit_GFX *t) { g = t; }
  void setForegroundColor(uint16_t c) { fg = c; }

  int getUTF8Width(const char *s) {
    int w = 0;
    while (*s) w += glyphWidth(nextCodepoint(s));
    return w;
  }

  void drawUTF8(int x, int y, const char *s) {
    y -= 14;
    while (*s) {
      uint32_t cp = nextCodepoint(s);
      int idx = findGlyph(cp);
      int w = glyphWidth(cp);
      if (x + w > g->width()) break;
      if (idx >= 0) {
        const uint8_t *b = &cjkBitmaps[idx * 32];
        for (int r = 0; r < 16; r++) {
          uint16_t bits = (b[r * 2] << 8) | b[r * 2 + 1];
          for (int c = 0; c < w; c++)
            if (bits & (0x8000 >> c)) g->drawPixel(x + c, y + r, fg);
        }
      }
      x += w;
    }
  }

  // UTF-8 自動換行：英文單字/網址/數字不拆開，標點符號不放在行首（允許超出 16px）
  std::vector<String> wrap(const String &s, int w) {
    static const char *NO_START = "，。、；：！？」』）》〉…,.!?;:)]";
    std::vector<String> lines;
    String line; int lineW = 0;
    size_t i = 0;
    while (i < s.length()) {
      uint8_t c = s[i];
      if (c == '\r') { i++; continue; }
      if (c == '\n') { lines.push_back(line); line = ""; lineW = 0; i++; continue; }
      String tok;
      if (c < 0x80 && c != ' ') {                     // 連續英數字當成一個字
        size_t j = i;
        while (j < s.length() && (uint8_t)s[j] < 0x80 && s[j] != ' ' && s[j] != '\n') j++;
        tok = s.substring(i, j); i = j;
      } else {
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        tok = s.substring(i, i + len); i += len;
      }
      int tw = getUTF8Width(tok.c_str());
      bool noStart = strstr(NO_START, tok.c_str()) != nullptr;
      if (lineW > 0 && lineW + tw > w && !(noStart && lineW + tw <= w + 16)) {
        lines.push_back(line); line = ""; lineW = 0;
        if (tok == " ") continue;
      }
      if (tw > w) {                                   // 超長英文：逐字硬切
        for (size_t k = 0; k < tok.length(); k++) {
          if (lineW + 8 > w) { lines.push_back(line); line = ""; lineW = 0; }
          line += tok[k]; lineW += 8;
        }
      } else { line += tok; lineW += tw; }
    }
    if (line.length()) lines.push_back(line);
    return lines;
  }

 private:
  uint16_t fg = 0xFFFF;

  static uint32_t nextCodepoint(const char *&s) {
    uint8_t c = *s++;
    if (c < 0x80) return c;
    int n = (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : 3;
    uint32_t cp = c & (0x3F >> n);
    while (n-- && (*s & 0xC0) == 0x80) cp = (cp << 6) | (*s++ & 0x3F);
    return cp;
  }
  static int findGlyph(uint32_t cp) {
    if (cp > 0xFFFF) return -1;
    int lo = 0, hi = CJK_FONT_COUNT - 1;
    while (lo <= hi) {
      int m = (lo + hi) / 2;
      if (cjkCodes[m] == cp) return m;
      if (cjkCodes[m] < cp) lo = m + 1; else hi = m - 1;
    }
    return -1;
  }
  static int glyphWidth(uint32_t cp) {
    int i = findGlyph(cp);
    if (i < 0) return cp < 0x80 ? 8 : 16;
    return (cjkWide[i / 8] >> (i % 8)) & 1 ? 16 : 8;
  }
};
CjkText u8f;

// ---------------------------------------------------------------- UI 狀態 ----
enum UiState { UI_IDLE, UI_LISTEN, UI_THINK, UI_SPEAK, UI_MSG, UI_HELP, UI_SONG, UI_CAMERA, UI_QR, UI_VIDEO, UI_PORTAL };
volatile UiState uiState = UI_IDLE;
String uiTitle, uiText;
uint32_t uiSince = 0;
bool uiSad = false;
SemaphoreHandle_t uiLock = nullptr;
volatile int micLevel = 0;         // 目前麥克風音量（給 UI 顯示）
volatile int speakLevel = 0;       // 目前播放音量（嘴巴開合）
volatile uint32_t lastInteraction = 0;
uint16_t *photoPix = nullptr;      // 最近一張照片（1024x576 RGB565，PSRAM）
volatile bool photoShow = false;   // 對話中顯示照片
volatile uint32_t photoAt = 0;
volatile bool cameraActive = false;     // 攝影模式：待機畫面變成鏡頭即時影像，仍可聽指令
volatile bool recActive = false;        // 錄影中（錄影迴圈抓影格，UI 只顯示最新一張）
volatile uint32_t recStart = 0;
const uint8_t *volatile recLastFrame = nullptr;
volatile size_t recLastLen = 0;

// ---- 唱歌：YouTube 歌單 ----
struct Song { String id, title; };
std::vector<Song> songs;
SemaphoreHandle_t songLock = nullptr;
uint16_t *thumbPix = nullptr;      // 歌曲封面 320x180 RGB565（PSRAM）
volatile bool thumbOk = false;
volatile bool portalOpen = false;                          // 工作站選單開著（回答問題後回到選單）
volatile bool songPlaying = false, songLoading = false;   // 播放雲端 MP3 中／下載中
volatile uint32_t songPosMs = 0, songDurMs = 0;
uint8_t qrMods[45 * 45];           // QR 碼模組（最多 version 7 = 45x45）
volatile int qrSize = 0;

void setUi(UiState s, const String &title = "", const String &text = "", bool sad = false) {
  xSemaphoreTake(uiLock, portMAX_DELAY);
  uiState = s; uiTitle = title; uiText = text; uiSad = sad; uiSince = millis();
  xSemaphoreGive(uiLock);
}

// ------------------------------------------------------------------ 音訊 ----
volatile int volumePct = DEFAULT_VOLUME;

#define TTS_BUF_BYTES (3 * 1024 * 1024)        // 約 65 秒語音
#define TTS_PREBUFFER (TTS_SAMPLE_RATE * 2 / 2)  // 先緩衝 0.5 秒
struct {
  uint8_t *buf = nullptr;
  size_t cap = 0;
  volatile size_t written = 0;
  volatile size_t played = 0;
  volatile bool downloadDone = true;
  volatile bool playing = false;
} tts;
TaskHandle_t playTaskHandle;

// 錄音：44 bytes WAV header + PCM（PSRAM）
uint8_t *wavBuf = nullptr;
const size_t MAX_SAMPLES = MIC_SAMPLE_RATE * MAX_RECORD_SEC;
#define FRAME       320                        // 20ms @16kHz
#define PREROLL_FR  15                         // 保留觸發前 300ms，避免吃掉第一個字
int16_t preroll[PREROLL_FR][FRAME];
int prerollHead = 0, prerollCount = 0;
float noiseFloor = 150;
float lastRecAvgRms = 0;           // 上一段錄音的平均音量
bool lastRecHitMax = false;        // 上一段錄音是否一路沒停頓錄到上限（多半是音樂/噪音）
const float NOISE_FLOOR_MAX = 3000;

// 聽到的是噪音或音樂：把門檻提高到這個音量，避免同樣的聲音一直觸發（安靜後會自動降回）
void raiseNoiseFloor(float level) {
  float nf = min(NOISE_FLOOR_MAX, max(noiseFloor, level));
  if (nf > noiseFloor) Serial.printf("（噪音 %.0f → 門檻提高到 %d）\n", level, (int)(nf * VAD_RATIO));
  noiseFloor = nf;
}
int32_t dcState = 0;

// ------------------------------------------------------------------ 其它 ----
bool cameraOK = false;
String histRole[MAX_HISTORY_MSGS];
String histText[MAX_HISTORY_MSGS];
int histCount = 0;
String chatContext;                // 這次對話要附給 AI 的資料（班表）
const char *OPENAI_HOST = "https://api.openai.com";

// ---- 可由網頁修改的設定（存在 NVS；沒存過就用 config.h 的值）----
struct Settings { String ssid, pass, apiKey, gdrive, playlist; } cfg;
Preferences prefs;
WebServer server(80);
DNSServer dnsServer;
volatile bool portalMode = false;
char ipText[24] = "";
#define AP_SSID "XiaoKe-Setup"
#define AP_PASS "xiaoke123"

String cleanDefault(const char *v, const char *placeholder) {
  return strstr(v, placeholder) ? String("") : String(v);
}
// 鏡頭色彩：白平衡模式 0 自動 1 晴天 2 陰天 3 日光燈 4 鎢絲燈；飽和度/亮度/對比 -2 ~ 2
struct CamTune { int wb = 0, sat = 0, bri = 0, con = 0; } camTune;

void applyCamTune() {
  if (!cameraOK) return;
  camSetColor(CAM_SATURATION + camTune.sat * 32, camTune.bri * 20, 128 + camTune.con * 20);   // 白平衡由 ISP 自動
}

void loadCamTune() {
  prefs.begin("robot", true);
  camTune.wb = prefs.getInt("camWb", 0);
  camTune.sat = prefs.getInt("camSat", 0);
  camTune.bri = prefs.getInt("camBri", 0);
  camTune.con = prefs.getInt("camCon", 0);
  prefs.end();
}

void loadSettings() {
  prefs.begin("robot", true);
  cfg.ssid   = prefs.isKey("ssid")   ? prefs.getString("ssid")   : cleanDefault(WIFI_SSID, "你的WiFi");
  cfg.pass   = prefs.isKey("pass")   ? prefs.getString("pass")   : cleanDefault(WIFI_PASSWORD, "你的WiFi");
  cfg.apiKey = prefs.isKey("apikey") ? prefs.getString("apikey") : cleanDefault(OPENAI_API_KEY, "請填入");
  cfg.gdrive = prefs.isKey("gdrive") ? prefs.getString("gdrive") : String(GDRIVE_SCRIPT_URL);
  cfg.playlist = prefs.isKey("playlist") ? prefs.getString("playlist") : String(YT_PLAYLIST_URL);
  prefs.end();
}
String authHeader() { return "Bearer " + cfg.apiKey; }

// ============================================================================
//  按鍵：觸控螢幕（任何位置）或板上 BOOT 鍵
// ============================================================================
bool buttonPressed() {
  return touchPressed() || digitalRead(PIN_BOOT) == LOW;
}

void updateVolumeFromPot() {}            // 本板沒有音量旋鈕（用語音「大聲一點／小聲一點」調整）

// ============================================================================
//  畫面：白色頭盔 + 深藍面罩 + 青色發光表情（背景不畫底色）
// ============================================================================
enum Expr { EX_NORMAL, EX_HAPPY, EX_WINK, EX_LOVE, EX_SURPRISE, EX_SAD, EX_ANGRY, EX_SHY,
            EX_BORED, EX_SLEEPY, EX_LISTEN, EX_THINK, EX_DEAD };
volatile int speakExpr = EX_HAPPY;      // GPT 回覆帶的心情，說話時顯示

// 發光：先畫兩層淡色光暈，再畫本體
static void glowRR(int x, int y, int w, int h, int r, uint16_t c, int g) {
  fillRR(x - g, y - g, w + 2 * g, h + 2 * g, r + g, blend565(C_VISOR, c, 0.22f));
  fillRR(x - g / 2, y - g / 2, w + g, h + g, r + g / 2, blend565(C_VISOR, c, 0.45f));
  fillRR(x, y, w, h, r, c);
}
static void glowArc(int cx, int cy, int r, float a0, float a1, int th, uint16_t c) {
  strokeArc(cx, cy, r, a0, a1, th + th, blend565(C_VISOR, c, 0.25f));
  strokeArc(cx, cy, r, a0, a1, th, c);
}
static void glowLine(float x0, float y0, float x1, float y1, int th, uint16_t c) {
  strokeLine(x0, y0, x1, y1, th + th, blend565(C_VISOR, c, 0.25f));
  strokeLine(x0, y0, x1, y1, th, c);
}

// vx,vy = 面罩中心（512x300 邏輯座標）；s = 縮放（1 = 頭盔寬 248）；lx,ly = 眼神方向；mouthOpen = 說話張嘴程度（0~1）
void drawRobot(int vx, int vy, float s, int ex, float lx, float ly, bool blink,
               float mouthOpen, uint32_t now, uint16_t /*antennaColor*/, bool /*signal*/) {
  const float k = s * 2;                                      // 換成 1024x600 像素
  const int X = vx * 2, Y = vy * 2;
  auto S = [&](float v) { return (int)(v * k + 0.5f); };
  uint16_t glow = ex == EX_ANGRY ? C_RED2 : C_GLOW;

  // ---- 耳罩（灰藍，左右兩側，在頭盔後面）----
  for (int side = -1; side <= 1; side += 2) {
    int cx = X + side * S(121);
    fillRRGrad(cx - S(19), Y - S(44), S(38), S(92), S(16), RGB(176, 190, 210), RGB(104, 120, 146));
    fillRR(cx - S(5) + side * S(6), Y - S(30), S(9), S(56), S(5), RGB(206, 216, 232));   // 反光
  }

  // ---- 頭盔：陰影 + 白色漸層 + 左上反光 ----
  fillRR(X - S(125), Y - S(96), S(250), S(198), S(90), RGB(128, 138, 154));
  fillRRGrad(X - S(124), Y - S(99), S(248), S(194), S(88), RGB(252, 253, 255), RGB(196, 203, 214));
  fillEllipse(X - S(62), Y - S(80), S(34), S(9), RGB(255, 255, 255));

  // ---- 面罩：外框 + 深藍漸層 + 玻璃反光 ----
  fillRR(X - S(102), Y - S(71), S(204), S(142), S(56), RGB(52, 70, 96));
  fillRRGrad(X - S(98), Y - S(67), S(196), S(134), S(52), RGB(30, 52, 80), RGB(9, 18, 32));
  fillEllipse(X - S(50), Y - S(52), S(32), S(7), RGB(44, 70, 102));

  const int ey = Y - S(10), dx = S(44), my = Y + S(34);
  const int th = max(3, S(5));                                // 線條粗細
  bool talking = mouthOpen > 0.05f;
  bool smiley = ex == EX_NORMAL || ex == EX_HAPPY || ex == EX_WINK || ex == EX_LOVE || ex == EX_SHY;

  // ---- 腮紅（害羞、愛心）----
  if (ex == EX_SHY || ex == EX_LOVE)
    for (int side = -1; side <= 1; side += 2)
      fillEllipse(X + side * S(68), Y + S(20), S(15), S(6), blend565(C_VISOR, C_PINK, 0.55f));

  // ---- 嘴巴 ----
  if (talking) {
    int rx = S(15), ry = max(S(4), S(4 + 13 * mouthOpen));
    fillEllipse(X, my - S(4), rx + S(3), ry + S(3), blend565(C_VISOR, glow, 0.25f));
    fillEllipse(X, my - S(4), rx, ry, glow);
    if (ry > S(7)) fillEllipse(X, my - S(4), rx - S(5), ry - S(5), RGB(14, 60, 82));
  } else if (smiley) {
    glowArc(X, my - S(18), S(18), 35, 145, th, glow);         // 微笑 ︶
  } else if (ex == EX_SURPRISE || ex == EX_LISTEN) {
    int r = S(ex == EX_SURPRISE ? 9 : 7) + (ex == EX_LISTEN ? min(S(6), (int)(micLevel / 600)) : 0);
    fillCircleHi(X, my - S(2), r + S(3), blend565(C_VISOR, glow, 0.25f));
    fillCircleHi(X, my - S(2), r, glow);
    if (ex == EX_LISTEN) fillCircleHi(X, my - S(2), max(1, r - th), C_VISOR);
  } else if (ex == EX_SAD || ex == EX_ANGRY) {
    glowArc(X, my + S(12), S(16), 215, 325, th, glow);        // 嘴角下垂 ︵
  } else if (ex == EX_THINK) {
    for (int i = 0; i < 3; i++)
      fillCircleHi(X + (i - 1) * S(14), my - S(2), S(4), (now / 250) % 3 == (unsigned)i ? glow : C_GLOW_DIM);
  } else if (ex == EX_SLEEPY) {
    glowArc(X, my + S(4), S(8), 200, 340, th, glow);
  } else {                                                    // BORED / DEAD：一字嘴
    glowLine(X - S(13), my - S(2), X + S(13), my - S(2), th, glow);
  }

  // ---- 眼睛 ----
  const int ew = S(22), eh = S(32);
  for (int side = -1; side <= 1; side += 2) {
    int cx = X + side * dx, x = cx + (int)(lx * 2), y = ey + (int)(ly * 2);
    switch (ex) {
      case EX_HAPPY: case EX_SHY:                             // 瞇眼笑 ∩
        glowArc(cx, ey + S(8), S(12), 200, 340, th + 1, glow); break;
      case EX_WINK:                                           // 左眼眨（一條線），右眼張開
        if (side < 0) glowLine(cx - S(11), ey, cx + S(11), ey, th + 1, glow);
        else glowRR(x - ew / 2, y - eh / 2, ew, eh, ew / 2, glow, S(5));
        break;
      case EX_LOVE: {
        int r = S(15) + (int)(S(2) * sinf(now / 150.0f));
        fillHeart(cx, ey - S(2), r + S(3), blend565(C_VISOR, C_PINK, 0.3f));
        fillHeart(cx, ey - S(2), r, C_PINK);
        break;
      }
      case EX_SURPRISE:
        fillCircleHi(cx, ey, S(14), blend565(C_VISOR, glow, 0.3f));
        fillCircleHi(cx, ey, S(10), glow);
        break;
      case EX_BORED:                                          // 無聊：一條線
        glowLine(cx - S(12), ey + S(2), cx + S(12), ey + S(2), th + 1, glow); break;
      case EX_SLEEPY:                                         // 想睡：∪
        glowArc(cx, ey - S(6), S(11), 20, 160, th, glow); break;
      case EX_DEAD:
        glowLine(cx - S(9), ey - S(9), cx + S(9), ey + S(9), th, glow);
        glowLine(cx - S(9), ey + S(9), cx + S(9), ey - S(9), th, glow);
        break;
      case EX_THINK:
        glowRR(cx + S(6) - ew / 2, ey - S(8) - S(13), ew, S(26), ew / 2, glow, S(5)); break;
      default: {                                              // NORMAL / LISTEN / SAD / ANGRY
        int w = ex == EX_LISTEN ? S(25) : ew, h = ex == EX_LISTEN ? S(36) : ex == EX_ANGRY ? S(24) : ex == EX_SAD ? S(26) : eh;
        if (blink && ex != EX_LISTEN) glowLine(x - w / 2, y, x + w / 2, y, th + 1, glow);
        else glowRR(x - w / 2, y - h / 2, w, h, w / 2, glow, S(5));
        if (ex == EX_SAD)                                     // 擔心：眉毛內側上揚
          glowLine(x - S(13), y - h / 2 - S(7) + S(5) * side, x + S(13), y - h / 2 - S(7) - S(5) * side, th, glow);
        if (ex == EX_ANGRY)                                   // 生氣：眉毛內側下壓
          glowLine(x - S(13), y - h / 2 - S(9) - S(6) * side, x + S(13), y - h / 2 - S(9) + S(6) * side, th, glow);
      }
    }
  }

  // ---- 想睡的 Zzz（畫在頭盔外右上）----
  if (ex == EX_SLEEPY) {
    int kz = (now / 500) % 3;
    cv->setTextColor(C_GLOW);
    cv->setTextSize(max(1, (int)(2 * s + 0.5f)));
    cv->setCursor(vx + (int)(100 * s), vy - (int)(96 * s) - kz * 3);
    cv->print("z");
    cv->setTextSize(max(1, (int)(3 * s + 0.5f)));
    cv->setCursor(vx + (int)(114 * s), vy - (int)(116 * s) - kz * 3);
    cv->print("Z");
  }
}

// ---- 功能說明（按鍵翻頁）----
#define DEVELOPER_CREDIT "此系統是吳玉柱先生與 Claude AI 共同開發，有任何意見請聯絡："
#define DEVELOPER_EMAIL  "achir1015@gmail.com"
struct HelpLine { String text; uint16_t color; };
std::vector<std::vector<HelpLine>> helpPages;
volatile int helpPage = 0;
const int HELP_LINES = 11;

void buildHelp() {
  struct Para { const char *t; uint16_t c; };
  static const Para P[] = {
    {"一、系統功能", C_YELLOW},
    {"【聊天與查詢】", C_GLOW},
    {"1. 聲控聊天：直接說話，停頓一下就自動送出；環境吵雜時「按住螢幕說話，放開送出」。", C_WHITE},
    {"2. 上網查詢：問天氣、新聞、股價、比賽結果等即時資訊，" ROBOT_NAME "會自己上網搜尋再用口語回答。", C_WHITE},
    {"3. 班表查詢：問「我明天上什麼班」「這個月休幾天」「誰明天上大夜」，讀取病房助理與護理師月班表回答。", C_WHITE},
    {"4. 工作流程：問「白班幾點交班」「大夜班有哪些工作」「現在該做什麼」，依工作時間軸回答。", C_WHITE},
    {"5. " PORTAL_NAME "：點主畫面右下角按鈕或說「工作站」開啟觸控選單，點工具看說明與 QR 碼；技術稽核、督導手冊、學科題庫、術科、長照組合的內容可以直接用說的問（例如「冰枕的稽核項目有哪些」）。", C_WHITE},
    {"【影像】", C_GLOW},
    {"6. 拍照辨識：說「拍照」「這是什麼」「上面寫什麼」，拍照後回答，照片顯示在螢幕並存到雲端硬碟。", C_WHITE},
    {"7. 攝影：說「攝影」「開鏡頭」顯示 200 萬畫素鏡頭即時影像（綠字攝影中），仍可繼續下指令；說「關閉鏡頭」或點螢幕回主畫面。", C_WHITE},
    {"8. 錄製：攝影中說「錄製」開始錄有聲影片（紅字錄製中），說「停止」後說「存檔中」並上傳雲端硬碟，最長約 2 分鐘。", C_WHITE},
    {"9. 鏡頭色彩：在設定網頁可即時調整白平衡、飽和度、亮度、對比。", C_WHITE},
    {"【娛樂與生活】", C_GLOW},
    {"10. 唱歌：說「唱首歌」隨機唱一首" SONG_COMPOSER "的創作歌曲，也可以點歌（例如「唱千江有水千江月」）；歌曲放在雲端硬碟資料夾，點螢幕停止。", C_WHITE},
    {"11. 時間與農民曆：上方顯示日期、星期、農曆與 24 小時制時間；右側農民曆顯示干支、生肖、宜忌、沖煞與節氣（建除十二神簡易版，僅供參考），也可以問「今天宜什麼」「今天農曆幾號」。", C_WHITE},
    {"12. 表情：依心情開心、愛心、驚訝、難過、生氣、眨眼、害羞；太久沒互動會想睡覺。", C_WHITE},
    {"13. 音量與記憶：說「大聲一點／小聲一點」調音量；說「清除記憶」重新開始對話。", C_WHITE},
    {"【設定】", C_GLOW},
    {"14. 網頁設定：開啟螢幕左下角的網址，可修改 Wi-Fi、密碼、API Key、雲端硬碟上傳網址、歌單與鏡頭色彩。", C_WHITE},
    {"15. 連不上 Wi-Fi 時會開熱點 XiaoKe-Setup（密碼 xiaoke123），手機連上後開 192.168.4.1 設定。", C_WHITE},
    {"【觸控用法】", C_GLOW},
    {"16. 主畫面：點一下螢幕看本說明（看完最後一頁回到聊天）；按住螢幕＝說話（環境吵雜時用）；板上 BOOT 鍵功能相同。", C_WHITE},
    {"17. 唱歌、" PORTAL_NAME "、攝影畫面：點一下螢幕回主畫面；錄製中：點一下停止並存檔。", C_WHITE},
    {"\f", 0},
    {"二、創意開發者", C_YELLOW},
    {"", C_WHITE},
    {DEVELOPER_CREDIT, C_WHITE},
    {DEVELOPER_EMAIL, C_GLOW},
    {"", C_WHITE},
    {"謝謝你陪" ROBOT_NAME "聊天！", C_PINK},
  };
  helpPages.clear();
  std::vector<HelpLine> page;
  for (auto &p : P) {
    if (!strcmp(p.t, "\f")) { if (page.size()) helpPages.push_back(page); page.clear(); continue; }
    std::vector<String> lines = u8f.wrap(p.t, SCREEN_W - 24);
    if (lines.empty()) lines.push_back("");
    bool heading = p.c == C_GLOW && lines.size() == 1;  // 分類標題不要孤零零留在頁尾
    if (page.size() && (page.size() + lines.size() > (size_t)HELP_LINES || (heading && page.size() + 3 > (size_t)HELP_LINES))
        && lines.size() <= (size_t)HELP_LINES) {
      helpPages.push_back(page);                       // 同一項目不跨頁
      page.clear();
    }
    for (auto &l : lines) {
      if ((int)page.size() == HELP_LINES) { helpPages.push_back(page); page.clear(); }
      page.push_back({l, p.c});
    }
  }
  if (page.size()) helpPages.push_back(page);
}

void drawHelp(uint32_t now) {
  int total = helpPages.size(), pg = constrain((int)helpPage, 0, total - 1);
  bool last = pg == total - 1;
  u8f.setForegroundColor(C_GLOW);
  u8f.drawUTF8(8, 64, "功能說明");
  char num[12];
  snprintf(num, sizeof(num), "%d / %d", pg + 1, total);
  u8f.setForegroundColor(C_GRAY);
  u8f.drawUTF8(SCREEN_W - 8 - u8f.getUTF8Width(num), 64, num);
  cv->drawFastHLine(8, 70, SCREEN_W - 16, C_BAR_LINE);
  for (size_t i = 0; i < helpPages[pg].size(); i++) {
    u8f.setForegroundColor(helpPages[pg][i].color);
    u8f.drawUTF8(8, 90 + i * LINE_H, helpPages[pg][i].text.c_str());
  }
  if (last) drawRobot(430, 228, 0.32f, (now / 1500) % 2 ? EX_HAPPY : EX_LOVE, 0, 0, false, 0, now, C_GLOW, false);
  const char *foot = last ? "點螢幕：回到聊天" : "點螢幕：下一頁";
  u8f.setForegroundColor(C_GREEN);
  u8f.drawUTF8((SCREEN_W - u8f.getUTF8Width(foot)) / 2, 294, foot);
}

// ---- 唱歌畫面 ----
void drawNote(int x, int y, uint16_t c) {        // 八分音符
  cv->fillCircle(x, y, 4, c);
  cv->drawFastVLine(x + 3, y - 14, 14, c);
  cv->drawFastVLine(x + 4, y - 14, 14, c);
  cv->drawLine(x + 4, y - 14, x + 9, y - 9, c);
  cv->drawLine(x + 4, y - 13, x + 9, y - 8, c);
}

// QR 碼：白底黑點（邏輯座標 x,y，邊長 w）
void drawQr(int x, int y, int w) {
  cv->fillRect(x, y, w, w, C_WHITE);
  int n = qrSize;
  if (n <= 0) return;
  int m = (w * 2) / (n + 4), off = (w * 2 - m * n) / 2;       // 以像素為單位，點更細緻
  for (int yy = 0; yy < n; yy++)
    for (int xx = 0; xx < n; xx++)
      if (qrMods[yy * n + xx]) fillRectHi(x * 2 + off + xx * m, y * 2 + off + yy * m, m, m, C_BLACK);
}

// 病房助理工作站：大 QR 碼 + 說明
void drawPortal(const String &title, const String &text, uint32_t now) {
  int tw = u8f.getUTF8Width(title.c_str());
  u8f.setForegroundColor(C_ORANGE);
  u8f.drawUTF8((SCREEN_W - tw) / 2, 66, title.c_str());
  drawQr(12, 76, 200);
  std::vector<String> lines = u8f.wrap(text, SCREEN_W - 232 - 8);   // 右邊：說明文字
  u8f.setForegroundColor(C_WHITE);
  for (size_t i = 0; i < lines.size() && i < 8; i++) u8f.drawUTF8(232, 96 + i * LINE_H, lines[i].c_str());
  float mouth = speakLevel > 0 ? constrain(speakLevel / 6000.0f, 0.0f, 1.0f) : 0;
  drawRobot(430, 238, 0.3f, EX_HAPPY, 0, 0, false, mouth, now, C_GLOW, false);
  u8f.setForegroundColor(C_GREEN);
  u8f.drawUTF8(12, 294, "手機掃描進入｜點螢幕：回主畫面");
}

void drawSong(const String &title, uint32_t now) {
  // 封面 320x180（邏輯座標 1:1，實際放大 2 倍 = 640x360）
  const int TX = 8, TY = 52, TW = 320, TH = 180;
  if (thumbOk) {
    for (int y = 0; y < TH * 2; y++) {
      const uint16_t *src = thumbPix + (y / 2) * 320;
      uint16_t *dst = hiBuf + (TY * 2 + y) * LCD_W + TX * 2;
      for (int x = 0; x < TW * 2; x++) dst[x] = src[x / 2];
    }
  } else {
    cv->fillRect(TX, TY, TW, TH, C_VISOR);
    for (int i = 0; i < 3; i++) drawNote(TX + 120 + i * 36, TY + 100 - (i % 2) * 14, C_GLOW);
  }
  cv->drawRect(TX - 1, TY - 1, TW + 2, TH + 2, C_HELMET);

  drawQr(344, 52, 160);
  u8f.setForegroundColor(C_GRAY);
  if (qrSize > 0) u8f.drawUTF8(344 + (160 - u8f.getUTF8Width("手機掃描 YouTube")) / 2, 230, "手機掃描 YouTube");
  char st[40] = "";
  if (songLoading) snprintf(st, sizeof(st), "歌曲下載中…");
  else if (songPlaying) snprintf(st, sizeof(st), "播放 %u:%02u / %u:%02u", (unsigned)(songPosMs / 60000), (unsigned)(songPosMs / 1000 % 60),
                                 (unsigned)(songDurMs / 60000), (unsigned)(songDurMs / 1000 % 60));
  if (st[0]) {
    u8f.setForegroundColor(C_GREEN);
    u8f.drawUTF8(344, 252, st);
    u8f.setForegroundColor(C_GRAY);
    u8f.drawUTF8(344, 272, "點螢幕：停止");
  }

  // 歌名與創作者
  u8f.setForegroundColor(C_ORANGE);
  u8f.drawUTF8(8, 254, "正在唱：");
  u8f.setForegroundColor(C_GLOW_DIM);
  u8f.drawUTF8(96, 254, "詞曲：" SONG_COMPOSER);
  std::vector<String> lines = u8f.wrap("《" + title + "》", 320);
  u8f.setForegroundColor(C_WHITE);
  for (size_t i = 0; i < lines.size() && i < 2; i++) u8f.drawUTF8(8, 274 + i * LINE_H, lines[i].c_str());

  // 唱歌的小柯 + 飄動音符
  float beat = (sinf(now / 160.0f) + 1) / 2;
  float mouth = speakLevel > 0 ? constrain(speakLevel / 6000.0f, 0.0f, 1.0f) : beat;
  drawRobot(440, 266, 0.2f, EX_HAPPY, 0, 0, false, mouth, now, C_ORANGE, false);
  for (int i = 0; i < 3; i++) {
    int ph = (now / 25 + i * 40) % 120;
    drawNote(380 + i * 14 - ph / 12, 290 - ph / 3, i == 1 ? C_PINK : C_GLOW);
  }
}

// ---- 農曆 ----
// 西元日期 → 距 2000-01-01 的天數（Howard Hinnant 演算法）
long daysSince2000(int y, int m, int d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468 - 10957;
}

// 回傳例如「八月二十」「閏六月初一」；超出表格範圍回傳空字串
String lunarString(const struct tm &t) {
  long d = daysSince2000(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  int yi = -1;
  for (int i = 0; i < LUNAR_YEARS - 1; i++)
    if (d >= LUNAR[i].cny && d < LUNAR[i + 1].cny) { yi = i; break; }
  if (yi < 0) return "";
  long off = d - LUNAR[yi].cny;
  int leap = LUNAR[yi].leap, n = leap ? 13 : 12, month = 1, day = 1;
  bool isLeap = false;
  for (int i = 0; i < n; i++) {
    int len = (LUNAR[yi].bits >> i) & 1 ? 30 : 29;
    if (off < len) {
      if (!leap || i < leap) month = i + 1;
      else if (i == leap) { month = leap; isLeap = true; }
      else month = i;
      day = off + 1;
      break;
    }
    off -= len;
  }
  static const char *MN[] = {"正", "二", "三", "四", "五", "六", "七", "八", "九", "十", "冬", "臘"};
  static const char *DG[] = {"一", "二", "三", "四", "五", "六", "七", "八", "九"};
  static const char *TP[] = {"初", "十", "廿"};
  String ds = day == 10 ? "初十" : day == 20 ? "二十" : day == 30 ? "三十" : String(TP[day / 10]) + DG[day % 10 - 1];
  return String(isLeap ? "閏" : "") + MN[month - 1] + "月" + ds;
}

// 上方日期時間列：日期、星期、農曆（左）＋ 24 小時制時間（中）＋ Wi-Fi、音量（右）
void drawTopBar() {
  struct tm t;
  bool ok = getLocalTime(&t, 0);
  static const char *WD[] = {"週日", "週一", "週二", "週三", "週四", "週五", "週六"};
  char dateStr[16] = "--/--", timeStr[12] = "--:--:--";
  if (ok) {
    snprintf(dateStr, sizeof(dateStr), "%d/%02d/%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &t);
  }
  cv->fillRect(0, 0, SCREEN_W, BAR_H, C_BG);
  cv->drawFastHLine(0, BAR_H - 1, SCREEN_W, C_BAR_LINE);
  static int lunarDay = -1;
  static String lunar;
  if (ok && t.tm_yday != lunarDay) {            // 一天只算一次
    lunarDay = t.tm_yday;
    String l = lunarString(t);
    lunar = l.startsWith("閏") || l.isEmpty() ? l : "農曆" + l;
  }
  u8f.setForegroundColor(C_GLOW);
  u8f.drawUTF8(6, 19, (String(dateStr) + " " + (ok ? WD[t.tm_wday] : "")).c_str());
  u8f.setForegroundColor(C_ORANGE);
  u8f.drawUTF8(6, 39, ok ? lunar.c_str() : "");
  cv->setTextSize(4);
  cv->setTextColor(C_WHITE);
  cv->setCursor((SCREEN_W - 8 * 24) / 2 + 14, 7);
  cv->print(timeStr);
  char vol[8];
  snprintf(vol, sizeof(vol), "%d%%", (int)volumePct);
  u8f.setForegroundColor(C_YELLOW);
  u8f.drawUTF8(470, 19, "音量");
  u8f.drawUTF8(470, 39, vol);
  cv->fillCircle(458, 13, 4, WiFi.status() == WL_CONNECTED ? C_GREEN : C_RED);   // Wi-Fi 燈號
}

// 右側農民曆卡片（x0..x0+w）
void drawAlmanac(int x0, int y0, int w, int h) {
  static Almanac alm;
  static int almDay = -1;
  struct tm t;
  bool ok = getLocalTime(&t, 0);
  if (ok && t.tm_yday != almDay) { almDay = t.tm_yday; alm = computeAlmanac(t); }
  cv->fillRoundRect(x0, y0, w, h, 8, C_PANEL);
  cv->drawRoundRect(x0, y0, w, h, 8, C_BAR_LINE);
  int x = x0 + 10, y = y0 + 22, tw = w - 20;
  u8f.setForegroundColor(C_YELLOW);
  u8f.drawUTF8(x, y, "農民曆");
  if (!ok || !alm.ok) {
    u8f.setForegroundColor(C_GRAY);
    u8f.drawUTF8(x, y + 26, "等待網路校時…");
    return;
  }
  u8f.setForegroundColor(C_ORANGE);
  String yz = alm.yearGZ + "年【" + alm.zodiac + "】";
  u8f.drawUTF8(x0 + w - 10 - u8f.getUTF8Width(yz.c_str()), y, yz.c_str());
  cv->drawFastHLine(x, y + 6, tw, C_BAR_LINE);
  y += 26;
  u8f.setForegroundColor(C_WHITE);
  u8f.drawUTF8(x, y, (alm.monthGZ + "月　" + alm.dayGZ + "日　" + alm.jianchu + "日").c_str());
  y += LINE_H + 2;
  u8f.setForegroundColor(C_GLOW);
  u8f.drawUTF8(x, y, (alm.chong + "　煞" + alm.sha).c_str());
  y += LINE_H + 6;
  auto block = [&](const char *label, uint16_t lc, const String &txt) {
    cv->fillRoundRect(x, y - 14, 20, 18, 4, lc);
    u8f.setForegroundColor(C_BLACK);
    u8f.drawUTF8(x + 2, y, label);
    std::vector<String> ls = u8f.wrap(txt, tw - 26);
    u8f.setForegroundColor(C_WHITE);
    for (size_t i = 0; i < ls.size() && i < 3; i++) u8f.drawUTF8(x + 26, y + i * LINE_H, ls[i].c_str());
    y += max((size_t)1, min(ls.size(), (size_t)3)) * LINE_H + 6;
  };
  block("宜", C_GREEN, alm.yi);
  block("忌", C_RED2, alm.ji);
  u8f.setForegroundColor(C_ORANGE);
  if (alm.termToday.length()) u8f.drawUTF8(x, y + 2, ("今日節氣：" + alm.termToday).c_str());
  else u8f.drawUTF8(x, y + 2, ("下個節氣：" + alm.nextTerm).c_str());
}

// 麥克風音量條（讓人知道它一直在聽）
void drawMicBars(int x, int y, bool listening) {
  int lvl = constrain(map(micLevel, 0, 3000, 0, 100), 0, 100);
  for (int i = 0; i < 4; i++) {
    int h = 3 + (lvl * (4 - abs(i * 2 - 3)) / 3) * 14 / 100;
    cv->fillRoundRect(x + i * 7, y - h, 5, h, 2, listening ? C_GREEN : C_GRAY);
  }
}

// 底部置中的提示框
void drawPill(const char *msg, int y, uint16_t color) {
  int w = u8f.getUTF8Width(msg);
  cv->fillRoundRect((SCREEN_W - w) / 2 - 10, y - 18, w + 20, 24, 8, C_BG);
  u8f.setForegroundColor(color);
  u8f.drawUTF8((SCREEN_W - w) / 2, y, msg);
}

// UI task：約 15 fps 重畫整個畫面
void uiTask(void *) {
  uint32_t nextBlink = millis() + 3000, nextLook = 0, nextIdle = millis() + 8000, idleUntil = 0;
  int idleExpr = EX_NORMAL;
  float lx = 0, ly = 0, tx = 0, ty = 0;
  for (;;) {
    uint32_t now = millis();
    UiState st; String title, text; uint32_t since; bool sad;
    xSemaphoreTake(uiLock, portMAX_DELAY);
    st = uiState; title = uiTitle; text = uiText; since = uiSince; sad = uiSad;
    xSemaphoreGive(uiLock);

    // 眼神：待機時東張西望
    if (now > nextLook) {
      if (st == UI_IDLE) { tx = random(-12, 13); ty = random(-6, 7); }
      else { tx = 0; ty = 0; }
      nextLook = now + random(1200, 3500);
    }
    lx += (tx - lx) * 0.35f; ly += (ty - ly) * 0.35f;

    // 眨眼（偶爾連眨兩下）
    bool blink = false;
    if (now >= nextBlink) {
      uint32_t d = now - nextBlink;
      if (d < 130 || (d > 260 && d < 390 && (nextBlink & 1))) blink = true;
      else if (d >= 400) nextBlink = now + random(2500, 6000);
    }

    // 待機時的隨機小表情
    if (st == UI_IDLE && now > nextIdle) {
      int r = random(100);
      idleExpr = r < 28 ? EX_HAPPY : r < 48 ? EX_WINK : r < 60 ? EX_LOVE : r < 72 ? EX_SURPRISE : r < 86 ? EX_BORED : EX_SHY;
      idleUntil = now + (idleExpr == EX_WINK ? 800 : idleExpr == EX_SURPRISE ? 900 : 1800);
      nextIdle = now + random(6000, 14000);
    }

    bool sleepy = st == UI_IDLE && now - lastInteraction > SLEEPY_AFTER_SEC * 1000UL;
    int ex = EX_NORMAL;
    switch (st) {
      case UI_LISTEN: ex = EX_LISTEN; break;
      case UI_THINK:  ex = EX_THINK;  break;
      case UI_SPEAK:  ex = speakExpr; break;
      case UI_MSG:    ex = sad ? EX_DEAD : EX_THINK; break;
      default:        ex = sleepy ? EX_SLEEPY : now < idleUntil ? idleExpr : EX_NORMAL;
    }
    float mouth = st == UI_SPEAK ? constrain(speakLevel / 6000.0f, 0.0f, 1.0f) : 0;
    if (st == UI_VIDEO) { vTaskDelay(pdMS_TO_TICKS(30)); continue; }   // 播 MV：畫面由播放器自己畫

    if (cameraActive && (st == UI_IDLE || st == UI_LISTEN || st == UI_CAMERA)) {   // 攝影：鏡頭即時影像（全螢幕）
      uint32_t tg = millis();
      bool ok = cameraOK && camGrab(hiBuf + 12 * LCD_W, LCD_W, 576, LCD_W);
      static uint32_t camLog = 0, camFrames = 0;
      camFrames++;
      if (millis() - camLog > 2000) {
        Serial.printf("[攝影] ok=%d grab=%ums 張數=%u 內部RAM=%uK\n", ok, (unsigned)(millis() - tg), (unsigned)camFrames,
                      (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        camLog = millis(); camFrames = 0;
      }
      fillRectHi(0, 0, LCD_W, 12, C_BG);
      fillRectHi(0, 588, LCD_W, 12, C_BG);
      if (!ok) {
        cv->fillScreen(C_BG);
        u8f.setForegroundColor(C_GRAY);
        u8f.drawUTF8(200, 150, "等待鏡頭畫面…");
      }
      char lab[40];
      uint16_t labColor = C_GREEN;                                 // 攝影中：綠色；錄製中：紅色
      if (recActive) {
        unsigned sec = (now - recStart) / 1000;
        snprintf(lab, sizeof(lab), "錄製中 %02u:%02u", sec / 60, sec % 60);
        labColor = C_RED;
      } else if (st == UI_LISTEN) {
        snprintf(lab, sizeof(lab), "攝影中・聆聽中…");
      } else {
        snprintf(lab, sizeof(lab), "攝影中");
      }
      int lw = u8f.getUTF8Width(lab);
      cv->fillRoundRect(6, 6, lw + 34, 24, 8, C_BG);
      if (recActive) { if ((now / 400) % 2) cv->fillCircle(18, 18, 6, C_RED); }   // 錄製中：紅點閃爍
      else cv->fillCircle(18, 18, 6, C_GREEN);                                     // 攝影中：綠點
      u8f.setForegroundColor(labColor);
      u8f.drawUTF8(30, 24, lab);
      drawPill(recActive ? "說「停止」結束並上傳｜點螢幕也可停止" : "說「錄製」錄影｜點螢幕：回到原畫面", 290, C_GREEN);
      lcdFlush(hiBuf);
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    if (st == UI_QR || st == UI_SONG || st == UI_PORTAL || (st == UI_HELP && helpPages.size())) {
      cv->fillScreen(C_BG);
      if (st == UI_QR) drawPortal(title, text, now);
      else if (st == UI_PORTAL) drawPortalMenu(now);
      else if (st == UI_SONG) drawSong(title, now);
      else drawHelp(now);
      drawTopBar();
      lcdFlush(hiBuf);
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    bool photoFull = photoShow && now - photoAt < PHOTO_SHOW_MS;
    if (photoFull) {                                                // 剛拍的照片：全螢幕
      memcpy(hiBuf + 12 * LCD_W, photoPix, LCD_W * 576 * 2);
      fillRectHi(0, 0, LCD_W, 12, C_BG);
      fillRectHi(0, 588, LCD_W, 12, C_BG);
      drawPill(st == UI_THINK ? "拍好了！讓我看看…" : "拍好了！", 290, C_GLOW);
      lcdFlush(hiBuf);
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    cv->fillScreen(C_BG);
    bool compact = (st == UI_THINK || st == UI_SPEAK || st == UI_MSG) && (title.length() || text.length());
    if (!compact) {
      drawRobot(150, 170, 0.88f, ex, lx, ly, blink, mouth, now, C_GLOW, false);
      drawAlmanac(300, 52, 206, 218);
      drawTopBar();
      const char *hint = st == UI_LISTEN ? "聆聽中…" : sleepy ? "想睡…" : VOICE_ACTIVATION ? "待命中，直接跟我說話" : "按住螢幕說話";
      u8f.setForegroundColor(st == UI_LISTEN ? C_GREEN : C_GRAY);
      u8f.drawUTF8(8, 278, hint);
      u8f.setForegroundColor(C_GLOW_DIM);
      u8f.drawUTF8(8, 296, ipText);                                  // 設定網頁位址
      drawMicBars(266, 296, st == UI_LISTEN);
      drawBtn(BTN_PORTAL_IDLE, PORTAL_NAME, C_GLOW_DIM, C_WHITE);   // 點這裡開工作站，其他地方 = 功能說明
    } else {
      if (photoShow) {                                              // 回答時左側顯示照片縮圖
        const int TWD = 176, THT = 99, X0 = 8, Y0 = 170;            // 邏輯座標
        for (int y = 0; y < THT * 2; y++) {
          const uint16_t *src = photoPix + (y * 576 / (THT * 2)) * LCD_W;
          uint16_t *dst = hiBuf + (Y0 * 2 + y) * LCD_W + X0 * 2;
          for (int x = 0; x < TWD * 2; x++) dst[x] = src[x * LCD_W / (TWD * 2)];
        }
        cv->drawRect(X0 - 1, Y0 - 1, TWD + 2, THT + 2, C_HELMET);
        drawRobot(96, 108, 0.4f, ex, 0, 0, blink, mouth, now, C_GLOW, false);
      } else {
        drawRobot(100, 170, 0.62f, ex, lx * 0.6f, ly * 0.6f, blink, mouth, now, C_GLOW, false);
      }
      drawTopBar();
      const int TX = 200, TW = SCREEN_W - TX - 10, PER = 11;
      u8f.setForegroundColor(st == UI_MSG ? (sad ? C_RED : C_YELLOW) : st == UI_SPEAK ? C_ORANGE : C_GLOW);
      u8f.drawUTF8(TX, 68, title.c_str());
      std::vector<String> lines = u8f.wrap(text, TW);
      int pages = max(1, (int)((lines.size() + PER - 1) / PER));
      int page = (int)((now - since) / 4500) % pages;               // 字太多時自動翻頁
      u8f.setForegroundColor(C_WHITE);
      for (int i = 0; i < PER && page * PER + i < (int)lines.size(); i++)
        u8f.drawUTF8(TX, 92 + i * LINE_H, lines[page * PER + i].c_str());
    }

    lcdFlush(hiBuf);
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}

// ============================================================================
//  音訊：錄音 / 播放
// ============================================================================
void writeWavHeader(uint8_t *h, uint32_t dataBytes) {
  uint32_t sr = MIC_SAMPLE_RATE, byteRate = sr * 2, chunk = 36 + dataBytes;
  memcpy(h, "RIFF", 4);      memcpy(h + 4, &chunk, 4);
  memcpy(h + 8, "WAVEfmt ", 8);
  uint32_t fmtLen = 16;      memcpy(h + 16, &fmtLen, 4);
  uint16_t fmt = 1, ch = 1;  memcpy(h + 20, &fmt, 2); memcpy(h + 22, &ch, 2);
  memcpy(h + 24, &sr, 4);    memcpy(h + 28, &byteRate, 4);
  uint16_t align = 2, bits = 16; memcpy(h + 32, &align, 2); memcpy(h + 34, &bits, 2);
  memcpy(h + 36, "data", 4); memcpy(h + 40, &dataBytes, 4);
}

// 讀 20ms 麥克風資料（ES8311 24kHz 立體聲 → 單聲道 → 16kHz），回傳 RMS
int readFrame(int16_t *out) {
  static int16_t raw[FRAME * 3 / 2 * 2];        // 480 個立體聲樣本
  size_t got = audioI2S.readBytes((char *)raw, sizeof(raw)) / 2;
  int32_t mono[FRAME * 3 / 2];
  for (int i = 0; i < FRAME * 3 / 2; i++) {
    int32_t s = (size_t)(i * 2) < got ? (int32_t)raw[i * 2] * MIC_SOFT_GAIN : 0;
    dcState += (s - dcState) >> 6;              // 簡易去直流
    mono[i] = s - dcState;
  }
  int64_t sum = 0;
  for (int k = 0; k < FRAME / 2; k++) {        // 每 3 個 24kHz 樣本 → 2 個 16kHz 樣本
    const int32_t *m = &mono[k * 3];
    int32_t a = (m[0] * 2 + m[1]) / 3, b = (m[1] + m[2] * 2) / 3;
    out[k * 2] = constrain(a, -32768, 32767);
    out[k * 2 + 1] = constrain(b, -32768, 32767);
    sum += (int64_t)out[k * 2] * out[k * 2] + (int64_t)out[k * 2 + 1] * out[k * 2 + 1];
  }
  return (int)sqrtf((float)(sum / FRAME));
}

void flushMic(int ms) {
  int16_t f[FRAME];
  for (int i = 0; i < ms / 20; i++) readFrame(f);
  prerollCount = 0;
}

int vadThreshold() { return max((int)VAD_MIN_RMS, (int)(noiseFloor * VAD_RATIO)); }

// 錄一句話：byButton = 按鍵模式（放開結束）；否則偵測停頓結束。回傳樣本數（0 = 視為雜音）
size_t recordUtterance(bool byButton) {
  int16_t *pcm = (int16_t *)(wavBuf + 44);
  size_t n = 0;
  // 先放入觸發前的預錄
  for (int k = 0; k < prerollCount; k++) {
    int idx = (prerollHead - prerollCount + k + PREROLL_FR) % PREROLL_FR;
    memcpy(pcm + n, preroll[idx], FRAME * 2);
    n += FRAME;
  }
  prerollCount = 0;

  setUi(UI_LISTEN);
  lastInteraction = millis();
  int thr = vadThreshold();
  int silentMs = 0, voicedMs = 0;
  double rmsSum = 0; int frames = 0;
  lastRecHitMax = true;
  while (n + FRAME <= MAX_SAMPLES) {
    int rms = readFrame(pcm + n);
    n += FRAME;
    micLevel = rms;
    rmsSum += rms; frames++;
    if (byButton) {
      if (!buttonPressed()) break;
      voicedMs += 20;
    } else {
      if (rms > thr * 0.7f) { silentMs = 0; voicedMs += 20; }
      else silentMs += 20;
      if (silentMs >= VAD_SILENCE_MS) { lastRecHitMax = false; break; }
    }
  }
  if (byButton) lastRecHitMax = false;
  lastRecAvgRms = frames ? rmsSum / frames : 0;
  micLevel = 0;
  if (!byButton) {
    // 去掉尾端靜音（保留 200ms）
    size_t trim = (size_t)max(0, silentMs - 200) * MIC_SAMPLE_RATE / 1000;
    n = n > trim ? n - trim : 0;
    if (voicedMs < VAD_MIN_SPEECH_MS) {
      Serial.printf("[VAD] 太短：有聲 %dms、共 %ums、平均音量 %.0f、門檻 %d\n", voicedMs, (unsigned)(frames * 20), lastRecAvgRms, thr);
      return 0;
    }
  } else if (n < MIC_SAMPLE_RATE / 2) {
    return 0;
  }
  return n;
}

// 播放 task：從 tts.buf 取資料送 I2S（單聲道 → 立體聲），套用音量，並計算嘴巴開合
void playTask(void *) {
  int16_t out[512];
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (!tts.downloadDone && tts.written < TTS_PREBUFFER) vTaskDelay(5);
    uint32_t ramp = 0;                         // 開頭淡入，避免喇叭瞬間大電流讓板子掉電重開（brownout）
    lcdBacklight(min(LCD_BRIGHTNESS, SPEAK_BRIGHTNESS));   // 說話時螢幕調暗，把電流讓給喇叭
    for (;;) {
      size_t avail = (tts.written - tts.played) & ~(size_t)1;
      if (avail == 0) {
        if (tts.downloadDone) break;
        vTaskDelay(2);
        continue;
      }
      size_t bytes = min(avail, sizeof(out) / 2);
      const int16_t *src = (const int16_t *)(tts.buf + tts.played);
      int vol = volumePct;
      int64_t acc = 0;
      for (size_t i = 0; i < bytes / 2; i++) {
        const uint32_t RAMP = TTS_SAMPLE_RATE * 15 / 100;   // 0.15 秒
        int32_t s = (int32_t)src[i] * vol / 100;
        if (ramp < RAMP) s = s * (int32_t)ramp++ / (int32_t)RAMP;
        s = constrain(s, -SPK_LIMIT, SPK_LIMIT);            // 限幅：削掉突然的大峰值
        out[i * 2] = out[i * 2 + 1] = s;
        acc += abs(src[i]);
      }
      speakLevel = (int)(acc / (int64_t)max((size_t)1, bytes / 2));
      audioI2S.write((uint8_t *)out, bytes * 2);
      tts.played += bytes;
    }
    speakLevel = 0;
    lcdBacklight(LCD_BRIGHTNESS);
    memset(out, 0, sizeof(out));             // 補靜音，避免 DMA 殘留造成嗡嗡聲
    for (int i = 0; i < 8; i++) audioI2S.write((uint8_t *)out, sizeof(out));
    tts.playing = false;
  }
}

// 讓 HTTPClient::writeToStream 直接把 TTS 音訊寫進 PSRAM 緩衝
class TtsSink : public Stream {
 public:
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t *data, size_t len) override {
    size_t space = tts.cap > tts.written ? tts.cap - tts.written : 0;
    size_t n = min(len, space);
    memcpy(tts.buf + tts.written, data, n);
    tts.written += n;
    return len;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

void waitPlaybackDone() {
  while (tts.playing) {
    updateVolumeFromPot();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ============================================================================
//  OpenAI API
// ============================================================================
String lastApiError;

String speechToText(size_t samples) {
  uint32_t dataBytes = samples * 2;
  writeWavHeader(wavBuf, dataBytes);
  size_t wavLen = 44 + dataBytes;

  const String B = "----ESP32S3RobotBoundary";
  String head =
      "--" + B + "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" STT_MODEL "\r\n" +
      "--" + B + "\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\nzh\r\n" +
      "--" + B + "\r\nContent-Disposition: form-data; name=\"prompt\"\r\n\r\n請用台灣繁體中文轉寫。常用詞：小柯、鏡頭、開鏡頭、攝影、錄製、停止、拍照、唱首歌、班表、白班、小夜班、大夜班、病房助理、護理師、工作流程、長照、照服員、居服員、居家督導。\r\n" +
      "--" + B + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
      "Content-Type: audio/wav\r\n\r\n";
  String tail = "\r\n--" + B + "--\r\n";

  size_t total = head.length() + wavLen + tail.length();
  uint8_t *body = (uint8_t *)ps_malloc(total);
  if (!body) { lastApiError = "記憶體不足"; return ""; }
  memcpy(body, head.c_str(), head.length());
  memcpy(body + head.length(), wavBuf, wavLen);
  memcpy(body + head.length() + wavLen, tail.c_str(), tail.length());

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(30000);
  http.begin(client, String(OPENAI_HOST) + "/v1/audio/transcriptions");
  http.addHeader("Authorization", authHeader());
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + B);
  int code = http.POST(body, total);
  String resp = http.getString();
  http.end();
  free(body);

  if (code != 200) {
    lastApiError = "STT " + String(code) + " " + resp.substring(0, 120);
    Serial.println(lastApiError);
    return "";
  }
  JsonDocument doc;
  deserializeJson(doc, resp);
  String text = doc["text"] | "";
  text.trim();
  return text;
}

// ---- 照片上傳 Google 雲端硬碟（背景執行，不影響對話）----
struct PhotoUpload { uint8_t *jpg; size_t len; char name[48]; };

void uploadTask(void *arg) {
  PhotoUpload *u = (PhotoUpload *)arg;
  size_t b64Len = 0;
  mbedtls_base64_encode(nullptr, 0, &b64Len, u->jpg, u->len);
  uint8_t *b64 = (uint8_t *)ps_malloc(b64Len + 1);
  if (b64 && mbedtls_base64_encode(b64, b64Len, &b64Len, u->jpg, u->len) == 0) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(30000);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);     // Apps Script 會 302 轉址回傳結果
    String url = cfg.gdrive + "?token=" + GDRIVE_TOKEN + "&name=" + u->name;
    http.begin(client, url);
    http.addHeader("Content-Type", "text/plain");
    int code = http.POST(b64, b64Len);
    String resp = http.getString();
    http.end();
    Serial.printf("照片上傳 %s → %d %s\n", u->name, code, resp.substring(0, 60).c_str());
  }
  free(b64);
  free(u->jpg);
  delete u;
  vTaskDelete(nullptr);
}

void queuePhotoUpload(const uint8_t *jpg, size_t len) {
  if (cfg.gdrive.length() == 0) return;
  PhotoUpload *u = new PhotoUpload;
  u->jpg = (uint8_t *)ps_malloc(len);
  if (!u->jpg) { delete u; return; }
  memcpy(u->jpg, jpg, len);
  u->len = len;
  struct tm t;
  if (getLocalTime(&t, 10)) strftime(u->name, sizeof(u->name), "XiaoKe_photo_%Y%m%d_%H%M%S.jpg", &t);
  else snprintf(u->name, sizeof(u->name), "XiaoKe_photo_%lu.jpg", (unsigned long)millis());
  // 檔名含中文，網址要編碼
  String enc;
  for (const char *c = u->name; *c; c++) {
    if (isalnum((uint8_t)*c) || *c == '_' || *c == '.' || *c == '-') enc += *c;
    else { char h[4]; snprintf(h, sizeof(h), "%%%02X", (uint8_t)*c); enc += h; }
  }
  strlcpy(u->name, enc.c_str(), sizeof(u->name));
  xTaskCreatePinnedToCore(uploadTask, "upload", 8192, u, 1, nullptr, 1);
}

// 拍照：顯示在螢幕、背景上傳雲端硬碟，並回傳給 GPT 用的 data URI
#define PHOTO_W 768                      // 送給 AI / 雲端硬碟的照片尺寸（硬體 JPEG 編碼，寬高需 16 的倍數）
#define PHOTO_H 432
String captureImageDataUri() {
  if (!cameraOK || !photoPix) return "";
  if (!camGrab(photoPix, LCD_W, 576, LCD_W, 1)) return "";    // 丟掉一張舊畫面，取最新的
  photoAt = millis();
  photoShow = true;

  String uri;
  jpegLockTake();
  for (int y = 0; y < PHOTO_H; y++) {                          // 1024x576 → 768x432
    const uint16_t *src = photoPix + (y * 576 / PHOTO_H) * LCD_W;
    uint16_t *dst = jpegInBuf + y * PHOTO_W;
    for (int x = 0; x < PHOTO_W; x++) dst[x] = src[x * LCD_W / PHOTO_W];
  }
  size_t len = 0;
  const uint8_t *jpg = jpegEncode(PHOTO_W, PHOTO_H, 85, &len);
  if (jpg) {
    queuePhotoUpload(jpg, len);
    size_t b64Len = 0;
    mbedtls_base64_encode(nullptr, 0, &b64Len, jpg, len);
    uint8_t *b64 = (uint8_t *)ps_malloc(b64Len + 1);
    if (b64 && mbedtls_base64_encode(b64, b64Len, &b64Len, jpg, len) == 0) {
      b64[b64Len] = 0;
      uri.reserve(b64Len + 32);
      uri = "data:image/jpeg;base64,";
      uri += (const char *)b64;
    }
    free(b64);
    Serial.printf("拍照 %ux%u JPEG %u bytes\n", PHOTO_W, PHOTO_H, (unsigned)len);
  } else Serial.println("JPEG 編碼失敗");
  jpegLockGive();
  return uri;
}

String nowString() {
  struct tm t;
  if (!getLocalTime(&t, 10)) return "未知";
  static const char *W[] = {"日", "一", "二", "三", "四", "五", "六"};
  char buf[48];
  strftime(buf, sizeof(buf), "%Y年%m月%d日 %H:%M", &t);
  return String(buf) + " 星期" + W[t.tm_wday] + "（農曆" + lunarString(t) + "）。" + almanacText(t);
}

void pushHistory(const char *role, const String &text) {
  if (histCount == MAX_HISTORY_MSGS) {
    for (int i = 1; i < MAX_HISTORY_MSGS; i++) { histRole[i - 1] = histRole[i]; histText[i - 1] = histText[i]; }
    histCount--;
  }
  histRole[histCount] = role;
  histText[histCount] = text;
  histCount++;
}

// 取出回覆開頭的心情標籤，例如「[love]好喜歡你！」→ EX_LOVE，並從文字中移除
int parseEmotion(String &reply) {
  int e = EX_HAPPY;
  if (reply.startsWith("[")) {
    int k = reply.indexOf(']');
    if (k > 0 && k < 16) {
      String tag = reply.substring(1, k);
      tag.toLowerCase();
      if (tag == "love") e = EX_LOVE;
      else if (tag == "surprise") e = EX_SURPRISE;
      else if (tag == "sad") e = EX_SAD;
      else if (tag == "angry") e = EX_ANGRY;
      else if (tag == "wink") e = EX_WINK;
      else if (tag == "shy") e = EX_SHY;
      reply = reply.substring(k + 1);
      reply.trim();
    }
  }
  return e;
}

// 去掉網址與 Markdown 連結（搜尋結果常附來源，語音不要唸出來）
String stripLinks(String t) {
  for (;;) {                                           // [文字](網址)：從 "](" 往回找最近的 '['
    int m = t.indexOf("](");
    if (m < 0) break;
    int a = t.lastIndexOf('[', m);
    int b = t.indexOf(')', m);
    if (a < 0 || b < 0) break;
    int from = a, to = b + 1;
    if (from > 0 && t[from - 1] == '(' && to < (int)t.length() && t[to] == ')') { from--; to++; }   // 外層括號
    while (from > 0 && t[from - 1] == ' ') from--;
    t.remove(from, to - from);
  }
  for (int h; (h = t.indexOf("http")) >= 0;) {         // 剩下的裸網址
    int e = h;
    while (e < (int)t.length() && (uint8_t)t[e] > ' ' && (uint8_t)t[e] < 0x80) e++;
    t.remove(h, e - h);
  }
  t.replace("\n\n", "\n");
  return t;
}

// 問題看起來需要即時資訊（只用來顯示「上網查詢中」，要不要搜尋由 AI 自己決定）
bool looksLikeSearch(const String &t) {
  return containsAny(t, {"天氣", "氣溫", "下雨", "颱風", "新聞", "最新", "股價", "股票", "匯率", "比賽", "比數", "賽程",
                         "查一下", "查查", "搜尋", "上網", "油價", "地震", "空氣品質", "營業時間"});
}

// OpenAI Responses API；ENABLE_WEB_SEARCH = 1 時 AI 會自己決定要不要上網搜尋；imageUri 非空時附上鏡頭畫面
String chatWithGPT(const String &userText, const String &imageUri) {
  JsonDocument doc;
  doc["model"] = CHAT_MODEL;
  doc["max_output_tokens"] = CHAT_MAX_TOKENS;
#if ENABLE_WEB_SEARCH
  if (imageUri.isEmpty() && chatContext.isEmpty()) {   // 看圖、查班表時不需要上網
    JsonObject ws = doc["tools"].to<JsonArray>().add<JsonObject>();
    ws["type"] = "web_search";
    ws["search_context_size"] = "medium";
    ws["user_location"]["type"] = "approximate";
    ws["user_location"]["country"] = "TW";
    ws["user_location"]["city"] = SEARCH_CITY;
  }
#endif
  doc["instructions"] = String("你是一台可愛的桌上型 AI 語音機器人，名字叫「" ROBOT_NAME "」，個性活潑、貼心又有點俏皮。") +
                   "一律使用台灣繁體中文回答，口語化、簡短（100 字以內），因為回答會用語音播放，"
                   "不要使用 Markdown、條列符號、表情符號或網址。"
                   "如果使用者附上照片，那是你鏡頭看到的畫面，請直接描述或回答。"
#if ENABLE_WEB_SEARCH
                   "遇到天氣、新聞、股價、比賽結果等需要即時資訊的問題，請先上網搜尋，再用口語簡短摘要重點，"
                   "搜尋到的數字（溫度、價格、比數等）要直接講出來，不要叫使用者自己去查；"
                   "不要唸出網址或來源網站名稱；沒指定地點時以" SEARCH_CITY_ZH "為準。"
#endif
                   "語音辨識偶爾會聽錯字，請依上下文推測使用者的意思。"
                   "你的創意開發者是吳玉柱先生，他與 Claude AI 共同開發了你；"
                   "被問到是誰做的、開發者、作者或設計者時，要熱情地介紹吳玉柱先生，"
                   "並說有任何意見可以寫信到 achir1015@gmail.com。"
                   "每次回答的最開頭，加上一個代表你當下心情的標籤，只能是以下其中一個："
                   "[happy] [love] [surprise] [sad] [angry] [wink] [shy]，標籤後面直接接回答內容。"
                   "現在時間：" + nowString() + "。";
  if (chatContext.length())
    doc["instructions"] = doc["instructions"].as<String>() +
        "以下是資料庫查到的資料（月班表、工作流程時間軸），請根據資料回答，資料沒有的不要亂編；"
        "使用者說「我」指的是" SCHEDULE_MY_NAME "；工作流程的時間是 24 小時制，問「現在該做什麼」時請對照現在時間。"
        "班別代號：" SCHEDULE_LEGEND "。回答時把代號說成中文班別。\n" + chatContext;

  JsonArray msgs = doc["input"].to<JsonArray>();
  for (int i = 0; i < histCount; i++) {
    JsonObject m = msgs.add<JsonObject>();
    m["role"] = histRole[i];
    m["content"] = histText[i];
  }
  JsonObject u = msgs.add<JsonObject>();
  u["role"] = "user";
  if (imageUri.length()) {
    JsonArray content = u["content"].to<JsonArray>();
    JsonObject t = content.add<JsonObject>();
    t["type"] = "input_text";
    t["text"] = userText;
    JsonObject im = content.add<JsonObject>();
    im["type"] = "input_image";
    im["image_url"] = imageUri;
    im["detail"] = "low";
  } else {
    u["content"] = userText;
  }

  String req;
  serializeJson(doc, req);
  doc.clear();

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(45000);
  http.begin(client, String(OPENAI_HOST) + "/v1/responses");
  http.addHeader("Authorization", authHeader());
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(req);
  req = String();
  String resp = http.getString();
  http.end();

  if (code != 200) {
    lastApiError = "Chat " + String(code) + " " + resp.substring(0, 120);
    Serial.println(lastApiError);
    return "";
  }
  // output 可能是 [web_search_call, message]，取 message 的文字
  JsonDocument filter;
  filter["output"][0]["type"] = true;
  filter["output"][0]["content"][0]["text"] = true;
  JsonDocument r;
  deserializeJson(r, resp, DeserializationOption::Filter(filter));
  resp = String();
  String reply;
  bool searched = false;
  for (JsonObject o : r["output"].as<JsonArray>()) {
    String type = o["type"] | "";
    if (type == "web_search_call") searched = true;
    if (type == "message")
      for (JsonObject c : o["content"].as<JsonArray>()) reply += c["text"] | "";
  }
  if (searched) Serial.println("（已上網搜尋）");
  reply.trim();
  speakExpr = parseEmotion(reply);                     // 先取出開頭的心情標籤，再去掉網址
  reply = stripLinks(reply);
  reply.replace("*", "");
  reply.replace("#", "");
  reply.trim();

  pushHistory("user", userText);
  pushHistory("assistant", reply);
  return reply;
}

bool speak(const String &text) {
  if (text.isEmpty()) return false;
  JsonDocument doc;
  doc["model"] = TTS_MODEL;
  doc["voice"] = TTS_VOICE;
  doc["input"] = text;
  doc["response_format"] = "pcm";
  doc["instructions"] = TTS_INSTRUCTIONS;   // tts-1 會忽略此欄位
  String req;
  serializeJson(doc, req);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(30000);
  http.begin(client, String(OPENAI_HOST) + "/v1/audio/speech");
  http.addHeader("Authorization", authHeader());
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(req);
  if (code != 200) {
    lastApiError = "TTS " + String(code) + " " + http.getString().substring(0, 120);
    Serial.println(lastApiError);
    http.end();
    return false;
  }

  tts.written = 0;
  tts.played = 0;
  tts.downloadDone = false;
  tts.playing = true;
  xTaskNotifyGive(playTaskHandle);

  TtsSink sink;
  http.writeToStream(&sink);
  tts.downloadDone = true;
  http.end();
  return true;
}

// ============================================================================
//  唱歌：從 YouTube 播放清單隨機選歌
// ============================================================================
// 邊下載邊解析播放清單網頁（約 1MB，不整頁存下來）：找 lockupViewModel → videoId → title
class PlaylistParser : public Stream {
 public:
  std::vector<Song> out;
  size_t write(uint8_t b) override { feed(b); return 1; }
  size_t write(const uint8_t *d, size_t n) override { for (size_t i = 0; i < n; i++) feed(d[i]); return n; }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
 private:
  int state = 0, mi = 0;
  bool esc = false;
  String buf, vid;
  void feed(uint8_t c) {
    static const char *M[] = {"\"lockupViewModel\":{\"contentImage\"", "\"videoId\":\"", "\"title\":{\"content\":\""};
    if (out.size() >= 80) return;
    if (state == 0 || state == 1 || state == 3) {
      const char *pat = M[state == 0 ? 0 : state == 1 ? 1 : 2];
      if (c == (uint8_t)pat[mi]) mi++;
      else mi = (c == (uint8_t)pat[0]) ? 1 : 0;
      if (pat[mi] == 0) { mi = 0; buf = ""; esc = false; state = state == 0 ? 1 : state == 1 ? 2 : 4; }
    } else if (state == 2) {
      buf += (char)c;
      if (buf.length() == 11) { vid = buf; state = 3; }
    } else if (state == 4) {
      if (c == '"' && !esc) { out.push_back({vid, unescape(buf)}); state = 0; }
      else { esc = (c == '\\' && !esc); buf += (char)c; }
      if (buf.length() > 300) state = 0;
    }
  }
  static String unescape(const String &s) {
    String r;
    for (size_t i = 0; i < s.length(); i++) {
      if (s[i] == '\\' && i + 1 < s.length()) {
        char n = s[++i];
        if (n == 'u' && i + 4 < s.length()) {                    // \uXXXX → UTF-8
          uint32_t cp = strtoul(s.substring(i + 1, i + 5).c_str(), nullptr, 16);
          i += 4;
          if (cp < 0x80) r += (char)cp;
          else if (cp < 0x800) { r += (char)(0xC0 | (cp >> 6)); r += (char)(0x80 | (cp & 0x3F)); }
          else { r += (char)(0xE0 | (cp >> 12)); r += (char)(0x80 | ((cp >> 6) & 0x3F)); r += (char)(0x80 | (cp & 0x3F)); }
        } else r += n == 'n' ? ' ' : n;
      } else r += s[i];
    }
    return r;
  }
};

void loadSongsFromNvs() {
  prefs.begin("robot", true);
  String raw = prefs.getString("songs", "");
  prefs.end();
  std::vector<Song> v;
  int start = 0;
  while (start < (int)raw.length()) {
    int nl = raw.indexOf('\n', start);
    if (nl < 0) nl = raw.length();
    String line = raw.substring(start, nl);
    int tab = line.indexOf('\t');
    if (tab == 11) v.push_back({line.substring(0, 11), line.substring(12)});
    start = nl + 1;
  }
  xSemaphoreTake(songLock, portMAX_DELAY);
  songs = v;
  xSemaphoreGive(songLock);
}

// 下載播放清單並存進 NVS；回傳歌曲數（失敗 -1）
int refreshPlaylist() {
  if (cfg.playlist.indexOf("list=") < 0) return -1;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(client, cfg.playlist);
  http.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126 Safari/537.36");
  http.addHeader("Accept-Language", "zh-TW,zh;q=0.9");
  int code = http.GET();
  if (code != 200) { http.end(); Serial.printf("歌單下載失敗 %d\n", code); return -1; }
  PlaylistParser parser;
  http.writeToStream(&parser);
  http.end();
  if (parser.out.empty()) { Serial.println("歌單解析不到歌曲"); return -1; }
  String raw;
  for (auto &x : parser.out) raw += x.id + "\t" + x.title + "\n";
  prefs.begin("robot", false);
  prefs.putString("songs", raw);
  prefs.end();
  xSemaphoreTake(songLock, portMAX_DELAY);
  songs = parser.out;
  xSemaphoreGive(songLock);
  Serial.printf("歌單更新：%d 首\n", (int)parser.out.size());
  return parser.out.size();
}

void playlistTask(void *) {
  refreshPlaylist();
  fetchMp3List();
  fetchPortalTools();
  vTaskDelete(nullptr);
}

// 下載 YouTube 封面（mqdefault 320x180）並用硬體 JPEG 解碼
bool fetchThumb(const String &id) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  http.begin(client, "https://i.ytimg.com/vi/" + id + "/mqdefault.jpg");
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    const size_t CAP = 64 * 1024;
    uint8_t *jpg = (uint8_t *)ps_malloc(CAP);
    size_t len = 0;
    WiFiClient *st = http.getStreamPtr();
    uint32_t t0 = millis();
    int total = http.getSize();
    while (jpg && http.connected() && len < CAP && (total < 0 || (int)len < total) && millis() - t0 < 8000) {
      size_t a = st->available();
      if (a) len += st->readBytes(jpg + len, min(a, CAP - len));
      else delay(5);
    }
    if (jpg && len > 0) ok = jpegDecodeTo(jpg, len, thumbPix, 320, 180);
    free(jpg);
  }
  http.end();
  return ok;
}

// 產生 QR 碼到 qrMods（最多 version 7 = 45x45）
bool makeQr(const String &text) {
  static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(7)], qr[qrcodegen_BUFFER_LEN_FOR_VERSION(7)];
  qrSize = 0;
  if (!qrcodegen_encodeText(text.c_str(), tmp, qr, qrcodegen_Ecc_LOW, 1, 7, qrcodegen_Mask_AUTO, true)) return false;
  int n = qrcodegen_getSize(qr);
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++) qrMods[y * n + x] = qrcodegen_getModule(qr, x, y);
  qrSize = n;
  return true;
}

uint32_t songSince = 0;
int lastSong = -1;

// ---- 唱歌：直接播放 Google 雲端硬碟資料夾裡的 MP3（自己創作的歌）----
//  資料夾設成「知道連結的任何人都能檢視」，小柯讀資料夾的公開清單頁，再下載 MP3 到 PSRAM 解碼播放
std::vector<Mp3Song> mp3Songs;

// 比對歌名用：去掉副檔名、空白與標點、複本尾碼「(1)」「_3」
String normTitle(String s) {
  if (s.endsWith(".mp3") || s.endsWith(".mp4")) s.remove(s.length() - 4);
  static const char *DROP[] = {" ", "　", "，", "。", "、", "：", "！", "？", "(", ")", "（", "）", "《", "》", "「", "」", "_", "-", ".", ",", ":", "!", "?"};
  for (auto d : DROP) s.replace(d, "");
  while (s.length() && isdigit((uint8_t)s[s.length() - 1])) s.remove(s.length() - 1);
  return s;
}

String songsFolderId() {
  String f = SONGS_MP3_FOLDER;
  int k = f.indexOf("folders/");
  if (k >= 0) { f = f.substring(k + 8); int q = f.indexOf('?'); if (q >= 0) f = f.substring(0, q); }
  return f;
}

// 讀雲端資料夾清單（embeddedfolderview 公開頁），更新 mp3Songs；回傳歌曲數（失敗 -1）
int fetchMp3List() {
  String folder = songsFolderId();
  if (folder.isEmpty()) return -1;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(client, "https://drive.google.com/embeddedfolderview?id=" + folder);
  int code = http.GET();
  String html = code == 200 ? http.getString() : String("");
  http.end();
  if (code != 200) { Serial.printf("MP3 歌單讀取失敗 %d\n", code); return -1; }
  std::vector<Mp3Song> v;
  int pos = 0;
  for (;;) {
    int a = html.indexOf("id=\"entry-", pos);
    if (a < 0) break;
    a += 10;
    int b = html.indexOf('"', a);
    int t = html.indexOf("flip-entry-title\">", b);
    if (b < 0 || t < 0) break;
    t += 18;
    int e = html.indexOf('<', t);
    String id = html.substring(a, b), title = html.substring(t, e);
    title.replace("&amp;", "&"); title.replace("&#39;", "'"); title.replace("&quot;", "\"");
    if (title.endsWith(".mp3") || title.endsWith(".MP3")) v.push_back({id, title.substring(0, title.length() - 4)});
    pos = e;
  }
  xSemaphoreTake(songLock, portMAX_DELAY);
  mp3Songs = v;
  xSemaphoreGive(songLock);
  Serial.printf("MP3 歌單：%d 首\n", (int)v.size());
  return v.size();
}

// 下載整首 MP3 到 PSRAM（一首約 2~4MB）；回傳長度，失敗 0
size_t downloadMp3(const String &id, uint8_t **out) {
  const size_t CAP = 12 * 1024 * 1024;
  *out = nullptr;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(client, "https://drive.usercontent.google.com/download?id=" + id + "&export=download&confirm=t");
  int code = http.GET();
  if (code != 200) { http.end(); Serial.printf("MP3 下載失敗 %d\n", code); return 0; }
  int total = http.getSize();
  size_t cap = total > 0 ? min((size_t)total, CAP) : CAP;
  uint8_t *buf = (uint8_t *)ps_malloc(cap);
  size_t len = 0;
  if (buf) {
    WiFiClient *st = http.getStreamPtr();
    uint32_t last = millis();
    while (len < cap && (total < 0 || (int)len < total) && millis() - last < 10000) {
      size_t a = st->available();
      if (a) { len += st->readBytes(buf + len, min(a, cap - len)); last = millis(); }
      else if (!http.connected()) break;
      else delay(2);
    }
  }
  http.end();
  if (len < 4096) { free(buf); return 0; }
  *out = buf;
  return len;
}

// 解碼並播放（阻塞到唱完或點螢幕）；回傳 true = 被點螢幕停止
bool playMp3(uint8_t *data, size_t len) {
  HMP3Decoder h = MP3InitDecoder();
  if (!h) return false;
  static int16_t pcm[MAX_NCHAN * MAX_NGRAN * MAX_NSAMP];
  static int16_t out[MAX_NGRAN * MAX_NSAMP * 2 * 2];
  uint8_t *p = data;
  int left = len;
  uint32_t ramp = 0, samples = 0;
  bool stopped = false, wasTouched = buttonPressed();
  songPosMs = 0;
  songPlaying = true;
  lcdBacklight(min(LCD_BRIGHTNESS, SPEAK_BRIGHTNESS));            // 和說話一樣：唱歌時螢幕調暗，避免掉電重開
  while (left > 0) {
    bool t = buttonPressed();
    if (t && !wasTouched) { stopped = true; break; }                // 點螢幕：停止
    wasTouched = t;
    int off = MP3FindSyncWord(p, left);
    if (off < 0) break;
    p += off; left -= off;
    int err = MP3Decode(h, &p, &left, pcm, 0);
    if (err == ERR_MP3_INDATA_UNDERFLOW) break;
    if (err) { if (left > 0) { p++; left--; } continue; }        // 壞掉的影格：跳過
    MP3FrameInfo fi;
    MP3GetLastFrameInfo(h, &fi);
    int n = fi.outputSamps / max(1, fi.nChans);
    if (songDurMs == 0 && fi.bitrate > 0) songDurMs = (uint32_t)((uint64_t)len * 8000 / fi.bitrate);
    // 轉成 24kHz 立體聲（轉檔時已是 24kHz；其他取樣率用最近鄰換算）
    int outN = fi.samprate == I2S_RATE ? n : (int)((int64_t)n * I2S_RATE / max(1, fi.samprate));
    outN = min(outN, (int)(sizeof(out) / 4));
    int vol = volumePct;
    int64_t acc = 0;
    for (int i = 0; i < outN; i++) {
      int si = fi.samprate == I2S_RATE ? i : (int)((int64_t)i * fi.samprate / I2S_RATE);
      int32_t s = fi.nChans == 2 ? ((int32_t)pcm[si * 2] + pcm[si * 2 + 1]) / 2 : pcm[si];
      s = s * vol / 100;
      const uint32_t RAMP = I2S_RATE / 5;
      if (ramp < RAMP) s = s * (int32_t)ramp++ / (int32_t)RAMP;
      s = constrain(s, -SPK_LIMIT, SPK_LIMIT);
      out[i * 2] = out[i * 2 + 1] = s;
      acc += abs(s);
    }
    speakLevel = (int)(acc / max(1, outN)) * 2;                    // 嘴巴跟著節奏開合
    audioI2S.write((uint8_t *)out, outN * 4);
    samples += outN;
    songPosMs = (uint32_t)((uint64_t)samples * 1000 / I2S_RATE);
  }
  memset(out, 0, sizeof(out));
  for (int i = 0; i < 4; i++) audioI2S.write((uint8_t *)out, sizeof(out));
  speakLevel = 0;
  songPlaying = false;
  lcdBacklight(LCD_BRIGHTNESS);
  MP3FreeDecoder(h);
  while (buttonPressed()) delay(10);                               // 等手指離開，避免接著打開說明
  return stopped;
}

// 找使用者點名的歌：「唱千江有水千江月」→ 對應的 MP3 索引，找不到 -1
int findRequestedMp3(const String &userText) {
  String u = normTitle(userText);
  int best = -1, bestLen = 0;
  xSemaphoreTake(songLock, portMAX_DELAY);
  for (size_t i = 0; i < mp3Songs.size(); i++) {
    String t = normTitle(mp3Songs[i].title);
    if (t.length() >= 6 && u.indexOf(t) >= 0 && (int)t.length() > bestLen) { best = i; bestLen = t.length(); }
  }
  xSemaphoreGive(songLock);
  return best;
}

// 用 MP3 歌名找 YouTube 播放清單裡同一首（封面與 QR 碼用）
bool findYoutube(const String &title, String &ytId) {
  String t = normTitle(title);
  bool ok = false;
  xSemaphoreTake(songLock, portMAX_DELAY);
  for (auto &s : songs) {
    String y = normTitle(s.title);
    if (y.length() && (y.indexOf(t) >= 0 || t.indexOf(y) >= 0)) { ytId = s.id; ok = true; break; }
  }
  xSemaphoreGive(songLock);
  return ok;
}

// 只有 YouTube 歌單時：介紹歌曲 + QR 碼（手機播放）
void singYoutubeOnly() {
  xSemaphoreTake(songLock, portMAX_DELAY);
  int n = songs.size();
  Song s;
  if (n) {
    int i = random(n);
    if (n > 1) while (i == lastSong) i = random(n);
    lastSong = i;
    s = songs[i];
  }
  xSemaphoreGive(songLock);
  if (!n) {
    speakExpr = EX_SAD;
    speakAndShow("我的歌單還是空的，請到設定網頁填入 YouTube 播放清單網址，再按更新歌單喔。");
    return;
  }
  Serial.println("唱歌：" + s.title + " https://youtu.be/" + s.id);
  setUi(UI_THINK, "選歌中…", "《" + s.title + "》");
  thumbOk = fetchThumb(s.id);
  makeQr("https://youtu.be/" + s.id);
  setUi(UI_SONG, s.title, s.id);
  songSince = millis();
  if (speak("我來唱《" + s.title + "》給你聽！這是" SONG_COMPOSER "創作的歌，用手機掃描 QR 碼，就能在 YouTube 聽完整版喔！"))
    waitPlaybackDone();
  songSince = millis();
  lastInteraction = millis();
}

void exitSong();

// 唱歌：有雲端 MP3 就直接唱出來；userText 有點歌名就唱那首
void singSong(const String &userText) {
  setUi(UI_THINK, "選歌中…", "");
  fetchMp3List();                                                  // 每次重讀，雲端新增的歌馬上就能唱
  xSemaphoreTake(songLock, portMAX_DELAY);
  int n = mp3Songs.size();
  xSemaphoreGive(songLock);
  if (!n) { singYoutubeOnly(); return; }

  int idx = findRequestedMp3(userText);
  if (idx < 0) {
    idx = random(n);
    if (n > 1) while (idx == lastSong) idx = random(n);
  }
  lastSong = idx;
  xSemaphoreTake(songLock, portMAX_DELAY);
  Mp3Song m = mp3Songs[idx];
  xSemaphoreGive(songLock);
  Serial.println("唱歌（MP3）：" + m.title);

  String ytId;
  bool hasYt = findYoutube(m.title, ytId);
  thumbOk = hasYt && fetchThumb(ytId);
  if (hasYt) makeQr("https://youtu.be/" + ytId); else qrSize = 0;
  songDurMs = 0; songPosMs = 0;
  songLoading = true;
  setUi(UI_SONG, m.title, hasYt ? ytId : String(""));
  songSince = millis();

  uint8_t *data = nullptr;                                         // 先下載（和介紹詞同時進行會搶網路，依序做）
  size_t len = downloadMp3(m.id, &data);
  songLoading = false;
  if (!len) {
    speakExpr = EX_SAD;
    speakAndShow("這首歌下載失敗，請確認雲端資料夾已經設成知道連結的人都能檢視。");
    exitSong();
    return;
  }
  if (speak("我來唱《" + m.title + "》給你聽！這是" SONG_COMPOSER "創作的歌。")) waitPlaybackDone();
  bool stopped = playMp3(data, len);
  free(data);
  Serial.println(stopped ? "（唱歌：點螢幕停止）" : "（唱歌：唱完了）");
  exitSong();
}

// ============================================================================
//  播 MV：雲端資料夾裡的 .xkv（tools/make_xkv.py 由 MP4 轉成：MJPEG 640x352 + MP3）
//  邊下載邊播：網路 task 填環狀緩衝 → 播放端拆 chunk；聲音給 I2S task，畫面以聲音時間為準顯示
// ============================================================================
std::vector<Mp3Song> mvSongs;

// 讀公開雲端資料夾清單（副檔名 ext），回傳歌曲數（失敗 -1）
int fetchDriveList(const char *folderUrl, const char *ext, std::vector<Mp3Song> &out) {
  String f = folderUrl;
  int k = f.indexOf("folders/");
  if (k >= 0) { f = f.substring(k + 8); int q = f.indexOf('?'); if (q >= 0) f = f.substring(0, q); }
  if (f.isEmpty()) return -1;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(client, "https://drive.google.com/embeddedfolderview?id=" + f);
  int code = http.GET();
  String html = code == 200 ? http.getString() : String("");
  http.end();
  if (code != 200) { Serial.printf("雲端清單讀取失敗 %d\n", code); return -1; }
  std::vector<Mp3Song> v;
  int pos = 0, el = strlen(ext);
  for (;;) {
    int a = html.indexOf("id=\"entry-", pos);
    if (a < 0) break;
    a += 10;
    int b = html.indexOf('"', a), t = html.indexOf("flip-entry-title\">", b);
    if (b < 0 || t < 0) break;
    t += 18;
    int e = html.indexOf('<', t);
    String id = html.substring(a, b), title = html.substring(t, e);
    title.replace("&amp;", "&"); title.replace("&#39;", "'"); title.replace("&quot;", "\"");
    String low = title; low.toLowerCase();
    if (low.endsWith(ext)) v.push_back({id, title.substring(0, title.length() - el)});
    pos = e;
  }
  xSemaphoreTake(songLock, portMAX_DELAY);
  out = v;
  xSemaphoreGive(songLock);
  return v.size();
}

// ---- 下載環狀緩衝（PSRAM）----
struct {
  uint8_t *buf = nullptr; size_t cap = 0;
  volatile size_t w = 0, r = 0;              // 累計寫入／讀出位元組
  volatile bool eof = false, stop = false, failed = false;
  String id;
} mvNet;

void mvNetTask(void *) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(client, "https://drive.usercontent.google.com/download?id=" + mvNet.id + "&export=download&confirm=t");
  int code = http.GET();
  if (code == 200) {
    int total = http.getSize();
    size_t got = 0;
    WiFiClient *st = http.getStreamPtr();
    uint32_t last = millis();
    while (!mvNet.stop && (total < 0 || (int)got < total) && millis() - last < 15000) {
      size_t space = mvNet.cap - (mvNet.w - mvNet.r);
      size_t a = st->available();
      if (!a || space < 4096) {
        if (!a && !http.connected()) break;
        delay(2);
        continue;
      }
      size_t wpos = mvNet.w % mvNet.cap;
      size_t n = min(min(a, space), mvNet.cap - wpos);
      n = st->readBytes(mvNet.buf + wpos, min(n, (size_t)16384));
      mvNet.w = mvNet.w + n;
      got += n;
      last = millis();
    }
  } else {
    Serial.printf("MV 下載失敗 %d\n", code);
    mvNet.failed = true;
  }
  http.end();
  mvNet.eof = true;
  vTaskDelete(nullptr);
}

// 從環狀緩衝讀 n bytes（等資料）；失敗回傳 false
bool mvRead(uint8_t *dst, size_t n) {
  size_t done = 0;
  while (done < n) {
    size_t avail = mvNet.w - mvNet.r;
    if (!avail) {
      if (mvNet.eof || mvNet.stop) return false;
      delay(2);
      continue;
    }
    size_t rpos = mvNet.r % mvNet.cap;
    size_t m = min(min(avail, n - done), mvNet.cap - rpos);
    memcpy(dst + done, mvNet.buf + rpos, m);
    mvNet.r = mvNet.r + m;
    done += m;
  }
  return true;
}

// ---- 聲音：解碼後的 PCM 環狀緩衝 → I2S task ----
#define MV_PCM_CAP (I2S_RATE * 3)                 // 3 秒
struct {
  int16_t *buf = nullptr;
  volatile size_t w = 0, r = 0;
  volatile uint32_t played = 0;                    // 已播出的樣本數 = 影音同步的時鐘
  volatile bool run = false, done = true;
} mvPcm;

void mvAudioTask(void *) {
  static int16_t out[480 * 2];
  uint32_t ramp = 0;
  while (mvPcm.run) {
    size_t avail = mvPcm.w - mvPcm.r;
    if (avail < 480) {                             // 網路跟不上：先送靜音，時鐘不前進（畫面會等）
      memset(out, 0, sizeof(out));
      audioI2S.write((uint8_t *)out, sizeof(out));
      continue;
    }
    int vol = volumePct;
    int64_t acc = 0;
    for (int i = 0; i < 480; i++) {
      int32_t s = (int32_t)mvPcm.buf[(mvPcm.r + i) % MV_PCM_CAP] * vol / 100;
      const uint32_t RAMP = I2S_RATE / 5;
      if (ramp < RAMP) s = s * (int32_t)ramp++ / (int32_t)RAMP;
      s = constrain(s, -SPK_LIMIT, SPK_LIMIT);
      out[i * 2] = out[i * 2 + 1] = s;
      acc += abs(s);
    }
    mvPcm.r = mvPcm.r + 480;
    speakLevel = (int)(acc / 480) * 2;
    audioI2S.write((uint8_t *)out, sizeof(out));
    mvPcm.played = mvPcm.played + 480;
  }
  memset(out, 0, sizeof(out));
  for (int i = 0; i < 4; i++) audioI2S.write((uint8_t *)out, sizeof(out));
  speakLevel = 0;
  mvPcm.done = true;
  vTaskDelete(nullptr);
}

// 顯示一張 JPEG 畫面（硬體解碼 → 放大 1.6 倍置中）
static uint8_t *mvJpgIn = nullptr;
static uint16_t *mvJpgOut = nullptr;
static size_t mvJpgInCap = 0, mvJpgOutCap = 0;
void mvShowFrame(const uint8_t *jpg, size_t len, int w, int h, bool hint) {
  if (len > mvJpgInCap) return;
  memcpy(mvJpgIn, jpg, len);
  jpeg_decode_cfg_t cfg = {};
  cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
  cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
  uint32_t n = 0;
  jpegLockTake();
  bool ok = jpeg_decoder_process(jpegDec, &cfg, mvJpgIn, len, (uint8_t *)mvJpgOut, mvJpgOutCap, &n) == ESP_OK;
  jpegLockGive();
  if (!ok) return;
  int ow = (w + 15) & ~15;
  int dw = LCD_W, dh = h * LCD_W / w, y0 = (LCD_H - dh) / 2;
  static int xmap[LCD_W];
  for (int x = 0; x < dw; x++) xmap[x] = x * w / dw;
  for (int y = 0; y < dh; y++) {
    const uint16_t *src = mvJpgOut + (y * h / dh) * ow;
    uint16_t *dst = hiBuf + (y0 + y) * LCD_W;
    for (int x = 0; x < dw; x++) dst[x] = src[xmap[x]];
  }
  if (hint) drawPill("點螢幕：停止播放", 296, C_GREEN);
  lcdFlush(hiBuf);
}

int lastMv = -1;

// 找使用者點名的 MV；找不到回傳 -1
int findRequestedMv(const String &userText) {
  String u = normTitle(userText);
  int best = -1, bestLen = 0;
  xSemaphoreTake(songLock, portMAX_DELAY);
  for (size_t i = 0; i < mvSongs.size(); i++) {
    String t = normTitle(mvSongs[i].title);
    if (t.length() >= 6 && u.indexOf(t) >= 0 && (int)t.length() > bestLen) { best = i; bestLen = t.length(); }
  }
  xSemaphoreGive(songLock);
  return best;
}

void playMv(const String &userText) {
  setUi(UI_THINK, "找 MV 中…", "");
  if (fetchDriveList(SONGS_MV_FOLDER, ".xkv", mvSongs) <= 0) {
    speakExpr = EX_SAD;
    speakAndShow("我還沒有 MV 可以播，請把轉好的 MV 檔放到雲端的 MV 資料夾喔。");
    return;
  }
  int n = mvSongs.size();
  int idx = findRequestedMv(userText);
  if (idx < 0) { idx = random(n); if (n > 1) while (idx == lastMv) idx = random(n); }
  lastMv = idx;
  Mp3Song m = mvSongs[idx];
  Serial.println("播 MV：" + m.title);
  if (speak("播放《" + m.title + "》的 MV！點一下螢幕就可以停止。")) waitPlaybackDone();

  // 緩衝區
  if (!mvNet.buf) { mvNet.cap = 6 * 1024 * 1024; mvNet.buf = (uint8_t *)ps_malloc(mvNet.cap); }
  if (!mvPcm.buf) mvPcm.buf = (int16_t *)ps_malloc(MV_PCM_CAP * 2);
  if (!mvJpgIn) {
    jpeg_decode_memory_alloc_cfg_t ic = {JPEG_DEC_ALLOC_INPUT_BUFFER}, oc = {JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    mvJpgIn = (uint8_t *)jpeg_alloc_decoder_mem(512 * 1024, &ic, &mvJpgInCap);
    mvJpgOut = (uint16_t *)jpeg_alloc_decoder_mem(1024 * 576 * 2, &oc, &mvJpgOutCap);
  }
  uint8_t *chunk = (uint8_t *)ps_malloc(512 * 1024);
  if (!mvNet.buf || !mvPcm.buf || !mvJpgIn || !mvJpgOut || !chunk) { free(chunk); showError("記憶體不足，無法播放 MV"); return; }

  mvNet.w = mvNet.r = 0; mvNet.eof = mvNet.stop = mvNet.failed = false; mvNet.id = m.id;
  setUi(UI_VIDEO, m.title, "");
  cv->fillScreen(C_BG);
  drawPill(("《" + m.title + "》MV 載入中…").c_str(), 160, C_GLOW);
  lcdFlush(hiBuf);
  xTaskCreatePinnedToCore(mvNetTask, "mvNet", 16384, nullptr, 3, nullptr, 0);

  uint8_t hdr[32];
  bool ok = mvRead(hdr, 32) && !memcmp(hdr, "XKV1", 4);
  int w = hdr[4] | hdr[5] << 8, h = hdr[6] | hdr[7] << 8, fps = hdr[8] | hdr[9] << 8;
  if (!ok || w <= 0 || w > 1024 || h <= 0 || h > 576 || fps <= 0) {
    mvNet.stop = true;
    while (!mvNet.eof) delay(10);
    free(chunk);
    setUi(UI_IDLE);
    speakExpr = EX_SAD;
    speakAndShow(mvNet.failed ? "MV 下載失敗，請確認雲端資料夾已經設成知道連結的人都能檢視。" : "這個 MV 檔案格式不對，請用轉檔工具重新轉一次。");
    return;
  }
  uint32_t t0 = millis();
  while (mvNet.w < 600 * 1024 && !mvNet.eof && millis() - t0 < 8000) delay(20);   // 先緩衝一點再開播

  HMP3Decoder dec = MP3InitDecoder();
  static int16_t pcm[MAX_NCHAN * MAX_NGRAN * MAX_NSAMP];
  mvPcm.w = mvPcm.r = mvPcm.played = 0;
  mvPcm.run = true; mvPcm.done = false;
  lcdBacklight(min(LCD_BRIGHTNESS, MV_BRIGHTNESS));
  xTaskCreatePinnedToCore(mvAudioTask, "mvAud", 4096, nullptr, 5, nullptr, 1);

  uint32_t frame = 0, shown = 0, dropped = 0, start = millis();
  bool stopped = false, wasTouched = buttonPressed();
  for (;;) {
    bool tch = buttonPressed();
    if (tch && !wasTouched) { stopped = true; break; }
    wasTouched = tch;
    uint8_t ch[4];
    if (!mvRead(ch, 4)) break;
    size_t len = ch[1] | ch[2] << 8 | ch[3] << 16;
    if (len > 512 * 1024 || !mvRead(chunk, len)) break;
    if (ch[0] == 'A') {
      uint8_t *p = chunk;
      int left = len;
      if (MP3Decode(dec, &p, &left, pcm, 0) == ERR_MP3_NONE) {
        MP3FrameInfo fi;
        MP3GetLastFrameInfo(dec, &fi);
        int ns = fi.outputSamps / max(1, fi.nChans);
        while (MV_PCM_CAP - (mvPcm.w - mvPcm.r) < (size_t)ns && mvPcm.run) {   // 聲音緩衝滿：等它播
          if (buttonPressed() && !wasTouched) { stopped = true; break; }
          delay(2);
        }
        if (stopped) break;
        for (int i = 0; i < ns; i++) mvPcm.buf[(mvPcm.w + i) % MV_PCM_CAP] = fi.nChans == 2 ? (pcm[i * 2] + pcm[i * 2 + 1]) / 2 : pcm[i];
        mvPcm.w = mvPcm.w + ns;
      }
    } else if (ch[0] == 'V') {
      uint32_t due = frame * 1000 / fps;            // 這張畫面該出現的時間（ms）
      frame++;
      uint32_t now = (uint32_t)((uint64_t)mvPcm.played * 1000 / I2S_RATE);
      if (now > due + 250) { dropped++; continue; } // 畫面落後太多：略過
      while (now + 5 < due) {                       // 太早：等聲音追上
        if (buttonPressed() && !wasTouched) { stopped = true; break; }
        delay(3);
        now = (uint32_t)((uint64_t)mvPcm.played * 1000 / I2S_RATE);
        if (mvPcm.w == mvPcm.r && mvNet.eof) break;
      }
      if (stopped) break;
      mvShowFrame(chunk, len, w, h, millis() - start < 4000);
      shown++;
    }
  }
  if (!stopped) while (mvPcm.w - mvPcm.r >= 480 && !buttonPressed()) delay(20);   // 把剩下的聲音播完
  mvNet.stop = true;
  mvPcm.run = false;
  while (!mvPcm.done || !mvNet.eof) delay(10);
  MP3FreeDecoder(dec);
  free(chunk);                                      // 播完把緩衝區還給系統（錄影、拍照要用）
  free(mvNet.buf); mvNet.buf = nullptr;
  free(mvPcm.buf); mvPcm.buf = nullptr;
  free(mvJpgIn); mvJpgIn = nullptr; mvJpgInCap = 0;
  free(mvJpgOut); mvJpgOut = nullptr; mvJpgOutCap = 0;
  lcdBacklight(LCD_BRIGHTNESS);
  Serial.printf("MV 結束：顯示 %u 張、略過 %u 張%s\n", (unsigned)shown, (unsigned)dropped, stopped ? "（點螢幕停止）" : "");
  while (buttonPressed()) delay(10);
  setUi(UI_IDLE);
  flushMic(200);
  lastInteraction = millis();
}

// ============================================================================
//  工作站語音問答：下載網站資料 → 板子上用關鍵字找最相關段落 → 交給 AI 回答
// ============================================================================
static const char *KNOW_NAME[] = {"照顧服務員技術稽核", "居家服務督導員工作手冊", "照服員單一級學科測驗", "照服員術科練習本", "長照組合計算系統"};
static const char *KNOW_URL[] = {
  "https://achir1015.github.io/caregiver-skills-audit/data.js",
  "https://achir1015.github.io/home-care-supervisor-manual/data.js",
  "https://docs.google.com/spreadsheets/d/1DHrXPAgagusv1DiVVf9hjIGy_IKOCTok2zkfMobNMPY/gviz/tq?tqx=out:csv&sheet=%E5%B7%A5%E4%BD%9C%E8%A1%A81",
  "https://achir1015.github.io/care-exam-practice/",
  "https://achir1015.github.io/Long-term-care-combined-calculation-system/"};
static const char *KNOW_EXAMPLE[] = {"「冰枕使用技術的稽核項目有哪些？」", "「居督員家訪要注意什麼？」",
                                     "「預防泌尿道感染的考題答案是什麼？」", "「術科洗手的步驟是什麼？」", "「BA01 基本身體清潔一次多少錢？」"};
static const std::initializer_list<const char *> KNOW_KEYS[] = {
  {"稽核"},
  {"督導", "居督", "工作手冊", "家訪", "個案管理"},
  {"學科", "題庫", "考題", "選擇題", "模擬考", "答案是"},
  {"術科", "考場", "操作步驟", "扣分", "易錯"},
  {"組合", "給付", "額度", "碼別", "自付", "部分負擔", "支付價格", "BA0", "BA1", "AA0", "GA0", "SC0"}};
const int KNOW_N = sizeof(KNOW_NAME) / sizeof(KNOW_NAME[0]);
static char *knowText[5] = {nullptr};
static size_t knowLen[5] = {0};
static uint32_t knowAt[5] = {0};

int knowledgeForTool(const String &name) {
  for (int i = 0; i < KNOW_N; i++) if (name == KNOW_NAME[i]) return i;
  return -1;
}

int matchKnowledge(const String &q) {
  String u = q;                                                   // 只把英文字母轉大寫（String::toUpperCase 會弄壞中文）
  for (size_t i = 0; i < u.length(); i++) if (u[i] >= 'a' && u[i] <= 'z') u.setCharAt(i, u[i] - 32);
  for (int i = 0; i < KNOW_N; i++)
    for (auto k : KNOW_KEYS[i]) if (u.indexOf(k) >= 0) return i;
  return -1;
}

bool loadKnowledge(int i) {
  if (knowText[i] && millis() - knowAt[i] < 6UL * 3600 * 1000) return true;   // 6 小時內用快取
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(client, KNOW_URL[i]);
  int code = http.GET();
  if (code != 200) { http.end(); Serial.printf("資料下載失敗 %s %d\n", KNOW_NAME[i], code); return knowText[i] != nullptr; }
  const size_t CAP = 1024 * 1024;
  char *buf = (char *)ps_malloc(CAP + 1);
  size_t len = 0;
  if (buf) {
    WiFiClient *st = http.getStreamPtr();
    int total = http.getSize();
    uint32_t last = millis();
    while (len < CAP && (total < 0 || (int)len < total) && millis() - last < 8000) {
      size_t a = st->available();
      if (a) { len += st->readBytes(buf + len, min(a, CAP - len)); last = millis(); }
      else if (!http.connected()) break;
      else delay(2);
    }
    buf[len] = 0;
  }
  http.end();
  if (!buf || len < 100) { free(buf); return knowText[i] != nullptr; }
  free(knowText[i]);
  knowText[i] = buf; knowLen[i] = len; knowAt[i] = millis();
  Serial.printf("資料載入 %s %u bytes\n", KNOW_NAME[i], (unsigned)len);
  return true;
}

// 找出和問題最相關的段落（以中文兩字詞比對），最多約 3500 bytes
String knowledgeContext(int i, const String &question) {
  if (!loadKnowledge(i)) return "";
  std::vector<String> grams;
  const char *q = question.c_str();
  int ql = question.length();
  for (int p = 0; p + 6 <= ql;) {                                 // 中文字 UTF-8 每字 3 bytes：取相鄰兩字
    uint8_t c = q[p];
    if ((c >> 4) == 14 && ((uint8_t)q[p + 3] >> 4) == 14) {
      String g = question.substring(p, p + 6);
      if (g != "什麼" && g != "哪些" && g != "多少" && g != "是不" && g != "可以" && g != "請問" && g != "怎麼" && g != "如何")
        grams.push_back(g);
      p += 3;
    } else p += (c < 0x80) ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
  }
  for (int p = 0; p < ql;) {                                      // 英數字詞（例如 BA01）
    if (isalnum((uint8_t)q[p])) { int e = p; while (e < ql && isalnum((uint8_t)q[e])) e++; if (e - p >= 2) grams.push_back(question.substring(p, e)); p = e; }
    else p++;
  }
  const char *t = knowText[i];
  size_t len = knowLen[i];
  const size_t CH = 700;
  struct Hit { size_t pos; int score; };
  std::vector<Hit> hits;
  for (size_t pos = 0; pos < len; pos += CH / 2) {                // 段落重疊一半，避免答案剛好被切開
    size_t end = min(len, pos + CH);
    int score = 0;
    for (auto &g : grams) {
      if (std::search(t + pos, t + end, g.c_str(), g.c_str() + g.length()) != t + end) score += g.length() > 6 ? 3 : 2;
    }
    if (score > 0) hits.push_back({pos, score});
  }
  std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) { return a.score > b.score; });
  String out = String("以下是「") + KNOW_NAME[i] + "」的資料片段（原始格式，可能含程式碼符號），請根據內容回答，資料沒有的不要亂編：\n";
  std::vector<size_t> used;
  for (auto &h : hits) {
    if (out.length() > 3500) break;
    bool dup = false;
    for (auto u : used) if ((h.pos > u ? h.pos - u : u - h.pos) < CH) dup = true;
    if (dup) continue;
    used.push_back(h.pos);
    size_t s = h.pos, e = min(len, h.pos + CH);
    while (s > 0 && ((uint8_t)t[s] & 0xC0) == 0x80) s--;          // 對齊 UTF-8 字元邊界
    while (e < len && ((uint8_t)t[e] & 0xC0) == 0x80) e++;
    out += "…" + String(t + s).substring(0, e - s) + "…\n";
  }
  Serial.printf("[問答] %s：%d 個關鍵詞、命中 %d 段，送出 %u bytes\n", KNOW_NAME[i], (int)grams.size(), (int)hits.size(), (unsigned)out.length());
  return hits.empty() ? String("「") + KNOW_NAME[i] + "」的資料裡找不到和問題相關的內容。\n" : out;
}


// ============================================================================
//  病房助理工作站：觸控選單（工具清單開機時從工作站網頁解析，新增工具會自動更新）
// ============================================================================
struct PortalCat { String id, name; };
struct PortalTool { String name, desc, url, cat; };
std::vector<PortalCat> portalCats;
std::vector<PortalTool> portalTools;
volatile int portalCat = -1, portalPage = 0, portalSel = -1;   // 類別（-1 = 全部）、頁數、選中的工具（-1 = 清單）
uint32_t portalSince = 0;
const int PORTAL_PER_PAGE = 8;

// 取出 key:'值' 或 key:"值"
String jsField(const String &s, const char *key) {
  String k = String(key) + ":";
  int a = s.indexOf(k);
  if (a < 0) return "";
  a += k.length();
  while (a < (int)s.length() && s[a] == ' ') a++;
  if (a >= (int)s.length()) return "";
  char q = s[a];
  if (q != '\'' && q != '"') return "";
  int b = s.indexOf(q, a + 1);
  return b > a ? s.substring(a + 1, b) : String("");
}

int fetchPortalTools() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  http.begin(client, PORTAL_URL);
  int code = http.GET();
  String html = code == 200 ? http.getString() : String("");
  http.end();
  if (code != 200) { Serial.printf("工作站清單讀取失敗 %d\n", code); return -1; }
  std::vector<PortalCat> cats;
  std::vector<PortalTool> tools;
  for (int p = 0; (p = html.indexOf("new Category(", p)) >= 0; p++) {   // new Category('admin', '排班與行政', …)
    int e = html.indexOf(')', p);
    String s = html.substring(p + 13, e);
    std::vector<String> parts;
    for (int i = 0; i < 2; i++) {
      int a = s.indexOf('\''), b = a >= 0 ? s.indexOf('\'', a + 1) : -1;
      if (a < 0 || b < 0) break;
      parts.push_back(s.substring(a + 1, b));
      s = s.substring(b + 1);
    }
    if (parts.size() == 2) cats.push_back({parts[0], parts[1]});
  }
  for (int p = 0; (p = html.indexOf("new Tool({", p)) >= 0; p++) {
    int e = html.indexOf("})", p);
    String s = html.substring(p, e);
    PortalTool t{jsField(s, "name"), jsField(s, "desc"), jsField(s, "url"), jsField(s, "category")};
    if (t.name.length() && t.url.length()) tools.push_back(t);
  }
  if (tools.empty()) return -1;
  xSemaphoreTake(songLock, portMAX_DELAY);
  portalCats = cats;
  portalTools = tools;
  xSemaphoreGive(songLock);
  Serial.printf("工作站：%d 類、%d 個工具\n", (int)cats.size(), (int)tools.size());
  return tools.size();
}

// 目前類別的工具索引
std::vector<int> portalFiltered() {
  std::vector<int> v;
  for (size_t i = 0; i < portalTools.size(); i++)
    if (portalCat < 0 || portalCat >= (int)portalCats.size() || portalTools[i].cat == portalCats[portalCat].id) v.push_back(i);
  return v;
}

String portalCatName(const String &id) {
  for (auto &c : portalCats) if (c.id == id) return c.name;
  return "";
}

// ---- 版面（邏輯座標 512x300）----
bool inBtn(const Btn &b, int x, int y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }
void drawBtn(const Btn &b, const char *label, uint16_t bg, uint16_t fg) {
  cv->fillRoundRect(b.x, b.y, b.w, b.h, 6, bg);
  u8f.setForegroundColor(fg);
  u8f.drawUTF8(b.x + (b.w - u8f.getUTF8Width(label)) / 2, b.y + b.h / 2 + 6, label);
}
Btn catTab(int i, int n) { int w = (SCREEN_W - 16) / n; return {8 + i * w, 80, w - 4, 24}; }
Btn toolCard(int k) { return {8 + (k % 2) * 250, 110 + (k / 2) * 46, 244, 42}; }

String clipText(const String &s, int w) {                       // 超過寬度就截斷加「…」
  if (u8f.getUTF8Width(s.c_str()) <= w) return s;
  String out;
  for (size_t i = 0; i < s.length();) {
    uint8_t c = s[i];
    int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
    String next = out + s.substring(i, i + len);
    if (u8f.getUTF8Width((next + "…").c_str()) > w) break;
    out = next; i += len;
  }
  return out + "…";
}

void drawPortalMenu(uint32_t now) {
  xSemaphoreTake(songLock, portMAX_DELAY);
  u8f.setForegroundColor(C_ORANGE);
  u8f.drawUTF8(8, 68, PORTAL_NAME);
  drawBtn(BTN_HOME, "回主畫面", C_BAR_LINE, C_WHITE);
  if (portalTools.empty()) {
    u8f.setForegroundColor(C_GRAY);
    u8f.drawUTF8(8, 120, "讀取工具清單中…（需要網路）");
    xSemaphoreGive(songLock);
    return;
  }
  int sel = portalSel;
  if (sel >= 0 && sel < (int)portalTools.size()) {                // ---- 詳細頁 ----
    const PortalTool &t = portalTools[sel];
    u8f.setForegroundColor(C_GLOW);
    u8f.drawUTF8(8, 104, t.name.c_str());
    u8f.setForegroundColor(C_GRAY);
    u8f.drawUTF8(8, 126, ("類別：" + portalCatName(t.cat)).c_str());
    std::vector<String> ls = u8f.wrap(t.desc, 272);
    u8f.setForegroundColor(C_WHITE);
    for (size_t i = 0; i < ls.size() && i < 3; i++) u8f.drawUTF8(8, 150 + i * LINE_H, ls[i].c_str());
    int y = 150 + min((int)ls.size(), 3) * LINE_H + 10;
    int k = knowledgeForTool(t.name);
    u8f.setForegroundColor(k >= 0 ? C_GREEN : C_GRAY);
    std::vector<String> hs = u8f.wrap(k >= 0 ? String("可以直接用說的問我這個工具的內容，例如：") + KNOW_EXAMPLE[k] : String("這個工具要用手機開啟操作，請掃描右邊的 QR 碼。"), 272);
    for (size_t i = 0; i < hs.size() && i < 4; i++) u8f.drawUTF8(8, y + i * LINE_H, hs[i].c_str());
    drawQr(296, 84, 200);
    u8f.setForegroundColor(C_GRAY);
    u8f.drawUTF8(296 + (200 - u8f.getUTF8Width("手機掃描開啟")) / 2, 300 - 6, "手機掃描開啟");
    drawBtn(BTN_BACK, "返回清單", C_BAR_LINE, C_WHITE);
    xSemaphoreGive(songLock);
    return;
  }
  // ---- 清單頁：類別分頁 + 工具卡 ----
  int n = portalCats.size() + 1;
  for (int i = 0; i < n; i++) {
    bool on = (i == 0 && portalCat < 0) || (i > 0 && portalCat == i - 1);
    drawBtn(catTab(i, n), i == 0 ? "全部" : portalCats[i - 1].name.c_str(), on ? C_GLOW_DIM : C_PANEL, on ? C_WHITE : C_GRAY);
  }
  std::vector<int> v = portalFiltered();
  int pages = max(1, (int)((v.size() + PORTAL_PER_PAGE - 1) / PORTAL_PER_PAGE));
  int pg = constrain((int)portalPage, 0, pages - 1);
  if (pages > 1) {
    drawBtn(BTN_PREV, "上一頁", pg > 0 ? C_BAR_LINE : C_PANEL, pg > 0 ? C_WHITE : C_GRAY);
    drawBtn(BTN_NEXT, "下一頁", pg < pages - 1 ? C_BAR_LINE : C_PANEL, pg < pages - 1 ? C_WHITE : C_GRAY);
    char pn[12]; snprintf(pn, sizeof(pn), "%d/%d", pg + 1, pages);
    u8f.setForegroundColor(C_GRAY);
    u8f.drawUTF8(200, 68, pn);
  }
  for (int k = 0; k < PORTAL_PER_PAGE && pg * PORTAL_PER_PAGE + k < (int)v.size(); k++) {
    const PortalTool &t = portalTools[v[pg * PORTAL_PER_PAGE + k]];
    Btn b = toolCard(k);
    cv->fillRoundRect(b.x, b.y, b.w, b.h, 6, C_PANEL);
    cv->drawRoundRect(b.x, b.y, b.w, b.h, 6, C_BAR_LINE);
    u8f.setForegroundColor(knowledgeForTool(t.name) >= 0 ? C_GLOW : C_WHITE);
    u8f.drawUTF8(b.x + 8, b.y + 18, clipText(t.name, b.w - 16).c_str());
    u8f.setForegroundColor(C_GRAY);
    u8f.drawUTF8(b.x + 8, b.y + 36, clipText(t.desc, b.w - 16).c_str());
  }
  u8f.setForegroundColor(C_GLOW_DIM);
  if (v.empty()) u8f.drawUTF8(8, 140, "這個類別沒有工具");
  xSemaphoreGive(songLock);
}

void openPortalMenu() {
  portalOpen = true;
  portalCat = -1; portalPage = 0; portalSel = -1;
  portalSince = millis();
  if (portalTools.empty()) fetchPortalTools();
  setUi(UI_PORTAL);
  Serial.println("（開啟" PORTAL_NAME "選單）");
}

// 選單中點螢幕；回傳 true = 要離開
bool portalTouch(int x, int y) {
  portalSince = millis();
  if (inBtn(BTN_HOME, x, y)) return true;
  xSemaphoreTake(songLock, portMAX_DELAY);
  if (portalSel >= 0) {
    if (inBtn(BTN_BACK, x, y)) portalSel = -1;
    xSemaphoreGive(songLock);
    setUi(UI_PORTAL);
    return false;
  }
  int n = portalCats.size() + 1;
  for (int i = 0; i < n; i++)
    if (inBtn(catTab(i, n), x, y)) { portalCat = i - 1; portalPage = 0; }
  std::vector<int> v = portalFiltered();
  int pages = max(1, (int)((v.size() + PORTAL_PER_PAGE - 1) / PORTAL_PER_PAGE));
  if (pages > 1 && inBtn(BTN_PREV, x, y) && portalPage > 0) portalPage = portalPage - 1;
  if (pages > 1 && inBtn(BTN_NEXT, x, y) && portalPage < pages - 1) portalPage = portalPage + 1;
  int pg = constrain((int)portalPage, 0, pages - 1);
  String url;
  for (int k = 0; k < PORTAL_PER_PAGE && pg * PORTAL_PER_PAGE + k < (int)v.size(); k++)
    if (inBtn(toolCard(k), x, y)) { portalSel = v[pg * PORTAL_PER_PAGE + k]; url = portalTools[portalSel].url; }
  xSemaphoreGive(songLock);
  if (url.length()) makeQr(url);
  setUi(UI_PORTAL);
  return false;
}

// ---- 攝影（即時影像）與錄影 ----
uint32_t cameraSince = 0;

void startCamera(bool announce) {
  if (!cameraOK) {
    speakExpr = EX_SAD;
    speakAndShow("我的鏡頭沒有偵測到，沒辦法攝影喔。");
    return;
  }
  if (cameraActive && announce) {
    speakExpr = EX_HAPPY;
    speakAndShow("已經在攝影囉！要錄影就說「錄製」。");
    return;
  }
  if (announce) {
    speakExpr = EX_HAPPY;
    speakAndShow("好的，開啟鏡頭！要錄影就說「錄製」，點一下螢幕回到原畫面。");
  }
  cameraSince = millis();
  cameraActive = true;
  setUi(UI_IDLE);
  Serial.println("（攝影模式）");
}

void enterCamera() { startCamera(true); }

void exitCamera() {
  while (buttonPressed()) delay(10);                     // 等放開按鍵，避免接著打開說明
  cameraActive = false;
  setUi(UI_IDLE);
  flushMic(200);
  lastInteraction = millis();
  Serial.println("（離開攝影模式）");
}

// 錄影時的影像：獨立 task 抓影格（主程式要同時聽「停止」）
struct {
  uint8_t *buf; size_t cap; volatile size_t len; size_t maxFrame;
  std::vector<uint32_t> off, sz;
  volatile bool run, done, full;
} recVid;

#define REC_W 480                        // 錄影解析度（硬體 JPEG 編碼，寬高需 16 的倍數）
#define REC_H 272
void recVideoTask(void *) {
  uint32_t lastShot = 0;
  while (recVid.run) {
    if (millis() - lastShot < 1000UL / REC_FPS) { delay(2); continue; }
    lastShot = millis();
    jpegLockTake();
    size_t len = 0;
    const uint8_t *jpg = camGrab(jpegInBuf, REC_W, REC_H, REC_W) ? jpegEncode(REC_W, REC_H, REC_JPEG_QUALITY, &len) : nullptr;
    if (jpg && len) {
      if (recVid.len + len + 2 > recVid.cap) { jpegLockGive(); recVid.full = true; break; }
      memcpy(recVid.buf + recVid.len, jpg, len);
      recVid.off.push_back(recVid.len);
      recVid.sz.push_back(len);
      recVid.len += (len + 1) & ~1u;
      recVid.maxFrame = max(recVid.maxFrame, len);
    }
    jpegLockGive();
  }
  recVid.done = true;
  vTaskDelete(nullptr);
}

bool isStopWord(const String &t) {
  if (containsAny(t, {"停止", "停下", "結束錄", "上傳", "不要錄了", "錄好了"})) return true;
  return t.length() <= 9 && t.indexOf("停") >= 0;        // 只說「停」「停！」
}

// 錄影時的聲音：獨立 task 持續讀麥克風，避免漏音
struct { int16_t *buf; size_t cap; volatile size_t n; volatile bool run; volatile bool done; } recAud;

void recAudioTask(void *) {
  while (recAud.run && recAud.n + FRAME <= recAud.cap) {
    readFrame(recAud.buf + recAud.n);
    recAud.n += FRAME;
  }
  recAud.done = true;
  vTaskDelete(nullptr);
}

// AVI（MJPEG 影像 + PCM 聲音）邊產生邊 base64，直接當 HTTP 上傳內容（不用第二份記憶體）
struct Seg { const uint8_t *p; uint32_t n; };

class AviB64Stream : public Stream {
 public:
  std::vector<Seg> segs;
  size_t rawTotal = 0, b64Total = 0, b64Sent = 0;
  int lastPct = -1;
  size_t write(uint8_t) override { return 0; }
  int available() override { return b64Sent < b64Total ? (int)min((size_t)0x7FFFFFFF, b64Total - b64Sent) : 0; }
  int read() override { char c; return readBytes(&c, 1) == 1 ? (uint8_t)c : -1; }
  int peek() override { return -1; }
  void flush() override {}
  size_t readBytes(char *buf, size_t n) override {
    size_t k = 0;
    while (k < n) {
      if (outPos >= outLen && !refill()) break;
      size_t m = min(n - k, outLen - outPos);
      memcpy(buf + k, out + outPos, m);
      outPos += m; k += m;
    }
    b64Sent += k;
    int pct = b64Total ? (int)(b64Sent * 100 / b64Total) : 0;
    if (pct / 5 != lastPct / 5) {
      lastPct = pct;
      setUi(UI_THINK, "影片上傳中…", String(pct) + "%");
    }
    return k;
  }
 private:
  uint8_t out[1032];
  size_t outLen = 0, outPos = 0, seg = 0, segPos = 0;
  size_t rawRead(uint8_t *dst, size_t max) {
    size_t k = 0;
    while (k < max && seg < segs.size()) {
      size_t m = min(max - k, (size_t)segs[seg].n - segPos);
      if (segs[seg].p) memcpy(dst + k, segs[seg].p + segPos, m);
      else memset(dst + k, 0, m);                       // p = nullptr：補齊用的 0
      k += m; segPos += m;
      if (segPos >= segs[seg].n) { seg++; segPos = 0; }
    }
    return k;
  }
  bool refill() {
    uint8_t raw[768];                                   // 3 的倍數：中間不會出現 '=' 補位
    size_t r = rawRead(raw, sizeof(raw));
    if (!r) return false;
    size_t o = 0;
    mbedtls_base64_encode(out, sizeof(out), &o, raw, r);
    outLen = o; outPos = 0;
    return true;
  }
};

// 16bit PCM → 8bit μ-law（G.711，電話音質，檔案只有 1/4）
static uint8_t linear2ulaw(int pcm) {
  const int BIAS = 0x84, CLIP = 32635;
  int sign = 0;
  if (pcm < 0) { sign = 0x80; pcm = -pcm; }
  if (pcm > CLIP) pcm = CLIP;
  pcm += BIAS;
  int exponent = 7;
  for (int mask = 0x4000; (pcm & mask) == 0 && exponent > 0; exponent--, mask >>= 1) {}
  int mantissa = (pcm >> (exponent + 3)) & 0x0F;
  return ~(sign | (exponent << 4) | mantissa);
}

static void put32(uint8_t *&p, uint32_t v) { memcpy(p, &v, 4); p += 4; }
static void put16(uint8_t *&p, uint16_t v) { memcpy(p, &v, 2); p += 2; }
static void putCC(uint8_t *&p, const char *cc) { memcpy(p, cc, 4); p += 4; }

// 錄影 REC_SECONDS 秒並上傳到 Google 雲端硬碟；回傳給使用者聽的結果
String recordVideo() {
  if (!cameraOK) return "我的鏡頭沒有偵測到，沒辦法錄影喔。";
  if (cfg.gdrive.isEmpty()) return "還沒設定雲端硬碟上傳網址，請先到設定網頁填好，再叫我錄製喔。";
  if (cfg.gdrive.indexOf("script.google.com") < 0)
    return "雲端硬碟上傳網址填錯了，要填 Apps Script 的網頁應用程式網址，不是資料夾網址喔。";
  if (!cameraActive) startCamera(false);

  // 借用語音播放緩衝區（錄影時不會說話），錄完再還回去
  free(tts.buf);
  tts.buf = nullptr;
  tts.cap = 0;
  // 可用記憶體依比例分給聲音（32KB/秒）與影像（約 70KB/秒），兩者差不多同時滿
  size_t freeBlk = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  size_t budget = freeBlk > 4 * 1024 * 1024 ? min(freeBlk - 2 * 1024 * 1024, (size_t)12 * 1024 * 1024) : 0;
  recAud.cap = min((size_t)MIC_SAMPLE_RATE * REC_SECONDS, budget * 32 / 77 / 2);
  recAud.buf = (int16_t *)ps_malloc(recAud.cap * 2);
  size_t vcap = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  vcap = vcap > 3 * 1024 * 1024 ? min(vcap - 2 * 1024 * 1024, (size_t)8 * 1024 * 1024) : 0;
  uint8_t *vbuf = vcap ? (uint8_t *)ps_malloc(vcap) : nullptr;
  if (!recAud.buf || !vbuf) {
    free(recAud.buf); free(vbuf);
    tts.buf = (uint8_t *)ps_malloc(TTS_BUF_BYTES);
  tts.cap = tts.buf ? TTS_BUF_BYTES : 0;
    return "記憶體不夠，沒辦法錄影。";
  }
  Serial.printf("錄影：影像緩衝 %u KB，聲音 %u KB\n", (unsigned)(vcap / 1024), (unsigned)(recAud.cap * 2 / 1024));


  recVid.buf = vbuf; recVid.cap = vcap; recVid.len = 0; recVid.maxFrame = 0;
  recVid.off.clear(); recVid.sz.clear();
  recVid.off.reserve(REC_SECONDS * REC_FPS + 8);
  recVid.sz.reserve(REC_SECONDS * REC_FPS + 8);
  recVid.run = true; recVid.done = false; recVid.full = false;
  bool stopped = false, heardStop = false;

  flushMic(60);
  recAud.n = 0; recAud.run = true; recAud.done = false;
  recLastFrame = nullptr; recLastLen = 0;
  recStart = millis();
  recActive = true;
  setUi(UI_IDLE);
  xTaskCreatePinnedToCore(recAudioTask, "recAud", 4096, nullptr, 4, nullptr, 1);
  xTaskCreatePinnedToCore(recVideoTask, "recVid", 4096, nullptr, 3, nullptr, 1);
  Serial.println("（開始錄影，說「停止」結束）");

  // 一邊錄一邊聽：對錄到的聲音做語音偵測，一句話結束就送辨識，聽到「停止」就結束
  int thr = vadThreshold();
  size_t pos = 0, segStart = 0;
  bool inSpeech = false;
  int loud = 0, silentMs = 0, voicedMs = 0;
  bool btnPrev = buttonPressed();
  while (!heardStop && !stopped) {
    bool b = buttonPressed();
    if (b && !btnPrev) { stopped = true; break; }        // 按鍵：停止
    btnPrev = b;
    if (recVid.full || recAud.done || millis() - recStart > REC_SECONDS * 1000UL) break;   // 記憶體滿或到上限
    size_t avail = recAud.n;
    bool checkSeg = false;
    size_t segEnd = 0;
    while (pos + FRAME <= avail && !checkSeg) {
      int64_t sum = 0;
      const int16_t *f = recAud.buf + pos;
      for (int i = 0; i < FRAME; i++) sum += (int32_t)f[i] * f[i];
      int rms = (int)sqrtf((float)(sum / FRAME));
      micLevel = rms;
      if (!inSpeech) {
        loud = rms > thr ? loud + 1 : 0;
        if (loud >= 3) {
          inSpeech = true;
          segStart = pos > FRAME * 15 ? pos - FRAME * 15 : 0;   // 往前多留 0.3 秒
          silentMs = 0; voicedMs = 60;
        }
      } else {
        if (rms > thr * 0.7f) { silentMs = 0; voicedMs += 20; } else silentMs += 20;
        if (silentMs >= 700 || pos - segStart > (size_t)MIC_SAMPLE_RATE * 6) {   // 一句話結束（最長 6 秒）
          inSpeech = false;
          if (voicedMs >= 300) { checkSeg = true; segEnd = pos; }
        }
      }
      pos += FRAME;
    }
    if (checkSeg) {
      size_t n = min(segEnd - segStart, MAX_SAMPLES);
      memcpy(wavBuf + 44, recAud.buf + segStart, n * 2);
      String heard = speechToText(n);                    // 影像與聲音照常在背景錄
      Serial.println("（錄影中聽到：" + heard + "）");
      if (isStopWord(heard)) heardStop = true;
    }
    delay(20);
  }
  recVid.run = false;
  recAud.run = false;
  while (!recVid.done || !recAud.done) delay(5);
  uint32_t durMs = millis() - recStart;
  recActive = false;
  micLevel = 0;
  while (buttonPressed()) delay(10);

  std::vector<uint32_t> &off = recVid.off, &len = recVid.sz;
  size_t vlen = recVid.len, maxFrame = recVid.maxFrame;
  bool full = recVid.full || recAud.done && recAud.n + FRAME > recAud.cap;
  size_t frames = off.size();
  size_t ulawN = recAud.n / 2;                           // 8kHz 樣本數 = μ-law 位元組數
  uint8_t *ulaw = (uint8_t *)recAud.buf;
  for (size_t i = 0; i < ulawN; i++)                     // 兩點平均降頻到 8kHz，再編成 μ-law（寫在原緩衝區前段）
    ulaw[i] = linear2ulaw(((int)recAud.buf[2 * i] + recAud.buf[2 * i + 1]) / 2);
  size_t audBytes = ulawN;
  if (recAud.n) durMs = (uint32_t)((uint64_t)recAud.n * 1000 / MIC_SAMPLE_RATE);   // 以聲音長度為準，影音才會同步
  Serial.printf("錄影結束：%u 張，%u ms，影像 %u KB%s%s\n", (unsigned)frames, (unsigned)durMs, (unsigned)(vlen / 1024),
                full ? "（記憶體滿）" : "", heardStop ? "（聽到停止）" : stopped ? "（按鍵停止）" : "");
  String result;
  if (frames < 5 || durMs < 1000) {
    result = "錄到的畫面太少，沒有存檔。";
  } else {
    // ---- 組 AVI ----
    bool hasAudio = audBytes > 0;
    uint32_t fpsRate = (uint32_t)((uint64_t)frames * 1000000ULL / durMs);   // scale = 1000
    uint8_t *chunkHdr = (uint8_t *)ps_malloc(frames * 16 + 8);
    uint8_t *idx = (uint8_t *)ps_malloc(frames * 32 + 8);
    static const uint8_t zero2[2] = {0, 0};
    AviB64Stream st;
    std::vector<Seg> movi;
    uint32_t moviSize = 4, idxCount = 0, audPos = 0, maxAud = 0;
    uint8_t *ip = idx;
    for (size_t i = 0; i < frames; i++) {
      uint8_t *h = chunkHdr + i * 16;
      uint8_t *hp = h;
      putCC(hp, "00dc"); put32(hp, len[i]);
      movi.push_back({h, 8});
      movi.push_back({vbuf + off[i], len[i]});
      if (len[i] & 1) movi.push_back({zero2, 1});
      putCC(ip, "00dc"); put32(ip, 0x10); put32(ip, moviSize); put32(ip, len[i]); idxCount++;
      moviSize += 8 + ((len[i] + 1) & ~1u);
      if (hasAudio) {
        uint32_t end = (uint32_t)((uint64_t)ulawN * (i + 1) / frames);
        uint32_t n = end - audPos;
        if (n) {
          putCC(hp, "01wb"); put32(hp, n);
          movi.push_back({h + 8, 8});
          movi.push_back({ulaw + audPos, n});
          putCC(ip, "01wb"); put32(ip, 0x10); put32(ip, moviSize); put32(ip, n); idxCount++;
          moviSize += 8 + n;
          audPos = end;
          maxAud = max(maxAud, n);
        }
      }
    }
    uint32_t idxSize = idxCount * 16;
    uint32_t hdrlSize = 4 + 64 + 124 + (hasAudio ? 100 : 0);
    uint32_t riffSize = 4 + (8 + hdrlSize) + (8 + moviSize) + (8 + idxSize);

    static uint8_t hdr[512];
    uint8_t *p = hdr;
    putCC(p, "RIFF"); put32(p, riffSize); putCC(p, "AVI ");
    putCC(p, "LIST"); put32(p, hdrlSize); putCC(p, "hdrl");
    putCC(p, "avih"); put32(p, 56);
    put32(p, (uint32_t)((uint64_t)durMs * 1000 / frames));          // 每張微秒
    put32(p, (uint32_t)((vlen + audBytes) * 1000ULL / durMs));      // 每秒最大位元組
    put32(p, 0); put32(p, 0x110);                                   // HASINDEX | ISINTERLEAVED
    put32(p, frames); put32(p, 0); put32(p, hasAudio ? 2 : 1);
    put32(p, maxFrame + 8); put32(p, REC_W); put32(p, REC_H);
    put32(p, 0); put32(p, 0); put32(p, 0); put32(p, 0);
    // 影像串流
    putCC(p, "LIST"); put32(p, 116); putCC(p, "strl");
    putCC(p, "strh"); put32(p, 56);
    putCC(p, "vids"); putCC(p, "MJPG"); put32(p, 0); put16(p, 0); put16(p, 0); put32(p, 0);
    put32(p, 1000); put32(p, fpsRate); put32(p, 0); put32(p, frames);
    put32(p, maxFrame); put32(p, 0xFFFFFFFF); put32(p, 0);
    put16(p, 0); put16(p, 0); put16(p, REC_W); put16(p, REC_H);
    putCC(p, "strf"); put32(p, 40);
    put32(p, 40); put32(p, REC_W); put32(p, REC_H); put16(p, 1); put16(p, 24);
    putCC(p, "MJPG"); put32(p, REC_W * REC_H * 3); put32(p, 0); put32(p, 0); put32(p, 0); put32(p, 0);
    if (hasAudio) {                                                // 聲音串流：8kHz 8bit 單聲道 μ-law
      putCC(p, "LIST"); put32(p, 92); putCC(p, "strl");
      putCC(p, "strh"); put32(p, 56);
      putCC(p, "auds"); put32(p, 0); put32(p, 0); put16(p, 0); put16(p, 0); put32(p, 0);
      put32(p, 1); put32(p, MIC_SAMPLE_RATE / 2); put32(p, 0); put32(p, ulawN);
      put32(p, maxAud); put32(p, 0xFFFFFFFF); put32(p, 1);
      put16(p, 0); put16(p, 0); put16(p, 0); put16(p, 0);
      putCC(p, "strf"); put32(p, 16);
      put16(p, 7); put16(p, 1); put32(p, MIC_SAMPLE_RATE / 2); put32(p, MIC_SAMPLE_RATE / 2); put16(p, 1); put16(p, 8);   // 7 = μ-law
    }
    putCC(p, "LIST"); put32(p, moviSize); putCC(p, "movi");
    st.segs.push_back({hdr, (uint32_t)(p - hdr)});
    for (auto &m : movi) st.segs.push_back(m);
    static uint8_t idxHdr[8];
    uint8_t *q = idxHdr;
    putCC(q, "idx1"); put32(q, idxSize);
    st.segs.push_back({idxHdr, 8});
    st.segs.push_back({idx, idxSize});
    st.rawTotal = 8 + riffSize;
    st.b64Total = (st.rawTotal + 2) / 3 * 4;
    Serial.printf("AVI %u KB（base64 %u KB）上傳中…\n", (unsigned)(st.rawTotal / 1024), (unsigned)(st.b64Total / 1024));

    // ---- 上傳 ----
    char name[48];
    struct tm t;
    if (getLocalTime(&t, 10)) strftime(name, sizeof(name), "%Y%m%d_%H%M%S", &t);
    else snprintf(name, sizeof(name), "%lu", (unsigned long)millis());
    String fname = String("XiaoKe_video_") + name + ".avi";
    String enc;
    for (size_t i = 0; i < fname.length(); i++) {
      char c = fname[i];
      if (isalnum((uint8_t)c) || c == '_' || c == '.' || c == '-') enc += c;
      else { char hx[4]; snprintf(hx, sizeof(hx), "%%%02X", (uint8_t)c); enc += hx; }
    }
    setUi(UI_THINK, "影片上傳中…", "0%");
    tts.buf = (uint8_t *)ps_malloc(160 * 1024);
    tts.cap = tts.buf ? 160 * 1024 : 0;
    if (tts.buf) speak(heardStop ? "好的，停止錄製，存檔中，請稍等喔！" : "錄製結束，存檔中，請稍等喔！");
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(180000);
    http.begin(client, cfg.gdrive + "?token=" GDRIVE_TOKEN "&type=video&name=" + enc);
    http.addHeader("Content-Type", "text/plain");
    int code = http.sendRequest("POST", &st, st.b64Total);
    String resp = code == 200 ? http.getString() : String("");
    http.end();
    Serial.printf("影片上傳 → %d %s\n", code, resp.substring(0, 60).c_str());
    if (code == 200 || code == 302)                              // Apps Script 存好檔後會 302 轉址
      result = String(full ? "記憶體滿了，先幫你存這一段。" : "") + "錄好了！" + String(durMs / 1000) + "秒的影片已經存到雲端硬碟囉。";
    else
      result = "影片上傳失敗（" + String(code) + "），請檢查雲端硬碟上傳網址。";
    if (code == 200 || code == 302) Serial.println(heardStop ? "（聽到停止）" : stopped ? "（按鍵停止）" : "（自動結束）");
    free(chunkHdr);
    free(idx);
  }
  waitPlaybackDone();
  free(tts.buf);
  free(vbuf);
  free(recAud.buf);
  recAud.buf = nullptr;
  tts.buf = (uint8_t *)ps_malloc(TTS_BUF_BYTES);
  tts.cap = tts.buf ? TTS_BUF_BYTES : 0;
  if (!tts.buf) Serial.println("警告：語音緩衝區重新配置失敗");
  flushMic(200);
  lastInteraction = millis();
  return result;
}

// ---- 病房助理工作站 ----
uint32_t qrSince = 0;

void showPortal() {
  makeQr(PORTAL_URL);
  setUi(UI_QR, PORTAL_NAME,
        "長期照顧、照服員、居服員、居家督導、長照 2.0／3.0 等資料，都可以在" PORTAL_NAME "查詢。請用手機掃描左邊的 QR 碼進入。");
  qrSince = millis();
  Serial.println("（顯示" PORTAL_NAME " QR 碼）");
  if (speak("關於長期照顧、照服員、居服員、居家督導和長照二點零、三點零的資料，都可以在" PORTAL_NAME
            "查詢喔！請用手機掃描螢幕上的 QR 碼進入查詢。"))
    waitPlaybackDone();
  qrSince = millis();
  lastInteraction = millis();
}

void exitSong() {
  setUi(UI_IDLE);
  flushMic(200);
  lastInteraction = millis();
}

// ============================================================================
//  班表查詢：Google Firebase Firestore（REST API）
// ============================================================================
bool looksLikeSchedule(const String &t) {
  return containsAny(t, {"班表", "上班", "上什麼班", "值班", "排班", "輪班", "休假", "休幾天", "放假", "休息幾天",
                         "大夜", "小夜", "白班", "夜班", "上幾天班", "哪天休", "什麼班"});
}

// 從問題判斷要查哪幾個月（預設本月；月底順便查下個月）
std::vector<std::pair<int, int>> scheduleMonths(const String &t) {
  struct tm now;
  getLocalTime(&now, 10);
  int y = now.tm_year + 1900, m = now.tm_mon + 1;
  std::vector<std::pair<int, int>> v;
  auto add = [&](int yy, int mm) {
    while (mm > 12) { mm -= 12; yy++; }
    while (mm < 1) { mm += 12; yy--; }
    for (auto &x : v) if (x.first == yy && x.second == mm) return;
    v.push_back({yy, mm});
  };
  // 找出「N月」：往前看阿拉伯數字或中文數字（一～十二）
  static const char *CNUM[] = {"十二", "十一", "十", "九", "八", "七", "六", "五", "四", "三", "二", "一"};
  static const int CVAL[] = {12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
  for (int pos = t.indexOf("月"); pos >= 0; pos = t.indexOf("月", pos + 1)) {
    int mm = 0, k = pos;
    while (k > 0 && isdigit((uint8_t)t[k - 1])) k--;
    if (k < pos) mm = t.substring(k, pos).toInt();
    else
      for (int i = 0; i < 12 && !mm; i++) {
        int L = strlen(CNUM[i]);
        if (pos >= L && t.substring(pos - L, pos) == CNUM[i]) mm = CVAL[i];
      }
    if (mm >= 1 && mm <= 12 && t.substring(max(0, pos - 3), pos) != "個") add(mm < m - 6 ? y + 1 : y, mm);
  }
  if (t.indexOf("下個月") >= 0 || t.indexOf("下月") >= 0) add(y, m + 1);
  if (t.indexOf("上個月") >= 0 || t.indexOf("上月") >= 0) add(y, m - 1);
  if (v.empty()) {
    add(y, m);
    if (now.tm_mday >= 24) add(y, m + 1);           // 月底問「下週」可能跨月
  }
  return v;
}

// 讀取一個月的班表，轉成精簡文字（10 分鐘快取）
String fetchScheduleMonth(const char *cat, const char *label, int y, int m) {
  static String cKey[8], cVal[8];
  static uint32_t cAt[8] = {0};
  char doc[48];
  snprintf(doc, sizeof(doc), "%s-%04d-%02d", cat, y, m);
  for (int i = 0; i < 8; i++)
    if (cKey[i] == doc && millis() - cAt[i] < 600000UL) return cVal[i];

  String url = String("https://firestore.googleapis.com/v1/projects/" FIREBASE_PROJECT "/databases/(default)/documents/schedules/") + doc;
  if (strlen(FIREBASE_API_KEY)) url += "?key=" FIREBASE_API_KEY;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  http.begin(client, url);
  int code = http.GET();
  String out;
  if (code == 404) {
    out = String(y) + "年" + m + "月" + label + "班表：尚未建立。\n";
  } else if (code != 200) {
    Serial.printf("班表讀取失敗 %d\n", code);
    http.end();
    return String(y) + "年" + m + "月" + label + "班表：讀取失敗（" + code + "）。\n";
  } else {
    JsonDocument filter;
    JsonObject f = filter["fields"]["employees"]["arrayValue"]["values"][0]["mapValue"]["fields"].to<JsonObject>();
    f["name"]["stringValue"] = true;
    f["shifts"]["mapValue"]["fields"] = true;
    JsonDocument d;
    DeserializationError err = deserializeJson(d, http.getString(), DeserializationOption::Filter(filter),
                                               DeserializationOption::NestingLimit(24));   // Firestore 巢狀約 12 層
    if (err) { Serial.printf("班表 JSON 錯誤：%s\n", err.c_str()); http.end(); return String(y) + "年" + m + "月" + label + "班表：格式錯誤。\n"; }
    // 每天星期幾（2000-01-01 是星期六）
    static const char *W[] = {"日", "一", "二", "三", "四", "五", "六"};
    int days = daysSince2000(m == 12 ? y + 1 : y, m == 12 ? 1 : m + 1, 1) - daysSince2000(y, m, 1);
    out = String(y) + "年" + m + "月" + label + "班表（星期：";
    for (int dd = 1; dd <= days; dd++) out += String(dd) + W[(daysSince2000(y, m, dd) + 6) % 7] + (dd < days ? " " : "）\n");
    for (JsonObject e : d["fields"]["employees"]["arrayValue"]["values"].as<JsonArray>()) {
      JsonObject ef = e["mapValue"]["fields"];
      String name = ef["name"]["stringValue"] | "";
      out += name + (name == SCHEDULE_MY_NAME ? "（我）" : "") + "：";
      JsonObject sh = ef["shifts"]["mapValue"]["fields"];
      for (int dd = 1; dd <= days; dd++) {
        const char *v = sh[String(dd)]["stringValue"] | "";
        if (*v) out += String(dd) + "=" + v + " ";
      }
      out += "\n";
    }
  }
  http.end();
  int slot = 0;
  for (int i = 1; i < 8; i++) if (cAt[i] < cAt[slot]) slot = i;
  cKey[slot] = doc; cVal[slot] = out; cAt[slot] = millis();
  Serial.printf("班表 %s：%d bytes\n", doc, out.length());
  return out;
}

// ============================================================================
//  工作流程查詢：Firebase「線上專案進度時間軸表」
// ============================================================================
bool looksLikeWorkflow(const String &t) {
  return containsAny(t, {"流程", "工作", "幾點", "要做什麼", "該做什麼", "做什麼事", "交班", "接班", "庫存", "專案",
                         "任務", "時間軸", "注意什麼", "要注意", "洗澡班", "教學班"});
}

String utf8Prefix(const String &s, int maxChars) {     // 依字數（不是位元組）截斷
  int chars = 0;
  for (size_t i = 0; i < s.length();) {
    uint8_t c = s[i];
    int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
    if (++chars > maxChars) return s.substring(0, i) + "…";
    i += len;
  }
  return s;
}

String hhmm(long mins) {
  mins = ((mins % 1440) + 1440) % 1440;
  char b[8];
  snprintf(b, sizeof(b), "%02ld:%02ld", mins / 60, mins % 60);
  return b;
}

String firestoreGet(const String &path) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  http.begin(client, "https://firestore.googleapis.com/v1/projects/" TIMELINE_PROJECT "/databases/(default)/documents/" + path + "?pageSize=100");
  int code = http.GET();
  String body = code == 200 ? http.getString() : String("");
  if (code != 200) Serial.printf("時間軸讀取失敗 %s：%d\n", path.c_str(), code);
  http.end();
  return body;
}

// 讀取所有專案的班別與任務，轉成精簡文字（10 分鐘快取）
String fetchTimeline() {
  static String cache;
  static uint32_t cacheAt = 0;
  if (cache.length() && millis() - cacheAt < 600000UL) return cache;

  JsonDocument pf;
  JsonObject pff = pf["documents"][0].to<JsonObject>();
  pff["name"] = true;
  pff["fields"]["name"]["stringValue"] = true;
  pff["fields"]["legacy"]["booleanValue"] = true;
  JsonDocument pd;
  if (deserializeJson(pd, firestoreGet("projects"), DeserializationOption::Filter(pf), DeserializationOption::NestingLimit(24)))
    return "工作流程資料讀取失敗。\n";

  String out;
  for (JsonObject pj : pd["documents"].as<JsonArray>()) {
    String id = pj["name"] | "";
    id = id.substring(id.lastIndexOf('/') + 1);
    String pname = pj["fields"]["name"]["stringValue"] | "（未命名專案）";
    bool legacy = pj["fields"]["legacy"]["booleanValue"] | false;
    out += "【工作流程：" + pname + "】\n";

    JsonDocument sf;
    JsonObject sff = sf["documents"][0]["fields"].to<JsonObject>();
    sff["name"]["stringValue"] = true;
    JsonObject tf = sff["tasks"]["arrayValue"]["values"][0]["mapValue"]["fields"].to<JsonObject>();
    tf["name"]["stringValue"] = true;
    tf["desc"]["stringValue"] = true;
    tf["startMin"] = true;
    tf["endMin"] = true;
    JsonDocument sd;
    String path = legacy ? String("shifts") : "projects/" + id + "/shifts";
    if (deserializeJson(sd, firestoreGet(path), DeserializationOption::Filter(sf), DeserializationOption::NestingLimit(24))) {
      out += "（讀取失敗）\n";
      continue;
    }
    for (JsonObject sh : sd["documents"].as<JsonArray>()) {
      JsonObject f = sh["fields"];
      out += String(f["name"]["stringValue"] | "班別") + "：\n";
      struct T { long st, en; String name, desc; };
      std::vector<T> tasks;
      for (JsonObject t : f["tasks"]["arrayValue"]["values"].as<JsonArray>()) {
        JsonObject tf2 = t["mapValue"]["fields"];
        auto num = [](JsonObject v) -> long {
          if (v["integerValue"].is<const char *>()) return atol(v["integerValue"]);
          if (v["doubleValue"].is<double>()) return (long)v["doubleValue"].as<double>();
          if (v["stringValue"].is<const char *>()) return atol(v["stringValue"]);
          return 0;
        };
        tasks.push_back({num(tf2["startMin"]), num(tf2["endMin"]), tf2["name"]["stringValue"] | "",
                         tf2["desc"]["stringValue"] | ""});
      }
      std::sort(tasks.begin(), tasks.end(), [](const T &a, const T &b) { return a.st < b.st; });
      for (auto &t : tasks) {
        String d = t.desc;
        d.replace("\n", " ");
        out += " " + hhmm(t.st) + "-" + hhmm(t.en) + " " + t.name + (d.length() ? "：" + utf8Prefix(d, TIMELINE_DESC_MAX) : "") + "\n";
      }
    }
  }
  cache = out;
  cacheAt = millis();
  Serial.printf("工作流程：%d bytes\n", out.length());
  return out;
}

// ============================================================================
//  一次完整對話
// ============================================================================
bool containsAny(const String &s, std::initializer_list<const char *> words) {
  for (auto w : words) if (s.indexOf(w) >= 0) return true;
  return false;
}

// 語音辨識在雜音/靜音時常出現的幻覺字句
bool isNoiseTranscript(const String &t) {
  if (t.length() < 3) return true;                       // 少於一個中文字
  return containsAny(t, {"字幕", "訂閱", "觀看", "點贊", "點讚", "Amara", "明鏡", "請用台灣繁體中文轉寫", "常用詞", "謝謝大家"});
}

void showError(const String &msg) {
  setUi(UI_MSG, "哎呀，出了點問題", msg, true);
  delay(4000);
}

void speakAndShow(const String &reply) {
  setUi(UI_SPEAK, ROBOT_NAME "：", reply);
  if (!speak(reply)) { showError(lastApiError); return; }
  waitPlaybackDone();
  delay(300);
}

void handleUtterance(size_t samples) {
  // 1) 語音轉文字
  setUi(UI_THINK, "聽聽看你說了什麼…", "");
  lastApiError = "";
  String userText = speechToText(samples);
  if (userText.isEmpty() || isNoiseTranscript(userText)) {
    if (lastApiError.length()) showError(lastApiError);
    else raiseNoiseFloor(lastRecAvgRms * (lastRecHitMax ? 0.9f : 0.7f));   // 聽不出內容 = 噪音，提高門檻
    Serial.println("（忽略：" + userText + "）");
    return;
  }
  Serial.println("你：" + userText);
  lastInteraction = millis();

  // 2) 本地指令
  if (containsAny(userText, {"錄製", "錄制", "開始錄", "錄一段", "錄下來", "錄個影片", "錄影片"})) {
    String r = recordVideo();
    speakExpr = r.startsWith("錄好了") ? EX_HAPPY : EX_SAD;
    speakAndShow(r);
    return;
  }
  if (cameraActive && containsAny(userText, {"關閉鏡頭", "關鏡頭", "關掉鏡頭", "關閉相機", "關掉相機", "離開攝影", "結束攝影",
                                             "停止攝影", "關閉攝影", "回到原畫面"})) {
    exitCamera();
    speakExpr = EX_HAPPY;
    speakAndShow("好的，關閉鏡頭了。");
    return;
  }
  if (containsAny(userText, {"攝影", "開鏡頭", "打開鏡頭", "開啟鏡頭", "開箭頭", "打開箭頭", "相機", "攝像", "錄影",
                             "即時影像", "鏡頭畫面"})) {
    enterCamera();
    return;
  }
  if (containsAny(userText, {"工作站", "病房助理"}) && !containsAny(userText, {"關閉", "離開", "回主畫面"}) ||
      (matchKnowledge(userText) < 0 &&
       containsAny(userText, {"長期照顧", "長照", "照服員", "照顧服務員", "居服員", "居家服務", "居家督導",
                              "督導員", "長照2.0", "長照3.0", "長照二點零", "長照三點零"}))) {
    openPortalMenu();
    speakExpr = EX_HAPPY;
    speakAndShow("這是" PORTAL_NAME "，點一下工具可以看說明和 QR 碼；技術稽核、督導手冊、學科、術科和長照組合的內容，也可以直接用說的問我喔！");
    setUi(UI_PORTAL);
    return;
  }
  if (portalOpen && containsAny(userText, {"關閉工作站", "離開工作站", "回主畫面", "回到主畫面"})) {
    portalOpen = false;
    setUi(UI_IDLE);
    return;
  }
  String up = userText; up.replace(" ", ""); up.replace("ＭＶ", "MV"); up.replace("ｍｖ", "MV");
  for (size_t i = 0; i < up.length(); i++) if (up[i] >= 'a' && up[i] <= 'z') up.setCharAt(i, up[i] - 32);
  if ((up.indexOf("MV") >= 0 || containsAny(userText, {"音樂錄影帶", "歌曲影片", "看影片", "播影片", "放影片", "播放影片"})) &&
      !containsAny(userText, {"錄製", "錄影"})) {
    playMv(userText);
    return;
  }
  if (containsAny(userText, {"唱歌", "唱首歌", "唱一首", "唱個歌", "來首歌", "來一首", "放歌", "聽歌", "播歌", "放首歌", "播首歌", "播一首", "放一首",
                             "聽首歌", "聽一首", "放音樂", "播音樂", "聽音樂", "來點音樂"}) ||
      (containsAny(userText, {"唱", "播", "放", "聽"}) && findRequestedMp3(userText) >= 0)) {   // 點歌：「唱千江有水千江月」
    singSong(userText);
    return;
  }
  String reply;
  if (containsAny(userText, {"更新歌單"})) {
    setUi(UI_THINK, "更新歌單中…", "");
    int cnt = refreshPlaylist();
    reply = cnt > 0 ? "歌單更新好了，一共有" + String(cnt) + "首歌！" : "歌單更新失敗，請檢查播放清單網址。";
  } else if (containsAny(userText, {"大聲", "音量調大", "音量大一點"})) {
    volumePct = min(100, volumePct + 20);
    reply = "好的，音量調到百分之" + String(volumePct) + "。";
  } else if (containsAny(userText, {"小聲", "音量調小", "音量小一點"})) {
    volumePct = max(10, volumePct - 20);
    reply = "好的，音量調到百分之" + String(volumePct) + "。";
  } else if (containsAny(userText, {"清除記憶", "忘記剛剛", "重新開始"})) {
    histCount = 0;
    reply = "好的，我已經把剛剛的對話忘掉了。";
  }

  speakExpr = EX_HAPPY;

  // 3) 問 GPT（需要時附上鏡頭畫面）
  if (reply.isEmpty()) {
    String imageUri;
    bool wantVision = containsAny(userText, {"拍照", "拍張", "拍一張", "照相", "看到", "看看", "這是什麼", "這是啥", "鏡頭", "幫我看", "唸給我聽", "上面寫"});
    bool wantSchedule = !wantVision && looksLikeSchedule(userText);
    bool wantWorkflow = !wantVision && looksLikeWorkflow(userText);
    int know = (!wantVision && !wantSchedule && !wantWorkflow) ? matchKnowledge(userText) : -1;
    setUi(UI_THINK, wantVision ? "拍照看看，思考中…" : wantSchedule ? "查詢班表中…" : wantWorkflow ? "查詢工作流程中…"
                    : know >= 0 ? String("查詢《") + KNOW_NAME[know] + "》中…"
                    : (ENABLE_WEB_SEARCH && looksLikeSearch(userText)) ? "上網查詢中…" : "思考中…",
          "你：" + userText);
    chatContext = "";
    if (wantSchedule)
      for (auto &ym : scheduleMonths(userText)) {
        static const char *CATS[] = SCHEDULE_CATEGORIES, *LABELS[] = SCHEDULE_LABELS;
        for (size_t c = 0; c < sizeof(CATS) / sizeof(CATS[0]); c++)
          chatContext += fetchScheduleMonth(CATS[c], LABELS[c], ym.first, ym.second);
      }
    if (wantWorkflow) chatContext += fetchTimeline();
    if (know >= 0) chatContext += knowledgeContext(know, userText);
    if (wantVision) imageUri = captureImageDataUri();
    reply = chatWithGPT(userText, imageUri);
    chatContext = "";
    if (reply.isEmpty()) {
      photoShow = false;
      showError(lastApiError.length() ? lastApiError : "AI 沒有回應");
      return;
    }
  }
  Serial.println(String(ROBOT_NAME "：") + reply);

  // 4) 說出回答（照片至少顯示 PHOTO_SHOW_MS）
  while (photoShow && millis() - photoAt < PHOTO_SHOW_MS) delay(50);
  speakAndShow(reply);
  photoShow = false;
  lastInteraction = millis();
}

// ============================================================================
//  初始化
// ============================================================================
void initDisplay() {
  hiBuf = (uint16_t *)heap_caps_aligned_alloc(64, LCD_W * LCD_H * 2, MALLOC_CAP_SPIRAM);
  cv = new Canvas2x();
  u8f.setTarget(cv);
  if (!lcdInit()) Serial.println("螢幕初始化失敗");
  cv->fillScreen(C_BG);
  lcdFlush(hiBuf);
  lcdBacklight(LCD_BRIGHTNESS);
}

bool initAudio() {
  return audioInit(MIC_PGA_GAIN);
}

bool initCamera() {
#if ENABLE_CAMERA
  return camInit(CAM_VFLIP, CAM_HMIRROR);
#else
  return false;
#endif
}

// ============================================================================
//  設定網頁
// ============================================================================
String maskKey(const String &k) {
  if (k.length() < 12) return k.length() ? "已設定" : "";
  return k.substring(0, 7) + "…" + k.substring(k.length() - 4);
}

void handleStatus() {
  JsonDocument d;
  d["name"] = ROBOT_NAME;
  d["mode"] = portalMode ? "ap" : "sta";
  d["ssid"] = cfg.ssid;
  d["hasPass"] = cfg.pass.length() > 0;
  d["apiKey"] = maskKey(cfg.apiKey);
  d["gdrive"] = cfg.gdrive;
  d["playlist"] = cfg.playlist;
  d["cam"] = cameraOK;
  d["camWb"] = camTune.wb;
  d["camSat"] = camTune.sat;
  d["camBri"] = camTune.bri;
  d["camCon"] = camTune.con;
  xSemaphoreTake(songLock, portMAX_DELAY);
  d["songs"] = (int)songs.size();
  xSemaphoreGive(songLock);
  d["ip"] = portalMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  d["rssi"] = portalMode ? 0 : WiFi.RSSI();
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

struct Net { String ssid; int rssi; bool secure; };

void handleScan() {
  int n = WiFi.scanNetworks();
  std::vector<Net> nets;
  for (int i = 0; i < n; i++) {                       // 同名只留訊號最強的
    String ss = WiFi.SSID(i);
    if (ss.isEmpty()) continue;
    auto it = std::find_if(nets.begin(), nets.end(), [&](const Net &x) { return x.ssid == ss; });
    if (it != nets.end()) { it->rssi = max(it->rssi, (int)WiFi.RSSI(i)); continue; }
    nets.push_back({ss, (int)WiFi.RSSI(i), WiFi.encryptionType(i) != WIFI_AUTH_OPEN});
  }
  WiFi.scanDelete();
  std::sort(nets.begin(), nets.end(), [](const Net &a, const Net &b) { return a.rssi > b.rssi; });
  JsonDocument d;
  JsonArray arr = d.to<JsonArray>();
  for (auto &x : nets) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = x.ssid;
    o["rssi"] = x.rssi;
    o["secure"] = x.secure;
  }
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

void handleSave() {
  String ssid = server.arg("ssid"), pass = server.arg("pass"), key = server.arg("apikey"), gd = server.arg("gdrive");
  String pl = server.arg("playlist");
  ssid.trim(); key.trim(); gd.trim(); pl.trim();
  if (ssid.isEmpty()) { server.send(400, "text/plain; charset=utf-8", "請選擇 Wi-Fi"); return; }
  prefs.begin("robot", false);
  // 密碼空白：同一個網路 = 不變更；換了網路 = 無密碼網路
  prefs.putString("pass", pass.length() || ssid != cfg.ssid ? pass : cfg.pass);
  prefs.putString("ssid", ssid);
  prefs.putString("apikey", key.length() ? key : cfg.apiKey);
  prefs.putString("gdrive", gd);
  if (pl != cfg.playlist) { prefs.putString("playlist", pl); prefs.remove("songs"); }
  prefs.end();
  server.send(200, "text/plain; charset=utf-8", "ok");
  Serial.println("設定已儲存，重新啟動");
  setUi(UI_MSG, "設定已儲存", "重新啟動中…");
  delay(1200);
  ESP.restart();
}

void webTask(void *) {
  for (;;) {
    if (portalMode) dnsServer.processNextRequest();
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void startWebServer() {
  static bool started = false;
  if (started) return;
  started = true;
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html; charset=utf-8", WEB_PAGE); });
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/schedule", HTTP_GET, [] {                // 測試：直接看會交給 AI 的班表文字，?q=問題
    String q = server.hasArg("q") ? server.arg("q") : String("今天誰上班");
    String out;
    for (auto &ym : scheduleMonths(q)) {
      static const char *CATS[] = SCHEDULE_CATEGORIES, *LABELS[] = SCHEDULE_LABELS;
      for (size_t c = 0; c < sizeof(CATS) / sizeof(CATS[0]); c++) out += fetchScheduleMonth(CATS[c], LABELS[c], ym.first, ym.second);
    }
    server.send(200, "text/plain; charset=utf-8", out);
  });
  server.on("/camera", HTTP_POST, [] {                 // 鏡頭色彩：立即套用並儲存（不用重開機）
    auto arg = [](const char *k, int lo, int hi, int def) {
      return server.hasArg(k) ? constrain(server.arg(k).toInt(), lo, hi) : def;
    };
    camTune.wb = arg("wb", 0, 4, camTune.wb);
    camTune.sat = arg("sat", -2, 2, camTune.sat);
    camTune.bri = arg("bri", -2, 2, camTune.bri);
    camTune.con = arg("con", -2, 2, camTune.con);
    applyCamTune();
    prefs.begin("robot", false);
    prefs.putInt("camWb", camTune.wb);
    prefs.putInt("camSat", camTune.sat);
    prefs.putInt("camBri", camTune.bri);
    prefs.putInt("camCon", camTune.con);
    prefs.end();
    server.send(200, "text/plain; charset=utf-8", "ok");
  });
  server.on("/preview", HTTP_POST, [] {                // 從網頁開關攝影畫面，方便邊看邊調
    if (server.arg("on") == "1") { cameraSince = millis(); cameraActive = true; }
    else cameraActive = false;
    if (!recActive) setUi(UI_IDLE);
    server.send(200, "text/plain", "ok");
  });
  server.on("/timeline", HTTP_GET, [] {                // 測試：直接看會交給 AI 的工作流程文字
    server.send(200, "text/plain; charset=utf-8", fetchTimeline());
  });
  server.on("/refresh", HTTP_POST, [] {
    int n = refreshPlaylist();
    server.send(n > 0 ? 200 : 500, "text/plain; charset=utf-8", n > 0 ? String(n) : String("更新失敗"));
  });
  server.on("/tone", HTTP_GET, [] {                    // 除錯：播測試音 /tone?f=1000&ms=1000&amp=8000
    int f = server.hasArg("f") ? server.arg("f").toInt() : 1000;
    int ms = server.hasArg("ms") ? server.arg("ms").toInt() : 1000;
    int amp = server.hasArg("amp") ? server.arg("amp").toInt() : 8000;
    static int16_t buf[480 * 2];
    for (int n = 0; n < ms / 20; n++) {
      for (int i = 0; i < 480; i++) buf[i * 2] = buf[i * 2 + 1] = (int16_t)(amp * sinf(2 * PI * f * (n * 480 + i) / 24000.0f));
      audioI2S.write((uint8_t *)buf, sizeof(buf));
    }
    server.send(200, "text/plain", "ok");
  });
  server.on("/know", HTTP_GET, [] {                    // 測試：工作站問答會交給 AI 的資料片段，?q=問題
    String q = server.arg("q");
    int k = matchKnowledge(q);
    server.send(200, "text/plain; charset=utf-8", k < 0 ? String("沒有對應的工作站資料") : knowledgeContext(k, q));
  });
  server.on("/heap", HTTP_GET, [] {                    // 除錯：記憶體狀況
    char b[200];
    snprintf(b, sizeof(b), "internal %u (largest %u, min ever %u)\nDMA %u\nPSRAM %u (largest %u)\n",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    server.send(200, "text/plain", b);
  });
  server.on("/pin", HTTP_GET, [] {                     // 除錯：設定 GPIO /pin?n=11&v=1
    int n = server.arg("n").toInt(), v = server.arg("v").toInt();
    pinMode(n, OUTPUT);
    digitalWrite(n, v);
    server.send(200, "text/plain", "ok");
  });
  server.on("/es", HTTP_GET, [] {                      // 除錯：ES8311 暫存器 /es（全部）、/es?r=0x32&v=0xBF（寫入）
    String out;
    xSemaphoreTake(i2cLock, portMAX_DELAY);
    if (server.hasArg("r") && server.hasArg("v"))
      i2cW(ES8311_ADDR, strtol(server.arg("r").c_str(), nullptr, 0), strtol(server.arg("v").c_str(), nullptr, 0));
    for (int r = 0; r <= 0x45; r++) {
      Wire.beginTransmission(ES8311_ADDR); Wire.write(r);
      int v = -1;
      if (Wire.endTransmission(false) == 0 && Wire.requestFrom((uint8_t)ES8311_ADDR, (uint8_t)1) == 1) v = Wire.read();
      char b[16]; snprintf(b, sizeof(b), "%02X=%02X%s", r, v & 0xFF, (r % 8 == 7) ? "\n" : " ");
      out += b;
    }
    xSemaphoreGive(i2cLock);
    server.send(200, "text/plain", out);
  });
  server.onNotFound([] {                               // 設定模式下所有網址都導向設定頁（手機會自動跳出）
    if (portalMode) { server.sendHeader("Location", "http://192.168.4.1/"); server.send(302, "text/plain", ""); }
    else server.send(404, "text/plain", "not found");
  });
  server.begin();
  xTaskCreatePinnedToCore(webTask, "web", 16384, nullptr, 1, nullptr, 0);
}

// 連不上 Wi-Fi 或尚未設定：開熱點 + 設定網頁，停在這裡等使用者儲存（儲存後會重新啟動）
void runSetupPortal(const String &reason) {
  portalMode = true;
  WiFi.disconnect();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(200);
  dnsServer.start(53, "*", WiFi.softAPIP());
  snprintf(ipText, sizeof(ipText), "http://%s", WiFi.softAPIP().toString().c_str());
  startWebServer();
  Serial.printf("設定模式：%s，熱點 %s / %s，http://%s\n", reason.c_str(), AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
  setUi(UI_MSG, "設定模式", reason + "\n1. 手機連 Wi-Fi「" AP_SSID "」\n   密碼 " AP_PASS "\n2. 開啟 http://192.168.4.1\n3. 選 Wi-Fi、填密碼與 API Key");
  while (true) delay(1000);
}

// allowPortal = 開機時連不上就進設定模式；執行中斷線則重新啟動
void connectWiFi(bool allowPortal) {
  if (cfg.ssid.isEmpty()) runSetupPortal("尚未設定 Wi-Fi");
  setUi(UI_MSG, "連線 Wi-Fi 中…", cfg.ssid);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    if (millis() - t0 > 20000) {
      if (allowPortal) runSetupPortal("連不上「" + cfg.ssid + "」");
      setUi(UI_MSG, "Wi-Fi 斷線", "重新啟動中…", true);
      delay(3000);
      ESP.restart();
    }
  }
  snprintf(ipText, sizeof(ipText), "http://%s", WiFi.localIP().toString().c_str());
  Serial.printf("WiFi IP: %s（設定網頁 %s 或 http://xiaoke.local）\n", WiFi.localIP().toString().c_str(), ipText);
  MDNS.begin("xiaoke");
  MDNS.addService("http", "tcp", 80);
  startWebServer();
  configTzTime("CST-8", "time.stdtime.gov.tw", "pool.ntp.org");
  setUi(UI_IDLE);
}

// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== ESP32-P4 7 吋 AI Voice Robot「" ROBOT_NAME "」===");

  pinMode(PIN_BOOT, INPUT_PULLUP);
  // 大於 512 bytes 的一般配置放 PSRAM（預設 4KB 以下都放內部 RAM，歌單/字串會把內部 RAM 吃光，HTTPS 的 DMA 加密就配置不到）
  // 掉電偵測：Arduino 預設第 7 級（最敏感），喇叭一出聲電壓稍降就重開機；調低門檻留一點餘裕
  brownout_hal_config_t bo = {};
  bo.threshold = BROWNOUT_LEVEL; bo.enabled = true; bo.reset_enabled = true;
  brownout_hal_config(&bo);
  i2cLock = xSemaphoreCreateMutex();
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);   // 觸控、ES8311、鏡頭共用
  initDisplay();
  if (!touchInit()) Serial.println("找不到觸控晶片 GT911");

  wavBuf = (uint8_t *)ps_malloc(44 + MAX_SAMPLES * 2);
  tts.buf = (uint8_t *)ps_malloc(TTS_BUF_BYTES);
  tts.cap = tts.buf ? TTS_BUF_BYTES : 0;
  photoPix = (uint16_t *)ps_malloc(LCD_W * 576 * 2);
  thumbPix = (uint16_t *)ps_malloc(320 * 180 * 2);
  songLock = xSemaphoreCreateMutex();

  bool audioOK = initAudio();
  cameraOK = initCamera();
  loadCamTune();
  applyCamTune();
  if (!jpegInit()) Serial.println("硬體 JPEG 初始化失敗");
  heap_caps_malloc_extmem_enable(512);   // 鏡頭初始化之後才啟用（鏡頭的 DMA 結構要在內部 RAM）
  xTaskCreatePinnedToCore(playTask, "play", 4096, nullptr, 5, &playTaskHandle, 1);

  // 畫面改由 UI task 繪製
  uiLock = xSemaphoreCreateMutex();
  lastInteraction = millis();
  buildHelp();
  xTaskCreatePinnedToCore(uiTask, "ui", 8192, nullptr, 1, nullptr, 0);

  if (!audioOK) showError("音訊（ES8311）初始化失敗");
  loadSettings();
  loadSongsFromNvs();
  connectWiFi(true);
  xTaskCreatePinnedToCore(playlistTask, "playlist", 8192, nullptr, 1, nullptr, 1);   // 背景更新歌單
  if (cfg.apiKey.isEmpty()) {
    setUi(UI_MSG, "尚未設定 API Key", String("請用手機或電腦開啟\n") + ipText + "\n輸入 OpenAI API Key");
    while (true) delay(1000);
  }

  // 校正背景噪音
  int16_t f[FRAME];
  float acc = 0;
  flushMic(200);
  for (int i = 0; i < 50; i++) acc += readFrame(f);
  noiseFloor = acc / 50;
  Serial.printf("背景噪音 rms=%.0f，觸發門檻=%d\n", noiseFloor, vadThreshold());

  speakAndShow(VOICE_ACTIVATION ? "嗨，我是" ROBOT_NAME "！隨時可以直接跟我說話喔。"
                                : "嗨，我是" ROBOT_NAME "！按住螢幕跟我說話吧。");
  flushMic(300);
  setUi(UI_IDLE);
}

// 按鍵：第一次按開啟說明，之後每按一次下一頁，最後一頁再按回到聊天
uint32_t helpSince = 0;
bool helpSpoken = false;

void exitHelp() {
  setUi(UI_IDLE);
  flushMic(200);
  lastInteraction = millis();
}

void onHelpButton() {
  helpSince = millis();
  lastInteraction = millis();
  if (uiState != UI_HELP) {
    helpPage = 0;
    helpSpoken = false;
    setUi(UI_HELP);
    return;
  }
  if (helpPage + 1 >= (int)helpPages.size()) { exitHelp(); return; }
  helpPage = helpPage + 1;
  setUi(UI_HELP);
  if (helpPage == (int)helpPages.size() - 1 && !helpSpoken) {     // 開發者頁：小柯親口介紹
    helpSpoken = true;
    if (speak("這個系統是吳玉柱先生和 Claude AI 共同開發的，有任何意見，歡迎寫信給吳先生喔！")) waitPlaybackDone();
    helpSince = millis();
  }
}

void loop() {
  updateVolumeFromPot();
  if (WiFi.status() != WL_CONNECTED) connectWiFi(false);

  if (cameraActive) {                                      // 攝影中：繼續聽指令；按鍵回到原畫面
    static bool camBtnPrev = true;
    bool b = buttonPressed();
    if (b && !camBtnPrev) { exitCamera(); camBtnPrev = true; return; }
    camBtnPrev = b;
    if (millis() - max(cameraSince, (uint32_t)lastInteraction) > CAMERA_PREVIEW_MAX_SEC * 1000UL) exitCamera();
  }

  if (VOICE_ACTIVATION) {                                   // 聲控模式：按鍵用來看功能說明
    static bool btnPrev = false;
    bool btn = buttonPressed();
    if (btn && !btnPrev) {
      delay(30);
      if (buttonPressed()) {
        if (uiState == UI_SONG) exitSong();
        else if (uiState == UI_QR) { setUi(UI_IDLE); while (buttonPressed()) delay(10); }
        else if (uiState == UI_PORTAL) {
          if (portalTouch(touchX / 2, touchY / 2)) { portalOpen = false; setUi(UI_IDLE); }
          while (buttonPressed()) delay(10);
        }
        else if (uiState == UI_IDLE && inBtn(BTN_PORTAL_IDLE, touchX / 2, touchY / 2)) { openPortalMenu(); while (buttonPressed()) delay(10); }
        else if (uiState == UI_HELP) onHelpButton();
        else {
          uint32_t t0 = millis();                          // 短按 = 功能說明；按住 = 說話（環境吵時用）
          while (buttonPressed() && millis() - t0 < 400) delay(10);
          if (buttonPressed()) {
            Serial.println("（按住說話）");
            size_t n = recordUtterance(true);
            if (n) handleUtterance(n);
            flushMic(300);
            if (portalOpen) setUi(UI_PORTAL);
            else if (uiState != UI_SONG && uiState != UI_CAMERA && uiState != UI_QR) setUi(UI_IDLE);
            btnPrev = false;
            return;
          }
          onHelpButton();
        }
      }
    }
    btnPrev = buttonPressed();
    if (uiState == UI_QR && millis() - qrSince > PORTAL_SHOW_SEC * 1000UL) setUi(UI_IDLE);   // QR 畫面逾時
    if (portalOpen && uiState == UI_PORTAL && millis() - max(portalSince, (uint32_t)lastInteraction) > PORTAL_SHOW_SEC * 1000UL) {
      portalOpen = false; setUi(UI_IDLE);                    // 工作站選單逾時
    }
    if (uiState == UI_SONG) {                               // 唱歌畫面：暫停聲控（手機正在放歌），時間到回到聊天
      if (millis() - songSince > SONG_SHOW_SEC * 1000UL) exitSong();
      delay(20);
      return;
    }
    if (uiState == UI_HELP) {                               // 看說明時暫停聲控，90 秒沒動作自動回到聊天
      if (millis() - helpSince > 90000) exitHelp();
      delay(20);
      return;
    }
  }

  // 持續聆聽：每次讀 20ms，存進預錄環狀緩衝
  int rms = readFrame(preroll[prerollHead]);
  prerollHead = (prerollHead + 1) % PREROLL_FR;
  if (prerollCount < PREROLL_FR) prerollCount++;
  micLevel = rms;

  static int loudFrames = 0;
  int thr = vadThreshold();
  bool trigger = false, byButton = false;
  if (!VOICE_ACTIVATION && buttonPressed()) { trigger = true; byButton = true; }   // 按鍵模式：按住說話
  else if (VOICE_ACTIVATION) {
    loudFrames = rms > thr ? loudFrames + 1 : 0;
    if (loudFrames >= 3) trigger = true;                          // 連續 60ms 大於門檻
    else if (rms < thr) noiseFloor += (rms - noiseFloor) * 0.01f;  // 慢慢追蹤背景噪音
  }

  static uint32_t lastLog = 0;
  if (millis() - lastLog > 5000) {
    lastLog = millis();
    Serial.printf("[聆聽] rms=%d 噪音=%.0f 門檻=%d 內部RAM=%uK(最大塊%uK) DMA=%uK PSRAM=%uK\n", rms, noiseFloor, thr,
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024), (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) / 1024), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  }

  if (trigger) {
    loudFrames = 0;
    size_t n = recordUtterance(byButton);
    if (n && lastRecHitMax) Serial.println("（錄到上限才停，可能有背景聲音；仍送出辨識）");
    if (n) handleUtterance(n);
    else Serial.println("（太短，視為雜音）");
    flushMic(300);
    if (portalOpen) setUi(UI_PORTAL);                                                            // 工作站選單：回答完回到選單
    else if (uiState != UI_SONG && uiState != UI_CAMERA && uiState != UI_QR) setUi(UI_IDLE);        // 唱歌畫面要留著
  }
}
