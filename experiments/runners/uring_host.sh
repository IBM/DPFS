#!/bin/bash

echo "Running synthetic experiments"

# io_uring advanced flags, saved for later
# --registerfiles --fixedbufs --sqthread_poll=1

TIME=niks
if [ -n "$MP" ]; then
TIME=$(python3 -c '
import datetime
t = 3*(2*3*8*5*70 + 2*3*70)
print(datetime.timedelta(seconds=t))
')
echo Running Multi-Processor experiments as well
else
TIME=$(python3 -c '
import datetime
t = 3*(2*3*8*1*70 + 2*3*70)
print(datetime.timedelta(seconds=t))
')
fi
echo "Running: synthetic fio experiments which will take $TIME"

BS_LIST=("4k" "16k" "64k")
QD_LIST=("2" "4" "8" "16" "32" "64" "128")
P_LIST=("1")
if [ -n "$MP" ]; then
	P_LIST=("1" "2" "4" "8")
fi

URING_A="--fixedbufs"
URING_B="--fixedbufs --registerfiles"
URING_SQ_THREAD_CPU=$(numactl --hardware | grep -Po "(?<=node $NUMA_NODE cpus: )\d+")
URING_C="--fixedbufs --registerfiles --sqthread_poll=1 --sq_thread_cpu=$URING_SQ_THREAD_CPU"
# NUMA_CORE runs the workload and polls on the device, the first core in the NUMA_NODE runs the submission queue poller in kernel space

URING_LIST=($URING_A $URING_B $URING_C)
for URING in "${URING_LIST[@]}"; do
	for RW in "randread" "randwrite"; do
		for BS in "${BS_LIST[@]}"; do
			for QD in "${QD_LIST[@]}"; do
				for P in "${P_LIST[@]}"; do
					echo fio RW=$RW BS=$BS QD=$QD P=$P
					sudo -E env FIO_CUSTOM_OPTIONS=$URING BS=$BS QD=$QD P=$P RW=$RW \
						./workloads/fio.sh > $OUT/fio_${RW}_${BS}_${QD}_${P}.out
				done
			done
		done
	done
	
	sudo sh -c "./setcpulatency 0 > /dev/null" &
	sleep 5
	echo "Running: fio latency experiments"
	for RW in "randread" "randwrite"; do
		for BS in "${BS_LIST[@]}"; do
			for QD in 1; do
				for P in "${P_LIST[@]}"; do
					echo fio RW=$RW BS=$BS QD=$QD P=$P
					sudo -E env FIO_CUSTOM_OPTIONS=$URING BS=$BS QD=$QD P=$P RW=$RW \
						./workloads/fio.sh > $OUT/fio_${RW}_${BS}_${QD}_${P}.out
				done
			done
		done
	done
	sudo pkill setcpulatency
	sleep 5
done
