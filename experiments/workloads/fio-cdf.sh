#!/bin/bash

if [[ -z $MNT || -z $RW || -z $BS || -z $QD || -z $P ]]; then
	echo MNT, RW, BS, QD and P must be defined!
	exit 1
fi
# Default size of file is
SIZE="${SIZE:-1g}"
# Default run time is
RUNTIME="${RUNTIME:-60s}"
# NUMA defaults, based on ZRL:zac15
NUMA_NODE="${NUMA_NODE:-1}"
NUMA_CORE="${NUMA_CORE:-27}"
# Default log output folder is OUT, else the current folder
LOG_OUT="${OUT:-"./"}"

FIO_DEFAULTS="--ramp_time=10s --ioengine=io_uring --direct=1 --thread=1 \
  --group_reporting --time_based --random_generator=tausworthe64 --norandommap=1 \
  --lat_percentiles=1 --percentile_list=25.0:50.0:75.0:99.0 --write_lat_log=$LOG_OUT/fio_${RW}_${BS}_${QD}_${P}"

FIO="fio $FIO_DEFAULTS $FIO_CUSTOM_OPTIONS --filename ${MNT}/fio-$SIZE --size=$SIZE --runtime=$RUNTIME --bs=$BS --rw=$RW \
  --iodepth=$QD --numjobs=$P --name=\"${RW}_${BS}_${QD}_${P}\""

if [[ $P == 1 ]]; then
  sudo numactl -m $NUMA_NODE -C $NUMA_CORE $FIO
else
  sudo numactl -m $NUMA_NODE -N $NUMA_NODE $FIO
fi

rm $LOG_OUT/fio_${RW}_${BS}_${QD}_${P}_slat.1.log
rm $LOG_OUT/fio_${RW}_${BS}_${QD}_${P}_lat.1.log
sudo chown $USER:$USER $LOG_OUT/fio_${RW}_${BS}_${QD}_${P}_clat.1.log
sudo chmod 644 $LOG_OUT/fio_${RW}_${BS}_${QD}_${P}_clat.1.log
