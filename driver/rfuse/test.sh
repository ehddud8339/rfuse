echo "== fuse module =="
lsmod | grep '^fuse' || echo "(not loaded)"

echo "== mounts (fuse/fusectl) =="
mount | grep -E ' fuse|fusectl' || echo "(none)"

echo "== fuse connections =="
ls /sys/fs/fuse/connections 2>/dev/null || echo "(none)"

echo "== processes using /dev/fuse =="
lsof -nP /dev/fuse 2>/dev/null || echo "(none)"

echo "== fuser on /dev/fuse =="
fuser -v /dev/fuse 2>/dev/null || true

echo "== by-app mounts from /proc/mountinfo =="
grep -E '/run/user/[0-9]+/doc' /proc/*/mountinfo 2>/dev/null |
  sed -E 's|^/proc/([0-9]+)/.*|\1|' | sort -u |
  while read -r p; do
    comm=$(cat /proc/$p/comm 2>/dev/null)
    exe=$(readlink -f /proc/$p/exe 2>/dev/null)
    echo "PID=$p COMM=$comm EXE=$exe"
  done
