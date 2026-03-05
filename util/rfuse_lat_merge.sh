#!/usr/bin/env bash
set -euo pipefail

LOG_DIR="${HOME}/ehddud8339.storage/logs/RFUSE"

usage() {
  cat <<'USAGE'
Usage:
  rfuse_lat_merge.sh <user_log_filename> <merged_log_filename>

Description:
  1) Read current dmesg and extract lines containing "rfuse-lat"
  2) Read userspace log file
  3) Merge both logs by numeric "ts=" field and write one merged file
  4) Clear kernel log buffer with: sudo dmesg -C

Both input/output files are resolved under:
  ~/ehddud8339.storage/logs/RFUSE
USAGE
}

if [[ $# -ne 2 ]]; then
  usage >&2
  exit 1
fi

user_name="$1"
merged_name="$2"

if [[ -z "$user_name" || "$user_name" == */* ]]; then
  echo "user_log_filename must be a plain file name (no '/')." >&2
  exit 1
fi

if [[ -z "$merged_name" || "$merged_name" == */* ]]; then
  echo "merged_log_filename must be a plain file name (no '/')." >&2
  exit 1
fi

mkdir -p "$LOG_DIR"

user_log="${LOG_DIR}/${user_name}"
merged_log="${LOG_DIR}/${merged_name}"

if [[ ! -f "$user_log" ]]; then
  echo "Userspace log not found: $user_log" >&2
  exit 1
fi

tmp_kernel="$(mktemp)"
tmp_merge="$(mktemp)"
trap 'rm -f "$tmp_kernel" "$tmp_merge"' EXIT

echo "[rfuse-lat] collecting kernel logs from dmesg..." >&2
sudo dmesg | grep 'rfuse-lat' > "$tmp_kernel" || true

echo "[rfuse-lat] merging logs by ts=... into: $merged_log" >&2
{
  awk '
    {
      if (match($0, /ts=[0-9]+/)) {
        ts = substr($0, RSTART + 3, RLENGTH - 3);
        print ts "\tK\t" $0;
      }
    }
  ' "$tmp_kernel"

  awk '
    {
      if (match($0, /ts=[0-9]+/)) {
        ts = substr($0, RSTART + 3, RLENGTH - 3);
        print ts "\tU\t" $0;
      }
    }
  ' "$user_log"
} | sort -n -k1,1 > "$tmp_merge"

awk -F'\t' '{print "[" $2 "] " $3}' "$tmp_merge" > "$merged_log"

echo "[rfuse-lat] clearing dmesg buffer..." >&2
sudo dmesg -C

echo "[rfuse-lat] done. output: $merged_log" >&2
