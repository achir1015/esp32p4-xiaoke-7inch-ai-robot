// JC1060P470C 硬體探測：螢幕 / 觸控 / I2C 掃描 / ES8311 喇叭 / 鏡頭 / C6 WiFi
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <ESP_I2S.h>
#include <ESP_Video.h>
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_ldo_regulator.h"
#include "driver/i2c_master.h"
#include "ov02c10_cam.h"

#define LCD_W 1024
#define LCD_H 600
#define PIN_LCD_RST 5
#define PIN_BL      23
#define PIN_SDA     7
#define PIN_SCL     8
#define PIN_TP_RST  22
#define PIN_TP_INT  21
#define PIN_PA      11
#define I2S_MCLK 13
#define I2S_BCLK 12
#define I2S_WS   10
#define I2S_DOUT 9
#define I2S_DIN  48

struct InitCmd { uint8_t cmd; uint8_t len; uint8_t d[14]; };
// JD9165 初始化表：取自本板原廠韌體（backup @0x73FD8），0x00 參數在原廠為 .bss 零值
static const InitCmd kInit[] = {
  {0x30,1,{0x00}},
  {0xF7,4,{0x49,0x61,0x02,0x00}},
  {0x30,1,{0x01}},
  {0x04,1,{0x0C}},
  {0x05,1,{0x08}},
  {0x0B,1,{0x11}},
  {0x20,1,{0x04}},
  {0x1F,1,{0x05}},
  {0x23,1,{0x38}},
  {0x28,1,{0x18}},
  {0x29,1,{0x29}},
  {0x2A,1,{0x01}},
  {0x2B,1,{0x29}},
  {0x2C,1,{0x01}},
  {0x30,1,{0x02}},
  {0x00,1,{0x05}},
  {0x01,1,{0x22}},
  {0x02,1,{0x08}},
  {0x03,1,{0x12}},
  {0x04,1,{0x16}},
  {0x05,1,{0x64}},
  {0x06,1,{0x00}},
  {0x07,1,{0x00}},
  {0x08,1,{0x78}},
  {0x09,1,{0x00}},
  {0x0A,1,{0x04}},
  {0x0B,11,{0x16,0x17,0x0B,0x0D,0x0D,0x0D,0x11,0x10,0x07,0x07,0x09}},
  {0x0C,11,{0x09,0x1E,0x1E,0x1C,0x1C,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D}},
  {0x0D,11,{0x0A,0x05,0x0B,0x0D,0x0D,0x0D,0x11,0x10,0x06,0x06,0x08}},
  {0x0E,11,{0x08,0x1F,0x1F,0x1D,0x1D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D}},
  {0x0F,11,{0x0A,0x05,0x0D,0x0B,0x0D,0x0D,0x11,0x10,0x1D,0x1D,0x1F}},
  {0x10,11,{0x1F,0x08,0x08,0x06,0x06,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D}},
  {0x11,11,{0x16,0x17,0x0D,0x0B,0x0D,0x0D,0x11,0x10,0x1C,0x1C,0x1E}},
  {0x12,11,{0x1E,0x09,0x09,0x07,0x07,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D}},
  {0x13,4,{0x00,0x00,0x00,0x00}},
  {0x14,4,{0x00,0x00,0x41,0x41}},
  {0x15,4,{0x00,0x00,0x00,0x00}},
  {0x17,1,{0x00}},
  {0x18,1,{0x85}},
  {0x19,2,{0x06,0x09}},
  {0x1A,2,{0x05,0x08}},
  {0x1B,2,{0x0A,0x04}},
  {0x26,1,{0x00}},
  {0x27,1,{0x00}},
  {0x30,1,{0x06}},
  {0x12,14,{0x3F,0x25,0x27,0x35,0x1D,0x1B,0x1B,0x1A,0x18,0x0A,0x2A,0x21,0x19,0x30}},
  {0x13,14,{0x3F,0x26,0x27,0x35,0x1E,0x1C,0x1C,0x1A,0x18,0x0B,0x2A,0x21,0x19,0x30}},
  {0x30,1,{0x0A}},
  {0x02,1,{0x4F}},
  {0x0B,1,{0x40}},
  {0x30,1,{0x0D}},
  {0x0D,1,{0x04}},
  {0x10,1,{0x05}},
  {0x11,1,{0x0C}},
  {0x12,1,{0x05}},
  {0x13,1,{0x0C}},
  {0x30,1,{0x00}},
  {0x3A,1,{0x55}},
};

esp_lcd_panel_handle_t panel;
uint16_t *fb;
ESPVideoClass video;
ESPVideoCaptureDevClass cap;
I2SClass i2s;

void fill(uint16_t c, int x0, int y0, int w, int h) {
  for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) fb[y * LCD_W + x] = c;
}

bool initLcd() {
  esp_ldo_channel_handle_t ldo;
  esp_ldo_channel_config_t lc = {.chan_id = 3, .voltage_mv = 2500};
  if (esp_ldo_acquire_channel(&lc, &ldo) != ESP_OK) Serial.println("LDO3 失敗");
  pinMode(PIN_LCD_RST, OUTPUT);

  esp_lcd_dsi_bus_handle_t bus;
  esp_lcd_dsi_bus_config_t bc = {};
  bc.bus_id = 0; bc.num_data_lanes = 2; bc.phy_clk_src = MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT_LEGACY; bc.lane_bit_rate_mbps = 750;  // rev<3 晶片不支援 XTAL，用 LEGACY
  if (esp_lcd_new_dsi_bus(&bc, &bus) != ESP_OK) { Serial.println("DSI bus 失敗"); return false; }
  esp_lcd_panel_io_handle_t io;
  esp_lcd_dbi_io_config_t ic = {.virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8};
  esp_lcd_new_panel_io_dbi(bus, &ic, &io);

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
  if (esp_lcd_new_panel_dpi(bus, &pc, &panel) != ESP_OK) { Serial.println("DPI panel 失敗"); return false; }

  // 與樂鑫 esp_lcd_jd9165 相同：重置 → 送初始化指令 → 啟動 DPI
  digitalWrite(PIN_LCD_RST, HIGH); delay(5); digitalWrite(PIN_LCD_RST, LOW); delay(10); digitalWrite(PIN_LCD_RST, HIGH);
  delay(120);
  uint8_t madctl = 0x00, colmod = 0x55;
  esp_lcd_panel_io_tx_param(io, 0x36, &madctl, 1);
  esp_lcd_panel_io_tx_param(io, 0x3A, &colmod, 1);
  for (auto &c : kInit) esp_lcd_panel_io_tx_param(io, c.cmd, c.d, c.len);
  esp_lcd_panel_io_tx_param(io, 0x11, nullptr, 0); delay(120);
  esp_lcd_panel_io_tx_param(io, 0x29, nullptr, 0); delay(20);
  esp_lcd_panel_init(panel);
  esp_lcd_dpi_panel_get_frame_buffer(panel, 1, (void **)&fb);
  Serial.printf("fb=%p\n", fb);
  return fb != nullptr;
}

void flush() { esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, fb); }

void i2cScan() {
  Serial.print("I2C 裝置:");
  for (uint8_t a = 1; a < 127; a++) { Wire.beginTransmission(a); if (Wire.endTransmission() == 0) Serial.printf(" 0x%02X", a); }
  Serial.println();
}

uint8_t gtAddr = 0;
bool gtRead(uint16_t reg, uint8_t *buf, int n) {
  Wire.beginTransmission(gtAddr); Wire.write(reg >> 8); Wire.write(reg & 0xFF);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(gtAddr, (uint8_t)n) != n) return false;
  for (int i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}
void gtWrite(uint16_t reg, uint8_t v) {
  Wire.beginTransmission(gtAddr); Wire.write(reg >> 8); Wire.write(reg & 0xFF); Wire.write(v); Wire.endTransmission();
}

void tone440() {
  pinMode(PIN_PA, OUTPUT); digitalWrite(PIN_PA, HIGH);
  i2s.setPins(I2S_BCLK, I2S_WS, I2S_DOUT, I2S_DIN, I2S_MCLK);
  if (!i2s.begin(I2S_MODE_STD, 24000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) { Serial.println("I2S 失敗"); return; }
  // ES8311 從屬模式（與 SparkBot 版相同）
  const uint8_t seq[][2] = {
    {0x44,0x08},{0x44,0x08},{0x00,0x1F},{0x00,0x00},{0x01,0x30},{0x02,0x00},{0x03,0x10},{0x16,0x24},{0x04,0x10},{0x05,0x00},
    {0x0B,0x00},{0x0C,0x00},{0x10,0x1F},{0x11,0x7F},{0x00,0x80},{0x01,0x3F},{0x06,0x03},{0x07,0x00},{0x08,0xFF},
    {0x09,0x0C},{0x0A,0x0C},{0x13,0x10},{0x1B,0x0A},{0x1C,0x6A},{0x0D,0x01},{0x0E,0x02},{0x12,0x00},{0x14,0x1A},
    {0x15,0x40},{0x17,0xBF},{0x37,0x08},{0x45,0x00},{0x32,0xBF},{0x31,0x00}};
  for (auto &r : seq) {
    for (int k = 0; k < 3; k++) { Wire.beginTransmission(0x18); Wire.write(r[0]); Wire.write(r[1]); if (Wire.endTransmission() == 0) break; delay(5); }
    if (r[0] == 0x00 && r[1] == 0x1F) delay(20);
  }
  static int16_t buf[480];
  for (int n = 0; n < 50; n++) {   // 1 秒 440Hz
    for (int i = 0; i < 480; i++) buf[i] = (int16_t)(6000 * sinf(2 * PI * 440 * (n * 480 + i) / 24000.0f));
    i2s.write((uint8_t *)buf, sizeof(buf));
  }
  // 錄 1 秒算音量
  long sum = 0; int cnt = 0; int16_t mx = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < 1000) {
    size_t r = i2s.readBytes((char *)buf, sizeof(buf));
    for (size_t i = 0; i < r / 2; i++) { sum += abs(buf[i]); cnt++; if (abs(buf[i]) > mx) mx = abs(buf[i]); }
  }
  Serial.printf("麥克風 平均=%ld 最大=%d 樣本=%d\n", cnt ? sum / cnt : 0, mx, cnt);
}

void initCam() {
  esp_log_level_set("*", ESP_LOG_DEBUG);
  if (!ov02c10Begin((i2c_master_bus_handle_t)i2cBusHandle(0), true)) return;
  if (!cap.begin(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, 2)) { Serial.println("開啟鏡頭裝置失敗"); return; }
  if (!cap.setFormat(ESP_VIDEO_FORMAT_RGB565)) Serial.println("設定格式失敗");
  if (!cap.startCapture()) { Serial.println("啟動擷取失敗"); return; }
  delay(300);
  for (uint16_t r : {0x0100, 0x0103, 0x3501, 0x3502, 0x3508, 0x380c, 0x380e, 0x4800, 0x4f00, 0x3016, 0x301b}) {
    uint8_t v = 0xEE; ovR(ov02c10Dev, r, &v);
    Serial.printf("[OV reg] %04X=%02X\n", r, v);
  }
  Serial.printf("鏡頭 OK %lux%lu\n", (unsigned long)cap.getWidth(), (unsigned long)cap.getHeight());
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.printf("PSRAM %u\n", ESP.getPsramSize());
  pinMode(PIN_BL, OUTPUT); digitalWrite(PIN_BL, HIGH);
  if (initLcd()) {
    fill(0xF800, 0, 0, 341, 300); fill(0x07E0, 341, 0, 341, 300); fill(0x001F, 682, 0, 342, 300);
    fill(0xFFFF, 0, 300, 512, 300); fill(0xFFE0, 512, 300, 512, 300);
    flush();
  }
  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  pinMode(PIN_TP_RST, OUTPUT); pinMode(PIN_TP_INT, OUTPUT);
  digitalWrite(PIN_TP_INT, LOW); digitalWrite(PIN_TP_RST, LOW); delay(10);
  digitalWrite(PIN_TP_RST, HIGH); delay(60); pinMode(PIN_TP_INT, INPUT); delay(50);
  i2cScan();
  for (uint8_t a : {0x5D, 0x14}) { Wire.beginTransmission(a); if (Wire.endTransmission() == 0) { gtAddr = a; break; } }
  if (gtAddr) { uint8_t id[4]; gtRead(0x8140, id, 4); Serial.printf("GT911 @0x%02X ID=%c%c%c%c\n", gtAddr, id[0], id[1], id[2], id[3]); }
  tone440();
  initCam();
  i2cScan();
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks();
  Serial.printf("WiFi 掃描到 %d 個\n", n);
  for (int i = 0; i < n && i < 8; i++) Serial.printf("  %s %d\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i));
  Serial.println("PROBE_DONE");
}

uint32_t lastCam = 0, lastBl = 0; bool blHigh = true;
void loop() {
  if (false && millis() - lastBl > 4000) { lastBl = millis(); blHigh = !blHigh; digitalWrite(PIN_BL, blHigh); Serial.printf("BL=%s\n", blHigh ? "HIGH" : "LOW"); }
  if (gtAddr) {
    uint8_t st;
    if (gtRead(0x814E, &st, 1) && (st & 0x80)) {
      int n = st & 0x0F;
      if (n) { uint8_t p[8]; gtRead(0x8150, p, 6); int x = p[0] | p[1] << 8, y = p[2] | p[3] << 8;
        Serial.printf("TOUCH x=%d y=%d\n", x, y); if (fb && x < LCD_W - 10 && y < LCD_H - 10) { fill(0x0000, x, y, 10, 10); flush(); } }
      gtWrite(0x814E, 0);
    }
  }
  if (cap.isCaptureStarted() && millis() - lastCam > 3000) {
    lastCam = millis();
    ESPVideoBufferClass b = cap.captureBuffer();
    if (b.valid()) {
      Serial.printf("CAM frame %s %lux%lu %u bytes\n", b.formatName(), (unsigned long)b.getWidth(), (unsigned long)b.getHeight(), (unsigned)b.size());
      // 左上角顯示縮小畫面
      uint16_t *s = (uint16_t *)b.data(); int w = b.getWidth(), h = b.getHeight(), step = max(1, w / 320);
      for (int y = 0; y < 240 && y * step < h; y++) for (int x = 0; x < 320; x++) fb[y * LCD_W + x] = s[(y * step) * w + x * step];
      flush();
    }
  }
  delay(20);
}
