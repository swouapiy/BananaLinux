#!/bin/sh
# A card not closed at the last power-off (/usr/libexec/banana/card-check) against made-up partitions: an exFAT boot
# sector with its dirty mark set or not, and a stand-in fsck.exfat that answers as told.
#   sh system/tests/card-check-test.sh
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
C=$HERE/rootfs/usr/libexec/banana/card-check
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
pass=0 fail=0
ok() { pass=$((pass + 1)); echo "  ok: $*"; }
bad() { fail=$((fail + 1)); echo "  FAIL: $*"; }
mkdir -p "$T/bin"
# fsck.exfat: notes that it ran, says what FSCK_SAYS has, exits FSCK_RC
printf '#!/bin/sh\necho "$*" >> "%s/fsck-ran"\necho "${FSCK_SAYS:-clean}"\nexit ${FSCK_RC:-0}\n' "$T" > "$T/bin/fsck.exfat"
chmod +x "$T/bin/fsck.exfat"

# part <name> <flags byte> [fs name]: a 512-byte boot sector
part() {
    dd if=/dev/zero of="$T/$1" bs=512 count=1 2>/dev/null
    printf '%-8s' "${3:-EXFAT}" | dd of="$T/$1" bs=1 seek=3 conv=notrunc 2>/dev/null
    printf "\\$(printf %03o "$2")" | dd of="$T/$1" bs=1 seek=106 conv=notrunc 2>/dev/null
}
# root [data]: /run (and the data partition's logs folder when "data")
root() {
    R=$T/root; rm -rf "$R" "$T/fsck-ran"; mkdir -p "$R/run/banana"
    [ "$1" = data ] && mkdir -p "$R/data/banana/logs"
    return 0
}
run() { rc=0; CARDCHECK_ROOT=$R PATH=$T/bin:$PATH sh "$C" "$@" || rc=$?; }
ran() { [ -f "$T/fsck-ran" ]; }
notice() { cat "$R/run/banana/card-notice" 2>/dev/null; }

echo "1. closed properly: no check"
root; part p 0; run "$T/p" card
[ $rc = 0 ] && ! ran && [ -z "$(notice)" ] && [ ! -e "$R/run/banana/card-check.log" ] && ok "exit 0, fsck not run, no log, no notice" || bad "rc $rc, ran $(ran && echo yes), notice '$(notice)'"

echo "2. another flag set (bit 0, the active FAT), not the dirty one: no check"
root; part p 1; run "$T/p" card
[ $rc = 0 ] && ! ran && ok "exit 0, fsck not run" || bad "rc $rc"

echo "3. not closed, nothing wrong: checked, no notice"
root; part p 2; run "$T/p" card
[ $rc = 0 ] && ran && [ -z "$(notice)" ] && ok "fsck ran, exit 0, no notice" || bad "rc $rc, ran $(ran && echo yes), notice '$(notice)'"
grep -q -- "-y $T/p" "$T/fsck-ran" && ok "fsck.exfat -y on the partition" || bad "fsck args: $(cat "$T/fsck-ran")"
grep -q "not closed at the last power-off: checking" "$R/run/banana/card-check.log" && grep -q "clean (" "$R/run/banana/card-check.log" &&
    ok "logged in /run (no data partition yet)" || bad "log: $(cat "$R/run/banana/card-check.log" 2>/dev/null)"
[ ! -e "$R/run/banana/checking" ] && ok "the trial clock's pause mark gone" || bad "checking mark left"

echo "4. not closed, repaired: the notice"
root; part p 3; FSCK_RC=1 FSCK_SAYS="fixed the bitmap" run "$T/p" card
[ $rc = 0 ] && [ "$(notice)" = "repaired card" ] && ok "exit 0, notice 'repaired card'" || bad "rc $rc, notice '$(notice)'"
grep -q "    fixed the bitmap" "$R/run/banana/card-check.log" && grep -q "repaired (" "$R/run/banana/card-check.log" &&
    ok "fsck's words and 'repaired' in the log" || bad "log: $(cat "$R/run/banana/card-check.log")"

echo "5. errors left, the second card: its notice, exit 1, logged on the data partition"
root data; part p 2; FSCK_RC=4 run "$T/p" sd2
[ $rc = 1 ] && [ "$(notice)" = "errors sd2" ] && ok "exit 1, notice 'errors sd2'" || bad "rc $rc, notice '$(notice)'"
grep -q "sd2 ($T/p): errors left (fsck.exfat 4" "$R/data/banana/logs/card-check.txt" && [ ! -e "$R/run/banana/card-check.log" ] &&
    ok "in banana/logs/card-check.txt" || bad "log: $(cat "$R/data/banana/logs/card-check.txt" 2>/dev/null)"

echo "6. not exFAT (FAT32), or not there: nothing done"
root; part p 2 "MSWIN4.1"; run "$T/p" card
[ $rc = 2 ] && ! ran && ok "FAT32: exit 2, fsck not run" || bad "rc $rc"
root; run "$T/nothing" card
[ $rc = 2 ] && ok "a partition that isn't there: exit 2" || bad "rc $rc"

echo "7. the check while it runs: boot-guard's pause mark there"
root; part p 2
printf '#!/bin/sh\n[ -e "%s/root/run/banana/checking" ] && echo yes > "%s/mark-seen"\nexit 0\n' "$T" "$T" > "$T/bin/fsck.exfat"
run "$T/p" card
[ -f "$T/mark-seen" ] && ok "/run/banana/checking present during fsck" || bad "no mark during fsck"

echo "card-check: $pass passed, $fail failed"
[ $fail = 0 ]
