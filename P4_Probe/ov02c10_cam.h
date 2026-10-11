// ============================================================================
//  ov02c10_cam.h — JC1060P470C 板載 OV02C10（200 萬畫素 MIPI-CSI）鏡頭驅動
//    Arduino 內建的 esp_cam_sensor 沒有 OV02C10，這裡自己實作 esp_cam_sensor 介面，
//    再交給 esp_video 建立 /dev/video0（MIPI-CSI）與 ISP 自動曝光/白平衡管線。
//    初始化暫存器取自原廠韌體；曝光/增益暫存器依 Linux ov02c10 驅動。
//    ISP 調校參數借用 SC2336（同為 2MP RAW10），白平衡/色彩可能與原廠略有差異。
// ============================================================================
#pragma once
#include <ESP_Video.h>
#include "esp_cam_sensor.h"
extern "C" {
#include "esp_sccb_i2c.h"
}
#include "esp_sccb_intf.h"
#include "esp_video_device.h"
#include "driver/i2c_master.h"
#include "ov02c10_regs.h"

#define OV02C10_ADDR      0x36
#define OV02C10_PID       0x5602
#define OV02C10_EXP_MIN   4
#define OV02C10_EXP_MARGIN 8
#define OV02C10_IPA_NAME  "SC2336"   // 借用的 ISP 調校檔

struct esp_video_csi_device_config_t { bool dont_init_ldo; };
struct esp_video_isp_config_t { const char *isp_dev; const char *cam_dev; const void *ipa_config; };
extern "C" {
esp_err_t esp_video_create_csi_video_device(esp_cam_sensor_device_t *cam_dev, const esp_video_csi_device_config_t *config);
esp_err_t esp_video_create_isp_video_device(void);
esp_err_t esp_video_isp_pipeline_init(const esp_video_isp_config_t *config);
const void *esp_ipa_pipeline_get_config(const char *name);
}

// 類比增益 1x ~ 15.5x，每 1/16 一階（暫存器值 0x10 ~ 0xF8）
#define OV02C10_GAIN_STEPS (0xF8 - 0x10 + 1)
static uint32_t ov02c10GainMap[OV02C10_GAIN_STEPS];

static const esp_cam_sensor_isp_info_t ov02c10IspInfo = {
  .isp_v1_info = {
    .version = SENSOR_ISP_INFO_VERSION_DEFAULT,
    .pclk = 81000000,
    .hts = 2280,
    .vts = 1164,
    .exp_def = 0x37e,
    .gain_def = 16,                 // 2x
    .tline_ns = 28148,
    .bayer_type = ESP_CAM_SENSOR_BAYER_GBRG,
  }
};

static const esp_cam_sensor_format_t ov02c10Format = {
  .name = "MIPI_1lane_24Minput_RAW10_1288x728_30fps",
  .format = ESP_CAM_SENSOR_PIXFORMAT_RAW10,
  .port = ESP_CAM_SENSOR_MIPI_CSI,
  .xclk = 24000000,
  .width = 1288,
  .height = 728,
  .regs = OV02C10_INIT_REGS,
  .regs_size = sizeof(OV02C10_INIT_REGS) / sizeof(OV02C10_INIT_REGS[0]),
  .fps = 30,
  .isp_info = &ov02c10IspInfo,
  .mipi_info = { .mipi_clk = 400000000, .hs_settle = 0, .lane_num = 1, .line_sync_en = false },
  .reserved = NULL,
};

esp_cam_sensor_device_t *ov02c10Dev = nullptr;
struct Ov02c10State { uint32_t exp, gainIdx; };
static Ov02c10State ov02c10St;

static esp_err_t ovW(esp_cam_sensor_device_t *d, uint16_t r, uint8_t v) { return esp_sccb_transmit_reg_a16v8(d->sccb_handle, r, v); }
static esp_err_t ovR(esp_cam_sensor_device_t *d, uint16_t r, uint8_t *v) { return esp_sccb_transmit_receive_reg_a16v8(d->sccb_handle, r, v); }

static uint32_t ovExpMax(esp_cam_sensor_device_t *d) { return d->cur_format->isp_info->isp_v1_info.vts - OV02C10_EXP_MARGIN; }
// 曝光（行數）↔ 微秒
static uint32_t ovLinesToUs(uint32_t lines) { return (uint32_t)((uint64_t)lines * 28148 / 1000); }
static uint32_t ovUsToLines(uint32_t us) { return (uint32_t)((uint64_t)us * 1000 / 28148); }

static esp_err_t ovSetExp(esp_cam_sensor_device_t *d, uint32_t v) {
  v = constrain(v, (uint32_t)OV02C10_EXP_MIN, ovExpMax(d));
  esp_err_t r = ovW(d, 0x3501, v >> 8) | ovW(d, 0x3502, v & 0xFF);
  if (r == ESP_OK) ov02c10St.exp = v;
  return r;
}
static esp_err_t ovSetGain(esp_cam_sensor_device_t *d, uint32_t idx) {
  idx = min(idx, (uint32_t)OV02C10_GAIN_STEPS - 1);
  uint32_t g = 0x10 + idx;                              // 1/16 倍為單位
  esp_err_t r = ovW(d, 0x3508, g >> 4) | ovW(d, 0x3509, (g & 0x0F) << 4);
  if (r == ESP_OK) ov02c10St.gainIdx = idx;
  return r;
}

static int ovQueryParaDesc(esp_cam_sensor_device_t *d, esp_cam_sensor_param_desc_t *q) {
  switch (q->id) {
    case ESP_CAM_SENSOR_EXPOSURE_VAL:
      q->type = ESP_CAM_SENSOR_PARAM_TYPE_NUMBER;
      q->number.minimum = OV02C10_EXP_MIN; q->number.maximum = ovExpMax(d); q->number.step = 1;
      q->default_value = d->cur_format->isp_info->isp_v1_info.exp_def;
      return ESP_OK;
    case ESP_CAM_SENSOR_EXPOSURE_US:
      q->type = ESP_CAM_SENSOR_PARAM_TYPE_NUMBER;
      q->number.minimum = ovLinesToUs(OV02C10_EXP_MIN); q->number.maximum = ovLinesToUs(ovExpMax(d));
      q->number.step = max(ovLinesToUs(1), (uint32_t)1);
      q->default_value = ovLinesToUs(d->cur_format->isp_info->isp_v1_info.exp_def);
      return ESP_OK;
    case ESP_CAM_SENSOR_GAIN:
      q->type = ESP_CAM_SENSOR_PARAM_TYPE_ENUMERATION;
      q->enumeration.count = OV02C10_GAIN_STEPS;
      q->enumeration.elements = ov02c10GainMap;
      q->default_value = d->cur_format->isp_info->isp_v1_info.gain_def;
      return ESP_OK;
    case ESP_CAM_SENSOR_GROUP_EXP_GAIN:
      q->type = ESP_CAM_SENSOR_PARAM_TYPE_U8;
      q->u8.size = sizeof(esp_cam_sensor_gh_exp_gain_t);
      return ESP_OK;
    case ESP_CAM_SENSOR_VFLIP:
    case ESP_CAM_SENSOR_HMIRROR:
      q->type = ESP_CAM_SENSOR_PARAM_TYPE_NUMBER;
      q->number.minimum = 0; q->number.maximum = 1; q->number.step = 1; q->default_value = 0;
      return ESP_OK;
    default: return ESP_ERR_INVALID_ARG;
  }
}

static int ovGetPara(esp_cam_sensor_device_t *d, uint32_t id, void *arg, size_t size) {
  if (id == ESP_CAM_SENSOR_EXPOSURE_VAL) { *(uint32_t *)arg = ov02c10St.exp; return ESP_OK; }
  if (id == ESP_CAM_SENSOR_GAIN) { *(uint32_t *)arg = ov02c10St.gainIdx; return ESP_OK; }
  return ESP_ERR_NOT_SUPPORTED;
}

// 0x3820 bit[4:3] 上下翻轉、0x3821 bit[2] 左右鏡像（OmniVision 慣例）
static esp_err_t ovSetBits(esp_cam_sensor_device_t *d, uint16_t reg, uint8_t mask, bool on) {
  uint8_t v = 0;
  if (ovR(d, reg, &v) != ESP_OK) return ESP_FAIL;
  return ovW(d, reg, on ? (v | mask) : (v & ~mask));
}

static int ovSetPara(esp_cam_sensor_device_t *d, uint32_t id, const void *arg, size_t size) {
  switch (id) {
    case ESP_CAM_SENSOR_EXPOSURE_VAL: return ovSetExp(d, *(const uint32_t *)arg);
    case ESP_CAM_SENSOR_EXPOSURE_US:  return ovSetExp(d, ovUsToLines(*(const uint32_t *)arg));
    case ESP_CAM_SENSOR_GAIN:         return ovSetGain(d, *(const uint32_t *)arg);
    case ESP_CAM_SENSOR_GROUP_EXP_GAIN: {
      const esp_cam_sensor_gh_exp_gain_t *v = (const esp_cam_sensor_gh_exp_gain_t *)arg;
      uint32_t e = v->exposure_us ? ovUsToLines(v->exposure_us) : v->exposure_val;
      if (!e) return ESP_ERR_INVALID_ARG;
      ovW(d, 0x3208, 0x00);                               // group hold：曝光與增益同一幀生效
      esp_err_t r = ovSetExp(d, e) | ovSetGain(d, v->gain_index);
      ovW(d, 0x3208, 0x10); ovW(d, 0x3208, 0xA0);
      return r;
    }
    case ESP_CAM_SENSOR_VFLIP:   return ovSetBits(d, 0x3820, 0x18, *(const int *)arg);
    case ESP_CAM_SENSOR_HMIRROR: return ovSetBits(d, 0x3821, 0x04, *(const int *)arg);
    default: return ESP_ERR_INVALID_ARG;
  }
}

static int ovQueryFormats(esp_cam_sensor_device_t *d, esp_cam_sensor_format_array_t *f) {
  f->count = 1; f->format_array = &ov02c10Format; return ESP_OK;
}
static int ovQueryCap(esp_cam_sensor_device_t *d, esp_cam_sensor_capability_t *c) { c->fmt_raw = 1; return ESP_OK; }

static int ovSetFormat(esp_cam_sensor_device_t *d, const esp_cam_sensor_format_t *f) {
  if (!f) f = &ov02c10Format;
  const Ov02c10Reg *r = (const Ov02c10Reg *)f->regs;
  for (int i = 0; r[i].reg != 0xFFFF; i++) {
    if (ovW(d, r[i].reg, r[i].val) != ESP_OK) { Serial.printf("OV02C10 寫入失敗 0x%04X\n", r[i].reg); return ESP_CAM_SENSOR_ERR_FAILED_SET_FORMAT; }
    if (r[i].reg == 0x0103) delay(10);                  // 軟體重置後等待
  }
  d->cur_format = f;
  ov02c10St.exp = f->isp_info->isp_v1_info.exp_def;
  ov02c10St.gainIdx = f->isp_info->isp_v1_info.gain_def;
  return ESP_OK;
}
static int ovGetFormat(esp_cam_sensor_device_t *d, esp_cam_sensor_format_t *f) {
  if (!d->cur_format) return ESP_FAIL;
  memcpy(f, d->cur_format, sizeof(*f)); return ESP_OK;
}

static int ovIoctl(esp_cam_sensor_device_t *d, uint32_t cmd, void *arg) {
  switch (cmd) {
    case ESP_CAM_SENSOR_IOC_SW_RESET: { esp_err_t r = ovW(d, 0x0103, 0x01); delay(10); return r; }
    case ESP_CAM_SENSOR_IOC_S_STREAM: d->stream_status = *(int *)arg; return ovW(d, 0x0100, *(int *)arg ? 1 : 0);
    case ESP_CAM_SENSOR_IOC_S_TEST_PATTERN: return ovW(d, 0x4503, *(int *)arg ? 0x80 : 0x00);
    case ESP_CAM_SENSOR_IOC_S_REG: { auto *s = (esp_cam_sensor_reg_val_t *)arg; return ovW(d, s->regaddr, s->value); }
    case ESP_CAM_SENSOR_IOC_G_REG: { auto *s = (esp_cam_sensor_reg_val_t *)arg; uint8_t v; esp_err_t r = ovR(d, s->regaddr, &v); s->value = v; return r; }
    case ESP_CAM_SENSOR_IOC_G_CHIP_ID: *(esp_cam_sensor_id_t *)arg = d->id; return ESP_OK;
    default: return ESP_OK;
  }
}
static int ovDelete(esp_cam_sensor_device_t *d) { free(d); return ESP_OK; }

static const esp_cam_sensor_ops_t ov02c10Ops = {
  .query_para_desc = ovQueryParaDesc,
  .get_para_value = ovGetPara,
  .set_para_value = ovSetPara,
  .query_support_formats = ovQueryFormats,
  .query_support_capability = ovQueryCap,
  .set_format = ovSetFormat,
  .get_format = ovGetFormat,
  .priv_ioctl = ovIoctl,
  .del = ovDelete,
};

// 建立 OV02C10 → /dev/video0（MIPI-CSI）+ ISP 管線；i2cBus = 與觸控/ES8311 共用的 I2C
bool ov02c10Begin(i2c_master_bus_handle_t i2cBus, bool ldoAlreadyOn) {
  for (int i = 0; i < OV02C10_GAIN_STEPS; i++) ov02c10GainMap[i] = (uint32_t)(16 + i) * 1000 / 16;

  esp_sccb_io_handle_t io;
  sccb_i2c_config_t sc = {};
  sc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  sc.device_address = OV02C10_ADDR;
  sc.scl_speed_hz = 100000;
  if (sccb_new_i2c_io(i2cBus, &sc, &io) != ESP_OK) { Serial.println("鏡頭 SCCB 建立失敗"); return false; }

  esp_cam_sensor_device_t *dev = (esp_cam_sensor_device_t *)calloc(1, sizeof(esp_cam_sensor_device_t));
  ov02c10Dev = dev;
  dev->name = (char *)OV02C10_IPA_NAME;   // esp_video 依名稱找 ISP 調校檔
  dev->sccb_handle = io;
  dev->xclk_pin = GPIO_NUM_NC; dev->reset_pin = GPIO_NUM_NC; dev->pwdn_pin = GPIO_NUM_NC;
  dev->sensor_port = ESP_CAM_SENSOR_MIPI_CSI;
  dev->ops = &ov02c10Ops;
  dev->cur_format = &ov02c10Format;

  uint8_t h = 0, l = 0;
  ovR(dev, 0x300A, &h); ovR(dev, 0x300B, &l);
  dev->id.pid = (h << 8) | l;
  if (dev->id.pid != OV02C10_PID) { Serial.printf("鏡頭 PID=0x%04X 不是 OV02C10\n", dev->id.pid); free(dev); return false; }
  Serial.println("偵測到 OV02C10");
  // esp_video 不會呼叫 set_format，要自己先寫入整張初始化表（否則鏡頭用出廠預設值，CSI 收不到畫面；
  // 10/09 曾誤以為正常，是因為鏡頭沒斷電、還留著原廠韌體寫的設定）
  if (ovSetFormat(dev, &ov02c10Format) != ESP_OK) { Serial.println("鏡頭初始化表寫入失敗"); free(dev); return false; }

  if (esp_video_create_isp_video_device() != ESP_OK) Serial.println("ISP 裝置建立失敗");
  esp_video_csi_device_config_t cc = { .dont_init_ldo = ldoAlreadyOn };
  if (esp_video_create_csi_video_device(dev, &cc) != ESP_OK) { Serial.println("CSI 裝置建立失敗"); return false; }
  const void *ipa = esp_ipa_pipeline_get_config(OV02C10_IPA_NAME);
  if (ipa) {
    esp_video_isp_config_t ic = { ESP_VIDEO_ISP1_DEVICE_NAME, ESP_VIDEO_MIPI_CSI_DEVICE_NAME, ipa };
    if (esp_video_isp_pipeline_init(&ic) != ESP_OK) Serial.println("ISP 管線啟動失敗（沒有自動曝光）");
  } else Serial.println("找不到 ISP 調校檔");
  return true;
}
