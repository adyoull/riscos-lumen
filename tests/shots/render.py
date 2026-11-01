#!/usr/bin/env python3
"""Turns shot's record (ops.txt) into PNG screenshots.

   Everything inside Lumen's window and menus is drawn from what Lumen
   itself asked for: its rectangles, triangles, text, sprites (the real
   thumbnails it made) and menu blocks. What the Wimp would draw on a real
   machine (window borders and tools, menu frames, the icon bar, the
   backdrop) is an approximation of RISC OS 5's default look, and the
   desktop font is stood in for by DejaVu Sans.

   render.py OUT_DIR APP_ICON.png"""
import os, sys
from PIL import Image, ImageDraw, ImageFont

W, H = 3200, 1800                      # OS units; the picture is half that in pixels
FONT = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', 26)
GREY = ['#FFFFFF', '#DDDDDD', '#BBBBBB', '#999999', '#777777', '#555555', '#333333', '#000000']
CREAM = '#EEEEBB'
BACKDROP = '#6E8098'
TOOL = 44                              # tool icon size, OS units
ITEM_H = 44


def rgb(v):
    v = int(v, 16)
    return ((v >> 8) & 255, (v >> 16) & 255, (v >> 24) & 255)


def fy(y):                             # OS y (up) to canvas y (down)
    return H - y


def bevel(d, x0, y0, x1, y1, face=GREY[1]):
    """A RISC OS 5 style 3D tool icon, in canvas coordinates."""
    d.rectangle([x0, y0, x1, y1], fill=face, outline=GREY[7], width=2)
    d.line([x0 + 2, y1 - 3, x0 + 2, y0 + 2, x1 - 3, y0 + 2], fill='#FFFFFF', width=3)
    d.line([x0 + 3, y1 - 3, x1 - 3, y1 - 3, x1 - 3, y0 + 3], fill=GREY[3], width=3)


def glyph(d, kind, x0, y0):
    c = (x0 + TOOL // 2, y0 + TOOL // 2)
    k = GREY[7]
    if kind == 'back':
        d.rectangle([c[0] - 13, c[1] - 11, c[0] + 3, c[1] + 3], outline=k, width=2)
        d.rectangle([c[0] - 4, c[1] - 3, c[0] + 12, c[1] + 11], fill=GREY[1], outline=k, width=2)
    elif kind == 'close':
        d.line([c[0] - 10, c[1] - 10, c[0] + 10, c[1] + 10], fill=k, width=4)
        d.line([c[0] + 10, c[1] - 10, c[0] - 10, c[1] + 10], fill=k, width=4)
    elif kind == 'toggle':
        d.rectangle([c[0] - 12, c[1] - 12, c[0] + 12, c[1] + 12], outline=k, width=2)
        d.rectangle([c[0] - 12, c[1] - 12, c[0], c[1]], fill=k)
    elif kind == 'up':
        d.polygon([(c[0], c[1] - 9), (c[0] - 11, c[1] + 7), (c[0] + 11, c[1] + 7)], fill=k)
    elif kind == 'down':
        d.polygon([(c[0], c[1] + 9), (c[0] - 11, c[1] - 7), (c[0] + 11, c[1] - 7)], fill=k)
    elif kind == 'size':
        d.line([c[0] - 10, c[1] + 10, c[0] + 10, c[1] - 10], fill=k, width=3)
        d.line([c[0], c[1] + 10, c[0] + 10, c[1]], fill=k, width=3)


def text_c(d, cx, top, h, s, fill):
    w = FONT.getlength(s)
    d.text((cx - w / 2, top + h / 2 + 9), s, font=FONT, fill=fill, anchor='ls')


def window_chrome(d, win, title, extent, focused, vscroll=True):
    x0, y0, x1, y1, sx, sy = win
    X0, X1 = x0, x1
    T0, T1 = fy(y1) - TOOL, fy(y1)                 # title bar, canvas y
    S0 = x1 if vscroll else x1 - TOOL              # where the title bar's right icon starts
    # outer border
    d.rectangle([X0 - 2, T0 - 2, (S0 + TOOL if vscroll else x1) + 2, fy(y0) + 2], outline=GREY[7], width=2)
    # title bar
    bevel(d, X0, T0, X0 + TOOL, T1, GREY[1]); glyph(d, 'back', X0, T0)
    bevel(d, X0 + TOOL, T0, X0 + 2 * TOOL, T1, GREY[1]); glyph(d, 'close', X0 + TOOL, T0)
    d.rectangle([X0 + 2 * TOOL, T0, S0, T1], fill=CREAM if focused else GREY[2], outline=GREY[7], width=2)
    tw = FONT.getlength(title)
    mid = (X0 + 2 * TOOL + S0) / 2
    d.text((mid - tw / 2, T0 + 32), title, font=FONT, fill=GREY[7], anchor='ls')
    bevel(d, S0, T0, S0 + TOOL, T1, GREY[1]); glyph(d, 'toggle', S0, T0)
    if not vscroll:
        return
    # vertical scroll bar: arrows, well, slider, adjust-size icon
    Y_top, Y_bot = fy(y1), fy(y0)
    bevel(d, S0, Y_top, S0 + TOOL, Y_top + TOOL); glyph(d, 'up', S0, Y_top)
    bevel(d, S0, Y_bot - 2 * TOOL, S0 + TOOL, Y_bot - TOOL); glyph(d, 'down', S0, Y_bot - 2 * TOOL)
    bevel(d, S0, Y_bot - TOOL, S0 + TOOL, Y_bot); glyph(d, 'size', S0, Y_bot - TOOL)
    well0, well1 = Y_top + TOOL, Y_bot - 2 * TOOL
    d.rectangle([S0, well0, S0 + TOOL, well1], fill=GREY[3], outline=GREY[7], width=2)
    ext_h = max(1, extent[3] - extent[1])
    vis_h = y1 - y0
    frac = min(1.0, vis_h / ext_h)
    start = min(1.0, -sy / ext_h)
    span = well1 - well0 - 8
    s0 = well0 + 4 + span * start
    s1 = s0 + max(40, span * frac)
    bevel(d, S0 + 4, s0, S0 + TOOL - 4, s1, GREY[1])


def icon_bar(img, d, app_icon):
    top = fy(136)
    d.rectangle([0, top, W, H], fill=GREY[1])
    d.line([0, top, W, top], fill=GREY[7], width=2)
    # a filing system icon and a resources icon on the left
    def label(cx, s):
        w = FONT.getlength(s)
        d.text((cx - w / 2, H - 14), s, font=FONT, fill=GREY[7], anchor='ls')
    cx = 110
    d.rounded_rectangle([cx - 28, top + 18, cx + 28, top + 82], radius=6, fill='#2B4A7A', outline=GREY[7], width=2)
    d.polygon([(cx + 10, top + 18), (cx + 28, top + 36), (cx + 28, top + 18)], fill=GREY[1])
    for i in range(4):
        d.rectangle([cx - 20 + i * 11, top + 22, cx - 14 + i * 11, top + 36], fill='#E2C26A')
    label(cx, 'RISCOSpi')
    cx = 300
    d.rectangle([cx - 34, top + 30, cx + 34, top + 80], fill='#E8C46A', outline=GREY[7], width=2)
    d.rectangle([cx - 34, top + 22, cx - 4, top + 32], fill='#E8C46A', outline=GREY[7], width=2)
    label(cx, 'Apps')
    # Lumen's icon (the sprite from !Sprites, 34 x 34 pixels = 68 OS units) and a display icon
    cx = W - 330
    ic = app_icon.resize((68, 68), Image.NEAREST)
    img.paste(ic, (cx - 34, top + 16), ic)
    label(cx, 'Lumen')
    cx = W - 130
    d.rectangle([cx - 34, top + 18, cx + 34, top + 66], fill='#9CC4E4', outline=GREY[7], width=3)
    d.rectangle([cx - 10, top + 66, cx + 10, top + 76], fill=GREY[4])
    d.rectangle([cx - 24, top + 76, cx + 24, top + 82], fill=GREY[4])
    label(cx, '1600x900')


def parse(path):
    shots, cur = [], None
    lines = open(path, encoding='latin-1').read().splitlines()
    i = 0
    while i < len(lines):
        ln = lines[i]
        tag, _, rest = ln.partition(' ')
        if tag == 'SHOT':
            cur = {'name': rest, 'ops': [], 'menus': []}
            shots.append(cur)
        elif tag == 'WINDOW':
            v = [int(x) for x in rest.split()]
            cur['win'], cur['focus'] = v[:6], v[6]
            cur['vscroll'] = v[7] if len(v) > 7 else 1
        elif tag == 'TITLE':
            cur['title'] = rest
        elif tag == 'EXTENT':
            cur['extent'] = [int(x) for x in rest.split()]
        elif tag == 'MENU':
            x, y = (int(v) for v in rest.split())
            i, tree = parse_menu(lines, i + 1)
            cur['menus'].append((x, y, tree))
            continue
        else:
            cur['ops'].append(ln)
        i += 1
    return shots


def parse_menu(lines, i):
    """Returns (next line, menu) where menu = dict(title, width, items)."""
    _, depth, width, *title = lines[i].split(' ')
    depth, menu = int(depth), {'title': ' '.join(title), 'width': int(width), 'items': []}
    i += 1
    while i < len(lines) and lines[i].startswith('I '):
        p = lines[i].split(' ', 5)
        flags, shaded, has_sub, text = int(p[2]), int(p[3]), int(p[4]), p[5] if len(p) > 5 else ''
        item = {'flags': flags, 'shaded': shaded, 'sub': None, 'has_sub': has_sub, 'text': text}
        menu['items'].append(item)
        i += 1
        if has_sub and i < len(lines) and lines[i].startswith('M ') and int(lines[i].split(' ')[1]) == depth + 1:
            i, item['sub'] = parse_menu(lines, i)
        if flags & 0x80:
            break
    if i < len(lines) and lines[i] == 'ENDMENU':
        i += 1
    return i, menu


def draw_ops(img, shot):
    layer = Image.new('RGB', (W, H), GREY[1])
    d = ImageDraw.Draw(layer)
    out_dir = shot['dir']
    tfg = (0, 0, 0)
    for ln in shot['ops']:
        tag, _, rest = ln.partition(' ')
        if tag == 'RECT':
            x0, y0, x1, y1, c = rest.split()
            x0, y0, x1, y1 = int(x0), int(y0), int(x1), int(y1)
            d.rectangle([min(x0, x1), fy(max(y0, y1)), max(x0, x1), fy(min(y0, y1))], fill=rgb(c))
        elif tag == 'TRI':
            v = rest.split()
            pts = [(int(v[k]), fy(int(v[k + 1]))) for k in (0, 2, 4)]
            d.polygon(pts, fill=rgb(v[6]))
        elif tag == 'TCOL':
            tfg = rgb(rest.split()[0])
        elif tag == 'TEXT':
            x, y, s = rest.split(' ', 2)
            d.text((int(x), fy(int(y))), s, font=FONT, fill=tfg, anchor='ls')
        elif tag == 'SPRITE':
            name, w, h, x, y, dw, dh = rest.split()
            w, h, x, y, dw, dh = (int(v) for v in (w, h, x, y, dw, dh))
            raw = open(os.path.join(out_dir, name), 'rb').read()
            sp = Image.frombytes('RGB', (w, h), raw).resize((dw, dh), Image.NEAREST)
            layer.paste(sp, (x, fy(y + dh)))
    x0, y0, x1, y1 = shot['win'][:4]
    box = (x0, fy(y1), x1, fy(y0))
    img.paste(layer.crop(box), box[:2])


def draw_menu(d, x, top, menu, path, depth=0):
    """Draws a menu with its title bar's top at canvas y top; follows path."""
    w = menu['width']
    n = len(menu['items'])
    d.rectangle([x, top, x + w, top + ITEM_H], fill=GREY[2], outline=GREY[7], width=2)
    text_c(d, x + w / 2, top, ITEM_H, menu['title'], GREY[7])
    body = top + ITEM_H
    d.rectangle([x, body, x + w, body + n * ITEM_H + 8], fill='#FFFFFF', outline=GREY[7], width=2)
    open_sub = None
    for k, it in enumerate(menu['items']):
        iy = body + 4 + k * ITEM_H
        hl = depth < len(path) and path[depth] == k
        if hl:
            d.rectangle([x + 2, iy, x + w - 2, iy + ITEM_H], fill=GREY[6])
        col = '#FFFFFF' if hl else (GREY[3] if it['shaded'] else GREY[7])
        if it['flags'] & 0x01:
            d.line([x + 8, iy + 24, x + 13, iy + 31, x + 23, iy + 13], fill=col, width=4)
        if it['flags'] & 0x04:
            d.rectangle([x + 12, iy + 4, x + w - 12, iy + ITEM_H - 4], fill='#FFFFFF', outline=GREY[7], width=2)
            d.text((x + 22, iy + 31), 'Holiday picks', font=FONT, fill=GREY[7], anchor='ls')
        else:
            d.text((x + 28, iy + 31), it['text'], font=FONT, fill=col, anchor='ls')
        if it['has_sub']:
            ax = x + w - 18
            d.polygon([(ax, iy + 14), (ax, iy + 30), (ax + 10, iy + 22)], fill=col)
        if it['flags'] & 0x02 and k < n - 1:
            for dx in range(x + 8, x + w - 8, 12):
                d.line([dx, iy + ITEM_H + 1, dx + 5, iy + ITEM_H + 1], fill=GREY[4], width=2)
        if hl and it['sub']:
            open_sub = (it['sub'], iy)
    if open_sub:
        sub, iy = open_sub
        return draw_menu(d, x + w + 4, iy - ITEM_H - 4, sub, path, depth + 1) or (x + w - 30, iy + 20)
    return None


def pointer(d, x, y):
    pts = [(0, 0), (0, 34), (9, 26), (16, 41), (22, 38), (15, 24), (27, 24)]
    d.polygon([(x + a, y + b) for a, b in pts], fill='#28C3E8', outline='#0B2F6B')


def render(out_dir, app_icon):
    shots = parse(os.path.join(out_dir, 'ops.txt'))
    made = []
    for shot in shots:
        shot['dir'] = out_dir
        img = Image.new('RGB', (W, H), BACKDROP)
        d = ImageDraw.Draw(img)
        icon_bar(img, d, app_icon)
        draw_ops(img, shot)
        window_chrome(d, shot['win'], shot['title'], shot['extent'], shot['focus'], shot['vscroll'])
        for (mx, my, tree) in shot['menus']:
            # Show > Projects, with the pointer on the project
            last = draw_menu(d, mx, fy(my), tree, [0, 4, 0])
            if last:
                pointer(d, last[0] + 260, last[1])
        out = img.resize((W // 2, H // 2), Image.LANCZOS)
        name = os.path.join(out_dir, 'lumen-%s.png' % shot['name'])
        out.save(name)
        made.append(name)
    return made


if __name__ == '__main__':
    for p in render(sys.argv[1], Image.open(sys.argv[2]).convert('RGBA')):
        print(p)
