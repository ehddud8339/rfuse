#!/usr/bin/env bash
set -euo pipefail

OUT_DIR="/tmp/RFUSE"
USER_LOG="${OUT_DIR}/user-log.txt"

usage() {
  cat <<'EOF'
Usage:
  rfuse-log-merge.sh <output_filename>

Description:
  - Read kernel logs from `dmesg` and keep only lines containing `rfuse-lat`
  - Read userspace log from `/tmp/RFUSE/user-log.txt`
  - Merge two streams by numeric `ts=` and write:
      /tmp/RFUSE/<output_filename>
EOF
}

if [[ $# -ne 1 ]]; then
  usage >&2
  exit 1
fi

out_name="$1"
if [[ -z "$out_name" || "$out_name" == */* ]]; then
  echo "output_filename must be a plain file name (no '/')." >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
out_path="${OUT_DIR}/${out_name}"

tmp_k="$(mktemp)"
tmp_m="$(mktemp)"
trap 'rm -f "$tmp_k" "$tmp_m"' EXIT

dmesg | grep 'rfuse-lat' > "$tmp_k" || true

{
  awk '
    {
      if (match($0, /ts=[0-9]+/)) {
        ts = substr($0, RSTART + 3, RLENGTH - 3);
        print ts "\tK\t" $0;
      }
    }
  ' "$tmp_k"

  if [[ -f "$USER_LOG" ]]; then
    awk '
      {
        if (match($0, /ts=[0-9]+/)) {
          ts = substr($0, RSTART + 3, RLENGTH - 3);
          print ts "\tU\t" $0;
        }
      }
    ' "$USER_LOG"
  fi
} | sort -n -k1,1 > "$tmp_m"

awk -F'\t' '{print "[" $2 "] " $3}' "$tmp_m" > "$out_path"
sudo dmesg -C
echo "$out_path"
