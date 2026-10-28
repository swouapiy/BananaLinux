#!/bin/sh
# bananad's sleep, tested without an RG DS: bananad runs in its own mount and PID namespaces against a made-up
# /sys (the battery, the LEDs, the backlights, the RTC, /sys/power) and /dev/input (FIFOs that carry real
# struct input_event records). /sys/power/state is a FIFO: bananad's write of "mem" blocks until the test reads it,
# which is the handheld being asleep; the test decides what the world looks like when it "wakes". A stand-in game
# (its process group must be stopped while asleep) and a stand-in banana-ui (it must get SIGUSR1 after a wake).
#
#   sudo system/bananad/tests/sleep-test.sh        (root: it mounts in a private namespace; nothing outside changes)
#   sleep-test.sh --build <dir>                      only the programs, into <dir> (for a runner of its own, then --inside)
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
ROOTFS=$(cd "$HERE/../rootfs" && pwd)
if [ "$1" != --inside ]; then
    if [ "$1" = --build ]; then W=$2; mkdir -p "$W"; else W=$(mktemp -d); fi
    cc -O2 -o "$W/bananad" "$HERE/bananad.c"
    cc -O2 -o "$W/banana-ui" -x c - <<'EOF'
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
static void on(int s) { FILE *f = fopen("/run/test/ui-signals", "a"); if (f) { fprintf(f, "SIGUSR1\n"); fclose(f); } }
int main(void) { signal(SIGUSR1, on); for (;;) pause(); }
EOF
    # stand-in emulators for Power off after sleeping, one program under each name bananad looks for (drastic,
    # bananads, retroarch): what reaches it (SIGUSR1, SIGTERM, the lines on its stdin) in /run/test/<name>.log, then
    # "saved" a moment later and it quits, on a signal or on RetroArch's QUIT
    cc -O2 -o "$W/drastic" -x c - <<'EOF'
#include <fcntl.h>
#include <libgen.h>
#include <stdlib.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static volatile sig_atomic_t got;
static void on(int s) { got = s; }
static const char *name;
static void say(const char *m) { char p[128]; snprintf(p, sizeof p, "/run/test/%s.log", name); FILE *f = fopen(p, "a"); if (f) { fprintf(f, "%s\n", m); fclose(f); } }
int main(int argc, char **argv)
{
	(void)argc; name = basename(argv[0]);
	signal(SIGUSR1, on); signal(SIGTERM, on);
	fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
	char buf[512]; size_t have = 0;
	for (;;) {
		if (got) {
			say(got == SIGUSR1 ? "USR1" : "TERM"); usleep(300000);
			const char *rf = getenv("TEST_RESUME");	/* the resume state, when the test wants one written */
			if (got == SIGUSR1 && rf && *rf) { FILE *f = fopen(rf, "w"); if (f) { fputs("state", f); fclose(f); } }
			say("saved"); return 0;
		}
		ssize_t r = read(0, buf + have, sizeof buf - 1 - have);
		if (r > 0) {
			have += r; buf[have] = 0;
			char *nl;
			while ((nl = strchr(buf, '\n'))) {
				*nl = 0; say(buf);
				int quit = !strcmp(buf, "QUIT");
				memmove(buf, nl + 1, strlen(nl + 1) + 1); have = strlen(buf);
				if (quit) { usleep(300000); say("saved"); return 0; }
			}
		}
		usleep(20000);
	}
}
EOF
    cp "$W/drastic" "$W/bananads"; cp "$W/drastic" "$W/retroarch"
    [ "$1" = --build ] && exit 0
    mkdir -p /data 2>/dev/null || true
    exec unshare -m -p -f --mount-proc sh "$0" --inside "$W" "$ROOTFS"
fi

# ---- inside: pid 1 is this shell, so bananad's "shut down" (SIGUSR2 to init) comes here ------------------------
W=$2 ROOTFS=$3
POWEROFF=0
trap 'POWEROFF=1' USR2
mount -t tmpfs none /sys/power
mount -t tmpfs none /sys/class
mount -t tmpfs none /dev/input 2>/dev/null || { mkdir -p /dev/input; mount -t tmpfs none /dev/input; }
mount -t tmpfs none /run
mount -t tmpfs none /data
mkdir -p /etc/banana && mount -t tmpfs none /etc/banana && cp -r "$ROOTFS/etc/banana/." /etc/banana/
mkdir -p /run/banana /run/test /data/banana/logs /sys/class/rtc/rtc0 /sys/class/power_supply/battery
echo Battery > /sys/class/power_supply/battery/type
echo Discharging > /sys/class/power_supply/battery/status
echo 50 > /sys/class/power_supply/battery/capacity
: > /sys/class/rtc/rtc0/wakealarm
echo "s2idle [deep]" > /sys/power/mem_sleep
echo 0 > /sys/power/pm_async
for l in green:power red:status amber:charging; do mkdir -p /sys/class/leds/$l; : > /sys/class/leds/$l/trigger; : > /sys/class/leds/$l/brightness; done
for b in backlight0 backlight1; do mkdir -p /sys/class/backlight/$b; echo 255 > /sys/class/backlight/$b/max_brightness; echo 200 > /sys/class/backlight/$b/brightness; echo 0 > /sys/class/backlight/$b/bl_power; done
mkfifo /sys/power/state /dev/input/event0 /run/test/wake
exec 5<>/sys/power/state        # the "kernel" takes bananad's "mem" at once (never read)...
exec 6<>/run/test/wake          # ...and bananad waits on this until wake() sends a byte (BANANAD_TEST_WAKE)
exec 3<>/dev/input/event0       # a writer that stays, so bananad's poll doesn't see a hang-up
mkfifo /run/test/touch; exec 4<>/run/test/touch     # the stand-in touchscreen bananad reads unpolled (BANANAD_TEST_TOUCH)
echo 0 > /run/test/lid

pass=0 fail=0
ok() { pass=$((pass + 1)); echo "  ok: $*"; }
bad() { fail=$((fail + 1)); echo "  FAIL: $*"; tail -n 4 /data/banana/logs/sleep.log 2>/dev/null | sed 's/^/    sleep.log: /'; }   # (what bananad did)
# an input event: type code value (then a SYN_REPORT)
ev() { python3 -c "
import struct, sys
t, c, v = (int(x) for x in sys.argv[1:4])
sys.stdout.buffer.write(struct.pack('llHHi', 0, 0, t, c, v) + struct.pack('llHHi', 0, 0, 0, 0, 0))" "$@" >&3; }
EV_SW=5 SW_LID=0 EV_KEY=1 KEY_POWER=116
# a touch on the stand-in touchscreen, stamped with the monotonic clock (as bananad asks the real one for)
touch() { python3 -c "
import struct, sys, time
t = time.clock_gettime(time.CLOCK_MONOTONIC); s = int(t); u = int((t - s) * 1e6)
sys.stdout.buffer.write(struct.pack('llHHi', s, u, 3, 0x35, 100) + struct.pack('llHHi', s, u, 0, 0, 0))" >&4; }
# is bananad asleep (its "mem" written, waiting for the wake: it says so in wake.asleep)? wait up to 3 s
asleep() { for i in $(seq 30); do [ -e /run/test/wake.asleep ] && return 0; sleep 0.1; done; return 1; }
# the wake: one byte, and only to a bananad that is asleep (a byte left over would end its next sleep at once)
wake() { if asleep; then printf x >&6; else bad "wake: bananad wasn't asleep"; fi; sleep 0.3; }
gstate() { awk '{print $3}' /proc/$GAME/stat; }
start() {   # start [bananad.conf lines]
    echo "s2idle [deep]" > /sys/power/mem_sleep     # the kernel's file always lists both
    printf '%s\n' "autosleep_minutes = 0" "sleep_check_hours = 0" "$@" > /data/banana/bananad.conf
    BANANAD_TEST_LID=/run/test/lid BANANAD_TEST_TOUCH=/run/test/touch BANANAD_TEST_WAKE=/run/test/wake "$W/bananad" & BD=$!
    sleep 0.5
}
stop() { kill $BD 2>/dev/null; wait $BD 2>/dev/null || true; rm -f /run/test/wake.asleep; }   # (killed asleep: no wake)

"$W/banana-ui" & UI=$!
setsid sleep 600 & GAME=$!
sleep 0.2
echo $GAME > /run/banana/game.pid

echo "1. lid closed, then opened"
start
echo 1 > /run/test/lid; ev $EV_SW $SW_LID 1
if asleep; then ok "suspended on the lid"; else bad "no suspend on the lid"; fi
[ "$(gstate)" = T ] && ok "game stopped while asleep" || bad "game not stopped ($(gstate))"
[ "$(cat /sys/power/pm_async)" = 1 ] && ok "devices resume in parallel (pm_async 1)" || bad "pm_async $(cat /sys/power/pm_async)"
grep -q "\[deep\]" /sys/power/mem_sleep && ok "deep sleep chosen" || bad "mem_sleep $(cat /sys/power/mem_sleep)"
echo 0 > /run/test/lid; wake
[ "$(gstate)" != T ] && ok "game running again after the wake" || bad "game still stopped"
rm -f /run/banana/game.pid; sleep 0.1
grep -q 'awake (lid opened)' /data/banana/logs/sleep.log && ok "sleep.log: $(tail -n1 /data/banana/logs/sleep.log | cut -c21-)" || bad "no wake in sleep.log"
stop

echo "2. menu asleep, woken with the lid still shut, then opened"
start
echo 1 > /run/test/lid; ev $EV_SW $SW_LID 1
asleep && ok "suspended" || bad "no suspend"
wake      # the power button bumped in a bag: the lid is still shut
if asleep; then ok "straight back to sleep with the lid shut"; else bad "stayed awake with the lid shut"; fi
[ ! -s /run/test/ui-signals ] && ok "the menu wasn't woken for it" || bad "the menu was woken with the lid shut"
echo 0 > /run/test/lid; wake
sleep 0.2
grep -q SIGUSR1 /run/test/ui-signals && ok "menu told to redraw after the real wake" || bad "menu not told to redraw"
grep -q 'slept again with the lid shut' /data/banana/logs/sleep.log && ok "logged the second sleep" || bad "second sleep not logged"
stop

echo "3. the battery guard: an RTC wake with the battery nearly empty shuts down cleanly"
echo $GAME > /run/banana/game.pid
echo 3 > /sys/class/power_supply/battery/capacity
start "sleep_check_hours = 0.002" "sleep_poweroff_percent = 5"
echo 1 > /run/test/lid; ev $EV_SW $SW_LID 1
asleep && ok "suspended with the RTC alarm set ($(cat /sys/class/rtc/rtc0/wakealarm))" || bad "no suspend"
: > /sys/class/rtc/rtc0/wakealarm; wake      # the alarm fired (the RTC clears it): the RTC woke it
for i in $(seq 30); do [ $POWEROFF = 1 ] && break; sleep 0.1; done
[ $POWEROFF = 1 ] && ok "shut down cleanly (init asked to power off)" || bad "no shutdown at 3%"
kill -0 $GAME 2>/dev/null && bad "the game wasn't told to quit and save" || ok "the game was told to quit and save first"
grep -q 'battery check: 3%' /data/banana/logs/sleep.log && ok "logged the battery check" || bad "battery check not logged"
stop
POWEROFF=0
setsid sleep 600 & GAME=$!; sleep 0.1; echo $GAME > /run/banana/game.pid

echo "4. an RTC wake with enough battery goes back to sleep"
echo 60 > /sys/class/power_supply/battery/capacity
start "sleep_check_hours = 0.002"
echo 1 > /run/test/lid; ev $EV_SW $SW_LID 1
asleep && ok "suspended" || bad "no suspend"
: > /sys/class/rtc/rtc0/wakealarm; wake
if asleep; then ok "back to sleep after the battery check"; else bad "awake after the battery check"; fi
[ "$(gstate)" = T ] && ok "the game stayed stopped" || bad "the game ran during the check"
echo 0 > /run/test/lid; wake; sleep 0.2
grep -q 'battery checked' /data/banana/logs/sleep.log && ok "logged" || bad "not logged"
stop

echo "5. a suspend that fails: the safer ways, then the panels dark until the lid opens"
rm -f /sys/power/state; : > /sys/power/state; chmod 000 /sys/power/state
# root can write a mode-000 file; a directory can't be written at all
rm -f /sys/power/state; mkdir /sys/power/state
start
echo 1 > /run/test/lid; ev $EV_SW $SW_LID 1
sleep 1.5
grep -q 'trying the safer way' /data/banana/logs/sleep.log && ok "fell back after the failure" || bad "no fallback"
[ "$(cat /sys/power/mem_sleep)" = s2idle ] && ok "s2idle tried" || bad "mem_sleep $(cat /sys/power/mem_sleep)"
[ "$(cat /sys/class/backlight/backlight0/bl_power)" = 4 ] && ok "panels dark with the lid shut" || bad "backlight bl_power $(cat /sys/class/backlight/backlight0/bl_power)"
echo 0 > /run/test/lid; ev $EV_SW $SW_LID 0; sleep 0.3
[ "$(cat /sys/class/backlight/backlight0/bl_power)" = 0 ] && ok "panels back when the lid opens" || bad "panels still dark"
stop
rmdir /sys/power/state; mkfifo /sys/power/state; exec 5<>/sys/power/state

echo "6. lid sleep off: closing the lid only turns the backlights off"
start "lid_sleep = no"
echo 1 > /run/test/lid; ev $EV_SW $SW_LID 1; sleep 0.3
[ "$(cat /sys/class/backlight/backlight1/bl_power)" = 4 ] && ok "backlights off" || bad "backlights on"
asleep 2>/dev/null && bad "it suspended" || ok "no suspend"
echo 0 > /run/test/lid; ev $EV_SW $SW_LID 0; sleep 0.3
[ "$(cat /sys/class/backlight/backlight1/bl_power)" = 0 ] && ok "backlights on again" || bad "still off"
stop

echo "7. idle in the menu: the panels dim, and come back at the first press"
rm -f /run/banana/game.pid
echo 200 > /sys/class/backlight/backlight0/brightness; echo 200 > /sys/class/backlight/backlight1/brightness
start "dim_minutes = 0.02" "dim_percent = 30"      # 1.2 s
sleep 2.5
[ "$(cat /sys/class/backlight/backlight1/brightness)" = 60 ] && ok "dimmed to 30% after the idle time" || bad "not dimmed: brightness $(cat /sys/class/backlight/backlight1/brightness)"
ev $EV_KEY 304 1; ev $EV_KEY 304 0; sleep 0.3         # BTN_SOUTH: any button
[ "$(cat /sys/class/backlight/backlight1/brightness)" = 200 ] && ok "back to 200 at the press" || bad "still dim: $(cat /sys/class/backlight/backlight1/brightness)"
echo $GAME > /run/banana/game.pid; sleep 2
[ "$(cat /sys/class/backlight/backlight1/brightness)" = 200 ] && ok "never dimmed while a game runs" || bad "dimmed during a game"
rm -f /run/banana/game.pid
stop

echo "8. charging: it sleeps as a phone does, the charging light lit at full, a look every 10 min; no turning off"
mkdir -p /sys/class/power_supply/charger
echo USB > /sys/class/power_supply/charger/type; echo 1 > /sys/class/power_supply/charger/online
echo $GAME > /run/banana/game.pid
start "poweroff_minutes = 0.02"     # (1.2 s: would turn off at the first look, but for the charger)
echo 1 > /run/test/lid; ev $EV_SW $SW_LID 1
asleep && ok "asleep on the charger (the lid)" || bad "no sleep on the charger"
[ "$(gstate)" = T ] && ok "game paused" || bad "game not paused ($(gstate))"
[ "$(cat /sys/class/leds/amber:charging/brightness)" = 255 ] && ok "charging light lit at full while asleep" || bad "charging light '$(cat /sys/class/leds/amber:charging/brightness)'"
[ "$(cat /sys/class/rtc/rtc0/wakealarm)" = +600 ] && ok "a look in 10 min (+600)" || bad "wakealarm $(cat /sys/class/rtc/rtc0/wakealarm)"
echo 100 > /sys/class/power_supply/battery/capacity
sleep 1.5; : > /sys/class/rtc/rtc0/wakealarm; wake      # the look: the battery full meanwhile
asleep && ok "back to sleep after the look" || bad "awake after the look"
[ $POWEROFF = 0 ] && ok "not turned off on the charger" || bad "turned off on the charger"
[ "$(cat /sys/class/leds/amber:charging/brightness)" = 0 ] && ok "charging light out once full" || bad "charging light '$(cat /sys/class/leds/amber:charging/brightness)'"
echo 50 > /sys/class/power_supply/battery/capacity
echo 0 > /run/test/lid; wake
[ "$(gstate)" != T ] && ok "game running again" || bad "game still paused"
grep -q 'awake (lid opened)' /data/banana/logs/sleep.log && ok "woke: the lid" || bad "woke: $(grep -o 'awake ([^)]*)' /data/banana/logs/sleep.log | tail -n1)"
rm -rf /sys/class/power_supply/charger
rm -f /run/banana/game.pid
stop
POWEROFF=0

echo "8b. charging and idle in the menu: it sleeps (it used to stay up, dimmed)"
mkdir -p /sys/class/power_supply/charger
echo USB > /sys/class/power_supply/charger/type; echo 1 > /sys/class/power_supply/charger/online
start "autosleep_minutes = 0.02"
asleep && ok "asleep when idle on the charger" || bad "stayed awake on the charger"
wake
rm -rf /sys/class/power_supply/charger
stop

echo "9. the power button: a press sleeps, and the press that woke it doesn't send it back to sleep"
echo $GAME > /run/banana/game.pid
start
ev $EV_KEY $KEY_POWER 1; ev $EV_KEY $KEY_POWER 0
asleep && ok "asleep after a press" || bad "no sleep on a press"
wake
ev $EV_KEY $KEY_POWER 1; ev $EV_KEY $KEY_POWER 0     # inside the second after the wake: the same press, bouncing
asleep && bad "back to sleep straight after the wake" || ok "a press just after the wake is ignored"
[ "$(gstate)" != T ] && ok "game running" || bad "game still stopped"
ev $EV_KEY $KEY_POWER 1; ev $EV_KEY $KEY_POWER 0     # later: a real press again
asleep && ok "a later press sleeps again" || bad "no sleep on a later press"
wake
grep -q 'sleep (power button)' /data/banana/logs/sleep.log && ok "logged" || bad "not logged"
stop

echo "10. the power button held: a clean shutdown, the game told to quit, no sleep on the release"
start "power_long_ms = 400"
ev $EV_KEY $KEY_POWER 1
for i in $(seq 30); do [ $POWEROFF = 1 ] && break; sleep 0.1; done
[ $POWEROFF = 1 ] && ok "shut down (init asked to power off)" || bad "no shutdown on a held press"
ev $EV_KEY $KEY_POWER 0
asleep 2>/dev/null && bad "it slept on the release" || ok "no sleep on the release"
kill -0 $GAME 2>/dev/null && bad "the game wasn't told to quit" || ok "the game was told to quit"
stop
POWEROFF=0

echo "11. a shutdown waits for a game that takes a moment to write its saves"
rm -f /run/test/saved
setsid sh -c 'trap "sleep 1; touch /run/test/saved; exit 0" TERM; while :; do sleep 0.1; done' 2>/dev/null & GAME=$!
sleep 0.2; echo $GAME > /run/banana/game.pid
trap 'POWEROFF=1; [ -f /run/test/saved ] && SAVED=1' USR2
SAVED=0
start "power_long_ms = 300"
ev $EV_KEY $KEY_POWER 1
for i in $(seq 70); do [ $POWEROFF = 1 ] && break; sleep 0.1; done
ev $EV_KEY $KEY_POWER 0
[ $POWEROFF = 1 ] && ok "shut down" || bad "no shutdown"
[ $SAVED = 1 ] && ok "the game's saves were written before the power-off" || bad "powered off before the game had saved"
stop
trap 'POWEROFF=1' USR2
POWEROFF=0

echo "12. a game that ignores SIGTERM doesn't hold the shutdown up"
setsid sh -c 'trap "" TERM; while :; do sleep 0.1; done' 2>/dev/null & GAME=$!
sleep 0.2; echo $GAME > /run/banana/game.pid
start "power_long_ms = 300"
t0=$(date +%s)
ev $EV_KEY $KEY_POWER 1
for i in $(seq 90); do [ $POWEROFF = 1 ] && break; sleep 0.1; done
ev $EV_KEY $KEY_POWER 0
[ $POWEROFF = 1 ] && ok "shut down anyway, after $(( $(date +%s) - t0 )) s" || bad "no shutdown with a game that won't quit"
stop
kill -9 -$GAME 2>/dev/null || true
POWEROFF=0
setsid sleep 600 & GAME=$!; sleep 0.1; echo $GAME > /run/banana/game.pid

echo "13. the lid shut and opened in a flick: one short sleep (or the sleep called off), then awake for good"
start
ev $EV_SW $SW_LID 1; ev $EV_SW $SW_LID 0       # (the lid's file says open: it was only a moment)
# the open event read before the suspend began calls it off (wake_inputs); read after, it is a short sleep
if asleep; then ok "slept on the close"; wake; else grep -q 'called off: the lid opened' /data/banana/logs/sleep.log && ok "the sleep called off by the open" || bad "neither a sleep nor a called-off one"; fi
asleep && bad "back to sleep after the flick" || ok "awake after the wake, the open event not taken again"
[ "$(gstate)" != T ] && ok "game running" || bad "game still stopped"
stop

echo "14. a charger plugged in while asleep with the lid shut: back to sleep, the charging light lit (as a phone)"
start
echo 1 > /run/test/lid; ev $EV_SW $SW_LID 1
asleep && ok "asleep" || bad "no sleep"
mkdir -p /sys/class/power_supply/charger
echo USB > /sys/class/power_supply/charger/type; echo 1 > /sys/class/power_supply/charger/online
wake      # the plug woke it; the lid is still shut
asleep && ok "back to sleep on the charger" || bad "awake on the charger with the lid shut"
[ "$(cat /sys/class/backlight/backlight0/bl_power)" = 4 ] && ok "panels dark" || bad "backlight bl_power $(cat /sys/class/backlight/backlight0/bl_power)"
[ "$(cat /sys/class/leds/amber:charging/brightness)" = 255 ] && ok "charging light lit at full" || bad "charging light '$(cat /sys/class/leds/amber:charging/brightness)'"
[ "$(gstate)" = T ] && ok "game still paused" || bad "game running with the lid shut ($(gstate))"
echo 0 > /run/test/lid; wake
[ "$(cat /sys/class/backlight/backlight0/bl_power)" = 0 ] && ok "panels back when the lid opens" || bad "panels still dark"
[ "$(gstate)" != T ] && ok "game running again" || bad "game still paused"
rm -rf /sys/class/power_supply/charger
stop

echo "15. one low battery reading while awake isn't a shutdown (a load spike); it takes two in a row"
echo 2 > /sys/class/power_supply/battery/capacity
start "battery_shutdown_percent = 3"
sleep 2
[ $POWEROFF = 0 ] && ok "no shutdown on the first low reading" || bad "shut down on one reading"
stop
echo 50 > /sys/class/power_supply/battery/capacity
POWEROFF=0

echo "16. bananad.conf read again on SIGHUP (Settings changed lid sleep): the lid then only darkens the panels"
start "lid_sleep = yes"
printf '%s\n' "autosleep_minutes = 0" "sleep_check_hours = 0" "lid_sleep = no" > /data/banana/bananad.conf
kill -HUP $BD; sleep 0.3
echo 1 > /run/test/lid; ev $EV_SW $SW_LID 1; sleep 0.3
asleep 2>/dev/null && bad "it slept: the new setting wasn't read" || ok "no sleep after the reload"
[ "$(cat /sys/class/backlight/backlight0/bl_power)" = 4 ] && ok "panels dark" || bad "panels on"
echo 0 > /run/test/lid; ev $EV_SW $SW_LID 0; sleep 0.3
stop
rm -f /run/banana/game.pid

echo "17. asleep long enough (poweroff_minutes): off cleanly, the game saving first"
echo $GAME > /run/banana/game.pid
start "poweroff_minutes = 0.02"     # 1.2 s asleep
echo 1 > /run/test/lid; ev $EV_SW $SW_LID 1
asleep && ok "asleep" || bad "no sleep"
[ "$(cat /sys/class/rtc/rtc0/wakealarm)" = +2 ] && ok "the RTC set for turning off (+2 s), not the battery check" || bad "wakealarm $(cat /sys/class/rtc/rtc0/wakealarm)"
sleep 1.5; : > /sys/class/rtc/rtc0/wakealarm; wake
for i in $(seq 30); do [ $POWEROFF = 1 ] && break; sleep 0.1; done
[ $POWEROFF = 1 ] && ok "turned off (init asked to power off)" || bad "not turned off"
kill -0 $GAME 2>/dev/null && bad "the game wasn't told to quit and save" || ok "the game was told to quit and save first"
grep -q 'turning off' /data/banana/logs/sleep.log && ok "logged: $(grep 'turning off' /data/banana/logs/sleep.log | tail -n1 | cut -c21-)" || bad "not logged"
grep -q "woke after .*: the RTC's alarm" /data/banana/logs/sleep.log && ok "logged the alarm's wake" || bad "the alarm's wake not logged"
# the lid still shut: it mustn't sleep again while rcK shuts down (it did, and froze the shutdown until the next wake)
for i in $(seq 70); do [ -e /run/test/wake.asleep ] && break; sleep 0.1; done
[ ! -e /run/test/wake.asleep ] && ok "no sleep again during the shutdown (7 s, the lid still shut)" || bad "slept again in the middle of the shutdown"
stop
POWEROFF=0; echo 0 > /run/test/lid
setsid sleep 600 & GAME=$!; sleep 0.1; echo $GAME > /run/banana/game.pid

echo "18. on the charger at the turn-off time: it sleeps on (the RG DS Plus doesn't charge switched off)"
start "poweroff_minutes = 0.02"
ev $EV_KEY $KEY_POWER 1; ev $EV_KEY $KEY_POWER 0
asleep && ok "asleep (the power button)" || bad "no sleep"
mkdir -p /sys/class/power_supply/charger
echo USB > /sys/class/power_supply/charger/type; echo 1 > /sys/class/power_supply/charger/online
sleep 1.5; : > /sys/class/rtc/rtc0/wakealarm; wake
[ $POWEROFF = 0 ] && ok "not turned off on the charger" || bad "turned off on the charger"
asleep && ok "asleep again" || bad "awake after the alarm"
wake      # a press: awake
rm -rf /sys/class/power_supply/charger
stop
POWEROFF=0

echo "19. sleep at never: off after the idle time awake; in a game only with the timers everywhere"
rm -f /run/banana/game.pid
start "poweroff_minutes = 0.02"     # (start's autosleep_minutes = 0)
sleep 2.5
[ $POWEROFF = 1 ] && ok "off after the idle time in the menu" || bad "not turned off when idle"
grep -q 'idle .* min: turning off' /data/banana/logs/sleep.log && ok "logged" || bad "not logged"
stop; POWEROFF=0
echo $GAME > /run/banana/game.pid
start "poweroff_minutes = 0.02"
sleep 2.5
[ $POWEROFF = 0 ] && ok "not during a game (the timers in the menu)" || bad "turned off during a game"
stop
start "poweroff_minutes = 0.02" "idle_in_games = yes"
for i in $(seq 30); do [ $POWEROFF = 1 ] && break; sleep 0.1; done
[ $POWEROFF = 1 ] && ok "off during an idle game with the timers everywhere" || bad "not turned off during a game"
kill -0 $GAME 2>/dev/null && bad "the game wasn't told to quit and save" || ok "the game was told to quit and save first"
stop; POWEROFF=0
setsid sleep 600 & GAME=$!; sleep 0.1; echo $GAME > /run/banana/game.pid

echo "20. the timers everywhere: a touch is input in a game (the stylus alone isn't idle)"
echo 200 > /sys/class/backlight/backlight0/brightness; echo 200 > /sys/class/backlight/backlight1/brightness
start "idle_in_games = yes" "dim_minutes = 0.03"     # 1.8 s
for i in 1 2 3 4 5 6 7; do touch; sleep 0.5; done
[ "$(cat /sys/class/backlight/backlight1/brightness)" = 200 ] && ok "touching kept the panels bright (3.5 s)" || bad "dimmed while touching: $(cat /sys/class/backlight/backlight1/brightness)"
sleep 2.5
[ "$(cat /sys/class/backlight/backlight1/brightness)" = 60 ] && ok "dimmed once the touching stopped" || bad "not dimmed: $(cat /sys/class/backlight/backlight1/brightness)"
touch; sleep 1.5
[ "$(cat /sys/class/backlight/backlight1/brightness)" = 200 ] && ok "a touch brought them back" || bad "still dim after a touch: $(cat /sys/class/backlight/backlight1/brightness)"
stop

echo "21. the timers everywhere: an idle game sleeps, paused, and carries on after the wake"
start "idle_in_games = yes" "autosleep_minutes = 0.02"
asleep && ok "asleep in a game" || bad "no sleep in a game"
[ "$(gstate)" = T ] && ok "the game paused" || bad "the game ran on ($(gstate))"
wake
[ "$(gstate)" != T ] && ok "the game carries on after the wake" || bad "the game still paused"
stop
kill -CONT -$GAME 2>/dev/null || true
rm -f /run/banana/game.pid

echo "22. the upper LED lit while asleep (led_sleep on: full, the kernel keeps it), back at its level after; off: dark"
G=/sys/class/leds/green:power/brightness
start
[ "$(cat $G)" = 128 ] && ok "awake: 128" || bad "awake: $(cat $G)"
echo 1 > /run/test/lid; ev $EV_SW $SW_LID 1
asleep && ok "asleep" || bad "no sleep"
[ "$(cat $G)" = 255 ] && ok "asleep: lit (255)" || bad "asleep: $(cat $G)"
echo 0 > /run/test/lid; wake
[ "$(cat $G)" = 128 ] && ok "back to 128 after the wake" || bad "after the wake: $(cat $G)"
stop
start "led_sleep = off"
echo 1 > /run/test/lid; ev $EV_SW $SW_LID 1
asleep && ok "asleep" || bad "no sleep"
[ "$(cat $G)" = 0 ] && ok "asleep with led_sleep off: dark" || bad "asleep: $(cat $G)"
echo 0 > /run/test/lid; wake
stop

echo "23. the panels dark as soon as it goes to sleep (not when the kernel gets round to it), lit again after"
echo 0 > /sys/class/backlight/backlight0/bl_power
start
ev $EV_KEY $KEY_POWER 1; ev $EV_KEY $KEY_POWER 0
asleep && ok "asleep" || bad "no sleep"
[ "$(cat /sys/class/backlight/backlight0/bl_power)" = 4 ] && ok "panels dark while asleep" || bad "panels lit while asleep ($(cat /sys/class/backlight/backlight0/bl_power))"
wake
[ "$(cat /sys/class/backlight/backlight0/bl_power)" = 0 ] && ok "panels lit after the wake" || bad "panels dark after the wake"
stop

echo "24. a power press after the turn-off time, the alarm not having woken it: a wake, not a shutdown"
start "poweroff_minutes = 0.02" "autosleep_minutes = 30"     # 1.2 s, the alarm +2 s (sleep set: not the awake turn-off)
ev $EV_KEY $KEY_POWER 1; ev $EV_KEY $KEY_POWER 0
asleep && ok "asleep (the power button)" || bad "no sleep"
sleep 3                             # past the alarm's time; wakealarm left set: it didn't fire
wake                                # the power button
[ $POWEROFF = 0 ] && ok "not turned off: someone woke it" || bad "turned off on a power press"
grep -q "not the RTC's alarm" /data/banana/logs/sleep.log && ok "logged: $(grep 'woke after' /data/banana/logs/sleep.log | tail -n1 | cut -c21-)" || bad "the wake's cause not logged"
stop
POWEROFF=0

echo "25. the buttons heard at once at start, while the sound is still being set up (a slow alsactl restore)"
mkdir -p "$W/bin"; printf '#!/bin/sh\nsleep 3\n' > "$W/bin/alsactl"; chmod +x "$W/bin/alsactl"
: > /data/banana/asound.state; : > /run/banana/boot-times
rm -f /run/banana/game.pid
P0=$PATH; PATH="$W/bin:$PATH"; start; PATH=$P0
[ ! -e /run/banana/bananad-ready ] && ok "not ready yet: the sound still being set up" || bad "ready before the sound was set up"
grep -q 'bananad: buttons live' /run/banana/boot-times && ok "boot-times: buttons live" || bad "no 'buttons live' in boot-times"
ev $EV_KEY $KEY_POWER 1; ev $EV_KEY $KEY_POWER 0
# within a second (a bananad still in its setup would take the press only after alsactl's 3 s)
s=1; for i in $(seq 10); do [ -e /run/test/wake.asleep ] && { s=0; break; }; sleep 0.1; done
[ $s = 0 ] && ok "the power button heard within a second, during the sound's setup (a sleep)" || bad "the power button not heard during the sound's setup"
wake
for i in $(seq 40); do [ -e /run/banana/bananad-ready ] && break; sleep 0.1; done
[ -e /run/banana/bananad-ready ] && ok "ready once the sound was set up" || bad "never ready"
grep -q 'bananad: sound ready' /run/banana/boot-times && ok "boot-times: $(grep 'bananad' /run/banana/boot-times | tr -s ' ' | tr '\n' ';')" || bad "no 'sound ready' in boot-times"
stop
rm -f /data/banana/asound.state

echo "26. a LED that turns up after bananad started (its driver probing late) is used once it's there"
A=/sys/class/leds/amber:charging
rm -rf $A
start
mkdir -p $A; : > $A/trigger; : > $A/brightness
mkdir -p /sys/class/power_supply/charger
echo USB > /sys/class/power_supply/charger/type; echo 1 > /sys/class/power_supply/charger/online
kill -HUP $BD; sleep 0.5      # (settings read again: the LEDs set again)
a=$(cat $A/brightness)
[ -n "$a" ] && [ "$a" != 0 ] && ok "the late charging LED lit on the charger ($a)" || bad "the late charging LED not used ('$a')"
rm -rf /sys/class/power_supply/charger
stop

echo "27. the flight recorder: buttons, the lid, a sleep, touches (no position while typing), a marker (Select held) and the stop, on the card"
# tev <type> <code> <value>...: events on the stand-in touchscreen, with the monotonic time (as the real one stamps them)
tev() { python3 -c "
import struct, sys, time
t = time.clock_gettime(time.CLOCK_MONOTONIC); s = int(t); u = int((t - s) * 1e6)
a = [int(x) for x in sys.argv[1:]]
sys.stdout.buffer.write(b''.join(struct.pack('llHHi', s, u, a[i], a[i + 1], a[i + 2]) for i in range(0, len(a), 3)))" "$@" >&4; }
rm -rf /data/banana/logs/sessions; rm -f /run/banana/keyboard
start "recorder = on"
[ -e /run/banana/recording ] && ok "recording (/run/banana/recording, for the menu's dot)" || bad "no /run/banana/recording"
F=$(cat /run/banana/recording 2>/dev/null)
ev $EV_KEY 304 1; ev $EV_KEY 304 0                          # B (BTN_SOUTH: the RG DS's B)
tev 3 57 1  3 53 100  3 54 200  0 0 0                      # a touch: tracking id 1 at 100,200, then the report
tev 3 57 -1  0 0 0                                          # its lift
sleep 0.5
: > /run/banana/keyboard
tev 3 57 2  3 53 300  3 54 50  0 0 0;  tev 3 57 -1  0 0 0  # a touch while typing
sleep 0.5; rm -f /run/banana/keyboard
ev $EV_KEY 314 1; ev $EV_KEY 314 0; sleep 0.3                  # a quick Select: no marker
grep -q "MARK" "$F" 2>/dev/null && bad "a marker from a quick press" || ok "a quick Select: no marker"
ev $EV_KEY 314 1; sleep 2.4; ev $EV_KEY 314 0                  # Select held 2 s: a marker (written at once)
sleep 0.5
grep -q "MARK .*marker 1" "$F" 2>/dev/null && ok "Select held 2 s: the marker on the card at once" || bad "no marker in $F"
sleep 0.5; ls "${F%.txt}"-marker-1-dmesg.txt >/dev/null 2>&1 && ok "the kernel's log saved beside it" || bad "no ${F%.txt}-marker-1-dmesg.txt"
[ "$(cat /sys/class/leds/green:power/brightness)" = 128 ] && ok "the power LED back after its blinks" || bad "LED $(cat /sys/class/leds/green:power/brightness)"
# banana-ui's events: a datagram on the recorder's socket, copied in (control characters made spaces)
python3 -c "
import socket; s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.sendto(b'setting\tTurn off when idle: 1 hour -> 5 minutes', '/run/banana/recorder.sock')
s.sendto(b'game\tstart: Peel Quest\n(nds, run-nds)', '/run/banana/recorder.sock')"
sleep 0.3
echo 1 > /run/test/lid; ev $EV_SW $SW_LID 1
asleep && ok "asleep" || bad "no sleep"
grep -q "button .*b down" "$F" && grep -q "lid .*closed" "$F" && ok "flushed before the sleep: the button, the lid" || bad "not flushed before the sleep: $(tail -n 3 "$F" 2>/dev/null | tr '\n' '|')"
grep -q "setting  Turn off when idle: 1 hour -> 5 minutes" "$F" && ok "banana-ui's event copied in (setting)" || bad "no setting line: $(grep -c setting "$F")"
grep -q "game     start: Peel Quest (nds, run-nds)" "$F" && ok "its newline made a space: one line each" || bad "game line: $(grep game "$F")"
grep -q "touch .*down 100,200" "$F" && grep -q "touch .*up 100,200" "$F" && ok "the touch, where (100,200)" || bad "touch lines: $(grep touch "$F" | tr '\n' '|')"
grep -q "keyboard open: where not recorded" "$F" && ! grep -q "300,50" "$F" && ok "the touch while typing, without where" || bad "while typing: $(grep touch "$F" | tr '\n' '|')"
echo 0 > /run/test/lid; wake; sleep 0.3
python3 -c "
import socket; socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM).sendto(b'menu\trestart (Settings)', '/run/banana/recorder.sock')"
stop    # (SIGTERM, as rcK's: before the unmount, the last lines written out at once, not at the next half-minute)
grep -q "menu     restart (Settings)" "$F" && grep -q "bananad  stopped" "$F" && ok "stopped: banana-ui's last event and the stop, on the card" || bad "at the stop: $(tail -n 2 "$F" | tr '\n' '|')"
sed -n '1,2p' "$F" | grep -q "flight recorder" && ok "the session's header" || bad "header: $(head -n1 "$F")"
start
[ ! -e /run/banana/recording ] && ok "recorder off: not recording" || bad "recording with recorder off"
n=$(ls /data/banana/logs/sessions | wc -l)
ev $EV_KEY 304 1; ev $EV_KEY 304 0; sleep 0.3
[ "$(ls /data/banana/logs/sessions | wc -l)" = "$n" ] && ok "recorder off: no new session" || bad "a session with recorder off"
stop

# more scenarios, a file per topic (tests/sleep-test.d), run here in turn with the helpers above
for t in "$HERE"/tests/sleep-test.d/*.sh; do [ -f "$t" ] && . "$t"; done

# Turning off after sleeping (poweroff_minutes): a game as banana-ui starts one (sh -c in a session of its own, BANANA_ROM set), the
# emulator a child of the launcher
game() {    # game <emulator> <rom> [stdin]
    rm -f /run/test/$1.log
    BANANA_ROM=$2 setsid sh -c '"$0" < "$1"; :' "$W/$1" "${3:-/dev/null}" & GAME=$!
    sleep 0.2; echo $GAME > /run/banana/game.pid
}
poweroff() { for i in $(seq 50); do [ $POWEROFF = 1 ] && return 0; sleep 0.1; done; return 1; }
echo 60 > /sys/class/power_supply/battery/capacity

echo "9. turning off after sleeping: the deadline's alarm, the lid still shut: SuperDrastic saves and quits, a shutdown"
mkdir -p /data/drastic/savestates /data/states/nds
export TEST_RESUME="/data/drastic/savestates/Peel Quest.resume.dss"
game drastic "/data/roms/nds/Peel Quest.nds"
: > /run/test/ui-signals; rm -f /data/banana/resume-boot
start "sleep_check_hours = 4" "poweroff_minutes = 0.02"    # 1.2 s
echo 1 > /run/test/lid; ev $EV_SW $SW_LID 1
asleep && ok "suspended" || bad "no suspend"
[ "$(cat /sys/class/rtc/rtc0/wakealarm)" = +2 ] && ok "the RTC alarm at the deadline (1.2 s, rounded up), not the 4 h battery check" || bad "wakealarm $(cat /sys/class/rtc/rtc0/wakealarm)"
[ "$(gstate)" = T ] && ok "game stopped while asleep" || bad "game not stopped ($(gstate))"
sleep 1.3; : > /sys/class/rtc/rtc0/wakealarm; wake
poweroff && ok "shut down cleanly (init asked to power off)" || bad "no shutdown at the deadline"
grep -qx USR1 /run/test/drastic.log 2>/dev/null && ok "drastic got SIGUSR1 (save a resume state and quit)" || bad "drastic: $(cat /run/test/drastic.log 2>/dev/null)"
grep -qx saved /run/test/drastic.log 2>/dev/null && ! kill -0 $GAME 2>/dev/null && ok "the shutdown waited for the save" || bad "shut down before the save"
[ "$(cat /data/banana/resume-boot 2>/dev/null)" = "$(printf 'rom=/data/roms/nds/Peel Quest.nds\nemulator=superdrastic')" ] && ok "resume-boot names the game" || bad "resume-boot: $(cat /data/banana/resume-boot 2>/dev/null)"
[ "$(cat /sys/class/backlight/backlight0/bl_power)" = 4 ] && ok "the panels stayed dark" || bad "bl_power $(cat /sys/class/backlight/backlight0/bl_power)"
[ ! -s /run/test/ui-signals ] && ok "the menu wasn't woken" || bad "the menu was told to redraw"
grep -q 'the game saved, then turning off' /data/banana/logs/sleep.log && ok "logged" || bad "not logged"
stop; POWEROFF=0; echo 0 > /run/test/lid; echo 0 > /sys/class/backlight/backlight0/bl_power

echo "9b. SuperDrastic quits without writing a resume state (an older one on the card): no resume-boot"
export TEST_RESUME=; rm -f /data/banana/resume-boot
touch -d '2026-01-01 12:00' "/data/drastic/savestates/Peel Quest.resume.dss"
game drastic "/data/roms/nds/Peel Quest.nds"
start "poweroff_minutes = 0.02"
echo 1 > /run/test/lid; ev $EV_SW $SW_LID 1
asleep && ok "suspended" || bad "no suspend"
sleep 1.3; : > /sys/class/rtc/rtc0/wakealarm; wake
poweroff && ok "shut down" || bad "no shutdown"
grep -qx USR1 /run/test/drastic.log 2>/dev/null && ok "drastic got SIGUSR1" || bad "drastic: $(cat /run/test/drastic.log 2>/dev/null)"
[ ! -e /data/banana/resume-boot ] && ok "no resume-boot (the old state isn't this save)" || bad "resume-boot written: $(cat /data/banana/resume-boot)"
grep -q 'quit without a new resume state' /data/banana/logs/sleep.log && ok "logged" || bad "not logged"
stop; POWEROFF=0; echo 0 > /run/test/lid; echo 0 > /sys/class/backlight/backlight0/bl_power

echo "10. a power-button sleep with the lid open: BananaDS saves at the deadline"
export TEST_RESUME="/data/states/nds/Banana Kart DS.resume.mst"
game bananads "/data/roms/nds/Banana Kart DS.nds"
start "sleep_check_hours = 0" "poweroff_minutes = 0.02"     # (the deadline is the only alarm)
ev $EV_KEY $KEY_POWER 1; ev $EV_KEY $KEY_POWER 0
asleep && ok "suspended on the power button" || bad "no suspend"
[ "$(cat /sys/class/rtc/rtc0/wakealarm)" = +2 ] && ok "the alarm set with no battery check" || bad "wakealarm $(cat /sys/class/rtc/rtc0/wakealarm)"
sleep 1.3; : > /sys/class/rtc/rtc0/wakealarm; wake
poweroff && ok "shut down" || bad "no shutdown"
grep -qx USR1 /run/test/bananads.log 2>/dev/null && grep -qx saved /run/test/bananads.log && ok "bananads got SIGUSR1 and saved" || bad "bananads: $(cat /run/test/bananads.log 2>/dev/null)"
grep -qx emulator=bananads /data/banana/resume-boot 2>/dev/null && ok "resume-boot: bananads" || bad "resume-boot: $(cat /data/banana/resume-boot 2>/dev/null)"
stop; POWEROFF=0

export TEST_RESUME=
echo "11. RetroArch: the slot down to its automatic one, SAVE_STATE, QUIT (through its command pipe)"
mkdir -p /data/retroarch; printf 'state_slot = "2"\n' > /data/retroarch/retroarch.cfg
rm -f /run/banana/retroarch.cmd; mkfifo /run/banana/retroarch.cmd; exec 8<>/run/banana/retroarch.cmd     # (5 is the "kernel"'s state FIFO)
game retroarch "/data/roms/gba/Orchard Tactics.gba" /run/banana/retroarch.cmd
start "poweroff_minutes = 0.02"
echo 1 > /run/test/lid; ev $EV_SW $SW_LID 1
asleep && ok "suspended" || bad "no suspend"
sleep 1.3; : > /sys/class/rtc/rtc0/wakealarm; wake
poweroff && ok "shut down" || bad "no shutdown"
ra=$(grep -v AUDIO_REINIT /run/test/retroarch.log 2>/dev/null | tr '\n' ' ')     # (bananad's at its start: the pipe is there)
[ "$ra" = "STATE_SLOT_MINUS STATE_SLOT_MINUS STATE_SLOT_MINUS STATE_SLOT_MINUS SAVE_STATE QUIT saved " ] &&
    ok "RetroArch: slot 2 down past 0, saved, quit" || bad "retroarch: $ra"
grep -qx emulator=retroarch /data/banana/resume-boot 2>/dev/null && ok "resume-boot: retroarch" || bad "resume-boot: $(cat /data/banana/resume-boot 2>/dev/null)"
stop; POWEROFF=0; exec 8>&-; rm -f /run/banana/retroarch.cmd /data/banana/resume-boot; echo 0 > /run/test/lid

echo "12. another program (a port): SIGTERM, no resume-boot"
setsid sleep 600 & GAME=$!; sleep 0.1; echo $GAME > /run/banana/game.pid
start "poweroff_minutes = 0.02"
echo 1 > /run/test/lid; ev $EV_SW $SW_LID 1
asleep && ok "suspended" || bad "no suspend"
sleep 1.3; : > /sys/class/rtc/rtc0/wakealarm; wake
poweroff && ok "shut down" || bad "no shutdown"
kill -0 $GAME 2>/dev/null && bad "the game wasn't stopped" || ok "the game got SIGTERM"
[ ! -e /data/banana/resume-boot ] && ok "no resume-boot (nothing to resume)" || bad "resume-boot written"
stop; POWEROFF=0; echo 0 > /run/test/lid

echo "13. asleep on the charger the deadline doesn't count; unplugged, it starts from there; opening the lid first wins"
mkdir -p /sys/class/power_supply/charger
echo USB > /sys/class/power_supply/charger/type; echo 1 > /sys/class/power_supply/charger/online
setsid sleep 600 & GAME=$!; sleep 0.1; echo $GAME > /run/banana/game.pid
start "poweroff_minutes = 0.05"     # 3 s
echo 1 > /run/test/lid; ev $EV_SW $SW_LID 1
asleep && ok "asleep on the charger (a real sleep, as a phone's)" || bad "no sleep on the charger"
[ "$(cat /sys/class/rtc/rtc0/wakealarm)" = +600 ] && ok "a look every 10 min on the charger, no turn-off alarm" || bad "wakealarm $(cat /sys/class/rtc/rtc0/wakealarm)"
sleep 4; : > /sys/class/rtc/rtc0/wakealarm; wake      # the 10-minute look, past the deadline
asleep && [ $POWEROFF = 0 ] && kill -0 $GAME 2>/dev/null && ok "past the deadline on the charger: asleep again, no shutdown" || bad "shut down on the charger"
echo 0 > /sys/class/power_supply/charger/online
sleep 0.5; : > /sys/class/rtc/rtc0/wakealarm; wake    # (the cable out wakes it; the lid still shut)
asleep && ok "asleep again once unplugged" || bad "no sleep after unplugging"
[ "$(cat /sys/class/rtc/rtc0/wakealarm)" = +3 ] && ok "the deadline counted from the unplugging (+3)" || bad "wakealarm $(cat /sys/class/rtc/rtc0/wakealarm)"
echo 0 > /run/test/lid; wake       # the lid opened before the alarm
[ $POWEROFF = 0 ] && [ "$(gstate)" != T ] && ok "awake and playing, no shutdown" || bad "shut down or still paused after the lid opened"
grep -q 'awake (lid opened)' /data/banana/logs/sleep.log && ok "a normal wake" || bad "no wake logged"
rm -rf /sys/class/power_supply/charger
rm -f /run/banana/game.pid
stop

echo "15. a download (the Downloads tile's runner in transfer.pid): the lid shut makes it dark, the sleep waits for it"
asleep_in() { for i in $(seq $(($1 * 10))); do [ -e /run/test/wake.asleep ] && return 0; sleep 0.1; done; return 1; }
setsid sleep 600 & DL=$!; sleep 0.1; echo $DL > /run/banana/transfer.pid
start
echo 1 > /run/test/lid; ev $EV_SW $SW_LID 1; sleep 1
asleep 2>/dev/null && bad "it suspended during the download" || ok "no suspend during the download"
[ "$(cat /sys/class/backlight/backlight0/bl_power)" = 4 ] && ok "panels dark" || bad "backlight bl_power $(cat /sys/class/backlight/backlight0/bl_power)"
grep -q 'lid closed during a download' /data/banana/logs/sleep.log && ok "logged" || bad "not logged"
kill $DL; wait $DL 2>/dev/null || true      # the download done: asleep within the 5 s look
asleep_in 9 && ok "asleep once the download is done" || bad "awake after the download"
grep -q 'sleep (lid closed, the download done)' /data/banana/logs/sleep.log && ok "logged why" || bad "the sleep's reason not logged"
echo 0 > /run/test/lid; wake
stop
# (the idle sleep waits for it the same way, as for awake.pid; its whole minutes are too long for this test)
echo "   ...and with a game running, the lid sleeps as ever"
setsid sleep 600 & DL=$!; sleep 0.1; echo $DL > /run/banana/transfer.pid
echo $GAME > /run/banana/game.pid
start
echo 1 > /run/test/lid; ev $EV_SW $SW_LID 1
asleep && ok "suspended on the lid with a game running (the download is paused)" || bad "no suspend with a game running"
echo 0 > /run/test/lid; wake
kill $DL; wait $DL 2>/dev/null || true
rm -f /run/banana/game.pid /run/banana/transfer.pid
stop
echo "   ...and a runner left stopped (banana-ui went away during a game) holds nothing: the lid sleeps"
setsid sleep 600 & DL=$!; sleep 0.1; kill -STOP $DL; echo $DL > /run/banana/transfer.pid
start
echo 1 > /run/test/lid; ev $EV_SW $SW_LID 1
asleep && ok "suspended on the lid with the runner stopped" || bad "a stopped runner held the sleep"
echo 0 > /run/test/lid; wake
kill -CONT $DL; kill $DL; wait $DL 2>/dev/null || true
rm -f /run/banana/transfer.pid
stop

kill $UI $GAME 2>/dev/null || true
echo "sleep test: $pass passed, $fail failed"
[ $fail = 0 ]
