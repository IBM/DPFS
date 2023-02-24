FIO_DEFAULTS="--filename ${MNT}/fio-10g --size 10G --ioengine=io_uring --direct=1 --thread=1 --group_reporting --time_based --random_generator=tausworthe64 --norandommap=1"

# Default runtime is 30s
RUNTIME="${RUNTIME:-30s}"

fio $FIO_DEFAULTS --ramp_time=10s --runtime=$RUNTIME --bs=$BS --rw=$RW --iodepth=$IODEPTH --numjobs=$P --name="${RW}_${BS}_${IODEPTH}_${P}"
