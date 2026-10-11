# 小柯 AI 語音機器人：ESP32-P4 七吋版

把「小柯」AI 語音視覺聊天機器人（OpenAI 版）移植到 **Guition JC1060P470C**（ESP32-P4 + ESP32-C6、7 吋 1024×600 觸控螢幕）。

## 功能

- 聲控聊天：直接說話 → OpenAI 語音轉文字 → GPT 回答（可上網查天氣、新聞）→ OpenAI TTS 播放
- 拍照辨識、攝影、錄影上傳 Google 雲端硬碟（OV02C10 200 萬畫素鏡頭）
- 螢幕：白色頭盔機器人表情（眨眼、東張西望、聆聽、思考、說話、愛心、生氣…）
- 上方顯示西元日期、星期、農曆、24 小時制時間
- 右側**農民曆**：干支、生肖、建除、宜忌、沖煞、節氣（離線計算；宜忌為建除十二神簡易版，僅供參考）
- 班表／工作流程查詢（Firebase）
- **唱歌**：直接播放雲端硬碟資料夾裡的自創歌曲 MP3，可點歌（「唱千江有水千江月」），點螢幕停止
- **播 MV**：邊下載邊播雲端資料夾裡的 MV（`.xkv` = MJPEG 640×352 + MP3，由 `tools/make_xkv.py` 從 MP4 轉檔），畫面以聲音為準同步
- **病房助理工作站**：觸控選單（開機時從工作站網頁解析工具清單，新增工具自動更新），點工具看說明與 QR 碼；技術稽核、督導手冊、學科題庫、術科、長照組合的內容可以直接用說的問，小柯在板子上找出相關段落再交給 AI 回答
- 攝影、錄影上傳雲端硬碟（AVI MJPEG 480×272 + μ-law 聲音）
- 觸控螢幕取代按鍵：點一下看說明，按住說話

## 資料夾

| 資料夾 | 說明 |
|---|---|
| `XiaoKe_P4/` | 主程式 |
| `P4_Probe/` | 硬體探測程式（螢幕、觸控、I2C、喇叭、鏡頭、WiFi） |
| `C6_Update/` | 經 USB 更新 ESP32-C6 的 ESP-Hosted 韌體（不需另外接線） |

## 硬體（已實機驗證）

| 項目 | 腳位／說明 |
|---|---|
| I2C | SDA 7、SCL 8（GT911 0x5D、ES8311 0x18、OV02C10 0x36） |
| 螢幕 JD9165 | MIPI-DSI 2-lane 750Mbps，RST 5，背光 GPIO 23（高電位亮） |
| 觸控 GT911 | RST 22、INT 21 |
| 音訊 ES8311 | MCLK 13、BCLK 12、WS 10、DOUT 9、DIN 48，功放 GPIO 11 |
| 鏡頭 OV02C10 | MIPI-CSI 1-lane，自寫驅動 `ov02c10_cam.h` |
| WiFi | ESP32-C6 經 SDIO（ESP-Hosted） |

## 編譯

Arduino IDE 2.x + esp32 by Espressif **3.3.12**，需要 Adafruit GFX、ArduinoJson 程式庫。

```
FQBN: esp32:esp32:esp32p4:ChipVariant=prev3,PSRAM=enabled,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc
```

- 晶片是 rev v1.3，「Chip Variant」要選 **Before v3.00**
- 資料夾路徑不要有中文或「+」，否則會編譯失敗（cc1plus CreateProcess 錯誤）
- 複製 `XiaoKe_P4/config.example.h` 成 `config.h` 填入設定；也可以開機後用設定網頁修改

## 注意事項

- **C6 韌體要先更新**：原廠 ESP-Hosted 2.3.2 能掃描 WiFi 但連不上。燒 `C6_Update`，下載 `https://espressif.github.io/arduino-esp32/hosted/esp32c6-v2.12.13.bin`，再執行 `python C6_Update/c6_update.py COM12 <bin>`
- **喇叭**：板子沒有內建喇叭，要接在背面的 MX1.25 2P 喇叭座（8Ω 1–3W）。只接電腦 USB 時，喇叭一出聲電壓會掉而重開機，所以 `config.h` 預設 `SPK_LIMIT 9000`（限幅）、`SPEAK_BRIGHTNESS 25`（說話時螢幕調暗）、`BROWNOUT_LEVEL 4`；改用 5V/2A 充電器供電時，可以把 `SPK_LIMIT` 調高、`SPEAK_BRIGHTNESS` 改 100
- 螢幕與鏡頭的初始化暫存器值參考自本板原廠韌體
- **鏡頭**：esp_video 不會呼叫感測器的 set_format，驅動要自己先寫入整張初始化表；否則斷電重開後鏡頭用出廠預設值，CSI 收不到畫面
- **記憶體**：開機時 `heap_caps_malloc_extmem_enable(512)`（要在鏡頭初始化之後），讓小配置改放 PSRAM，內部 RAM 留給 HTTPS 的 DMA 加密
- 編譯路徑太長時連結會失敗（collect2 CreateProcess），請把專案放在短的英文路徑
- 歌曲 MP3／MV 檔案放在雲端硬碟（`config.h` 的 `SONGS_MP3_FOLDER`、`SONGS_MV_FOLDER`），資料夾要設成「知道連結的任何人都能檢視」

## 開發者

吳玉柱先生與 Claude AI 共同開發。意見請聯絡 achir1015@gmail.com
