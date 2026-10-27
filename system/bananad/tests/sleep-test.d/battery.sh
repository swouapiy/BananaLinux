# sleep-test.sh scenarios: what a sleep cost the battery (sleep.d/95-battery), on a wake and on a turn-off from sleep
# (sourced by sleep-test.sh, after its own: its helpers start, stop, ev, wake, asleep, ok and bad)
B=/sys/class/power_supply/battery
echo 50 > $B/capacity; echo 2000000 > $B/charge_now; echo 3800000 > $B/voltage_avg
: > /data/banana/logs/sleep.log

echo "battery 1. a sleep of an hour, woken: the charge used and the average current in sleep.log"
start
echo 1 > /run/test/lid; ev $EV_SW $SW_LID 1
asleep && ok "asleep" || bad "no sleep"
read -r t0 rest < /run/banana/sleep-battery && echo "$((t0 - 3600)) $rest" > /run/banana/sleep-battery   # (an hour ago)
echo 1990000 > $B/charge_now; echo 49 > $B/capacity                  # 10 mAh used
echo 0 > /run/test/lid; wake; sleep 1.5                               # (post runs in the background)
grep -q 'battery while asleep: 50% 3800 mV -> 49% 3800 mV, 10 mAh in 1.0 h: 10.0 mA on average$' /data/banana/logs/sleep.log &&
    ok "logged: $(grep 'battery while asleep' /data/banana/logs/sleep.log | tail -n1 | cut -c21-)" || bad "not logged: $(grep 'battery while' /data/banana/logs/sleep.log)"
stop

echo "battery 2. turned off from sleep (the battery check): logged too, with ', then turned off'"
echo 2000000 > $B/charge_now; echo 4 > $B/capacity
start "sleep_check_hours = 0.002" "sleep_poweroff_percent = 5"
echo 1 > /run/test/lid; ev $EV_SW $SW_LID 1
asleep && ok "asleep" || bad "no sleep"
read -r t0 rest < /run/banana/sleep-battery && echo "$((t0 - 7200)) $rest" > /run/banana/sleep-battery   # (two hours ago)
echo 1980000 > $B/charge_now; echo 3 > $B/capacity                   # 20 mAh used
: > /sys/class/rtc/rtc0/wakealarm; wake                               # the battery check's alarm
for i in $(seq 30); do [ $POWEROFF = 1 ] && break; sleep 0.1; done
[ $POWEROFF = 1 ] && ok "turned off at 3%" || bad "not turned off"
[ -e /run/banana/quiet-shutdown ] && ok "marked for banana-ui: no shutdown animation from sleep" || bad "no /run/banana/quiet-shutdown"
rm -f /run/banana/quiet-shutdown
grep -q 'battery while asleep: 4% 3800 mV -> 3% 3800 mV, 20 mAh in 2.0 h: 10.0 mA on average, then turned off$' /data/banana/logs/sleep.log &&
    ok "logged: $(grep 'then turned off' /data/banana/logs/sleep.log | tail -n1 | cut -c21-)" || bad "not logged: $(grep 'battery while' /data/banana/logs/sleep.log | tail -n1)"
stop
POWEROFF=0
echo 50 > $B/capacity
setsid sleep 600 & GAME=$!; sleep 0.1; echo $GAME > /run/banana/game.pid
