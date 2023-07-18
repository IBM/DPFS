#!/bin/bash

# Default size of file is
SIZE="${SIZE:-1g}"
# Default run time is
UNTIME="${RUNTIME:-60s}"

FIO_DEFAULTS="--ramp_time=10s --ioengine=io_uring --direct=1 --thread=1 \
  --group_reporting --time_based --random_generator=tausworthe64 --norandommap=1"

fio $FIO_DEFAULTS $FIO_CUSTOM_OPTIONS --filename_format ${MNT}/fio-$SIZE-\$jobnum --size=$SIZE --runtime=$RUNTIME --bs=$BS --rw=$RW \
  --iodepth=$QD --numjobs=$P --name="${RW}_${BS}_${QD}_${P}"
  
