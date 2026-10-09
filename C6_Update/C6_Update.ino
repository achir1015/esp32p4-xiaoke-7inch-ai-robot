// C6_Update：透過 USB 把 ESP-Hosted 新韌體傳給 P4，再由 P4 寫進 ESP32-C6（不需另外接線）
// 用法：電腦端執行 tools/c6_update.py <韌體.bin>
#include <Arduino.h>
#include "WiFi.h"
#include "esp32-hal-hosted.h"

void setup() {
  Serial.begin(115200);
  Serial.setRxBufferSize(8192);
  delay(1500);
  WiFi.STA.begin();
  delay(500);
  uint32_t a, b, c, x, y, z;
  hostedGetHostVersion(&a, &b, &c);
  hostedGetSlaveVersion(&x, &y, &z);
  Serial.printf("HOST %lu.%lu.%lu SLAVE %lu.%lu.%lu TARGET %s\n", a, b, c, x, y, z, hostedGetSlaveTargetName());
  Serial.printf("HAS_UPDATE %d\nURL %s\n", hostedHasUpdate(), hostedGetUpdateURL());
  Serial.println("READY");
}

static uint8_t buf[2048];
void loop() {
  if (!Serial.available()) { delay(10); return; }
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (!cmd.startsWith("OTA ")) return;
  long total = cmd.substring(4).toInt();
  if (total <= 0 || !hostedBeginUpdate()) { Serial.println("ERR begin"); return; }
  Serial.println("GO");
  long left = total;
  while (left > 0) {
    size_t want = min((long)sizeof(buf), left), got = 0;
    uint32_t t0 = millis();
    while (got < want && millis() - t0 < 5000) {
      int n = Serial.read(buf + got, want - got);
      if (n > 0) got += n; else delay(1);
    }
    if (got != want) { Serial.println("ERR timeout"); return; }
    if (!hostedWriteUpdate(buf, got)) { Serial.println("ERR write"); return; }
    left -= got;
    Serial.println("K");
  }
  if (!hostedEndUpdate()) { Serial.println("ERR end"); return; }
  if (!hostedActivateUpdate()) { Serial.println("ERR activate"); return; }
  Serial.println("SUCCESS");
  delay(1000);
  ESP.restart();
}
