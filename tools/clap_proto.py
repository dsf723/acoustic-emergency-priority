"""拍手检测原型（与固件将采用的子帧算法相同），在标注录音上验证。用法：python clap_proto.py rec.npz [SNR门限] [不应期ms]"""
import sys
import numpy as np

FS = 48828.125
CHANS = [0, 1, 2, 3, 4, 5, 7]


def levels(path):
    d = np.load(path)
    x = d["samples"][:, 1:, :][:, :, CHANS].astype(float) / 2 ** 23
    y = x[:, 1:, :] - 0.9 * x[:, :-1, :]
    sub = np.zeros((x.shape[0], 32))
    for j in range(32):
        seg = y[:, max(j * 32 - 1, 0):j * 32 + 31, :]
        sub[:, j] = np.sum(seg ** 2, axis=(1, 2)) / len(CHANS)
    subs = sub.reshape(-1)
    W = np.convolve(subs, np.ones(4), mode="full")[:len(subs)] / 128
    return 10 * np.log10(W + 1e-20), W


def detect(L, W, snr_db=18.0, refr_ms=150.0, rise_db=10.0):
    dt = 32 / FS
    floor = np.percentile(L, 20)
    out, j = [], 40
    while j < len(L) - 200:
        rise = L[j] - np.max(L[j - 30:j - 4])
        pk = j + int(np.argmax(L[j:j + 30]))              # 起点后 20ms 内的峰值
        if rise >= rise_db and L[pk] - floor >= snr_db:
            lp = L[pk]
            dec = lambda ms: lp - np.max(L[pk + int(ms / 1000 / dt): pk + int((ms + 10) / 1000 / dt)])
            crest = lp - 10 * np.log10(np.mean(W[max(pk - 75, 0):pk + 75]) + 1e-20)
            out.append((pk * dt, lp, lp - floor, rise, dec(20), dec(50), dec(100), crest))
            j = pk + int(refr_ms / 1000 / dt)
        else:
            j += 1
    return out, floor


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    L, W = levels(sys.argv[1])
    snr = float(sys.argv[2]) if len(sys.argv) > 2 else 18.0
    refr = float(sys.argv[3]) if len(sys.argv) > 3 else 150.0
    c, floor = detect(L, W, snr, refr)
    print(f"背景 {floor:.1f} dBFS；候选 {len(c)} 个")
    print("  时间    峰值   SNR   上升  衰减20ms 50ms 100ms 峰均比")
    for e in c:
        print(f"  {e[0]:6.2f} {e[1]:6.1f} {e[2]:5.1f} {e[3]:5.1f}  {e[4]:5.1f} {e[5]:5.1f} {e[6]:5.1f}  {e[7]:5.1f}")
