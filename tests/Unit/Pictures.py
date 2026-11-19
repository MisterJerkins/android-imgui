"""Writes pictures.h: the small PNG and JPEG files the unit tests decode.

Run it again only when the test pictures change:  python tests/unit/pictures.py
"""
import io
import os

from PIL import Image


def png_pixel(x, y):
    return (x * 16, y * 30, 200, 255 if x < 8 else 90)


def encoded(picture, kind, **options):
    data = io.BytesIO()
    picture.save(data, kind, **options)
    return data.getvalue()


def array(name, data):
    rows = [', '.join(f'0x{byte:02x}' for byte in data[start:start + 20]) for start in range(0, len(data), 20)]
    return f'inline constexpr unsigned char {name}[] = {{\n    ' + ',\n    '.join(rows) + ',\n};\n'


png = Image.new('RGBA', (16, 8))
png.putdata([png_pixel(x, y) for y in range(8) for x in range(16)])
jpeg = Image.new('RGB', (16, 8), (200, 120, 40))

header = '#pragma once\n\n// Made by pictures.py; see there for what the pictures hold.\n\n'
header += array('PICTURE_PNG', encoded(png, 'PNG')) + '\n' + array('PICTURE_JPEG', encoded(jpeg, 'JPEG', quality=95))
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pictures.h'), 'w', newline='\n') as output:
    output.write(header)
