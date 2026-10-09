// ============================================================================
//  config.example.h — 設定範本（複製成 config.h 再填入自己的值；沒有 config.h 時自動使用本檔）
//  config.h — 所有可調整的設定都在這裡（Wi-Fi、API Key、腳位、模型、音量…）
//  ⚠ 此檔含 API Key，請勿上傳到 GitHub 或分享給他人
// ============================================================================
#pragma once

// ---------------------------------------------------------------- Wi-Fi ----
#define WIFI_SSID       "你的WiFi名稱"
#define WIFI_PASSWORD   "你的WiFi密碼"

// --------------------------------------------------------------- OpenAI ----
#define OPENAI_API_KEY  "sk-請填入你的OpenAI金鑰"  

#define STT_MODEL       "gpt-4o-mini-transcribe"   // 或 "whisper-1"
#define CHAT_MODEL      "gpt-4o-mini"              // 需支援影像輸入（拍照辨識用）
#define TTS_MODEL       "gpt-4o-mini-tts"          // 或 "tts-1"
#define TTS_VOICE       "nova"                     // alloy / ash / coral / echo / fable / nova / onyx / sage / shimmer
#define TTS_INSTRUCTIONS "用可愛、活潑、溫暖的台灣口音中文說話，語氣像貼心的小朋友，語速稍快。"

#define ROBOT_NAME      "小柯"
#define MAX_HISTORY_MSGS 10        // 記住最近幾則對話（user+assistant 各算 1 則）
#define CHAT_MAX_TOKENS  300

// ------------------------------------------------------------ 上網搜尋 ----
// 1 = 問天氣、新聞、股價、比賽結果等即時資訊時，AI 會自己決定上網搜尋再回答（OpenAI web_search，搜尋另外計費）
#define ENABLE_WEB_SEARCH 1
#define SEARCH_CITY       "Taipei"   // 搜尋預設地區（英文城市名）
#define SEARCH_CITY_ZH    "台北"     // 沒講地點時以這裡為準

// ------------------------------------------------------- 班表查詢（Google Firebase Firestore）----
// 問「我明天上什麼班」「這個月休幾天」時，讀取 Firestore 文件 schedules/<類別>-<年>-<月> 交給 AI 回答
#define FIREBASE_PROJECT  "monthlyschedule-a5dea"
#define FIREBASE_API_KEY  ""          // Firestore 規則需要金鑰時才填（網頁應用程式設定的 apiKey）
#define SCHEDULE_CATEGORIES { "assistant", "nurse" }   // 文件名稱前綴，例如 assistant-2026-10、nurse-2026-10
#define SCHEDULE_LABELS     { "病房助理", "護理師" }        // 對應的中文名稱
#define SCHEDULE_MY_NAME  "吳玉柱"    // 使用者說「我」指的是誰
#define SCHEDULE_LEGEND   "（與月班表系統相同規則）代號含 D 或「白值」= 白班（如 D1、DD、D/F、D/W、D/洗、D/學）；含 E = 小夜班；含 N 或「夜8」= 大夜班；含 OFF、國、休 = 休假；例 = 例假；其他代號（如傷照）照原代號說"

// ------------------------------------------------- 工作流程查詢（Firebase：線上專案進度時間軸表）----
// 問「白班幾點交班」「現在該做什麼」時，讀取 projects 與各專案的 shifts（班別 → 時間軸任務）交給 AI 回答
#define TIMELINE_PROJECT  "project-timeline-d2188"
#define TIMELINE_DESC_MAX 150         // 每個任務說明最多保留幾個字

// ------------------------------------------------------------- 功能開關 ----
#define ENABLE_CAMERA   1          // 1 = 啟用鏡頭，說「拍照」「你看到什麼」會把畫面送給 AI 看
#define CAM_VFLIP       0          // 鏡頭畫面上下顛倒時改 1
#define CAM_HMIRROR     0          // 鏡頭畫面左右相反時改 1
#define CAMERA_PREVIEW_MAX_SEC 600 // 攝影（即時影像）最長顯示秒數，時間到自動回到原畫面
#define REC_SECONDS     90         // 錄影最長秒數；說「停止」才結束並上傳（記憶體滿時會自動結束，約 2 分鐘）
#define REC_FPS         8          // 錄影每秒張數（480x272 MJPEG + 8kHz μ-law 聲音，存成 AVI，約 6MB/分鐘）
#define REC_JPEG_QUALITY 40        // 錄影畫質（硬體編碼 1~100，數字越大越清楚、檔案越大）

// ------------------------------------------------------- 照片上傳 Google 雲端硬碟 ----
// 部署 tools/GoogleDrive_Upload.gs 後，把「網頁應用程式網址」貼到這裡；留空 = 不上傳
#define GDRIVE_SCRIPT_URL ""
#define GDRIVE_TOKEN      "CHANGE_ME_TOKEN"   // 必須和 .gs 裡的 TOKEN 相同
#define PHOTO_SHOW_MS     3000     // 拍照後全螢幕顯示照片的時間

// ------------------------------------------------------------ 唱歌（YouTube 歌單）----
// 說「唱首歌」會從這個播放清單隨機選一首，顯示封面、歌名與 QR 碼（手機掃描在 YouTube 播放）
#define YT_PLAYLIST_URL   "https://www.youtube.com/playlist?list=PLMiXt5EIkXI0"
#define SONG_COMPOSER     "吳玉柱"   // 詞曲創作者（版權所有）
#define SONG_SHOW_SEC     180        // 唱歌畫面停留秒數（按鍵可提早回到聊天）

// ------------------------------------------------------------ 病房助理工作站（QR 碼）----
// 說到「長照」「照服員」「居服員」「居家督導」等關鍵字時，顯示工作站 QR 碼讓手機掃描查詢
#define PORTAL_NAME       "病房助理工作站"
#define PORTAL_URL        "https://achir1015.github.io/ward-assistant-portal/"
#define PORTAL_SHOW_SEC   120        // QR 碼畫面停留秒數（按鍵或說別的話會提早離開）

// ------------------------------------------------------------ 聲控（免按鍵）----
#define VOICE_ACTIVATION  1        // 1 = 直接說話就會開始聆聽；0 = 只用按鍵
#define VAD_MIN_RMS       250      // 觸發錄音的最低音量（環境吵、常誤觸就調高；喊很大聲才有反應就調低）
#define VAD_RATIO         3.0      // 音量需高於背景噪音幾倍才觸發
#define VAD_SILENCE_MS    900      // 停頓多久視為說完
#define VAD_MIN_SPEECH_MS 400      // 少於這個長度的聲音視為雜音
#define SLEEPY_AFTER_SEC  90       // 多久沒互動表情變想睡

// ================================================================ 硬體 ====
//  Guition JC1060P470C：ESP32-P4 + ESP32-C6（Wi-Fi 6）、7 吋 1024x600 觸控螢幕、
//  ES8311 喇叭/麥克風、OV02C10 200 萬畫素鏡頭。腳位固定在 p4_hw.h（已實機驗證，不用改）
//  觸控螢幕 = 原本的按鍵：點一下看說明／下一頁，按住說話；板上 BOOT 鍵也可以用
// ---------------------------------------------------------------------------
#define LCD_BRIGHTNESS   70        // 螢幕亮度 10~100
#define SPEAK_BRIGHTNESS 40        // 說話時暫時調暗（USB 供電不足時避免掉電重開機；接 5V/2A 充電器可改 100）
#define BROWNOUT_LEVEL   4         // 掉電偵測門檻 0~7（7 最敏感）；還是常重開可再調低
#define MIC_PGA_GAIN     8         // ES8311 麥克風類比增益 0~10（x3dB）；聲音太小調高、破音調低
#define MIC_SOFT_GAIN    2         // 錄音數位放大倍數 1~4
#define CAM_SATURATION   190       // 鏡頭色彩飽和度（128 = 原始；畫面偏灰就調高）

// ---------------------------------------------------------------- 音訊 ----
#define MIC_SAMPLE_RATE  16000
#define MAX_RECORD_SEC   12
#define TTS_SAMPLE_RATE  24000     // OpenAI TTS pcm 格式固定 24kHz / 16bit / mono
#define DEFAULT_VOLUME   60        // 0~100
#define SPK_LIMIT        16000     // 喇叭輸出限幅（最大 32767）；接電腦 USB 供電一出聲就重開機時調低
