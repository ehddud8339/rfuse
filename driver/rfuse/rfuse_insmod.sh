#!/bin/bash

if [ "$1" == "first" ]; then
  sudo insmod fuse.ko
  echo first done
else
  for pid in $(sudo lsof -t /dev/fuse 2>/dev/null); do
    echo "  - killing PID $pid"
    sudo kill -9 "$pid" || true
  done

  for mp in $(grep -E '/run/user/[0-9]+/doc/by-app/' /proc/*/mountinfo 2>/dev/null | awk '{print $5}'); do
    echo "  - umount $mp"
    sudo umount -l "$mp" || true
  done

  for mp in $(mount | grep fuse | awk '{print $3}'); do
    echo "  - umount $mp"
    sudo umount -l "$mp" || true
  done

  for c in /sys/fs/fuse/connections/*; do
    [ -e "$c" ] || continue
    echo "  - abort connection $(basename "$c")"
    echo 1 | sudo tee "$c/abort" >/dev/null || true
  done

  sudo rmmod fuse || true
  sudo insmod fuse.ko
  echo done
fi
