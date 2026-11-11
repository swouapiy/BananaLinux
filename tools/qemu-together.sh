#!/bin/sh
# Play together between two handhelds, both in QEMU: the card tools/qemu-boot.sh leaves (its data partition made, the
# setup done, SSH on, a GBA game on it) copied twice and booted side by side, A and B, each with its own network for
# SSH and a second one between the two (QEMU's socket multicast: a Wi-Fi of their own, 10.9.0.1 and 10.9.0.2).
#   tools/qemu-together.sh <qemu-out/card.img> [out dir]
# The test, through each VM's keys and its SSH: Play together opened on both, A sees B and invites it, B accepts,
# linked; A starts the GBA game, B is asked and joins; then the two RetroArch netplay sessions connected (port 55435),
# A the host and B connected to it, with each one's screenshots. Needs what qemu-boot.sh needs; the card isn't changed.
set -e
SRC=$1; OUT=${2:-qemu-together}
[ -n "$SRC" ] && [ -f "$SRC" ] || { echo "usage: $0 <card.img from qemu-boot.sh> [out dir]" >&2; exit 2; }
HERE=$(cd "$(dirname "$0")/.." && pwd)
KEY=$(dirname "$SRC")/ssh-key
[ -f "$KEY" ] || { echo "no $KEY (qemu-boot.sh's SSH key, beside its card)" >&2; exit 2; }
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
pass=0 fail=0
ok() { pass=$((pass + 1)); echo "  ok: $*"; }
bad() { fail=$((fail + 1)); echo "  FAIL: $*"; }

BOOT_AT=$(sfdisk -J "$SRC" | python3 -c "import json,sys; p=json.load(sys.stdin)['partitiontable']['partitions']; print(p[0]['start']*512)")
mcopy -o -i "$SRC@@$BOOT_AT" ::/extlinux/extlinux.conf "$OUT/extlinux.conf"
LABEL=$(sed -n 's/^DEFAULT *//p' "$OUT/extlinux.conf" | head -n 1)
KERNEL=$(awk -v l="$LABEL" '$1 == "LABEL" { cur = $2 } $1 == "LINUX" && cur == l { print $2 }' "$OUT/extlinux.conf")
ROOT=$(awk -v l="$LABEL" '$1 == "LABEL" { cur = $2 } $1 == "APPEND" && cur == l { for (i = 2; i <= NF; i++) if ($i ~ /^root=/) print $i }' "$OUT/extlinux.conf")
mcopy -o -i "$SRC@@$BOOT_AT" "::$KERNEL" "$OUT/Image"
[ "$(head -c 4 "$OUT/Image" | od -An -tx1 | tr -d ' ')" = 04224d18 ] && lz4 -d -f -q "$OUT/Image" "$OUT/Image.plain" && mv -f "$OUT/Image.plain" "$OUT/Image"
APPEND="$ROOT rootfstype=squashfs ro rootwait quiet loglevel=3 console=ttyAMA0 video=Virtual-1:640x480e video=Virtual-2:640x480e banana.console=1 banana.top=Virtual-2 banana.touch=virtio"
echo "booting label $LABEL: $KERNEL, $ROOT"

# vm_start <A|B> <n>: its card a copy, its SSH on 127.0.0.1:22<n>0, its second network card on the shared one
vm_start() {
    cp "$SRC" "$OUT/card-$1.img"
    rm -f "$OUT/monitor-$1"; : > "$OUT/serial-$1.log"
    qemu-system-aarch64 -M virt -cpu cortex-a55 -smp 4 -m 1024 -kernel "$OUT/Image" -append "$APPEND" \
        -device sdhci-pci -drive if=none,id=sd,file="$OUT/card-$1.img",format=raw -device sd-card,drive=sd \
        -device virtio-gpu-pci,id=gpu,max_outputs=2,xres=640,yres=480 -device virtio-keyboard-pci -device virtio-tablet-pci \
        -device virtio-net-pci,netdev=n0 -netdev user,id=n0,hostfwd=tcp:127.0.0.1:22${2}0-:22 \
        -device virtio-net-pci,netdev=n1,mac=52:54:00:00:09:0$2 -netdev socket,id=n1,mcast=230.0.0.1:12399,localaddr=127.0.0.1 \
        -no-reboot -rtc base=utc -display none -serial file:"$OUT/serial-$1.log" -monitor unix:"$OUT/monitor-$1",server,nowait \
        > "$OUT/qemu-$1.log" 2>&1 &
    eval "QEMU_$1=\$!"
}
vm() {     # vm <A|B> <command>: over its SSH
    n=1; [ "$1" = B ] && n=2; shift
    ssh -q -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=3 -i "$KEY" -p 22${n}0 root@127.0.0.1 "$@"
}
mon() {    # mon <A|B> <monitor command>
    python3 - "$OUT/monitor-$1" "$2" <<'PY'
import socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.settimeout(5)
def drain():
    out = b""
    try:
        while True:
            d = s.recv(65536)
            if not d: break
            out += d
            if out.rstrip().endswith(b"(qemu)"): break
    except socket.timeout: pass
    return out
drain(); s.sendall((sys.argv[2] + "\n").encode()); time.sleep(0.2); sys.stdout.write(drain().decode(errors="replace"))
PY
}
key() { v=$1; shift; for k in "$@"; do mon $v "sendkey $k" > /dev/null; sleep 0.6; done; }
shot() {   # shot <A|B> <name>
    mon $1 "screendump $OUT/$1-$2-top.png -f png gpu 1" > /dev/null
    mon $1 "screendump $OUT/$1-$2-bot.png -f png gpu 0" > /dev/null
}
seen() { grep -q "$2" "$OUT/serial-$1.log" 2>/dev/null; }
wait_for() {   # wait_for <A|B> <pattern> <seconds> <what>: in its serial log
    for i in $(seq $(($3 * 2))); do seen $1 "$2" && { ok "$1: $4"; return 0; }; sleep 0.5; done
    bad "$1: $4: not within $3 s"; return 1
}
cleanup() { kill $QEMU_A $QEMU_B 2>/dev/null || true; }
trap cleanup EXIT

echo "1. both boot, each on the shared network"
vm_start A 1; vm_start B 2
for v in A B; do for i in $(seq 50); do [ -S "$OUT/monitor-$v" ] && break; sleep 0.2; done; done
wait_for A "banana-ui: the menu" 300 "the menu is up"
wait_for B "banana-ui: the menu" 300 "the menu is up"
sleep 3
# (both given their address first, then each one's ping: A's went out before B had one)
for v in A B; do
    n=1; [ $v = B ] && n=2
    for i in $(seq 60); do vm $v true 2>/dev/null && break; sleep 1; done
    # the second network card (the first is QEMU's own for SSH): the address its own, the name whatever it got
    vm $v "i=\$(ls /sys/class/net | grep -v '^lo\$' | sort | sed -n 2p); ip addr add 10.9.0.$n/24 brd + dev \$i 2>/dev/null || ifconfig \$i 10.9.0.$n netmask 255.255.255.0; ip link set \$i up 2>/dev/null || ifconfig \$i up; echo \$i" > "$OUT/$v-net.txt" 2>&1
done
for v in A B; do
    n=1; [ $v = B ] && n=2
    vm $v "ping -c 1 -W 3 10.9.0.$((3 - n))" > /dev/null 2>&1 && ok "$v: reaches the other on the shared network ($(cat "$OUT/$v-net.txt"))" || bad "$v: can't reach the other: $(cat "$OUT/$v-net.txt")"
done

echo "2. Play together opened on both; A sees B and invites it; B accepts"
for v in A B; do vm $v "echo '/usr/share/banana/apps/together/Play together.app' > /run/banana/launch"; done
sleep 8
shot A list; shot B list
# (A's helper a few HELLOs in: one each 2 s)
for i in $(seq 10); do vm A test -s /run/banana/together-peers && break; sleep 1; done
vm A cat /run/banana/together-peers > "$OUT/A-peers.txt" 2>&1 || true
[ "$(grep -c . "$OUT/A-peers.txt")" -ge 1 ] && ! grep -q "can't open" "$OUT/A-peers.txt" && ok "A sees a handheld: $(cut -f2,3,4 "$OUT/A-peers.txt" | head -n 1 | tr '\t' ' ')" || bad "A sees nobody: $(cat "$OUT/A-peers.txt")"
key A ret; sleep 3
vm B cat /run/banana/together-invite > "$OUT/B-invite.txt" 2>&1 && [ -s "$OUT/B-invite.txt" ] && ok "B is invited" || bad "B wasn't invited"
shot B invited
key B ret; sleep 3
vm A cat /run/banana/together-link > "$OUT/A-link.txt" 2>&1 || true
vm B cat /run/banana/together-link > "$OUT/B-link.txt" 2>&1 || true
grep -q '^linked' "$OUT/A-link.txt" && grep -q '^linked' "$OUT/B-link.txt" && ok "linked both ways" || bad "not linked: A $(cat "$OUT/A-link.txt"), B $(cat "$OUT/B-link.txt")"
shot A linked; shot B linked

echo "3. A starts the GBA game: B is asked and joins; their netplay connects"
key A backspace; sleep 1
vm A "echo '/data/roms/gba/Boot Test Advance.gba' > /run/banana/launch"
wait_for A "play together: hosting" 30 "hosting the game (START sent)"
wait_for B "play together: .* started .*: asked" 30 "asked to join"
sleep 2; shot B asked
key B ret
wait_for B "play together: joining" 20 "joining (Join)"
wait_for A "run-retroarch: netplay --host" 60 "RetroArch started as the netplay host"
wait_for B "run-retroarch: netplay --connect=10.9.0.1" 60 "RetroArch started connected to A"
for i in $(seq 60); do
    vm A "netstat -tn 2>/dev/null | grep -q ':55435 .*ESTABLISHED'" 2>/dev/null && vm B "netstat -tn 2>/dev/null | grep -q ':55435 .*ESTABLISHED'" 2>/dev/null && break
    sleep 2
done
vm A "netstat -tn 2>/dev/null | grep ':55435'" > "$OUT/A-netstat.txt" 2>&1 || true
vm B "netstat -tn 2>/dev/null | grep ':55435'" > "$OUT/B-netstat.txt" 2>&1 || true
grep -q ESTABLISHED "$OUT/A-netstat.txt" && grep -q ESTABLISHED "$OUT/B-netstat.txt" && ok "the netplay connection is up between them" ||
    bad "no netplay connection: A $(tr '\n' ' ' < "$OUT/A-netstat.txt"), B $(tr '\n' ' ' < "$OUT/B-netstat.txt")"
sleep 5; shot A game; shot B game
for v in A B; do
    vm $v "cat /run/banana/retroarch.log" > "$OUT/$v-retroarch.log" 2>&1 || true
    vm $v "cat /run/banana/together.log" > "$OUT/$v-together.log" 2>&1 || true
    grep -i netplay "$OUT/$v-retroarch.log" | tail -n 8 > "$OUT/$v-retroarch-netplay.txt" || true
done
echo "--- A's Play together helper"; sed 's/^/    /' "$OUT/A-together.log"
echo "--- B's Play together helper"; sed 's/^/    /' "$OUT/B-together.log"
echo "--- A's RetroArch on netplay"; sed 's/^/    /' "$OUT/A-retroarch-netplay.txt"
echo "--- B's RetroArch on netplay"; sed 's/^/    /' "$OUT/B-retroarch-netplay.txt"

echo "qemu together test: $pass passed, $fail failed ($OUT)"
[ $fail = 0 ]
