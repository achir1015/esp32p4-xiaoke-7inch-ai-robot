// 雲端歌曲清單的項目（放在獨立標頭，Arduino 自動產生函式宣告時才看得到）
#pragma once
#include <Arduino.h>
struct Mp3Song { String id, title; };
struct Btn { int x, y, w, h; };   // 觸控按鈕（邏輯座標 512x300）
// 工作站觸控選單的按鈕
static const Btn BTN_HOME = {410, 50, 96, 24};
static const Btn BTN_PORTAL_IDLE = {370, 274, 136, 24};       // 主畫面右下角
static const Btn BTN_PREV = {250, 50, 72, 24}, BTN_NEXT = {328, 50, 72, 24};
static const Btn BTN_BACK = {8, 268, 120, 26};                 // 詳細頁：返回清單
