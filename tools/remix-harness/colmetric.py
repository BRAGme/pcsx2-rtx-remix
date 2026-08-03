import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
w,h = im.size
px = im.crop((0, int(h*0.10), w, h)).getdata()
n=0; sat=0.0; lum=0.0; col=0
for r,g,b in px:
    mx=max(r,g,b); mn=min(r,g,b)
    if mx>12:
        n+=1; lum+=mx
        s=(mx-mn)/mx
        sat+=s
        if (mx-mn)>10: col+=1
print("%s: lit_px=%d mean_sat=%.4f mean_lum=%.1f coloured_px=%d (%.2f%% of lit)" %
      (sys.argv[2], n, sat/max(n,1), lum/max(n,1), col, 100.0*col/max(n,1)))
