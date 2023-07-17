#!/bin/bash

# Default size of file is
SIZE="${SIZE:-1g}"
# Default run time is
RUNTIME="${RUNTIME:-60s}"

FIO_DEFAULTS="--ioengine=io_uring --direct=1 --thread=1 --group_reporting --time_based --random_generator=tausworthe64 --norandommap=1 \
  --lat_percentiles=1 --percentile_list=25.0:50.0:75.0:99.0 --write_lat_log=\"$RESULTS/fio_${RW}_${BS}_${IODEPTH}_${P}\""

fio $FIO_DEFAULTS --filename ${MNT}/fio-$SIZE --size=$SIZE --ramp_time=10s --runtime=$RUNTIME --bs=$BS --rw=$RW --iodepth=$IODEPTH --numjobs=$P --name="${RW}_${BS}_${IODEPTH}_${P}" \
  > $OUT/fio_${RW}_${BS}_${IODEPTH}_${P}.out
