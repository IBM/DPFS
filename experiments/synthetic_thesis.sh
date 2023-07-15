#!/bin/bash

echo "Running synthetic experiments"

echo "Running: fio IOPS and throughput experiments"
BS_LIST=("4k" "16k" "64k")
QD_LIST=("2" "4" "8" "16" "32" "64" "128" "256")

for RW in "randread" "randwrite"; do
	for BS in "${BS_LIST[@]}"; do
		for IODEPTH in "${QD_LIST[@]}"; do
			for P in 1 2 4 8; do
				echo fio RW=$RW BS=$BS IODEPTH=$IODEPTH P=$P
				sudo env MNT=$MNT RW=$RW BS=$BS IODEPTH=$IODEPTH P=$P ./workloads/fio.sh > $OUT/fio_${RW}_${BS}_${IODEPTH}_${P}.out
			done
		done
	done
done

sudo ./setcpulatency 0 &
sleep 5
echo "Running: fio latency experiments"
for RW in "randread" "randwrite"; do
	for BS in "${BS_LIST[@]}"; do
		for IODEPTH in 1; do
			for P in 1; do
				echo fio RW=$RW BS=$BS IODEPTH=$IODEPTH P=$P
				sudo env MNT=$MNT RW=$RW BS=$BS IODEPTH=$IODEPTH P=$P ./workloads/fio.sh > $OUT/fio_${RW}_${BS}_${IODEPTH}_${P}.out
			done
		done
	done
done
sudo pkill setcpulatency
sleep 10
