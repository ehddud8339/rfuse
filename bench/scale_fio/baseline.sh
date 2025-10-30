#!/usr/bin/env bash
set -euo pipefail

CPUS=("0" "0-4" "0-9" "0-19" "0-39")
POLICY=("shared" "split")
WORKLOADS=("read" "randread" "write" "randwrite")
THREADS=("1" "2" "4" "8" "16" "32")
BS="4K"

LOGDIR="/home/ldy/src/ldy/rfuse/log_files/rfuse"
SCRIPT="./fio_scripts/basic.fio"

for cpu in ${CPUS[@]}; do
  RESULT="${LOGDIR}/cpus_${cpu}"
  mkdir -p ${RESULT}
  for bs in ${BS[@]}; do
    for workload in ${WORKLOADS[@]}; do
      for thread in ${THREADS[@]}; do
        sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"

        sudo fio ${SCRIPT} \
          --section=${workload} \
          --numjobs=${thread} --bs=${bs} \
          --cpus_allowed=${cpu} --cpus_allowed_policy=shared \
          --output="${RESULT}/${workload}_bs${BS}_njs${thread}.log"
      done
    done
  done
done
