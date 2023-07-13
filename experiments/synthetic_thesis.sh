#!/bin/bash

echo "Running synthetic experiments"

echo "Running: fio IOPS and throughput experiments"
BS_LIST=("4k" "16k" "64k")
QD_LIST=("2" "4" "8" "16" "32" "64" "128" "256")

for RW in "randread" "randwrite"; do
	for BS in "${BS_LIST[@]}"; do
		for IODEPTH in "${QD_LIST[@]}"; do
			for P in 1; do
				echo fio RW=$RW BS=$BS IODEPTH=$IODEPTH P=$P
				RW=$RW BS=$BS IODEPTH=$IODEPTH P=$P ./workloads/fio.sh > $OUT/fio_${RW}_${BS}_${IODEPTH}_${P}.out
			done
		done
	done
done

# Multicore vs QD experiment
# Thread count 1, 2, 4, 8
# BS fixed (16k, gives us room to improve)
# QD fixed (fastest aka 128)
echo "Running: fio IOPS multicore experiment"
for RW in "randread" "randwrite"; do
	for BS in "16k"; do
		for IODEPTH in "1" "2" "4" "8" "16" "32" "64" "128"; do
			for P in 1 2 4 8; do
				echo fio-multifile RW=$RW BS=$BS IODEPTH=$IODEPTH P=$P
				RW=$RW BS=$BS IODEPTH=$IODEPTH P=$P ./workloads/fio-multifile.sh > $OUT/fio_${RW}_${BS}_${IODEPTH}_${P}.out
			done
		done
	done
done

sleep 10
