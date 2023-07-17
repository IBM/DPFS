#!/bin/bash

# Default size of file is
SIZE="${SIZE:-1g}"
# Default run time is
RUNTIME="${RUNTIME:-60s}"

FIO_DEFAULTS="--ioengine=io_uring --direct=1 --thread=1 --group_reporting --time_based --random_generator=tausworthe64 --norandommap=1"

fio $FIO_DEFAULTS --filename_format ${MNT}/fio-$SIZE-\$jobnum --size=$SIZE --ramp_time=10s --runtime=$RUNTIME --bs=$BS --rw=$RW --iodepth=$IODEPTH --numjobs=$P --name="${RW}_${BS}_${IODEPTH}_${P}" \
  > $OUT/fio_${RW}_${BS}_${IODEPTH}_${P}.out
