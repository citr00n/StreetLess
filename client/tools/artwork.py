#!/usr/bin/env python3
"""Generate Streetless's original HOME icon, banner and short chime."""
import math
import os
import struct
import wave
import zlib

OUT = 'build/artwork'
os.makedirs(OUT, exist_ok=True)

FONT = {
    'S': ('111','100','111','001','111'), 'T': ('111','010','010','010','010'),
    'R': ('110','101','110','101','101'), 'E': ('111','100','110','100','111'),
    'L': ('100','100','100','100','111'),
}

def image(width, height):
    return bytearray((240, 245, 242) * width * height)

def rect(pixels, width, height, x, y, w, h, color):
    for yy in range(max(y, 0), min(y + h, height)):
        for xx in range(max(x, 0), min(x + w, width)):
            i = (yy * width + xx) * 3
            pixels[i:i+3] = bytes(color)

def emblem(p, w, h, x, y, scale):
    green, light, white = (0, 108, 79), (180, 220, 202), (255, 255, 255)
    rect(p, w, h, x, y, 20*scale, 20*scale, green)
    # Two handheld screens exchange a pass in the center.
    rect(p, w, h, x+2*scale, y+4*scale, 6*scale, 12*scale, white)
    rect(p, w, h, x+12*scale, y+4*scale, 6*scale, 12*scale, white)
    rect(p, w, h, x+3*scale, y+5*scale, 4*scale, 6*scale, light)
    rect(p, w, h, x+13*scale, y+5*scale, 4*scale, 6*scale, light)
    rect(p, w, h, x+7*scale, y+9*scale, 6*scale, 2*scale, green)
    rect(p, w, h, x+9*scale, y+8*scale, 2*scale, 4*scale, white)

def png(path, w, h, pixels):
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data))
    raw = b''.join(b'\0' + pixels[y*w*3:(y+1)*w*3] for y in range(h))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))

p = image(48, 48)
emblem(p, 48, 48, 4, 4, 2)
png(OUT + '/icon.png', 48, 48, p)
p = image(256, 128)
emblem(p, 256, 128, 108, 12, 2)
text = 'STREETLESS'
scale = 4
x = (256 - (len(text) * 4 - 1) * scale) // 2
for char in text:
    for yy, row in enumerate(FONT[char]):
        for xx, pixel in enumerate(row):
            if pixel == '1':
                rect(p, 256, 128, x + xx*scale, 72 + yy*scale, scale, scale, (22, 40, 34))
    x += 4*scale
rect(p, 256, 128, 48, 104, 160, 3, (0, 108, 79))
png(OUT + '/banner.png', 256, 128, p)
# A short two-note original chime, with smooth attack and decay.
rate = 22050
samples = bytearray()
for i in range(rate // 2):
    t = i / rate
    frequency = 659.25 if t < .25 else 783.99
    local = t if t < .25 else t - .25
    envelope = min(1.0, local * 40) * max(0.0, 1 - local * 4)
    value = int(4500 * envelope * math.sin(2 * math.pi * frequency * local))
    samples.extend(struct.pack('<h', value))
with wave.open(OUT + '/banner.wav', 'wb') as f:
    f.setnchannels(1)
    f.setsampwidth(2)
    f.setframerate(rate)
    f.writeframes(samples)
