#!/bin/bash

set -euo pipefail

# ===== User Config =====
#FS_TYPE=("ext4" "fuse" "rfuse")
FS_TYPE=("rfuse_Ecore")
FS_PATH="../../filesystems/stackfs"
DEVICE_NAME=("/dev/nvme1n1")

MOUNT_BASE="/mnt/RFUSE_EXT4"
MOUNT_POINT="/mnt/test"

SECTIONS=("read" "randread" "write" "randwrite") # 1.1
BS_LIST=("4k" "128k")                            # 1.2
NUM_JOBS=("1" "2" "4" "8" "16" "32")             # 1.3
FIO_BASE="fio_scripts/basic.fio"
RESULT_DIR="/home/ldy/src/ldy/rfuse"

# ===== Helpers =====
function drop_all_caches() {
  sudo sh -c 'sync'
  sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
  sleep 3
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
    cp /home/ldy/src/rfuse/filesystems/stackfs/StackFS_LowLevel.c.fuse /home/ldy/src/rfuse/filesystems/stackfs/StackFS_LowLevel.c
    make clean
    make
    ./StackFS_ll -r "${MOUNT_BASE}" "${MOUNT_POINT}" &
    popd >/dev/null
    sudo sync

  else
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
    cp /home/ldy/src/rfuse/filesystems/stackfs/StackFS_LowLevel.c.rfuse /home/ldy/src/rfuse/filesystems/stackfs/StackFS_LowLevel.c
    make clean
    make
    ./StackFS_ll -r "${MOUNT_BASE}" "${MOUNT_POINT}" &
    popd >/dev/null
    sudo sync
  fi

  echo "Waiting Initialization..."
  sleep 15
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
    cp /home/ldy/src/rfuse/filesystems/stackfs/StackFS_LowLevel.c.fuse /home/ldy/src/rfuse/filesystems/stackfs/StackFS_LowLevel.c
    make clean
    make
    ./StackFS_ll -r "${MOUNT_BASE}" "${MOUNT_POINT}" &
    popd >/dev/null
    sudo sync

  else
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
    cp /home/ldy/src/rfuse/filesystems/stackfs/StackFS_LowLevel.c.rfuse /home/ldy/src/rfuse/filesystems/stackfs/StackFS_LowLevel.c
    make clean
    make
    ./StackFS_ll -r "${MOUNT_BASE}" "${MOUNT_POINT}" &
    popd >/dev/null
    sudo sync

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
    cp /home/ldy/src/rfuse/driver/copy/rfuse_dev_basic.c /home/ldy/src/rfuse/driver/rfuse/rfuse_dev.c
    make clean
    make
    ./rfuse_insmod.sh
    popd >/dev/null

  elif [ "${fs}" == "rfuse_rr" ]; then
    echo "Change to rfuse driver"
    pushd "../../lib/librfuse" >/dev/null
    ./librfuse_install.sh
    popd >/dev/null

    pushd "../../driver/rfuse" >/dev/null
    cp /home/ldy/src/rfuse/driver/copy/rfuse_dev_rr.c /home/ldy/src/rfuse/driver/rfuse/rfuse_dev.c
    make clean
    make
    ./rfuse_insmod.sh
    popd >/dev/null

  elif [ "${fs}" == "rfuse_thr" ]; then
    echo "Change to rfuse driver"
    pushd "../../lib/librfuse" >/dev/null
    ./librfuse_install.sh
    popd >/dev/null

    pushd "../../driver/rfuse" >/dev/null
    cp /home/ldy/src/rfuse/driver/copy/rfuse_dev_thr.c /home/ldy/src/rfuse/driver/rfuse/rfuse_dev.c
    make clean
    make
    ./rfuse_insmod.sh
    popd >/dev/null
  elif [ "${fs}" == "rfuse_busy" ]; then
    echo "Change to rfuse driver"
    pushd "../../lib/librfuse" >/dev/null
    ./librfuse_install.sh
    popd >/dev/null

    pushd "../../driver/rfuse" >/dev/null
    cp /home/ldy/src/rfuse/driver/copy/rfuse_dev_busy.c /home/ldy/src/rfuse/driver/rfuse/rfuse_dev.c
    make clean
    make
    ./rfuse_insmod.sh
    popd >/dev/null
  elif [ "${fs}" == "rfuse_Ecore" ]; then
    echo "Change to rfuse driver"
    pushd "../../lib/librfuse" >/dev/null
    ./librfuse_install.sh
    popd >/dev/null

    pushd "../../driver/rfuse" >/dev/null
    cp /home/ldy/src/rfuse/driver/copy/rfuse_dev_Ecore.c /home/ldy/src/rfuse/driver/rfuse/rfuse_dev.c
    make clean
    make
    ./rfuse_insmod.sh
    popd >/dev/null

  else
    echo "Wront file system type: ( ext4 | fuse | rfuse | rfuse_busy )"
    exit 1
  fi
}

# ===== FIO runners =====
# section 반복 전, 해당 section의 최대 numjobs를 실행하여 파일을 생성 (프리필)
# - read/randread의 경우에도 실제 파일 생성이 필요하므로 --rw=write로 오버라이드
function prefill_for_section() {
  echo "[BG] Prefill (numjobs=32)"

  drop_all_caches

  # 프리필은 빠르게 끝내기 위해 큰 bs 사용(128k), rw=write로 강제
  fio "${FIO_BASE}" \
    --directory="${MOUNT_POINT}" \
    --section="write" \
    --bs=1M \
    --numjobs=32 \
    --eta=never \
    --output="/dev/null"
}

# 실험 1회 실행 (로그 파일명은 [section]_[bs]_[numjobs].log 로 저장)
function run_one_fio() {
  local section=$1
  local bs=$2
  local nj=$3
  local outdir=$4

  local outfile="${outdir}/${section}_${bs}_${nj}.log"
  echo "Run: section=${section}, bs=${bs}, numjobs=${nj} -> ${outfile}"

  drop_all_caches

  fio "${FIO_BASE}" \
    --directory="${MOUNT_POINT}" \
    --section="${section}" \
    --bs="${bs}" \
    --numjobs="${nj}" \
    --eta=never \
    --output="${outfile}"
}

# ===== Main =====
mkdir -p "${RESULT_DIR}"

# (FS) → (section → prefill → (bs → (numjobs))) → next FS   # 1.4
for fs in "${FS_TYPE[@]}"; do
  echo "==== FS: ${fs} ===="
  change_driver "${fs}"
  init_mount_point "${fs}" "${DEVICE_NAME[0]}"

  # FS 별 결과 디렉토리
  fs_outdir="${RESULT_DIR}/${fs}"
  mkdir -p "${fs_outdir}"

  prefill_for_section

  drop_all_caches

  for section in "${SECTIONS[@]}"; do
    echo "== Section: ${section} =="

    # 1.5 섹션 반복 전 프리필(최대 numjobs로 파일 생성)
    # prefill_for_section "${fs}" "${section}" "${fs_outdir}"

    # 1.4 섹션 반복 안에서 (bs → numjobs) 순서
    for bs in "${BS_LIST[@]}"; do
      for nj in "${NUM_JOBS[@]}"; do
        run_one_fio "${section}" "${bs}" "${nj}" "${fs_outdir}"
      done
    done
  done

  echo "==== FS done: ${fs} ===="
done

echo "All experiments completed. Results in: ${RESULT_DIR}"
