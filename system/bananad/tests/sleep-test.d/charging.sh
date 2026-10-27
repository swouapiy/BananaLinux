# Charging while off: rcS's "bananad --charge" (a charger's start, or turned off with the charger in), sourced by
# sleep-test.sh after its own scenarios (marks made with ": >": the sleep test's touch() is a touchscreen's). Only the LEDs on; the power button held starts it, the charger out turns it off.
mount -t tmpfs none /sys/devices/system/cpu     # (the made-up CPUs: the test's machine keeps its own online)
mkdir -p /sys/devices/system/cpu/cpufreq/policy0
echo performance > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor
for c in 1 2 3; do mkdir -p /sys/devices/system/cpu/cpu$c; echo 1 > /sys/devices/system/cpu/cpu$c/online; done
L=/sys/class/leds; CH=/sys/class/power_supply/charger
mkdir -p $CH; echo USB > $CH/type; echo 0 > $CH/online
echo 1 > /sys/class/backlight/backlight0/bl_power; echo 0 > /sys/class/backlight/backlight0/bl_power
charge() { r=0; "$W/bananad" --charge || r=$?; }
# the background one's end, up to 6 s (then stopped: a FAIL, not a hung test), its exit status in r
finish() { for i in $(seq 60); do kill -0 $CB 2>/dev/null || break; sleep 0.1; done; kill $CB 2>/dev/null || true; r=0; wait $CB || r=$?; }

echo "charging 1. not a charger's start (no mark, no PMIC to ask), or no charger: on with the boot at once"
charge; [ $r = 1 ] && ok "no charger's start: exit 1" || bad "exit $r"
: > /data/banana/.charge-next; charge
[ $r = 1 ] && ok "turned off with no charger in: exit 1" || bad "exit $r"
[ "$(cat /sys/devices/system/cpu/cpu1/online)" = 1 ] && ok "nothing changed (the CPUs)" || bad "cpu1 offline"

echo "charging 2. a restart's mark wins over the charger: a restart isn't a charger's start"
echo 1 > $CH/online
: > /data/banana/.restart; : > /data/banana/.charge-next; charge
[ $r = 1 ] && ok "a restart: exit 1" || bad "exit $r"
[ ! -e /data/banana/.restart ] && [ ! -e /data/banana/.charge-next ] && ok "both marks used up" || bad "marks left: $(ls -a /data/banana)"

echo "charging 3. turned off with the charger in: the panels dark, amber, asleep; a short press sleeps on, held starts it"
: > /data/banana/.charge-next
ev $EV_KEY $KEY_POWER 1; ev $EV_KEY $KEY_POWER 0      # an old press, waiting: not for the charging
BANANAD_TEST_WAKE=/run/test/wake "$W/bananad" --charge & CB=$!
asleep && ok "asleep while charging (an old press didn't start it)" || bad "not asleep"
[ -e /run/banana/charging ] && ok "/run/banana/charging while it charges" || bad "no /run/banana/charging"
[ "$(cat $L/amber:charging/brightness)" = 255 ] && [ "$(cat $L/green:power/brightness)" = 0 ] && ok "amber on at full (lit asleep), green off" ||
    bad "amber $(cat $L/amber:charging/brightness), green $(cat $L/green:power/brightness)"
[ "$(cat /sys/class/rtc/rtc0/wakealarm)" = +600 ] && ok "a look in 10 min (+600)" || bad "wakealarm $(cat /sys/class/rtc/rtc0/wakealarm)"
[ "$(cat /sys/class/backlight/backlight0/bl_power)" = 4 ] && [ "$(cat /sys/class/backlight/backlight1/bl_power)" = 4 ] && ok "both panels dark" || bad "bl_power not 4"
[ "$(cat /sys/devices/system/cpu/cpu1/online)$(cat /sys/devices/system/cpu/cpu3/online)" = 00 ] &&
    [ "$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_governor)" = powersave ] && ok "one CPU, powersave (for its looks)" || bad "CPUs as they were"
ev $EV_KEY $KEY_POWER 1; wake; sleep 0.3; ev $EV_KEY $KEY_POWER 0     # a short press wakes it
for i in $(seq 40); do [ -e /run/test/wake.asleep ] && break; sleep 0.1; done
asleep && ok "a short press: asleep again, still charging" || bad "awake after a short press"
kill -0 $CB 2>/dev/null || bad "a short press started it"
ev $EV_KEY $KEY_POWER 1; wake; sleep 1.4                              # held: it starts
finish
ev $EV_KEY $KEY_POWER 0
[ $r = 0 ] && ok "held a second: exit 0, the boot goes on" || bad "exit $r"
[ "$(cat /sys/devices/system/cpu/cpu1/online)" = 1 ] && ok "the CPUs back" || bad "cpu1 still offline"
[ ! -e /run/banana/charging ] && ok "/run/banana/charging gone" || bad "/run/banana/charging left"
grep -q "charging while off (turned off with the charger in): battery [0-9]*%" /data/banana/logs/charging.log &&
    grep -q "the power button: starting" /data/banana/logs/charging.log && ok "charging.log" || bad "charging.log: $(cat /data/banana/logs/charging.log)"

echo "charging 4. full: green instead of amber at the 10-minute look; the charger pulled out turns it off"
: > /data/banana/.charge-next
BANANAD_TEST_WAKE=/run/test/wake "$W/bananad" --charge & CB=$!
asleep && ok "asleep" || bad "not asleep"
echo 100 > /sys/class/power_supply/battery/capacity
: > /sys/class/rtc/rtc0/wakealarm; wake                               # the look: full meanwhile
asleep && ok "asleep again after the look" || bad "awake after the look"
[ "$(cat $L/green:power/brightness)" = 255 ] && [ "$(cat $L/amber:charging/brightness)" = 0 ] && ok "full: green on, amber off" ||
    bad "green $(cat $L/green:power/brightness), amber $(cat $L/amber:charging/brightness)"
grep -q "full after" /data/banana/logs/charging.log && ok "charging.log: full" || bad "full not logged"
echo 0 > $CH/online; wake                                             # the cable out woke it
finish
[ $r = 3 ] && ok "the charger out: exit 3 (rcS turns it off)" || bad "exit $r"
[ "$(cat $L/green:power/brightness)$(cat $L/amber:charging/brightness)" = 00 ] && ok "the LEDs off" || bad "LEDs left on"
echo 50 > /sys/class/power_supply/battery/capacity; rm -rf $CH
umount /sys/devices/system/cpu
