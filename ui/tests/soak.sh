#!/bin/sh
# banana-ui for hours, headless: the menu driven round after round (home, a library, the ready screen, Settings,
# the battery's notices), its memory read every 250. Fails when what it holds keeps growing (a leak a long
# session would feel), or, with SANITIZE=1 (AddressSanitizer, fewer rounds), when anything leaks or is misused.
#
#   ui/tests/soak.sh [rounds]        (default 4000, about 3 s; a million is about 15 min: hundreds of hours of use)
#   SANITIZE=1 ui/tests/soak.sh 20000
# The memory is compared from the first tenth to the end (a benchmark first, so its one-time buffers are in both).
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
ROUNDS=${1:-4000}
OUT=${OUT:-$HERE/tests/soak-out}
FX=$OUT/fixture
rm -rf "$OUT"; mkdir -p "$OUT"
"$HERE/tests/fixture.sh" "$FX" >/dev/null
make -C "$HERE" clean >/dev/null
if [ "${SANITIZE:-0}" = 1 ]; then
    make -C "$HERE" ALSA=0 CXXFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,undefined" >/dev/null
else
    make -C "$HERE" ALSA=0 >/dev/null
fi
# the rounds (no game started: A on the ready screen would launch it, B goes back first)
R="press right; press right; press left; press a; wait 300; press down; press down; press up; press a; wait 300; press b; wait 100; press b;"
R="$R press start; press a; press down; press down; press b; press b; battery 50; battery 15; press a; battery 60 charging; wait 1000;"
# a pass: home, a benchmark (its buffers made on the first pass, used again after), 250 rounds, the memory read;
# --repeat runs the pass over (a million rounds' script would be GBs)
PASS=250
{
    printf 'home 0; wait 2400; bench 20;'
    i=1
    while [ $i -le $PASS ]; do printf '%s' "$R"; i=$((i + 1)); done
    echo " rss"
} > "$OUT/script"
log=$OUT/soak.log
t0=$(date +%s)
"$HERE/banana-ui" --headless "$OUT/" --size 1024x768 --assets "$HERE/assets/rnds" \
    --splash "$HERE/assets/splash" --banana "$HERE/assets/banana" \
    --systems "$HERE/../system/rootfs/etc/banana/systems.conf" --roms "$FX/roms" --data "$FX/data" \
    --script "@$OUT/script" --repeat $(( (ROUNDS + PASS - 1) / PASS )) 2> "$log" || { echo "FAIL: banana-ui exited $? after $(( $(date +%s) - t0 )) s"; tail -n 30 "$log"; exit 1; }
echo "soak: $ROUNDS rounds in $(( $(date +%s) - t0 )) s"
[ "${SANITIZE:-0}" = 1 ] && { grep -q 'ERROR: \(Address\|Leak\)Sanitizer' "$log" && { echo "FAIL: the sanitizer:"; grep -A20 'Sanitizer' "$log" | head -40; exit 1; }; echo "soak: no leaks or misuse (AddressSanitizer, UBSan)"; exit 0; }
# the memory: the first tenth's highest against the end's; more than 4 MB and 5% more is a leak
sed -n 's/.*rss \([0-9]*\) kB.*/\1/p' "$log" > "$OUT/rss"
n=$(wc -l < "$OUT/rss")
[ "$n" -ge 10 ] || { echo "FAIL: only $n memory readings"; exit 1; }
base=$(head -n $((n / 10 + 1)) "$OUT/rss" | sort -n | tail -n 1)
end=$(tail -n 1 "$OUT/rss")
echo "soak: memory $base kB after the first tenth, $end kB at the end ($n readings: $(tr '\n' ' ' < "$OUT/rss" | cut -c1-200)...)"
grow=$((end - base))
if [ $grow -gt 4096 ] && [ $((grow * 100 / base)) -gt 5 ]; then
    echo "FAIL: it grew $grow kB over the run"; exit 1
fi
echo "soak: steady (grew $grow kB)"
