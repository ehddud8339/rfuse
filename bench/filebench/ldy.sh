#!/bin/bash

set -euo pipefail

# ===== User Config =====
FS_TYPE=("ext4" "fuse" "rfuse")
FS_PATH="../../filesystems/stackfs"
DEVICE_NAME=("/dev/nvme1n1")

MOUNT_BASE="/mnt/RFUSE_EXT4"
MOUNT_POINT="/mnt/test"

RESULT_DIR="/home/ldy/rfuse"

# ===== Helpers =====
function drop_all_caches() {
  sudo sh -c 'sync'
  sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
}

function kill_stackfs_if_running() {
  set +e
  pkill -f "StackFS_ll" 2>/dev/null
  sleep 1
  set -e
}

# ===== Mount helpers (from your base) =====
function init_mount_point() {
  local fs=$1
  local dev=$2

  drop_all_caches

  if [ "${fs}" == "ext4" ]; then
    set +e
    echo "Unmount ext4..."
    sudo umount "${MOUNT_POINT}" 2>/dev/null
    set -e

    echo "Mount ext4 (mkfs fresh)..."
    sudo mkfs.ext4 -F -E lazy_itable_init=0,lazy_journal_init=0 "${dev}"
    sudo mount "${dev}" "${MOUNT_POINT}"
    sudo sync

  elif [ "${fs}" == "fuse" ]; then
    set +e
    echo "Unmount fuse-stackfs..."
    sudo umount "${MOUNT_POINT}" 2>/dev/null

    echo "Unmount ext4..."
    sudo umount "${MOUNT_BASE}" 2>/dev/null
    set -e

    echo "Mount ext4 (mkfs fresh for lower fs)..."
    sudo mkfs.ext4 -F -E lazy_itable_init=0,lazy_journal_init=0 "${dev}"
    sudo mount "${dev}" "${MOUNT_BASE}"

    echo "Mount fuse-stackfs..."
    pushd "${FS_PATH}" >/dev/null

    cp StackFS_LowLevel.c.fuse StackFS_LowLevel.c
    make clean
    make
    ./StackFS_ll -r "${MOUNT_BASE}" "${MOUNT_POINT}" &
    popd >/dev/null
    sudo sync

  elif [ "${fs}" == "rfuse" ]; then
    set +e
    echo "Unmount rfuse-stackfs..."
    sudo umount "${MOUNT_POINT}" 2>/dev/null

    echo "Unmount ext4..."
    sudo umount "${MOUNT_BASE}" 2>/dev/null
    set -e

    echo "Mount ext4 (mkfs fresh for lower fs)..."
    sudo mkfs.ext4 -F -E lazy_itable_init=0,lazy_journal_init=0 "${dev}"
    sudo mount "${dev}" "${MOUNT_BASE}"

    echo "Mount rfuse-stackfs..."
    pushd "${FS_PATH}" >/dev/null

    cp StackFS_LowLevel.c.rfuse StackFS_LowLevel.c
    make clean
    make
    ./StackFS_ll -r "${MOUNT_BASE}" "${MOUNT_POINT}" &
    popd >/dev/null
    sudo sync

  else
    echo "Wrong file system type: ( ext4 | fuse | rfuse )"
    exit 1
  fi

  echo "Waiting Initialization..."
  sleep 60
}

function remount_point() {
  local fs=$1
  local dev=$2

  drop_all_caches

  if [ "${fs}" == "ext4" ]; then
    set +e
    echo "Unmount ext4..."
    sudo umount "${MOUNT_POINT}" 2>/dev/null
    set -e

    echo "Mount ext4..."
    sudo mount "${dev}" "${MOUNT_POINT}"
    sudo sync

  elif [ "${fs}" == "fuse" ]; then
    set +e
    echo "Unmount fuse-stackfs..."
    sudo umount "${MOUNT_POINT}" 2>/dev/null

    echo "Unmount ext4..."
    sudo umount "${MOUNT_BASE}" 2>/dev/null
    set -e

    echo "Mount ext4..."
    sudo mount "${dev}" "${MOUNT_BASE}"

    echo "Mount fuse-stackfs..."
    pushd "${FS_PATH}" >/dev/null
    cp StackFS_LowLevel.c.fuse StackFS_LowLevel.c
    make clean
    make
    ./StackFS_ll -r "${MOUNT_BASE}" "${MOUNT_POINT}" &
    popd >/dev/null
    sudo sync

  elif [ "${fs}" == "rfuse" ]; then
    set +e
    echo "Unmount rfuse-stackfs..."
    sudo umount "${MOUNT_POINT}" 2>/dev/null

    echo "Unmount ext4..."
    sudo umount "${MOUNT_BASE}" 2>/dev/null
    set -e

    echo "Mount ext4..."
    sudo mount "${dev}" "${MOUNT_BASE}"

    echo "Mount rfuse-stackfs..."
    pushd "${FS_PATH}" >/dev/null

    cp StackFS_LowLevel.c.rfuse StackFS_LowLevel.c
    make clean
    make
    ./StackFS_ll -r "${MOUNT_BASE}" "${MOUNT_POINT}" &
    popd >/dev/null
    sudo sync

  else
    echo "Wront file system type: ( ext4 | fuse | rfuse )"
    exit 1
  fi

  echo "Waiting Initialization..."
  sleep 60
}

function change_driver() {
  local fs=$1

  drop_all_caches

  if [ "${fs}" == "ext4" ]; then
    echo "ext4 doesn't need driver"

  elif [ "${fs}" == "fuse" ]; then
    echo "Change to fuse driver"
    pushd "../../lib/libfuse" >/dev/null
    ./libfuse_install.sh
    popd >/dev/null

    pushd "../../driver/fuse" >/dev/null
    ./fuse_insmod.sh
    popd >/dev/null

  elif [ "${fs}" == "rfuse" ]; then
    echo "Change to rfuse driver"
    pushd "../../lib/librfuse" >/dev/null
    ./librfuse_install.sh
    popd >/dev/null

    pushd "../../driver/rfuse" >/dev/null
    ./rfuse_insmod.sh
    popd >/dev/null

  else
    echo "Wront file system type: ( ext4 | fuse | rfuse )"
    exit 1
  fi
}

# ===== Main =====
mkdir -p "${RESULT_DIR}"

echo 0 | sudo tee /proc/sys/kernel/randomize_va_space

# (FS) → (section → prefill → (bs → (numjobs))) → next FS   # 1.4
for fs in "${FS_TYPE[@]}"; do
  echo "==== FS: ${fs} ===="
  change_driver "${fs}"

  # FS 별 결과 디렉토리
  fs_outdir="${RESULT_DIR}/${fs}"
  mkdir -p "${fs_outdir}"

  init_mount_point "${fs}" "${DEVICE_NAME[0]}"
  filebench -f workloads/fileserver.f | tee ${fs_outdir}/fileserver.log

  init_mount_point "${fs}" "${DEVICE_NAME[0]}"
  filebench -f workloads/webserver.f | tee ${fs_outdir}/webserver.log

  set +euo pipefail
  sudo umount ${MOUNT_POINT}

  sudo umount ${MOUNT_BASE}
  set -euo pipefail
  echo "==== FS done: ${fs} ===="
done

echo "All experiments completed. Results in: ${RESULT_DIR}"
