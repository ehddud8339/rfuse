#!/bin/bash

case "$1" in
first)
  sudo insmod fuse.ko
  echo first done
  exit 0
  ;;
block)
  # GUI 재시작 차단(되돌리기 쉬운 mask)
  sudo -u "$USER" systemctl --user stop xdg-document-portal.service xdg-desktop-portal.service 2>/dev/null || true
  sudo -u "$USER" systemctl --user stop 'gvfs*' 2>/dev/null || true
  sudo -u "$USER" systemctl --user stop 'snap.snap-store*' 2>/dev/null || true

  sudo -u "$USER" systemctl --user mask xdg-document-portal.service xdg-desktop-portal.service
  sudo -u "$USER" systemctl --user mask gvfs-daemon.service 'gvfs*' 'snap.snap-store*' 2>/dev/null || true
  echo "[block] user services masked."
  exit 0
  ;;
restore)
  # 차단 복구
  sudo -u "$USER" systemctl --user unmask xdg-document-portal.service xdg-desktop-portal.service gvfs-daemon.service 'gvfs*' 'snap.snap-store*'
  sudo -u "$USER" systemctl --user start xdg-document-portal.service xdg-desktop-portal.service 2>/dev/null || true
  sudo -u "$USER" systemctl --user start gvfs-daemon.service 2>/dev/null || true
  sudo -u "$USER" systemctl --user start 'snap.snap-store*' 2>/dev/null || true
  echo "[restore] user services unmasked & started."
  exit 0
  ;;
esac

# ===== 기존 동작(언로드→재로드) =====
for pid in $(sudo lsof -t /dev/fuse 2>/dev/null); do
  echo "  - killing PID $pid"
  sudo kill -9 "$pid" || true
done

# by-app 서브마운트 먼저
for mp in $(grep -E '/run/user/[0-9]+/doc/by-app/' /proc/*/mountinfo 2>/dev/null | awk '{print $5}'); do
  echo "  - umount $mp"
  sudo umount -l "$mp" || true
done

# 일반 FUSE 마운트
for mp in $(mount | grep fuse | awk '{print $3}'); do
  echo "  - umount $mp"
  sudo umount -l "$mp" || true
done

# 남은 FUSE 연결 강제 abort
for c in /sys/fs/fuse/connections/*; do
  [ -e "$c" ] || continue
  echo "  - abort connection $(basename "$c")"
  echo 1 | sudo tee "$c/abort" >/dev/null || true
done

sudo rmmod fuse || true
sudo insmod fuse.ko
echo done
