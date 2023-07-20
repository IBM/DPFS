#!/bin/bash

echo "Running synthetic clat experiments"

# io_uring advanced flags, saved for later
# --registerfiles --fixedbufs --sqthread_poll=1

BS_LIST=("4k")
QD_LIST=("16" "128")

sudo sh -c "./setcpulatency 0 > /dev/null" &
sleep 5
echo "Running: fio latency experiments"
for RW in "randread" "randwrite"; do
	for BS in "${BS_LIST[@]}"; do
		for QD in "${QD_LIST[@]}"; do
			for P in 1; do
				echo fio-cdf RW=$RW BS=$BS QD=$QD P=$P
				sudo -E env BS=$BS QD=$QD P=$P RW=$RW \
					./workloads/fio-cdf.sh > $OUT/fio_${RW}_${BS}_${QD}_${P}.out
			done
		done
	done
done
sudo pkill setcpulatency
sleep 10

