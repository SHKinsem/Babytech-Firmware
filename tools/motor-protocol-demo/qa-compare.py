from PIL import Image, ImageOps, ImageDraw
from pathlib import Path
root=Path(__file__).resolve().parent
ref=Image.open(root/'references/design.png').convert('RGB')
actual=Image.open(root/'qa/desktop.png').convert('RGB')
assert ref.size == actual.size, (ref.size,actual.size)
canvas=Image.new('RGB',(ref.width*2,ref.height+36),'#eef0f4')
draw=ImageDraw.Draw(canvas)
draw.text((16,10),'SELECTED REFERENCE',fill='#111827')
draw.text((ref.width+16,10),'IMPLEMENTATION',fill='#111827')
canvas.paste(ref,(0,36)); canvas.paste(actual,(ref.width,36))
canvas.save(root/'qa/comparison.png')
print(ref.size,actual.size)
