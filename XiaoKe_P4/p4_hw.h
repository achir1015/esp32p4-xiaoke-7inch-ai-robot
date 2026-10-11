// ============================================================================
//  p4_hw.h — Guition JC1060P470C（ESP32-P4 + C6，7 吋 1024x600）板載硬體
//    螢幕 JD9165（MIPI-DSI）、觸控 GT911、音訊 ES8311、鏡頭 OV02C10（MIPI-CSI）
//    腳位與螢幕參數已實機驗證（2026-10-09），見 config.h 說明
// ============================================================================
#pragma once
#include <Wire.h>
#include <ESP_I2S.h>
#include <ESP_Video.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_ldo_regulator.h"
#include "driver/i2c_master.h"
#include "driver/jpeg_encode.h"
#include "driver/jpeg_decode.h"
#include "ov02c10_cam.h"

// ------------------------------------------------------------------ 腳位 ----
#define PIN_I2C_SDA   7
#define PIN_I2C_SCL   8
#define PIN_LCD_RST   5
#define PIN_LCD_BL    23     // 高電位 = 背光亮（LEDC 調亮度）
#define PIN_TP_RST    22
#define PIN_TP_INT    21
#define PIN_PA        11     // 喇叭功放致能
#define PIN_I2S_MCLK  13
#define PIN_I2S_BCLK  12
#define PIN_I2S_WS    10
#define PIN_I2S_DOUT  9
#define PIN_I2S_DIN   48
#define PIN_BOOT      35     // 板上 BOOT 鍵

#define LCD_W 1024
#define LCD_H 600
#define CAM_W 1288           // OV02C10 經 ISP 輸出
#define CAM_H 728

// ================================================================ 螢幕 ====
struct LcdCmd { uint8_t cmd; uint8_t len; uint8_t d[14]; };
// JD9165 初始化表：取自本板原廠韌體（原廠為 .bss 零值的參數填 0x00）
static const LcdCmd LCD_INIT[] = {
  {0x30,1,{0x00}}, {0xF7,4,{0x49,0x61,0x02,0x00}}, {0x30,1,{0x01}}, {0x04,1,{0x0C}}, {0x05,1,{0x08}}, {0x0B,1,{0x11}},
  {0x20,1,{0x04}}, {0x1F,1,{0x05}}, {0x23,1,{0x38}}, {0x28,1,{0x18}}, {0x29,1,{0x29}}, {0x2A,1,{0x01}}, {0x2B,1,{0x29}},
  {0x2C,1,{0x01}}, {0x30,1,{0x02}}, {0x00,1,{0x05}}, {0x01,1,{0x22}}, {0x02,1,{0x08}}, {0x03,1,{0x12}}, {0x04,1,{0x16}},
  {0x05,1,{0x64}}, {0x06,1,{0x00}}, {0x07,1,{0x00}}, {0x08,1,{0x78}}, {0x09,1,{0x00}}, {0x0A,1,{0x04}},
  {0x0B,11,{0x16,0x17,0x0B,0x0D,0x0D,0x0D,0x11,0x10,0x07,0x07,0x09}},
  {0x0C,11,{0x09,0x1E,0x1E,0x1C,0x1C,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D}},
  {0x0D,11,{0x0A,0x05,0x0B,0x0D,0x0D,0x0D,0x11,0x10,0x06,0x06,0x08}},
  {0x0E,11,{0x08,0x1F,0x1F,0x1D,0x1D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D}},
  {0x0F,11,{0x0A,0x05,0x0D,0x0B,0x0D,0x0D,0x11,0x10,0x1D,0x1D,0x1F}},
  {0x10,11,{0x1F,0x08,0x08,0x06,0x06,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D}},
  {0x11,11,{0x16,0x17,0x0D,0x0B,0x0D,0x0D,0x11,0x10,0x1C,0x1C,0x1E}},
  {0x12,11,{0x1E,0x09,0x09,0x07,0x07,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D}},
  {0x13,4,{0x00,0x00,0x00,0x00}}, {0x14,4,{0x00,0x00,0x41,0x41}}, {0x15,4,{0x00,0x00,0x00,0x00}}, {0x17,1,{0x00}},
  {0x18,1,{0x85}}, {0x19,2,{0x06,0x09}}, {0x1A,2,{0x05,0x08}}, {0x1B,2,{0x0A,0x04}}, {0x26,1,{0x00}}, {0x27,1,{0x00}},
  {0x30,1,{0x06}},
  {0x12,14,{0x3F,0x25,0x27,0x35,0x1D,0x1B,0x1B,0x1A,0x18,0x0A,0x2A,0x21,0x19,0x30}},
  {0x13,14,{0x3F,0x26,0x27,0x35,0x1E,0x1C,0x1C,0x1A,0x18,0x0B,0x2A,0x21,0x19,0x30}},
  {0x30,1,{0x0A}}, {0x02,1,{0x4F}}, {0x0B,1,{0x40}}, {0x30,1,{0x0D}}, {0x0D,1,{0x04}}, {0x10,1,{0x05}}, {0x11,1,{0x0C}},
  {0x12,1,{0x05}}, {0x13,1,{0x0C}}, {0x30,1,{0x00}}, {0x3A,1,{0x55}},
};

esp_lcd_panel_handle_t lcdPanel = nullptr;
static SemaphoreHandle_t lcdDoneSem = nullptr;

static bool IRAM_ATTR lcdTransDone(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t *, void *) {
  BaseType_t hp = pdFALSE;
  xSemaphoreGiveFromISR(lcdDoneSem, &hp);
  return hp == pdTRUE;
}

void lcdBacklight(int pct) { ledcWrite(PIN_LCD_BL, constrain(pct, 0, 100) * 255 / 100); }

bool lcdInit() {
  ledcAttach(PIN_LCD_BL, 20000, 8);
  lcdBacklight(0);
  esp_ldo_channel_handle_t ldo;
  esp_ldo_channel_config_t lc = {};
  lc.chan_id = 3; lc.voltage_mv = 2500;                    // MIPI PHY 電源
  esp_ldo_acquire_channel(&lc, &ldo);
  pinMode(PIN_LCD_RST, OUTPUT);
  digitalWrite(PIN_LCD_RST, HIGH);

  esp_lcd_dsi_bus_handle_t bus;
  esp_lcd_dsi_bus_config_t bc = {};
  bc.bus_id = 0; bc.num_data_lanes = 2; bc.lane_bit_rate_mbps = 750;
  bc.phy_clk_src = MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT_LEGACY;   // rev<3 晶片不支援 XTAL（否則 abort）
  if (esp_lcd_new_dsi_bus(&bc, &bus) != ESP_OK) return false;
  esp_lcd_panel_io_handle_t io;
  esp_lcd_dbi_io_config_t ic = {};
  ic.virtual_channel = 0; ic.lcd_cmd_bits = 8; ic.lcd_param_bits = 8;
  if (esp_lcd_new_panel_io_dbi(bus, &ic, &io) != ESP_OK) return false;

  esp_lcd_dpi_panel_config_t pc = {};
  pc.virtual_channel = 0;
  pc.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
  pc.dpi_clock_freq_mhz = 54;
  pc.in_color_format = LCD_COLOR_FMT_RGB565;
  pc.num_fbs = 1;
  pc.video_timing.h_size = LCD_W; pc.video_timing.v_size = LCD_H;
  pc.video_timing.hsync_pulse_width = 40; pc.video_timing.hsync_back_porch = 160; pc.video_timing.hsync_front_porch = 160;
  pc.video_timing.vsync_pulse_width = 10; pc.video_timing.vsync_back_porch = 23; pc.video_timing.vsync_front_porch = 12;
  pc.flags.use_dma2d = 1;
  if (esp_lcd_new_panel_dpi(bus, &pc, &lcdPanel) != ESP_OK) return false;

  digitalWrite(PIN_LCD_RST, LOW); delay(10); digitalWrite(PIN_LCD_RST, HIGH); delay(120);
  uint8_t madctl = 0x00, colmod = 0x55;
  esp_lcd_panel_io_tx_param(io, 0x36, &madctl, 1);
  esp_lcd_panel_io_tx_param(io, 0x3A, &colmod, 1);
  for (auto &c : LCD_INIT) esp_lcd_panel_io_tx_param(io, c.cmd, c.d, c.len);
  esp_lcd_panel_io_tx_param(io, 0x11, nullptr, 0); delay(120);
  esp_lcd_panel_io_tx_param(io, 0x29, nullptr, 0); delay(20);

  lcdDoneSem = xSemaphoreCreateBinary();
  esp_lcd_dpi_panel_event_callbacks_t cbs = {};
  cbs.on_color_trans_done = lcdTransDone;
  esp_lcd_dpi_panel_register_event_callbacks(lcdPanel, &cbs, nullptr);
  if (esp_lcd_panel_init(lcdPanel) != ESP_OK) return false;
  return true;
}

// 把整個畫面（PSRAM 畫布）用 DMA2D 複製到螢幕，等複製完成才返回（之後才能再畫畫布）
static SemaphoreHandle_t lcdLock = nullptr;
void lcdFlush(const uint16_t *buf) {
  if (!lcdLock) lcdLock = xSemaphoreCreateMutex();
  xSemaphoreTake(lcdLock, portMAX_DELAY);                     // UI 與 MV 播放器不能同時送畫面
  xSemaphoreTake(lcdDoneSem, 0);                              // 清掉殘留訊號
  if (esp_lcd_panel_draw_bitmap(lcdPanel, 0, 0, LCD_W, LCD_H, buf) == ESP_OK)
    xSemaphoreTake(lcdDoneSem, pdMS_TO_TICKS(200));
  xSemaphoreGive(lcdLock);
}

// ================================================================ 觸控 ====
static uint8_t gtAddr = 0;
static SemaphoreHandle_t i2cLock = nullptr;

static bool gtRead(uint16_t reg, uint8_t *buf, int n) {
  Wire.beginTransmission(gtAddr); Wire.write(reg >> 8); Wire.write(reg & 0xFF);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(gtAddr, (uint8_t)n) != n) return false;
  for (int i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}
static void gtWrite(uint16_t reg, uint8_t v) {
  Wire.beginTransmission(gtAddr); Wire.write(reg >> 8); Wire.write(reg & 0xFF); Wire.write(v); Wire.endTransmission();
}

bool touchInit() {
  pinMode(PIN_TP_RST, OUTPUT); pinMode(PIN_TP_INT, OUTPUT);
  digitalWrite(PIN_TP_INT, LOW); digitalWrite(PIN_TP_RST, LOW); delay(10);
  digitalWrite(PIN_TP_RST, HIGH); delay(60);                 // INT 低 → 位址 0x5D
  pinMode(PIN_TP_INT, INPUT); delay(50);
  for (uint8_t a : {0x5D, 0x14}) { Wire.beginTransmission(a); if (Wire.endTransmission() == 0) { gtAddr = a; break; } }
  return gtAddr != 0;
}

// 讀觸控（約 50Hz 更新一次，其他時間回傳上次結果）；回傳是否有手指
volatile int touchX = 0, touchY = 0;
bool touchPressed() {
  static bool down = false;
  static uint32_t last = 0, lastSeen = 0;
  if (!gtAddr) return false;
  if (millis() - last < 20) return down;
  last = millis();
  uint8_t st = 0;
  xSemaphoreTake(i2cLock, portMAX_DELAY);
  bool ok = gtRead(0x814E, &st, 1);
  if (ok && (st & 0x80)) {
    if ((st & 0x0F) > 0) {
      uint8_t p[4];
      if (gtRead(0x8150, p, 4)) { touchX = p[0] | p[1] << 8; touchY = p[2] | p[3] << 8; }
      lastSeen = millis();
    }
    gtWrite(0x814E, 0);
  }
  xSemaphoreGive(i2cLock);
  down = millis() - lastSeen < 80;                           // 手指離開約 80ms 才算放開（避免閃斷）
  return down;
}

// ================================================================ 音訊 ====
//  ES8311 一顆晶片同時負責喇叭（DAC）與麥克風（ADC），共用全雙工 I2S 24kHz / 16-bit / 立體聲
#define ES8311_ADDR 0x18
#define I2S_RATE    24000
I2SClass audioI2S;

static bool i2cW(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr); Wire.write(reg); Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool es8311Init(uint8_t pgaGain) {
  uint8_t pga = 0x10 | (pgaGain > 10 ? 10 : pgaGain);
  const uint8_t seq[][2] = {
    {0x44, 0x08}, {0x44, 0x08}, {0x00, 0x1F}, {0x00, 0x00},
    {0x01, 0x30}, {0x02, 0x00}, {0x03, 0x10}, {0x16, 0x24}, {0x04, 0x10}, {0x05, 0x00},
    {0x0B, 0x00}, {0x0C, 0x00}, {0x10, 0x1F}, {0x11, 0x7F},
    {0x00, 0x80}, {0x01, 0x3F}, {0x06, 0x03}, {0x07, 0x00}, {0x08, 0xFF},
    {0x09, 0x0C}, {0x0A, 0x0C}, {0x13, 0x10}, {0x1B, 0x0A}, {0x1C, 0x6A},
    {0x0D, 0x01}, {0x0E, 0x02}, {0x12, 0x00}, {0x14, pga}, {0x15, 0x40},
    {0x17, 0xBF}, {0x37, 0x08}, {0x45, 0x00}, {0x32, 0xBF}, {0x31, 0x00},
  };
  bool ok = true;
  for (auto &r : seq) {
    bool w = false;
    for (int k = 0; k < 3 && !w; k++) { w = i2cW(ES8311_ADDR, r[0], r[1]); if (!w) delay(5); }
    if (!w) ok = false;
    if (r[0] == 0x00 && r[1] == 0x1F) delay(20);
  }
  return ok;
}

bool audioInit(uint8_t pgaGain) {
  pinMode(PIN_PA, OUTPUT);
  digitalWrite(PIN_PA, HIGH);
  audioI2S.setPins(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT, PIN_I2S_DIN, PIN_I2S_MCLK);
  bool okI2S = audioI2S.begin(I2S_MODE_STD, I2S_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  delay(20);                                                 // 等 MCLK 穩定再設定音訊晶片
  xSemaphoreTake(i2cLock, portMAX_DELAY);
  bool okCodec = es8311Init(pgaGain);
  xSemaphoreGive(i2cLock);
  Serial.printf("I2S: %s, ES8311: %s\n", okI2S ? "OK" : "FAIL", okCodec ? "OK" : "FAIL");
  return okI2S && okCodec;
}

// ============================================================ 硬體 JPEG ====
static jpeg_encoder_handle_t jpegEnc = nullptr;
static jpeg_decoder_handle_t jpegDec = nullptr;
uint16_t *jpegInBuf = nullptr;           // 編碼輸入（RGB565，最大 1024x576），需 DMA 對齊
static uint8_t *jpegOutBuf = nullptr;    // 編碼輸出
static size_t jpegInCap = 0, jpegOutCap = 0;
static SemaphoreHandle_t jpegLock = nullptr;

bool jpegInit() {
  jpegLock = xSemaphoreCreateMutex();
  jpeg_encode_engine_cfg_t ec = {}; ec.timeout_ms = 200;
  jpeg_decode_engine_cfg_t dc = {}; dc.timeout_ms = 200;
  if (jpeg_new_encoder_engine(&ec, &jpegEnc) != ESP_OK) return false;
  if (jpeg_new_decoder_engine(&dc, &jpegDec) != ESP_OK) return false;
  jpeg_encode_memory_alloc_cfg_t in = {JPEG_ENC_ALLOC_INPUT_BUFFER}, out = {JPEG_ENC_ALLOC_OUTPUT_BUFFER};
  jpegInBuf = (uint16_t *)jpeg_alloc_encoder_mem(1024 * 576 * 2, &in, &jpegInCap);
  jpegOutBuf = (uint8_t *)jpeg_alloc_encoder_mem(512 * 1024, &out, &jpegOutCap);
  return jpegInBuf && jpegOutBuf;
}

// 把 jpegInBuf 裡 w x h 的 RGB565 編成 JPEG；回傳指向內部緩衝的指標（下次編碼前有效），失敗回傳 nullptr
// 呼叫端要先 jpegLockTake()，用完 jpegLockGive()
void jpegLockTake() { xSemaphoreTake(jpegLock, portMAX_DELAY); }
void jpegLockGive() { xSemaphoreGive(jpegLock); }
const uint8_t *jpegEncode(int w, int h, int quality, size_t *outLen) {
  jpeg_encode_cfg_t cfg = {};
  cfg.width = w; cfg.height = h;
  cfg.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
  cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV420;
  cfg.image_quality = quality;
  uint32_t n = 0;
  if (jpeg_encoder_process(jpegEnc, &cfg, (const uint8_t *)jpegInBuf, w * h * 2, jpegOutBuf, jpegOutCap, &n) != ESP_OK) return nullptr;
  *outLen = n;
  return jpegOutBuf;
}

// JPEG → RGB565（dw x dh，最近鄰縮放）
bool jpegDecodeTo(const uint8_t *jpg, size_t len, uint16_t *dst, int dw, int dh) {
  jpeg_decode_picture_info_t info;
  if (jpeg_decoder_get_info(jpg, len, &info) != ESP_OK || !info.width || !info.height) return false;
  int ow = (info.width + 15) & ~15, oh = (info.height + 15) & ~15;   // 輸出以 16 對齊
  jpeg_decode_memory_alloc_cfg_t ic = {JPEG_DEC_ALLOC_INPUT_BUFFER}, oc = {JPEG_DEC_ALLOC_OUTPUT_BUFFER};
  size_t icap = 0, ocap = 0;
  uint8_t *inb = (uint8_t *)jpeg_alloc_decoder_mem(len, &ic, &icap);
  uint16_t *outb = (uint16_t *)jpeg_alloc_decoder_mem(ow * oh * 2, &oc, &ocap);
  bool ok = false;
  if (inb && outb) {
    memcpy(inb, jpg, len);
    jpeg_decode_cfg_t cfg = {};
    cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    uint32_t n = 0;
    xSemaphoreTake(jpegLock, portMAX_DELAY);
    ok = jpeg_decoder_process(jpegDec, &cfg, inb, len, (uint8_t *)outb, ocap, &n) == ESP_OK;
    xSemaphoreGive(jpegLock);
    if (ok)
      for (int y = 0; y < dh; y++) {
        const uint16_t *row = outb + (y * (int)info.height / dh) * ow;
        for (int x = 0; x < dw; x++) dst[y * dw + x] = row[x * (int)info.width / dw];
      }
  }
  free(inb); free(outb);
  return ok;
}

// ================================================================ 鏡頭 ====
ESPVideoCaptureDevClass camDev;
static SemaphoreHandle_t camLock = nullptr;

bool camInit(bool vflip, bool hmirror) {
  camLock = xSemaphoreCreateMutex();
  if (!ov02c10Begin((i2c_master_bus_handle_t)i2cBusHandle(0), true)) return false;
  if (!camDev.begin(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, 2)) { Serial.println("開啟鏡頭裝置失敗"); return false; }
  if (!camDev.setFormat(ESP_VIDEO_FORMAT_RGB565)) { Serial.println("鏡頭格式設定失敗"); return false; }
  if (vflip) camDev.setSensorVFlip(true);
  if (hmirror) camDev.setSensorHFlip(true);
  if (!camDev.startCapture()) { Serial.println("鏡頭啟動失敗"); return false; }
  Serial.printf("鏡頭 OK %lux%lu\n", (unsigned long)camDev.getWidth(), (unsigned long)camDev.getHeight());
  return true;
}

// 抓一張最新畫面，最近鄰縮放到 dst（dw x dh，每列 stride 個像素）；skip = 先丟掉幾張舊畫面
bool camGrab(uint16_t *dst, int dw, int dh, int stride, int skip = 0) {
  xSemaphoreTake(camLock, portMAX_DELAY);
  for (int i = 0; i < skip; i++) { ESPVideoBufferClass b = camDev.captureBuffer(); }
  ESPVideoBufferClass b = camDev.captureBuffer();
  bool ok = b.valid();
  if (ok) {
    const uint16_t *s = (const uint16_t *)b.data();
    int sw = b.getWidth(), sh = b.getHeight();
    static int xmap[1024];
    for (int x = 0; x < dw && x < 1024; x++) xmap[x] = x * sw / dw;
    for (int y = 0; y < dh; y++) {
      const uint16_t *row = s + (y * sh / dh) * sw;
      uint16_t *o = dst + y * stride;
      for (int x = 0; x < dw; x++) o[x] = row[xmap[x]];
    }
  }
  xSemaphoreGive(camLock);
  return ok;
}

// ISP 色彩：飽和度 / 對比 0~255（128 = 原始）、亮度 -127~127
void camSetColor(int sat, int bri, int con) {
  static int fd = -1;                                         // 一直開著：關閉 ISP 裝置會讓鏡頭停止出畫面
  if (fd < 0) fd = open(ESP_VIDEO_ISP1_DEVICE_NAME, O_RDWR);
  if (fd < 0) { Serial.println("ISP 裝置開啟失敗，無法調色"); return; }
  struct v4l2_ext_control c[3] = {};
  c[0].id = V4L2_CID_SATURATION; c[0].value = constrain(sat, 0, 255);
  c[1].id = V4L2_CID_BRIGHTNESS; c[1].value = constrain(bri, -127, 127);
  c[2].id = V4L2_CID_CONTRAST;   c[2].value = constrain(con, 0, 255);
  struct v4l2_ext_controls cs = {};
  cs.ctrl_class = V4L2_CTRL_CLASS_USER;
  cs.count = 3;
  cs.controls = c;
  if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &cs) != 0) Serial.println("ISP 調色失敗");

}
