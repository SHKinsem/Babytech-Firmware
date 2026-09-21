from PIL import Image,ImageDraw
from pathlib import Path
p=Path('qa');a=Image.open('references/design.png');b=Image.open('qa/desktop.png')
box=(350,240,1118,780)
x=a.crop(box);y=b.crop(box)
z=Image.new('RGB',(1536,570),'#eef0f4');d=ImageDraw.Draw(z);d.text((12,8),'REFERENCE - COMMAND & FRAMES',fill='#111827');d.text((780,8),'IMPLEMENTATION - COMMAND & FRAMES',fill='#111827');z.paste(x,(0,30));z.paste(y,(768,30));z.save(p/'comparison-detail.png')
