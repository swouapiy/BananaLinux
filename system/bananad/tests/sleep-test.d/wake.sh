# sleep-test.sh scenarios: what woke it (sleep.log's "awake (...)"), a press while the suspend gets under way, and the
# safer ways after a failure lasting one sleep only
# (sourced by sleep-test.sh, after its own: its helpers start, stop, ev, wake, asleep, ok and bad)

echo "wake 1. put to sleep and woken with the power button, the lid open all along: the power button, not the lid"
echo 0 > /run/test/lid
start "autosleep_minutes = 30"
ev $EV_KEY $KEY_POWER 1; ev $EV_KEY $KEY_POWER 0
asleep && ok "asleep (the power button)" || bad "no sleep"
ev $EV_KEY $KEY_POWER 1; ev $EV_KEY $KEY_POWER 0; wake    # the press that wakes it, waiting in its device
grep -q 'awake (power button)' /data/banana/logs/sleep.log && ok "logged: $(grep 'awake (' /data/banana/logs/sleep.log | tail -n1 | cut -c21-)" || bad "woke: $(grep -o 'awake ([^)]*)' /data/banana/logs/sleep.log | tail -n1)"
stop

echo "wake 2. the same with the press's event not (yet) there: still the power button (the lid never moved)"
: > /data/banana/logs/sleep.log
start "autosleep_minutes = 30"
ev $EV_KEY $KEY_POWER 1; ev $EV_KEY $KEY_POWER 0
asleep && ok "asleep" || bad "no sleep"
wake
grep -q 'awake (power button)' /data/banana/logs/sleep.log && ok "logged the power button" || bad "woke: $(grep -o 'awake ([^)]*)' /data/banana/logs/sleep.log | tail -n1)"
stop

echo "wake 3. a power press while the suspend is under way (the hooks): called off, not lost"
: > /data/banana/logs/sleep.log
printf '#!/bin/sh\n[ "$1" = pre ] && sleep 1\nexit 0\n' > /etc/banana/sleep.d/00-test-slow; chmod +x /etc/banana/sleep.d/00-test-slow
echo $GAME > /run/banana/game.pid
start "autosleep_minutes = 30"
ev $EV_KEY $KEY_POWER 1; ev $EV_KEY $KEY_POWER 0
sleep 0.4; ev $EV_KEY $KEY_POWER 1; ev $EV_KEY $KEY_POWER 0     # pressed again before it got to sleep
sleep 1.2
[ -e /run/test/wake.asleep ] && bad "it suspended anyway" || ok "no suspend"
grep -q 'called off: the power button pressed before it suspended' /data/banana/logs/sleep.log && ok "logged: $(grep 'called off' /data/banana/logs/sleep.log | cut -c21-)" || bad "not called off"
[ "$(cat /sys/class/backlight/backlight0/bl_power)" = 0 ] && ok "panels lit" || bad "panels dark"
[ "$(gstate)" != T ] && ok "the game carries on" || bad "the game is still stopped"
rm -f /etc/banana/sleep.d/00-test-slow /run/banana/game.pid
stop

echo "wake 4. after a sleep that failed (the safer ways used), the next sleep tries deep and in parallel again"
rm -f /sys/power/state; mkdir /sys/power/state     # (writes fail)
start
echo 1 > /run/test/lid; ev $EV_SW $SW_LID 1
sleep 1.5
grep -q 'trying the safer way' /data/banana/logs/sleep.log && ok "fell back after the failure" || bad "no fallback"
echo 0 > /run/test/lid; ev $EV_SW $SW_LID 0; sleep 0.3
rmdir /sys/power/state; mkfifo /sys/power/state; exec 5<>/sys/power/state
echo "[s2idle] deep" > /sys/power/mem_sleep        # (as the kernel shows it after the fallback)
echo 1 > /run/test/lid; ev $EV_SW $SW_LID 1
asleep && ok "asleep" || bad "no sleep"
grep -q '^deep' /sys/power/mem_sleep && ok "deep chosen again" || bad "mem_sleep $(cat /sys/power/mem_sleep)"
[ "$(cat /sys/power/pm_async)" = 1 ] && ok "devices in parallel again (pm_async 1)" || bad "pm_async $(cat /sys/power/pm_async)"
echo 0 > /run/test/lid; wake
grep -q 'awake (lid opened)' /data/banana/logs/sleep.log && ok "woke: the lid" || bad "woke: $(grep -o 'awake ([^)]*)' /data/banana/logs/sleep.log | tail -n1)"
stop
