#!/bin/sh
# run.sh <in dir> (in the pmu-glcmp container): every PALETTE_ON program, before and after the rewrite, on
# every R8 texture (old program + RGBA32 original vs new program + R8) and every kept RGBA32 texture (old vs
# new program), with every palette and three vertex colours; prints each pair's changed bytes, then totals.
D=$1; G=$(dirname "$0")/glcmp; T=$(mktemp -d)
progs=$(awk '$1=="prog"{print $2}' $D/cases.txt)
pals=$(awk '$1=="pal"{print $2}' $D/cases.txt)
cmpone() { # <orig frag> <new frag> <tex a> <fmt a> <tex b> <fmt b> <w> <h> <pal> <colour...>
  $G $1 $3 $7 $8 $4 $9 $T/a.raw ${10} ${11} ${12} ${13} || { echo "FAIL render $1 $3"; return; }
  $G $2 $5 $7 $8 $6 $9 $T/b.raw ${10} ${11} ${12} ${13} || { echo "FAIL render $2 $5"; return; }
  n=$(cmp -l $T/a.raw $T/b.raw | wc -l); if [ $n -gt 0 ]; then echo "DIFF $n bytes: $1 $3 $9 ${10} ${11} ${12} ${13}"; else echo "SAME $(basename $1) $(basename $3) $(basename $9) ${10} ${11} ${12} ${13}"; fi
}
pal0=$(echo $pals | cut -d' ' -f1); base=$(echo $progs | cut -d' ' -f1)
awk '$1=="tex"||$1=="kept"' $D/cases.txt | while read kind t w h name; do
  if [ $kind = tex ]; then A="$D/t_$t.rgba rgba $D/t_$t.r8 r8"; else A="$D/k_$t.rgba rgba $D/k_$t.rgba rgba"; fi
  # every program with the first palette; the first program with every palette and three vertex colours
  for p in $progs; do cmpone $D/orig_$p.glsl $D/new_$p.glsl $A $w $h $D/pal_$pal0.rgb 255 255 255 255; done
  for pal in $pals; do cmpone $D/orig_$base.glsl $D/new_$base.glsl $A $w $h $D/pal_$pal.rgb 255 255 255 255; done
  for c in "200 120 60 255" "255 255 255 90"; do cmpone $D/orig_$base.glsl $D/new_$base.glsl $A $w $h $D/pal_$pal0.rgb $c; done
  echo "done $kind $t $name ($w x $h)"
done
