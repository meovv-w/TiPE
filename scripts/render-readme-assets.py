#!/usr/bin/env python3
"""Create README artwork from actual TiPE screenshots (optional: Pillow)."""
from pathlib import Path
import argparse
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--control-captures', type=Path, default=ROOT / 'build/control-capture',
                    help='TIPE_CONTROL_CAPTURE_DIR used with tipe-control-window-tests')
args = parser.parse_args()
OUT = ROOT / 'docs/assets'
OUT.mkdir(parents=True, exist_ok=True)
FONT = '/usr/share/fonts/google-noto-sans-cjk-fonts/NotoSansCJK-Regular.ttc'
BOLD = '/usr/share/fonts/google-noto-sans-cjk-fonts/NotoSansCJK-Bold.ttc'

def font(size, bold=False):
    return ImageFont.truetype(BOLD if bold else FONT, size)

def backdrop(size):
    im = Image.new('RGB', size)
    pixels = im.load()
    for y in range(size[1]):
        for x in range(size[0]):
            glow = max(0, 1 - ((x-size[0]*.83)**2+(y-size[1]*.2)**2)**.5/900)
            pixels[x,y] = (int(12+29*glow), int(15+21*glow), int(24+65*glow))
    return im

def card(draw, box):
    draw.rounded_rectangle(box, radius=28, fill='#181d29', outline='#30384d', width=2)

def text(draw, xy, value, size=30, fill='#eff2fa', bold=False):
    draw.text(xy, value, font=font(size,bold), fill=fill)

hero=backdrop((1600,750)); d=ImageDraw.Draw(hero)
d.rounded_rectangle((96,80,194,178), radius=26, fill='#7162ff')
text(d,(119,79),'T',70,bold=True)
text(d,(219,86),'TiPE',60,bold=True)
text(d,(99,231),'让输入，越来越懂你。',78,bold=True)
text(d,(103,357),'本地优先的中文拼音输入法',36,fill='#bfc8de')
text(d,(105,430),'安静学习你的选择，也认真对待每一次输入。',26,fill='#8d9bb8')
for x,label in [(105,'C++20 · Fcitx5'),(404,'自动后台学习'),(703,'原生光标定位')]:
 d.rounded_rectangle((x,510,x+267,569),radius=17,fill='#242b40',outline='#424b68')
 text(d,(x+22,521),label,25,fill='#d1d9f2')
text(d,(105,649),'Linux / Wayland',23,fill='#8292b2')
text(d,(1020,649),'本地数据  ·  默认不联网',23,fill='#a9b9d9')
hero.save(OUT/'hero.png',optimize=True)

show=backdrop((1640,840)); d=ImageDraw.Draw(show)
text(d,(76,47),'少一点打扰，多一点顺手。',44,bold=True)
text(d,(78,120),'真实候选窗截图 · 默认单行，展开保留列数与宽度',26,fill='#aebbd5')
card(d,(56,195,1584,365));text(d,(80,213),'01  紧凑输入',22,fill='#9eafff')
compact=Image.open(ROOT/'docs/evidence/2026-10-21/gtk4-compact.png').convert('RGB')
show.paste(compact,(76,266))
card(d,(56,397,1584,810));text(d,(80,415),'02  向下展开',22,fill='#9eafff')
expanded=Image.open(ROOT/'docs/evidence/2026-10-21/gtk4-expanded.png').convert('RGB')
show.paste(expanded,((1640-expanded.width)//2,464))
show.save(OUT/'candidates.png',optimize=True)
# Unmodified screenshots from the real control-window widget tree.
for source,target in [('page-0.png','control-status.png'),('page-1.png','control-models.png'),
                      ('page-2.png','control-support.png')]:
 image=Image.open(args.control_captures/source)
 image.save(OUT/target,optimize=True)
print('Generated hero, actual candidates and control screenshots in docs/assets')
