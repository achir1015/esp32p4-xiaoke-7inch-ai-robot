# MP4 → 小柯 MV 檔（.xkv）：MJPEG 影像 + MP3 聲音交錯，ESP32-P4 邊下載邊播
# 用法：python make_xkv.py 輸入.mp4 輸出.xkv
#
# 檔案格式（little-endian）：
#   檔頭 32 bytes："XKV1" u16 寬 u16 高 u16 fps u16 保留 u32 影格數 u32 長度(ms) u32 取樣率 填 0 到 32 bytes
#   之後是 chunk：u8 類型（'A' = MP3 影格、'V' = JPEG 影格）+ u24 長度 + 資料
#   每張畫面前面先放好「到這張畫面之後 0.5 秒」為止的聲音，播放端以聲音時間為準顯示畫面
import os, sys, struct, subprocess, tempfile

W, H, FPS, SR = 640, 352, 12, 24000
src, dst = sys.argv[1], sys.argv[2]
tmp = tempfile.mkdtemp()
mp3 = os.path.join(tmp, 'a.mp3')
mjpeg = os.path.join(tmp, 'v.mjpeg')

subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', src, '-vn', '-map', '0:a:0', '-af', 'loudnorm=I=-16:TP=-1.5:LRA=11,volume=-8dB',
                '-ac', '1', '-ar', str(SR), '-c:a', 'libmp3lame', '-b:a', '80k', '-id3v2_version', '0', '-write_xing', '0', mp3], check=True)
subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', src, '-map', '0:v:0', '-an',
                '-vf', f'fps={FPS},scale={W}:{H}:force_original_aspect_ratio=decrease,pad={W}:{H}:(ow-iw)/2:(oh-ih)/2:black',
                '-c:v', 'mjpeg', '-pix_fmt', 'yuvj420p', '-q:v', '6', '-f', 'mjpeg', mjpeg], check=True)

# ---- 切 MP3 影格（MPEG-2 Layer III，24kHz：每格 576 樣本 = 24ms）----
a = open(mp3, 'rb').read()
BR = {1: [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320],     # MPEG-1
      2: [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160]}         # MPEG-2/2.5
frames, i = [], 0
while i + 4 <= len(a):
    if a[i] != 0xFF or (a[i + 1] & 0xE0) != 0xE0: i += 1; continue
    ver = (a[i + 1] >> 3) & 3; br_i = (a[i + 2] >> 4) & 15; sr_i = (a[i + 2] >> 2) & 3; pad = (a[i + 2] >> 1) & 1
    if br_i in (0, 15) or sr_i == 3: i += 1; continue
    mpeg1 = ver == 3
    rate = [[11025, 12000, 8000], None, [22050, 24000, 16000], [44100, 48000, 32000]][ver][sr_i]
    br = BR[1 if mpeg1 else 2][br_i] * 1000
    n = (144 if mpeg1 else 72) * br // rate + pad
    frames.append(a[i:i + n]); i += n
samples_per_frame = 1152 if mpeg1 else 576
aud_ms = [k * samples_per_frame * 1000 / SR for k in range(len(frames))]

# ---- 切 JPEG 影格 ----
v = open(mjpeg, 'rb').read()
jpgs, p = [], 0
while True:
    s = v.find(b'\xff\xd8', p)
    if s < 0: break
    e = v.find(b'\xff\xd9', s + 2)
    if e < 0: break
    jpgs.append(v[s:e + 2]); p = e + 2

dur_ms = int(max(len(frames) * samples_per_frame * 1000 / SR, len(jpgs) * 1000 / FPS))
with open(dst, 'wb') as f:
    f.write(struct.pack('<4sHHHHIII', b'XKV1', W, H, FPS, 0, len(jpgs), dur_ms, SR).ljust(32, b'\0'))
    ai = 0
    def chunk(t, d):
        f.write(bytes([ord(t)]) + struct.pack('<I', len(d))[:3] + d)
    for k, j in enumerate(jpgs):
        t_end = (k + 1) * 1000 / FPS + 500
        while ai < len(frames) and aud_ms[ai] < t_end:
            chunk('A', frames[ai]); ai += 1
        chunk('V', j)
    while ai < len(frames):
        chunk('A', frames[ai]); ai += 1
print(f'{os.path.basename(dst)}: {len(jpgs)} 張畫面、{len(frames)} 段聲音、{dur_ms / 1000:.0f} 秒、{os.path.getsize(dst) / 1e6:.1f} MB')
