# sleep-test.sh scenarios: bananad.conf values out of range brought back into it
# (sourced by sleep-test.sh, after its own: its helpers start, stop, ev, wake, asleep, ok and bad)

echo "conf 1. power_long_ms = 0 and battery_shutdown_percent = 100: a tap sleeps, nothing shuts down"
echo 50 > /sys/class/power_supply/battery/capacity
start "autosleep_minutes = 30" "power_long_ms = 0" "battery_shutdown_percent = 100" "mem_sleep = deeep"
ev $EV_KEY $KEY_POWER 1; ev $EV_KEY $KEY_POWER 0
asleep && ok "a tap of the power button: asleep, not off" || bad "no sleep"
grep -q "\[deep\]" /sys/power/mem_sleep && ok "mem_sleep = deeep taken as deep" || bad "mem_sleep $(cat /sys/power/mem_sleep)"
wake; sleep 2.5
[ $POWEROFF = 0 ] && ok "not shut down (battery_shutdown_percent brought to 20)" || bad "shut down"
stop
POWEROFF=0
