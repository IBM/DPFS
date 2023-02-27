#!/bin/bash
FIO_DEFAULTS="--filename_format ${MNT}/fio-5g-\$jobnum --size 5G --direct=1 --ioengine=io_uring --thread=1 --group_reporting --time_based --random_generator=tausworthe64 --norandommap=1"

# Default runtime is 30s
RUNTIME="${RUNTIME:-60s}"

fio $FIO_DEFAULTS --ramp_time=10s --runtime=$RUNTIME --bs=$BS --rw=$RW --iodepth=$IODEPTH --numjobs=$P --name="${RW}_${BS}_${IODEPTH}_${P}"
