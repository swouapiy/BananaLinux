#!/bin/sh
# banana-together: two handhelds on one machine (each its own port and folder, their HELLOs to each other instead of
# the broadcast address): seen by each other, an invitation declined, another accepted (linked both ways), the link
# left; both inviting each other at once is a link; a third one asking while linked is told no
cd "$(dirname "$0")/.." || exit 1
B=$PWD/banana-together
[ -x "$B" ] || { echo "together-test: no banana-together (make banana-together)"; exit 1; }
T=$(mktemp -d)
trap 'kill $PA $PB $PC 2>/dev/null; rm -rf "$T"' EXIT
pass=0 fail=0
ok() { pass=$((pass + 1)); echo "  ok: $*"; }
bad() { fail=$((fail + 1)); echo "  FAIL: $*"; }
# start <name> <port> <id> <ports to announce to...>
start() {
    n=$1 p=$2 id=$3; shift 3
    mkdir -p "$T/$n"; printf '%s\t0.5.2.2\tmenu\t\n' "$n" > "$T/$n/together-me"
    a=; for q in "$@"; do a="${a:+$a,}127.0.0.1:$q"; done
    TOGETHER_RUN=$T/$n TOGETHER_PORT=$p TOGETHER_ID=$id TOGETHER_ANNOUNCE=$a "$B" serve 2>"$T/$n.log" &
}
# say <name> <command>: as the menu, on its control socket
say() { python3 -I -c 'import socket, sys; s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM); s.sendto(sys.argv[2].encode(), sys.argv[1])' "$T/$1/together.sock" "$2"; }
# wait_for <file> <pattern> [seconds]
wait_for() { for i in $(seq $((${3:-5} * 10))); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done; return 1; }
gone() { for i in $(seq 50); do [ -s "$1" ] || return 0; sleep 0.1; done; return 1; }

start Ann 46101 aaaaaaaaaaaaaaaa 46102 46103; PA=$!
start Bob 46102 bbbbbbbbbbbbbbbb 46101 46103; PB=$!

echo "1. seen by each other: name, version, in the menu"
wait_for "$T/Ann/together-peers" "^bbbbbbbbbbbbbbbb	127.0.0.1:46102	Bob	0.5.2.2	menu" && ok "Ann sees Bob" || bad "Ann's peers: $(cat "$T/Ann/together-peers" 2>&1)"
wait_for "$T/Bob/together-peers" "^aaaaaaaaaaaaaaaa	.*	Ann	" && ok "Bob sees Ann" || bad "Bob's peers: $(cat "$T/Bob/together-peers" 2>&1)"

echo "2. Ann invites Bob, Bob declines"
say Ann "invite bbbbbbbbbbbbbbbb"
wait_for "$T/Ann/together-link" "^waiting	bbbbbbbbbbbbbbbb" && ok "Ann waits for an answer" || bad "Ann's link: $(cat "$T/Ann/together-link" 2>&1)"
wait_for "$T/Bob/together-invite" "^aaaaaaaaaaaaaaaa	Ann" && ok "Bob is invited by Ann" || bad "Bob's invite: $(cat "$T/Bob/together-invite" 2>&1)"
say Bob decline
wait_for "$T/Ann/together-news" "declined	Bob" && ok "Ann told: Bob declined" || bad "Ann's news: $(cat "$T/Ann/together-news" 2>&1)"
gone "$T/Ann/together-link" && gone "$T/Bob/together-invite" && ok "no link, no invitation left" || bad "left: $(cat "$T/Ann/together-link" "$T/Bob/together-invite" 2>&1)"

echo "3. again, and Bob accepts: linked both ways, and their HELLOs say so"
say Ann "invite bbbbbbbbbbbbbbbb"
wait_for "$T/Bob/together-invite" "Ann" || bad "no second invitation"
say Bob accept
wait_for "$T/Ann/together-link" "^linked	bbbbbbbbbbbbbbbb	127.0.0.1:46102	Bob" && ok "Ann linked with Bob" || bad "Ann's link: $(cat "$T/Ann/together-link" 2>&1)"
wait_for "$T/Bob/together-link" "^linked	aaaaaaaaaaaaaaaa	.*	Ann" && ok "Bob linked with Ann" || bad "Bob's link: $(cat "$T/Bob/together-link" 2>&1)"
wait_for "$T/Ann/together-news" "accepted	Bob" && ok "Ann told: Bob accepted" || bad "Ann's news: $(cat "$T/Ann/together-news" 2>&1)"
wait_for "$T/Ann/together-peers" "	Bob	.*	aaaaaaaaaaaaaaaa$" && ok "Bob's HELLOs say he's with Ann" || bad "Ann's peers: $(cat "$T/Ann/together-peers")"

echo "4. a third one asking while linked is told no"
start Cid 46103 cccccccccccccccc 46101 46102; PC=$!
wait_for "$T/Cid/together-peers" "Ann" || bad "Cid doesn't see Ann"
say Cid "invite aaaaaaaaaaaaaaaa"
wait_for "$T/Cid/together-news" "declined	Ann" && ok "Cid told no" || bad "Cid's news: $(cat "$T/Cid/together-news" 2>&1)"
[ ! -s "$T/Ann/together-invite" ] && ok "Ann wasn't asked" || bad "Ann's invite: $(cat "$T/Ann/together-invite")"
kill $PC; PC=

echo "5. Bob leaves: Ann told, unlinked; still linked after the HELLOs went on for 7 s before that"
sleep 7
grep -q "^linked" "$T/Ann/together-link" && ok "still linked after 7 s" || bad "the link dropped by itself: $(cat "$T/Ann/together-link" 2>&1)"
say Bob leave
wait_for "$T/Ann/together-news" "left	Bob" && ok "Ann told: Bob left" || bad "Ann's news: $(cat "$T/Ann/together-news" 2>&1)"
gone "$T/Ann/together-link" && gone "$T/Bob/together-link" && ok "both unlinked" || bad "links: $(cat "$T/Ann/together-link" "$T/Bob/together-link" 2>&1)"

echo "6. both inviting each other at once: linked"
say Ann "invite bbbbbbbbbbbbbbbb"; say Bob "invite aaaaaaaaaaaaaaaa"
wait_for "$T/Ann/together-link" "^linked" && wait_for "$T/Bob/together-link" "^linked" && ok "linked both ways" || bad "links: $(cat "$T/Ann/together-link" "$T/Bob/together-link" 2>&1)"

echo "6b. linked, Ann starts a game: Bob told what and from where; he joins, Ann told; then he stays out of the next"
say Ann "$(printf 'start nes\tContra (USA).nes\t131088\tfceumm\tContra')"
wait_for "$T/Bob/together-start" "^1	Ann	127.0.0.1	nes	Contra (USA).nes	131088	fceumm	Contra$" && ok "Bob told: Ann started Contra" || bad "Bob's start: $(cat "$T/Bob/together-start" 2>&1)"
say Bob "answer join"
wait_for "$T/Ann/together-news" "joining	Bob" && ok "Ann told: Bob joins" || bad "Ann's news: $(cat "$T/Ann/together-news" 2>&1)"
say Ann "$(printf 'start snes\tMario.sfc\t1\tsnes9x\tMario')"
wait_for "$T/Bob/together-start" "^2	Ann	" || bad "no second start"
say Bob "answer stay"
wait_for "$T/Ann/together-news" "stayout	Bob" && ok "Ann told: Bob stays out" || bad "Ann's news: $(cat "$T/Ann/together-news" 2>&1)"
printf 'game\tContra\n' > "$T/Ann/together-state"
wait_for "$T/Bob/together-peers" "	Ann	0.5.2.2	game	Contra	" && ok "Bob sees Ann playing Contra" || bad "Bob's peers: $(cat "$T/Bob/together-peers")"

echo "7. Bob stops (the handheld turned off): Ann told he left, and its files gone"
kill $PB; wait $PB 2>/dev/null; PB=
wait_for "$T/Ann/together-news" "left	Bob" && ok "Ann told: Bob left" || bad "Ann's news: $(cat "$T/Ann/together-news" 2>&1)"
[ ! -e "$T/Bob/together-link" ] && [ ! -e "$T/Bob/together.sock" ] && ok "Bob's files and socket gone" || bad "Bob left files: $(ls "$T/Bob")"

echo "together-test: $pass passed, $fail failed"
[ $fail = 0 ]
