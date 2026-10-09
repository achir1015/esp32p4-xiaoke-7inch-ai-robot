# 經 USB 把 ESP-Hosted 韌體傳給 P4（需先燒 C6_Update.ino），由 P4 寫入 ESP32-C6
# 用法：python c6_update.py COM12 firmware\esp32c6-v2.12.13.bin
import serial, sys, time, os

port, path = sys.argv[1], sys.argv[2]
data = open(path, 'rb').read()
s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 10
s.dtr = s.rts = False
s.open()
time.sleep(0.5)
s.reset_input_buffer()
s.write(f'OTA {len(data)}\n'.encode())
line = s.readline().decode(errors='replace').strip()
if line != 'GO':
    sys.exit(f'P4 沒有回應 GO：{line!r}')
sent = 0
t0 = time.time()
while sent < len(data):
    chunk = data[sent:sent + 2048]
    s.write(chunk)
    ack = s.readline().decode(errors='replace').strip()
    if ack != 'K':
        sys.exit(f'\n失敗於 {sent} bytes：{ack!r}')
    sent += len(chunk)
    if sent % (64 * 1024) < 2048 or sent == len(data):
        print(f'\r{sent * 100 // len(data)}%  {sent}/{len(data)}', end='', flush=True)
print(f'\n傳送完成 {time.time() - t0:.0f} 秒')
for _ in range(5):
    line = s.readline().decode(errors='replace').strip()
    if line:
        print(line)
    if line in ('SUCCESS',) or line.startswith('ERR'):
        break
