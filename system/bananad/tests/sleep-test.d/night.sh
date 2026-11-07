# sleep-test.sh scenarios: the night test (usr/libexec/banana/night-test and bananad's hook for it), sourced by
# sleep-test.sh after its own: its helpers start, stop, ev, wake, asleep, ok and bad
NT=$HERE/../rootfs/usr/libexec/banana/night-test
echo 0 > /run/test/lid

echo "night 1. bananad's hook: the test's alarm, its wake a full one (the lid shut too), the next sleep on request"
start
echo 1 > /run/test/lid; echo 7 > /run/banana/night-test
: > /run/banana/night-test-sleep; kill -HUP $BD                        # (as the helper does)
asleep && ok "asleep on the test's request" || bad "no sleep on /run/banana/night-test-sleep"
[ ! -e /run/banana/night-test-sleep ] && ok "the request used up" || bad "the request left"
[ "$(cat /sys/class/rtc/rtc0/wakealarm)" = +7 ] && ok "the test's alarm (+7), not the battery check's" || bad "wakealarm $(cat /sys/class/rtc/rtc0/wakealarm)"
: > /sys/class/rtc/rtc0/wakealarm; wake
sleep 6.5                                                              # (past the lid's 5 s: the test holds it off)
[ ! -e /run/test/wake.asleep ] && ok "awake with the lid shut (a full wake, held 6 s)" || bad "back asleep with the lid shut"
grep -q 'awake (night test)' /data/banana/logs/sleep.log && ok "sleep.log: awake (night test)" || bad "no 'awake (night test)' line"
: > /run/banana/night-test-sleep; kill -HUP $BD
asleep && ok "asleep again on the next request" || bad "no second sleep"
rm -f /run/banana/night-test; echo 0 > /run/test/lid; wake
stop

echo "night 2. the helper, the phases in seconds: wakes, asleep with the light and without, its report, the light put back"
: > /data/banana/logs/night-test.txt
echo "led_sleep = on" > /data/banana/bananad.conf
start
NT_STRESS=9 NT_EVERY=2 NT_SLEEP=3 NT_NOOFF=1 sh $NT run & NP=$!
# (the "RTC": each sleep woken by its alarm)
for i in $(seq 400); do
    kill -0 $NP 2>/dev/null || break
    if [ -e /run/test/wake.asleep ]; then : > /sys/class/rtc/rtc0/wakealarm; printf x >&6; sleep 0.3; fi
    sleep 0.1
done
kill -0 $NP 2>/dev/null && { bad "the helper still running after 40 s"; kill $NP; }
grep -q 'wake stress: [1-9][0-9]* wakes, 0 failed, suspends failed 0' /data/banana/logs/night-test.txt && ok "report: $(grep 'wake stress:' /data/banana/logs/night-test.txt | cut -c21-)" ||
    bad "report: $(cat /data/banana/logs/night-test.txt)"
grep -q 'asleep, the light on: ' /data/banana/logs/night-test.txt && grep -q 'asleep, the light off: ' /data/banana/logs/night-test.txt &&
    ok "both sleep phases reported" || bad "sleep phases missing"
grep -q 'turned off at' /data/banana/logs/night-test.txt && ok "the end reported" || bad "no end"
[ "$(sed -n 's/^led_sleep = //p' /data/banana/bananad.conf)" = on ] && ok "the light setting put back (on)" || bad "bananad.conf: $(cat /data/banana/bananad.conf)"
[ ! -e /run/banana/night-test ] && [ ! -e /run/banana/night-test.pid ] && ok "its files gone" || bad "files left: $(ls /run/banana | grep night)"
stop
rm -f /data/banana/bananad.conf
