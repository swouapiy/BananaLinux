# sleep-test.sh scenarios: the power button with the lid shut (sourced by sleep-test.sh, after its own: its helpers
# start, stop, ev, wake, asleep, ok and bad)

echo "lid 1. asleep with the lid shut, the power button held as long as a long press: off, the game saving first"
echo $GAME > /run/banana/game.pid
start "power_long_ms = 600"
echo 1 > /run/test/lid; ev $EV_SW $SW_LID 1
asleep && ok "asleep (the lid)" || bad "no sleep"
ev $EV_KEY $KEY_POWER 1; wake; sleep 0.8
for i in $(seq 30); do [ $POWEROFF = 1 ] && break; sleep 0.1; done
ev $EV_KEY $KEY_POWER 0
[ $POWEROFF = 1 ] && ok "turned off (init asked to power off)" || bad "not turned off"
kill -0 $GAME 2>/dev/null && bad "the game wasn't told to quit and save" || ok "the game was told to quit and save first"
grep -q 'the power button held with the lid shut: turning off' /data/banana/logs/sleep.log && ok "sleep.log says so" || bad "not logged"
stop
POWEROFF=0; echo 0 > /run/test/lid
setsid sleep 600 & GAME=$!; sleep 0.1; echo $GAME > /run/banana/game.pid

echo "lid 2. asleep with the lid shut, a short press: back to sleep, not off"
start "power_long_ms = 600"
echo 1 > /run/test/lid; ev $EV_SW $SW_LID 1
asleep && ok "asleep (the lid)" || bad "no sleep"
ev $EV_KEY $KEY_POWER 1; wake; ev $EV_KEY $KEY_POWER 0
asleep && ok "asleep again after the short press" || bad "awake after a short press with the lid shut"
sleep 0.8
[ $POWEROFF = 0 ] && ok "not turned off" || bad "a short press turned it off"
echo 0 > /run/test/lid; wake
stop
POWEROFF=0
