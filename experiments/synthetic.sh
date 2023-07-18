#!/bin/bash

echo "Running synthetic experiments"

# io_uring advanced flags, saved for later
# --registerfiles --fixedbufs --sqthread_poll=1

TIME=niks
if [ -n "$MP" ]; then
TIME=$(python3 -c '
import datetime
t = 2*3*8*5*70 + 2*3*70
print(datetime.timedelta(seconds=t))
')
echo Running Multi-Processor experiments as well
else
TIME=$(python3 -c '
import datetime
t = 2*3*8*1*70 + 2*3*70
print(datetime.timedelta(seconds=t))
')
fi
echo "Running: synthetic fio experiments which will take $TIME"

BS_LIST=("4k" "16k" "64k")
QD_LIST=("2" "4" "8" "16" "32" "64" "128")
P_LIST=("1")
if [ -n "$MP" ]; then
	P_LIST=("1" "2" "4" "8" "10")
fi


for RW in "randread" "randwrite"; do
	for BS in "${BS_LIST[@]}"; do
		for QD in "${QD_LIST[@]}"; do
			for P in "${P_LIST[@]}"; do
				echo fio RW=$RW BS=$BS QD=$QD P=$P
				sudo env OUT=$OUT MNT=$MNT BS=$BS QD=$QD P=$P RW=$RW ./workloads/fio.sh > $OUT/fio_${RW}_${BS}_${QD}_${P}.out
			done
		done
	done
done

sudo ./setcpulatency 0 &
sleep 5
echo "Running: fio latency experiments"
for RW in "randread" "randwrite"; do
	for BS in "${BS_LIST[@]}"; do
		for QD in 1; do
			for P in 1; do
				echo fio-cdf RW=$RW BS=$BS QD=$QD P=$P
				sudo env OUT=$OUT MNT=$MNT BS=$BS QD=$QD P=$P RW=$RW ./workloads/fio-cdf.sh > $OUT/fio_${RW}_${BS}_${QD}_${P}.out
			done
		done
	done
done
sudo pkill setcpulatency
sleep 10
