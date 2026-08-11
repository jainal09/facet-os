#!/usr/bin/env python3
"""
Render Facet's UI sounds.

These are authored, not sampled. The reason is the hardware: the cube's speaker
is a cheap micro-driver, measured on the actual device as silent below ~500 Hz,
faint around 500, and usably present from there up to 8 kHz with no resonant
buzz anywhere. Sounds therefore have to be *placed* in that band deliberately —
fundamentals at 1.2-2.5 kHz where it is strong, partials reaching ~6 kHz for the
shimmer that makes a small driver read as expensive instead of tinny, and a hard
high-pass at 500 Hz because energy the driver cannot move does not simply vanish,
it comes back as distortion.

Shopping for samples cannot hit a target that specific, and authoring them keeps
the repo free of licence questions. Everything here is CC0.

Output: 16-bit mono 22050 Hz, matching the BSP's I2S default so the ESP32 never
has to reconfigure the codec.

    python3 render.py

Stdlib only, no numpy — a few seconds of audio is nothing to compute.
"""

import math
import struct
import wave
import random

RATE = 22050
HPF_HZ = 500.0          # measured floor of the driver

random.seed(7)          # reproducible noise for the tick


# ---------------------------------------------------------------- primitives

def silence(dur):
    return [0.0] * int(RATE * dur)


def add(buf, start, sig):
    """Mix sig into buf at start seconds, growing buf as needed."""
    i0 = int(RATE * start)
    need = i0 + len(sig) - len(buf)
    if need > 0:
        buf.extend([0.0] * need)
    for i, v in enumerate(sig):
        buf[i0 + i] += v
    return buf


def partial(freq, dur, amp=1.0, attack=0.006, decay=None, detune=0.0):
    """One exponentially decaying sine with a soft attack.

    The attack matters more than anything else here: a zero-length attack is a
    click, and a click on a cheap driver is the single most 'cheap' sound it can
    make. 6 ms is inaudible as a delay but removes the edge entirely.
    """
    n = int(RATE * dur)
    if decay is None:
        decay = dur / 3.0
    out = [0.0] * n
    na = max(1, int(RATE * attack))
    w = 2.0 * math.pi * freq / RATE
    w2 = 2.0 * math.pi * (freq + detune) / RATE
    for i in range(n):
        env = math.exp(-i / (RATE * decay))
        if i < na:
            # raised-cosine attack, smoother than linear
            env *= 0.5 - 0.5 * math.cos(math.pi * i / na)
        s = math.sin(w * i)
        if detune:
            s = 0.5 * s + 0.5 * math.sin(w2 * i)
        out[i] = amp * env * s
    return out


def tone(freq, dur, amp=1.0, attack=0.012, release=0.05, harm2=0.0):
    """A held tone with soft edges — for the little melodic cues."""
    n = int(RATE * dur)
    out = [0.0] * n
    na = max(1, int(RATE * attack))
    nr = max(1, int(RATE * release))
    w = 2.0 * math.pi * freq / RATE
    for i in range(n):
        env = 1.0
        if i < na:
            env = 0.5 - 0.5 * math.cos(math.pi * i / na)
        elif i > n - nr:
            k = (n - i) / nr
            env = 0.5 - 0.5 * math.cos(math.pi * k)
        s = math.sin(w * i)
        if harm2:
            s += harm2 * math.sin(2.0 * w * i)
        out[i] = amp * env * s
    return out


def noise_burst(dur, amp=1.0, attack=0.001, decay=0.012):
    n = int(RATE * dur)
    out = [0.0] * n
    na = max(1, int(RATE * attack))
    for i in range(n):
        env = math.exp(-i / (RATE * decay))
        if i < na:
            env *= i / na
        out[i] = amp * env * (random.random() * 2.0 - 1.0)
    return out


# ---------------------------------------------------------------- filters

def biquad(buf, b0, b1, b2, a1, a2):
    x1 = x2 = y1 = y2 = 0.0
    out = [0.0] * len(buf)
    for i, x0 in enumerate(buf):
        y0 = b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        out[i] = y0
        x2, x1 = x1, x0
        y2, y1 = y1, y0
    return out


def highpass(buf, hz=HPF_HZ, q=0.707):
    """2nd-order Butterworth high-pass (RBJ cookbook)."""
    w0 = 2.0 * math.pi * hz / RATE
    alpha = math.sin(w0) / (2.0 * q)
    cw = math.cos(w0)
    a0 = 1.0 + alpha
    return biquad(buf,
                  ((1.0 + cw) / 2.0) / a0,
                  (-(1.0 + cw)) / a0,
                  ((1.0 + cw) / 2.0) / a0,
                  (-2.0 * cw) / a0,
                  (1.0 - alpha) / a0)


def bandpass(buf, hz, q=1.2):
    w0 = 2.0 * math.pi * hz / RATE
    alpha = math.sin(w0) / (2.0 * q)
    cw = math.cos(w0)
    a0 = 1.0 + alpha
    return biquad(buf, alpha / a0, 0.0, -alpha / a0,
                  (-2.0 * cw) / a0, (1.0 - alpha) / a0)


def normalise(buf, peak_dbfs):
    peak = max((abs(v) for v in buf), default=0.0)
    if peak == 0.0:
        return buf
    target = 10.0 ** (peak_dbfs / 20.0)
    g = target / peak
    # tanh knee rather than a hard ceiling: this driver distorts when pushed,
    # and soft saturation is far kinder to it than clipping
    return [math.tanh(v * g * 1.15) / math.tanh(1.15) * target for v in buf]


def write_wav(path, buf, peak_dbfs):
    buf = highpass(buf)
    buf = normalise(buf, peak_dbfs)
    frames = b"".join(struct.pack("<h", max(-32767, min(32767, int(v * 32767))))
                      for v in buf)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(frames)
    print("%-12s %5.0f ms  %6d bytes  peak %+.0f dBFS"
          % (path, 1000.0 * len(buf) / RATE, len(frames), peak_dbfs))


# ---------------------------------------------------------------- space

def comb(buf, delay, feedback, damp=0.25):
    out = [0.0] * len(buf)
    line = [0.0] * delay
    idx = 0
    store = 0.0
    for i in range(len(buf)):
        y = line[idx]
        store = y * (1.0 - damp) + store * damp
        line[idx] = buf[i] + store * feedback
        idx = (idx + 1) % delay
        out[i] = y
    return out


def allpass(buf, delay, feedback=0.5):
    out = [0.0] * len(buf)
    line = [0.0] * delay
    idx = 0
    for i in range(len(buf)):
        y = line[idx]
        line[idx] = buf[i] + y * feedback
        idx = (idx + 1) % delay
        out[i] = y - buf[i]
    return out


def reverb(buf, mix=0.30, feedback=0.76, tail=0.45):
    """A small Schroeder reverb.

    This is the single change that stops these sounding like beeps. A dry decaying
    tone is a beep no matter how nicely you shape it; the same tone with a short
    room around it reads as a designed sound. Freeverb's comb/allpass delays,
    halved for 22050 Hz.
    """
    buf = buf + [0.0] * int(RATE * tail)
    wet = [0.0] * len(buf)
    for d in (558, 594, 638, 678):
        c = comb(buf, d, feedback)
        for i in range(len(buf)):
            wet[i] += c[i] * 0.25
    for d in (112, 278, 220, 170):
        wet = allpass(wet, d)
    return [buf[i] * (1.0 - mix) + wet[i] * mix for i in range(len(buf))]


def struck(f0, dur, amp=1.0, ratios=None, amps=None, decays=None, attack=0.003):
    """A struck/plucked note: a stack of partials, each decaying faster the higher
    it sits. That decay gradient is what your ear reads as 'something was hit'
    rather than 'an oscillator was switched on', which is the whole difference
    between a marimba and a beep."""
    if ratios is None:
        ratios = [1.000, 2.010, 3.010, 4.220, 5.430]
        amps   = [1.000, 0.340, 0.170, 0.075, 0.035]
        decays = [1.000, 0.620, 0.400, 0.260, 0.170]
    out = silence(dur)
    for r, a, d in zip(ratios, amps, decays):
        f = f0 * r
        if f > 8200.0:
            continue
        add(out, 0.0, partial(f, dur, amp * a, attack=attack,
                              decay=d * dur * 0.42,
                              detune=1.8 if r < 3 else 0.0))
    return out


def air(dur, amp, hz, q=1.1, decay=0.010):
    """A whisper of filtered noise on the attack. Real objects make a bit of
    broadband noise when struck; without it a partial stack still sounds
    synthetic."""
    return bandpass(noise_burst(dur, amp, 0.0004, decay), hz, q)


# ---------------------------------------------------------------- the sounds
#
# Every clip is padded with a little silence at both ends. The head keeps the
# power amp's turn-on from landing on the first sample, and the tail gives the
# I2S DMA something harmless to be playing when the clip logically ends — without
# it the ring was still draining when the codec closed, which truncated the tail
# and put an audible crack on the end of the bell.

HEAD = 0.020
TAIL = 0.130


def pad(buf):
    return silence(HEAD) + buf + silence(TAIL)


def tick():
    """Rotation detent. Not a blip — a short wooden knock with a resonant body,
    so it reads as a mechanical click you feel rather than a tone you hear."""
    buf = silence(0.075)
    add(buf, 0.000, air(0.020, 1.00, 2200, 0.9, 0.0035))     # the contact
    add(buf, 0.000, air(0.030, 0.45, 3600, 1.6, 0.0060))     # brightness
    add(buf, 0.001, struck(1650, 0.070, 0.55,
                           ratios=[1.0, 2.76, 5.40],
                           amps=[1.0, 0.40, 0.14],
                           decays=[0.42, 0.26, 0.16], attack=0.0010))
    return pad(reverb(buf, mix=0.16, feedback=0.62, tail=0.12)),  -8.0


def start():
    """Session begins. Two struck glass notes rising, with air and a room."""
    buf = silence(0.34)
    add(buf, 0.000, air(0.020, 0.30, 3000, 1.4, 0.004))
    add(buf, 0.000, struck(1174.7, 0.26, 0.80))              # D6
    add(buf, 0.095, air(0.020, 0.26, 3600, 1.4, 0.004))
    add(buf, 0.095, struck(1568.0, 0.30, 0.95))              # G6
    return pad(reverb(buf, mix=0.32, tail=0.42)), -13.0


def pause():
    """Laid flat. The same gesture falling, a touch darker and slower."""
    buf = silence(0.36)
    add(buf, 0.000, air(0.020, 0.24, 2800, 1.4, 0.004))
    add(buf, 0.000, struck(1568.0, 0.26, 0.75))
    add(buf, 0.105, struck(1174.7, 0.34, 0.90))
    return pad(reverb(buf, mix=0.34, tail=0.45)), -14.0


def resume():
    """Stood back up. One quick struck note plus its fifth — an acknowledgement,
    not an announcement."""
    buf = silence(0.26)
    add(buf, 0.000, air(0.016, 0.22, 3200, 1.4, 0.003))
    add(buf, 0.000, struck(1318.5, 0.20, 0.70))              # E6
    add(buf, 0.055, struck(1760.0, 0.24, 0.85))              # A6
    return pad(reverb(buf, mix=0.30, tail=0.38)), -15.0


def done():
    """Completion. A struck bell — inharmonic partials, each decaying faster the
    higher it sits. Two strikes, the second a fifth up at half level, then a real
    tail so it resolves instead of stopping."""
    ratios = [1.000, 2.008, 2.97, 4.13, 5.42, 6.79]
    amps   = [1.000, 0.520, 0.330, 0.180, 0.105, 0.055]
    decays = [0.520, 0.360, 0.250, 0.170, 0.110, 0.075]

    def strike(f0, amp, t_len):
        out = silence(t_len)
        add(out, 0.0, air(0.018, 0.20 * amp, 4200, 1.5, 0.005))
        for r, a, d in zip(ratios, amps, decays):
            f = f0 * r
            if f > 8200.0:
                continue
            add(out, 0.0, partial(f, t_len, amp * a, attack=0.008,
                                  decay=d, detune=2.5 if r < 3 else 0.0))
        return out

    buf = silence(1.35)
    add(buf, 0.000, strike(1244.5, 1.00, 1.05))              # D#6
    add(buf, 0.420, strike(1244.5 * 1.5, 0.50, 0.90))        # a fifth above
    return pad(reverb(buf, mix=0.34, feedback=0.80, tail=0.60)), -11.0


if __name__ == "__main__":
    for name, fn in (("tick", tick), ("start", start), ("pause", pause),
                     ("resume", resume), ("done", done)):
        buf, peak = fn()
        write_wav("%s.wav" % name, buf, peak)
