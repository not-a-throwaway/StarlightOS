#!/usr/bin/env python3
"""Summarise a captured WAV: dominant frequency per window, runs of tone/silence, L/R balance."""
import sys, wave, numpy as np
def load(path):
    w = wave.open(path, 'rb'); n = w.getnframes(); ch = w.getnchannels(); rate = w.getframerate(); sw = w.getsampwidth()
    raw = w.readframes(n); a = np.frombuffer(raw, dtype='<i2').astype(np.float32) / 32768.0
    return a.reshape(-1, ch), rate
def windows(a, rate, win=0.1):
    n = int(rate * win); out = []
    for i in range(0, len(a) - n, n):
        seg = a[i:i+n]; mono = seg.mean(axis=1) if seg.ndim > 1 else seg
        rms = float(np.sqrt((mono**2).mean()))
        l = float(np.sqrt((seg[:,0]**2).mean())); r = float(np.sqrt((seg[:,-1]**2).mean()))
        if rms < 0.004: out.append((i/rate, 0.0, rms, l, r)); continue
        sp = np.abs(np.fft.rfft(mono * np.hanning(len(mono)))); f = np.fft.rfftfreq(len(mono), 1/rate)
        out.append((i/rate, float(f[sp.argmax()]), rms, l, r))
    return out
def runs(ws):
    res = []; cur = None
    for t, f, rms, l, r in ws:
        key = 0 if f == 0 else int(round(f / 20.0) * 20)
        if cur and abs(cur[1] - key) <= 20: cur[2] = t + 0.1; cur[3] = max(cur[3], rms); cur[4] += l; cur[5] += r; cur[6] += 1
        else:
            if cur: res.append(cur)
            cur = [t, key, t + 0.1, rms, l, r, 1]
    if cur: res.append(cur)
    return res
if __name__ == '__main__':
    a, rate = load(sys.argv[1]); print('rate', rate, 'channels', a.shape[1], 'duration %.1fs' % (len(a)/rate), 'peak %.3f' % np.abs(a).max())
    for t0, f, t1, rms, l, r, n in runs(windows(a, rate)):
        if f == 0 and (t1 - t0) < 0.35: continue
        print('%6.1f-%6.1fs  %s  rms %.3f  L/R %.2f' % (t0, t1, ('silence' if f == 0 else '%5d Hz' % f), rms, (l/(r+1e-9)) if f else 1.0))
