#!/bin/sh
# The host checks in CI's Linux from any machine with Docker (on a Mac: Colima or Docker Desktop). banana-ui needs
# linux/input.h, inotify and libdrm, and bananad's sleep test needs unshare, so they don't build on macOS itself.
#
#   tools/dev.sh                                  a shell, the repo at /work
#   tools/dev.sh make test                        banana-ui's headless test (PNGs in ui/tests/out, on this machine)
#   tools/dev.sh system/bananad/tests/sleep-test.sh     bananad's sleep test (the container runs as root)
#   tools/dev.sh ci                               everything CI's checks job runs, but the defconfig check
#
# The build's files stay in the repo (ui/banana-ui is a Linux binary: run it through here too).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMAGE=banana-dev
# (cached: a second after the first build, which takes a few minutes)
docker build -q -t $IMAGE "$ROOT/tools/dev" >/dev/null
[ $# = 0 ] && set -- bash
if [ "$1" = ci ]; then
    set -- sh -ec '
        make test
        make -C ui clean && make -C ui
        make -C system/bananad clean
        make -C system/bananad CFLAGS="-O2 -Werror"
        make -C system/bananad clean
        make -C system/bananad CC=aarch64-linux-gnu-gcc CFLAGS="-O2 -Werror"
        make -C system/bananad clean
        system/bananad/tests/sleep-test.sh
        for f in system/rootfs/etc/init.d/* system/rootfs/usr/bin/* system/rootfs/usr/libexec/banana/* system/rootfs/etc/banana/sleep.d/* buildroot/board/rgds/*.sh tools/*.sh; do sh -n "$f"; done
        echo "ci: all checks passed"'
fi
[ -t 0 ] && TTY=-it || TTY=-i
# --privileged: the sleep test mounts in its own namespaces
exec docker run --rm $TTY --privileged -v "$ROOT:/work" -w /work $IMAGE "$@"
