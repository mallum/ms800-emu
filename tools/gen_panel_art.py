#!/usr/bin/env python3
"""
Render the MS800 front-panel artwork.

Layout is traced from a photo of a real unit (charcoal steel fascia, white
Eurostile Bold Extended lettering, rubber keys in square recesses, slanted
red 7-segment display).

Lettering font: the real panel is Eurostile Bold Extended (commercial, not
bundled). If it is installed (or MS800_PANEL_FONT points at it) it is used
as-is; otherwise Saira ExtraBold at its widest setting (OFL, bundled), a
free Eurostile-inspired design, stands in with tightened tracking.

Outputs (all in logical 1x units scaled by ART_SCALE):
  assets/panel.bmp      static fascia: texture, labels, wordmark, recesses, keys up
  assets/sprites.bmp    key-down, rocker on/off, lit digits 0-F (additive)
  src/panel_layout.h    coordinates shared with ui_panel.c

Needs Pillow:  python3 -m pip install pillow
Run from the repository root:  python3 tools/gen_panel_art.py
"""
import os, random
from PIL import Image, ImageDraw, ImageFont, ImageFilter, ImageChops

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SAIRA = os.path.join(ROOT, "assets/fonts/Saira-Variable.ttf")

def find_eurostile():
    """(path, horizontal stretch) for an installed Eurostile, best first:
    Bold Extended as-is, else normal-width Bold widened to extended
    proportions. None if neither is installed."""
    env = os.environ.get("MS800_PANEL_FONT")
    if env and os.path.exists(env):
        return env, float(os.environ.get("MS800_PANEL_FONT_STRETCH", "1.0"))
    dirs = [os.path.expanduser("~/Library/Fonts"), "/Library/Fonts",
            "/System/Library/Fonts/Supplemental", os.path.expanduser("~/.fonts"),
            "/usr/share/fonts", "/usr/local/share/fonts"]
    ext, bold = [], []
    for d in dirs:
        for dp, _, files in os.walk(d):
            for f in files:
                if not f.lower().endswith((".ttf", ".otf", ".ttc")):
                    continue
                n = f.lower().replace(" ", "").replace("-", "").replace("_", "")
                if "eurostile" not in n:
                    continue
                rest = n.split("eurostile", 1)[1]
                if "ex" in rest: ext.append(os.path.join(dp, f))
                elif "bold" in rest: bold.append(os.path.join(dp, f))
    # prefer the bold extended cut (some files call it "Black")
    ext.sort(key=lambda h: (not any(w in h.lower() for w in ("bold", "black")), h))
    if ext: return ext[0], 1.0
    if bold: return sorted(bold)[0], 1.5
    return None

_found = find_eurostile()
EUROSTILE, EURO_STRETCH = _found if _found else (None, 1.0)
TRACK = -0.05 if EUROSTILE else -0.06   # letter-spacing, in units of cap height (panel print is tight)
S = 2                       # art scale (HiDPI)

# ── Layout (logical px) ─────────────────────────────────────────────────────
W, H = 950, 222

SW_X, SW_Y, SW_W, SW_H = 50, 66, 66, 92          # rocker recess
DISP_X, DISP_Y, DISP_W, DISP_H = 254, 54, 108, 86  # display window
DIG_W, DIG_H, DIG_T = 31, 52, 9.5                # digit box, segment thickness
DIG_SLANT = 0.17                                 # x shift per unit of height
DP_R = 3.0                                       # decimal point radius
DIG_PAD = 10                                     # glow margin around a digit sprite
DIG1_X = DISP_X + 12
DIG2_X = DISP_X + DISP_W - 22 - DIG_W
DIG_Y = DISP_Y + (DISP_H - DIG_H) // 2

BTN_X0, BTN_PITCH = 444, 86
BTN_W, BTN_H = 66, 62
ROW1_Y, ROW2_Y = 40, 120

WORD_X, WORD_Y, WORD_CAP, WORD_LEN = 138, 166, 21, 286
CAP_X, CAP_Y = 132, 98                           # DIGITAL / WAVE / SYNTHESIZER
LABEL_CAP = 8

C_FASCIA = (52, 55, 60)
C_INK = (226, 228, 230)
C_CAPTION = (150, 178, 222)   # "DIGITAL WAVE SYNTHESIZER" is printed light blue

def sc(*v): return tuple(int(round(x * S)) for x in v)

def load_font(size):
    if EUROSTILE:
        return ImageFont.truetype(EUROSTILE, size)
    f = ImageFont.truetype(SAIRA, size)
    f.set_variation_by_axes([800, 125])      # weight, width
    return f

def font_for_cap(cap):
    """Panel font whose capital H is `cap` logical px tall."""
    lo, hi = 4, 400
    while lo < hi:
        mid = (lo + hi + 1) // 2
        b = load_font(mid).getbbox("H")
        if b[3] - b[1] <= cap * S: lo = mid
        else: hi = mid - 1
    return load_font(lo)

def text_mask(s, cap, bold=0, width=None, stretch=1.3):
    """White-on-black L mask of string s, cropped to the ink. The extended
    face is widened by `stretch` (Saira only) and set with TRACK spacing,
    then optionally fitted to an exact logical width."""
    f = font_for_cap(cap)
    if EUROSTILE: stretch = EURO_STRETCH
    track = TRACK * cap * S
    l, t, r, b = f.getbbox(s)
    w = int(f.getlength(s) + track * len(s) + cap * S + 8)
    m = Image.new("L", (w, b - t + 8), 0)
    d = ImageDraw.Draw(m)
    x = 4.0
    for ch in s:
        d.text((x, 4 - t), ch, font=f, fill=255, stroke_width=bold, stroke_fill=255)
        x += f.getlength(ch) + (0.25 * cap * S if ch == " " else track)   # keep word gaps open
    m = m.crop(m.getbbox())
    if stretch != 1.0:
        m = m.resize((int(m.width * stretch), m.height), Image.LANCZOS)
    if width:
        m = m.resize((int(width * S), m.height), Image.LANCZOS)
    return m

def stamp(img, mask, x, y, col=C_INK, center=False):
    """Paint mask onto img at logical (x,y); center=True centres on x."""
    px, py = int(x * S), int(y * S)
    if center: px -= mask.width // 2
    img.paste(Image.new("RGB", mask.size, col), (px, py), mask)

def rrect(d, box, r, fill=None, outline=None, width=1):
    d.rounded_rectangle([int(v) for v in box], radius=int(r), fill=fill, outline=outline, width=width)

# ── Fascia texture ──────────────────────────────────────────────────────────
def fascia():
    rnd = random.Random(800)
    w, h = W * S, H * S
    noise = Image.effect_noise((w, h), 22).filter(ImageFilter.GaussianBlur(0.6))
    base = Image.new("RGB", (w, h), C_FASCIA)
    # fine speckle like the powder-coated steel
    img = Image.blend(base, Image.merge("RGB", [noise] * 3), 0.10)
    img = ImageChops.add(img, Image.new("RGB", (w, h), (0, 0, 0)))
    # slight top-lit gradient
    grad = Image.linear_gradient("L").resize((w, h))
    img = Image.composite(img, ImageChops.multiply(img, Image.new("RGB", (w, h), (225, 225, 228))), ImageChops.invert(grad))
    d = ImageDraw.Draw(img)
    # bent-steel top edge highlight and bottom shadow
    d.rectangle(sc(0, 0, W, 2), fill=(92, 95, 100))
    d.rectangle(sc(0, 2, W, 3), fill=(70, 73, 78))
    d.rectangle(sc(0, H - 3, W, H), fill=(26, 27, 30))
    for _ in range(int(w * h / 900)):          # sparse lighter flecks
        x, y = rnd.randrange(w), rnd.randrange(h)
        v = rnd.randrange(64, 84)
        d.point((x, y), fill=(v, v + 2, v + 6))
    return img

# ── Keys ────────────────────────────────────────────────────────────────────
def key(pressed):
    """A rubber key sitting in its square recess (BTN_W x BTN_H logical)."""
    w, h = BTN_W * S, BTN_H * S
    im = Image.new("RGB", (w, h), C_FASCIA)
    d = ImageDraw.Draw(im)
    # recess: dark outer lip top/left, lit lip bottom/right
    d.rectangle([0, 0, w - 1, h - 1], fill=(34, 36, 40))
    d.rectangle([0, 0, w - 1, 2 * S], fill=(18, 19, 22))
    d.rectangle([0, 0, 2 * S, h - 1], fill=(18, 19, 22))
    d.rectangle([0, h - S, w - 1, h - 1], fill=(84, 87, 93))
    d.rectangle([w - S, 0, w - 1, h - 1], fill=(76, 79, 85))
    # key cap
    inset = 6 * S + (S if pressed else 0)
    off = S if pressed else 0
    cap = [inset + off, inset + off, w - inset + off - 1, h - inset + off - 1]
    if not pressed:   # drop shadow under the cap
        rrect(d, [cap[0] + 2 * S, cap[1] + 3 * S, cap[2] + 2 * S, cap[3] + 3 * S], 3 * S, fill=(16, 17, 19))
    top, bot = ((44, 46, 51), (52, 54, 60)) if pressed else ((60, 62, 68), (44, 46, 51))
    g = Image.linear_gradient("L").resize((cap[2] - cap[0], cap[3] - cap[1]))
    face = Image.composite(Image.new("RGB", g.size, bot), Image.new("RGB", g.size, top), g)
    m = Image.new("L", g.size, 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, g.width - 1, g.height - 1], radius=3 * S, fill=255)
    im.paste(face, (cap[0], cap[1]), m)
    rrect(d, cap, 3 * S, outline=(22, 23, 26), width=S)
    if not pressed:   # catch-light along the top edge of the cap
        d.line([cap[0] + 4 * S, cap[1] + 2 * S, cap[2] - 4 * S, cap[1] + 2 * S], fill=(86, 89, 96), width=S)
    return im

# ── Rocker switch ───────────────────────────────────────────────────────────
def rocker(on):
    """Black rocker in a bezel; OFF printed above, ON below; the pressed-in
    half is darker, the raised half catches the light."""
    w, h = SW_W * S, SW_H * S
    im = Image.new("RGB", (w, h), C_FASCIA)
    d = ImageDraw.Draw(im)
    rrect(d, [0, 0, w - 1, h - 1], 4 * S, fill=(20, 20, 22), outline=(74, 76, 80), width=S)
    rrect(d, [5 * S, 5 * S, w - 5 * S - 1, h - 5 * S - 1], 3 * S, fill=(8, 8, 9))
    bx0, by0, bx1, by1 = 12 * S, 12 * S, w - 12 * S, h - 12 * S
    mid = (by0 + by1) // 2
    raised = (by0, mid) if on else (mid, by1)     # ON: bottom pressed in, top raised
    sunk = (mid, by1) if on else (by0, mid)
    rrect(d, [bx0, by0, bx1, by1], 4 * S, fill=(24, 24, 27))
    g = Image.linear_gradient("L").resize((bx1 - bx0, raised[1] - raised[0]))
    if not on: g = g.transpose(Image.FLIP_TOP_BOTTOM)
    hi = Image.composite(Image.new("RGB", g.size, (30, 30, 34)), Image.new("RGB", g.size, (64, 65, 70)), g)
    im.paste(hi, (bx0, raised[0]))
    d.rectangle([bx0, sunk[0], bx1, sunk[1]], fill=(14, 14, 16))
    d.line([bx0, mid, bx1, mid], fill=(6, 6, 7), width=S)
    edge = by0 + S if on else by1 - 2 * S
    d.line([bx0 + 3 * S, edge, bx1 - 3 * S, edge], fill=(92, 94, 100), width=S)
    rrect(d, [bx0, by0, bx1, by1], 4 * S, outline=(4, 4, 5), width=S)
    return im

# ── 7-segment digits ────────────────────────────────────────────────────────
SEG7 = [0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F, 0x77, 0x7C, 0x39, 0x5E, 0x79, 0x71]

def seg_polys(ox, oy):
    """Seven hexagonal segments for a digit whose upright box starts at
    (ox,oy) (sprite px). Returns {bit: polygon}; slant applied."""
    w, h, t = DIG_W * S, DIG_H * S, DIG_T * S
    g = S * 0.7                  # gap between segments
    ht, m = t / 2, h / 2
    # segment centre lines: x = ht / w-ht, y = ht / m / h-ht; tips stop g short
    def hseg(y):
        x0, x1 = ht + g, w - ht - g
        return [(x0, y), (x0 + ht, y - ht), (x1 - ht, y - ht), (x1, y),
                (x1 - ht, y + ht), (x0 + ht, y + ht)]
    def vseg(x, y0, y1):
        y0, y1 = y0 + g, y1 - g
        return [(x, y0), (x + ht, y0 + ht), (x + ht, y1 - ht), (x, y1),
                (x - ht, y1 - ht), (x - ht, y0 + ht)]
    raw = {0x01: hseg(ht), 0x40: hseg(m), 0x08: hseg(h - ht),
           0x20: vseg(ht, ht, m), 0x02: vseg(w - ht, ht, m),
           0x10: vseg(ht, m, h - ht), 0x04: vseg(w - ht, m, h - ht)}
    sl = DIG_SLANT
    return {b: [(ox + x + sl * (h - y), oy + y) for x, y in p] for b, p in raw.items()}

DSPR_W = DIG_W + int(DIG_SLANT * DIG_H) + int(2 * DP_R + 3) + 2 * DIG_PAD
DSPR_H = DIG_H + 2 * DIG_PAD

def dp_box(ox, oy):
    """Decimal point, bottom-right of the slanted digit."""
    cx = ox + DIG_W * S + (DP_R + 2.0) * S
    cy = oy + DIG_H * S - DP_R * S
    r = DP_R * S
    return [cx - r, cy - r, cx + r, cy + r]

def digit(n, lit_col=(255, 46, 86), ghost=False, dp=False):
    """A lit LED digit: saturated pinkish-red segments with a hotter core
    and a wide glow, like the real display seen through its red filter."""
    w, h = DSPR_W * S, DSPR_H * S
    m = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(m)
    for b, p in seg_polys(DIG_PAD * S, DIG_PAD * S).items():
        if SEG7[n] & b: d.polygon(p, fill=255)
    if dp: d.ellipse(dp_box(DIG_PAD * S, DIG_PAD * S), fill=255)
    if ghost:
        return m
    black = Image.new("RGB", (w, h), (0, 0, 0))
    img = Image.composite(Image.new("RGB", (w, h), lit_col), black, m)
    # crisp hot core: shrink the segment, soften only slightly
    core = m.filter(ImageFilter.MinFilter(5)).filter(ImageFilter.GaussianBlur(0.5 * S))
    img = ImageChops.add(img, Image.composite(Image.new("RGB", (w, h), (0, 95, 85)), black, core))
    # tight halo plus a faint wide bloom, so edges stay sharp
    for rad, col in ((0.6, (70, 10, 24)), (3.5, (90, 6, 16))):
        img = ImageChops.add(img, Image.composite(Image.new("RGB", (w, h), col), black,
                                                  m.filter(ImageFilter.GaussianBlur(rad * S))))
    return img

def display_window(img):
    d = ImageDraw.Draw(img)
    x, y, w, h = DISP_X, DISP_Y, DISP_W, DISP_H
    d.rectangle(sc(x - 2, y - 2, x + w + 2, y + h + 2), fill=(20, 21, 24))   # cut-out shadow
    d.rectangle(sc(x - 1, y + h + 1, x + w + 2, y + h + 2), fill=(80, 83, 88))
    d.rectangle(sc(x, y, x + w, y + h), fill=(26, 5, 8))                       # red filter
    # faint unlit segments behind the filter
    ghost = digit(8, ghost=True, dp=True)
    for gx in (DIG1_X, DIG2_X):
        tint = Image.new("RGB", ghost.size, (44, 8, 13))
        img.paste(tint, sc(gx - DIG_PAD, DIG_Y - DIG_PAD), ghost)
    # seam between the two LED packages
    mx = (DIG1_X + DIG_W + DIG2_X) / 2 + 3
    d.line(sc(mx, y + 8, mx, y + h - 8), fill=(14, 2, 4), width=S)
    # glassy sheen in the top-left
    sheen = Image.linear_gradient("L").resize(sc(w, h)).point(lambda v: max(0, 40 - v // 3))
    img.paste(Image.new("RGB", sheen.size, (255, 255, 255)), sc(x, y), sheen.point(lambda v: v // 3))

# ── Chevrons printed above / below the arrow keys ───────────────────────────
def chevron(d, cx, cy, up, w=14, h=7, lw=1.6):
    s = -1 if up else 1
    pts = [(cx - w / 2, cy - s * h / 2), (cx, cy + s * h / 2), (cx + w / 2, cy - s * h / 2)]
    d.line([sc(*p) for p in pts], fill=C_INK, width=int(lw * S), joint="curve")

def main():
    img = fascia()
    d = ImageDraw.Draw(img)

    # key recesses with keys up
    kup = key(False)
    for c in range(5):
        for y in (ROW1_Y, ROW2_Y):
            img.paste(kup, sc(BTN_X0 + c * BTN_PITCH, y))

    # labels: top row above, bottom row below
    top = ["CHANNEL", "PATCH", None, None, None]
    bot = ["FUNCTION", "PARAM", None, None, "ENTER"]
    for c in range(5):
        cx = BTN_X0 + c * BTN_PITCH + BTN_W / 2
        if top[c]: stamp(img, text_mask(top[c], LABEL_CAP), cx, ROW1_Y - 15, center=True)
        if bot[c]: stamp(img, text_mask(bot[c], LABEL_CAP), cx, ROW2_Y + BTN_H + 6, center=True)
    stamp(img, text_mask("NEXT", LABEL_CAP), BTN_X0 + 4 * BTN_PITCH + BTN_W / 2, ROW1_Y - 28, center=True)
    stamp(img, text_mask("PARAM", LABEL_CAP), BTN_X0 + 4 * BTN_PITCH + BTN_W / 2, ROW1_Y - 15, center=True)
    cx2 = BTN_X0 + 2 * BTN_PITCH + BTN_W / 2
    cx3 = BTN_X0 + 3 * BTN_PITCH + BTN_W / 2
    chevron(d, cx2, ROW1_Y - 11, True)
    chevron(d, cx3, ROW1_Y - 15, True); chevron(d, cx3, ROW1_Y - 9, True)
    chevron(d, cx2, ROW2_Y + BTN_H + 10, False)
    chevron(d, cx3, ROW2_Y + BTN_H + 7, False); chevron(d, cx3, ROW2_Y + BTN_H + 13, False)

    # power switch legends
    stamp(img, text_mask("OFF", LABEL_CAP), SW_X + SW_W / 2, SW_Y - 18, center=True)
    stamp(img, text_mask("ON", LABEL_CAP), SW_X + SW_W / 2, SW_Y + SW_H + 10, center=True)

    # model caption and wordmark
    for i, s in enumerate(("DIGITAL", "WAVE", "SYNTHESIZER")):
        stamp(img, text_mask(s, 6), CAP_X, CAP_Y + i * 10, col=C_CAPTION)
    stamp(img, text_mask("CHEETAR MS800", WORD_CAP, width=WORD_LEN), WORD_X, WORD_Y)

    display_window(img)
    img.save(os.path.join(ROOT, "assets/panel.bmp"))

    # sprite atlas: [key down][rocker off][rocker on][digits 0..F]
    kdn, roff, ron = key(True), rocker(False), rocker(True)
    digs = [digit(n) for n in range(16)]
    aw = kdn.width + roff.width + ron.width + 16 * digs[0].width
    ah = max(kdn.height, roff.height, digs[0].height)
    atlas = Image.new("RGB", (aw, ah), (0, 0, 0))
    x = 0
    pos = {}
    for name, im in (("KEYDOWN", kdn), ("ROCKER_OFF", roff), ("ROCKER_ON", ron)):
        atlas.paste(im, (x, 0)); pos[name] = x; x += im.width
    pos["DIGIT0"] = x
    for im in digs:
        atlas.paste(im, (x, 0)); x += im.width
    atlas.save(os.path.join(ROOT, "assets/sprites.bmp"))

    with open(os.path.join(ROOT, "src/panel_layout.h"), "w") as f:
        f.write("/* Generated by tools/gen_panel_art.py - edit the script, not this file. */\n")
        f.write("#pragma once\n\n")
        defs = dict(UI_WIN_W=W, UI_WIN_H=H, ART_SCALE=S,
                    SWITCH_X=SW_X, SWITCH_Y=SW_Y, SWITCH_W=SW_W, SWITCH_H=SW_H,
                    BTN_X0=BTN_X0, BTN_PITCH=BTN_PITCH, BTN_W=BTN_W, BTN_H=BTN_H,
                    ROW1_Y=ROW1_Y, ROW2_Y=ROW2_Y,
                    DIG1_X=DIG1_X - DIG_PAD, DIG2_X=DIG2_X - DIG_PAD, DIG_Y=DIG_Y - DIG_PAD,
                    DIG_SPR_W=DSPR_W, DIG_SPR_H=DSPR_H,
                    SPR_KEYDOWN_X=pos["KEYDOWN"], SPR_ROCKER_OFF_X=pos["ROCKER_OFF"],
                    SPR_ROCKER_ON_X=pos["ROCKER_ON"], SPR_DIGIT0_X=pos["DIGIT0"])
        for k, v in defs.items():
            f.write(f"#define {k:<17} {v}\n")
        f.write("/* SPR_*_X are sprite-atlas pixel offsets (already x ART_SCALE); the rest are logical px. */\n")
    print("font:", f"{EUROSTILE} (x{EURO_STRETCH} wide)" if EUROSTILE else "Saira ExtraBold (Eurostile stand-in)")
    print("wrote assets/panel.bmp, assets/sprites.bmp, src/panel_layout.h")

if __name__ == "__main__":
    main()
