#!/usr/bin/env python3
"""Rebuild device screenshots from a serial capture.

The firmware's CFG_PERF_SCROLL_SELFTEST harness dumps each screen as:

    SNAP_BEGIN <name> <w> <h> <cf> <stride>
    S:<base64...>          (repeated)
    SNAP_END <name>

Feed this script the RAW capture file (every line, unfiltered) and it writes
one 24-bit BMP per snapshot. A frame named "<x>_top" (the chord screenshot's
lv_layer_top pass, ARGB8888) is alpha-composited onto its base "<x>" instead
of written separately — that is how the bezel lobes end up in the picture. Pure stdlib; convert to PNG afterwards with
`sips -s format png *.bmp` on macOS.

Usage: python3 tools/snap_rx.py capture_raw.log [outdir]
"""
import base64
import struct
import sys
import os


def rgb565_to_bmp(pixels, w, h, stride, path):
    # BMP rows are bottom-up and padded to 4 bytes; we emit 24-bit BGR.
    row_out = (w * 3 + 3) & ~3
    img_size = row_out * h
    with open(path, "wb") as f:
        f.write(struct.pack("<2sIHHI", b"BM", 54 + img_size, 0, 0, 54))
        f.write(struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, img_size,
                            2835, 2835, 0, 0))
        pad = b"\x00" * (row_out - w * 3)
        for y in range(h - 1, -1, -1):
            row = bytearray()
            base = y * stride
            for x in range(w):
                px = pixels[base + 2 * x] | (pixels[base + 2 * x + 1] << 8)
                r = (px >> 11) & 0x1F
                g = (px >> 5) & 0x3F
                b = px & 0x1F
                row += bytes(((b * 255) // 31, (g * 255) // 63, (r * 255) // 31))
            f.write(row + pad)


def composite_bmp(base565, base_stride, top8888, top_stride, w, h, path):
    """RGB565 base + LVGL ARGB8888 top layer (bytes B,G,R,A per pixel)."""
    row_out = (w * 3 + 3) & ~3
    img_size = row_out * h
    with open(path, "wb") as f:
        f.write(struct.pack("<2sIHHI", b"BM", 54 + img_size, 0, 0, 54))
        f.write(struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, img_size,
                            2835, 2835, 0, 0))
        pad = b"\x00" * (row_out - w * 3)
        for y in range(h - 1, -1, -1):
            row = bytearray()
            b5 = y * base_stride
            t8 = y * top_stride
            for x in range(w):
                px = base565[b5 + 2 * x] | (base565[b5 + 2 * x + 1] << 8)
                r = ((px >> 11) & 0x1F) * 255 // 31
                g = ((px >> 5) & 0x3F) * 255 // 63
                b = (px & 0x1F) * 255 // 31
                o = t8 + 4 * x
                tb, tg, tr, ta = top8888[o], top8888[o + 1], top8888[o + 2], top8888[o + 3]
                if ta:
                    r = (tr * ta + r * (255 - ta)) // 255
                    g = (tg * ta + g * (255 - ta)) // 255
                    b = (tb * ta + b * (255 - ta)) // 255
                row += bytes((b, g, r))
            f.write(row + pad)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    outdir = sys.argv[2] if len(sys.argv) > 2 else "."
    os.makedirs(outdir, exist_ok=True)
    name = None
    meta = None
    chunks = []
    n_done = 0
    frames = {}
    with open(sys.argv[1], errors="replace") as f:
        for line in f:
            line = line.strip()
            # tools/capture.py raw files prefix a float timestamp column;
            # strip it so payload tests see the device's own line.
            parts = line.split(None, 1)
            if len(parts) == 2:
                try:
                    float(parts[0])
                    line = parts[1]
                except ValueError:
                    pass
            if "SNAP_BEGIN " in line:
                parts = line.split("SNAP_BEGIN ", 1)[1].split()
                name, meta = parts[0], tuple(int(x) for x in parts[1:5])
                chunks = []
            elif line.startswith("S:") and name:
                # "S:<seq>:<b64>" (current) or "S:<b64>" (legacy). Sequence
                # numbers let a corrupted/lost console line become one black
                # stripe instead of a lost frame.
                body = line[2:]
                seq = None
                head, sep, rest = body.partition(":")
                if sep and head.isdigit():
                    seq, body = int(head), rest
                chunks.append((seq, body))
            elif "SNAP_END " in line and name:
                w, h, cf, stride = meta
                data = bytearray()
                lost = 0
                expect = 0
                for seq, b in chunks:
                    if seq is not None and seq > expect:
                        data += b"\x00" * (384 * (seq - expect))
                        lost += seq - expect
                        expect = seq
                    try:
                        data += base64.b64decode(b, validate=True)
                    except Exception:
                        data += b"\x00" * 384
                        lost += 1
                    expect += 1
                pixels = bytes(data)
                want = stride * h
                if lost:
                    print(f"{name}: {lost} chunk(s) lost, filled black")
                if len(pixels) < want:
                    if want - len(pixels) <= 384 * 4:
                        pixels += b"\x00" * (want - len(pixels))
                        print(f"{name}: padded short tail")
                    else:
                        print(f"{name}: short data {len(pixels)}/{want}, skipped")
                        name = None
                        continue
                frames[name] = (w, h, cf, stride, pixels)
                print(f"{name}: {w}x{h} cf={cf} captured")
                name = None
    for fname, (w, h, cf, stride, pixels) in frames.items():
        if fname.endswith("_top"):
            continue
        top = frames.get(fname + "_top")
        path = os.path.join(outdir, f"{fname}.bmp")
        if top and top[2] == 16:  # LV_COLOR_FORMAT_ARGB8888
            composite_bmp(pixels, stride, top[4], top[3], w, h, path)
            print(f"{fname}: composited top layer -> {path}")
        else:
            rgb565_to_bmp(pixels, w, h, stride, path)
            print(f"{fname}: -> {path}")
        n_done += 1
    print(f"{n_done} snapshot(s) written")


if __name__ == "__main__":
    main()
