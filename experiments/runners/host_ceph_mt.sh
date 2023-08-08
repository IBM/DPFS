#!/bin/bash

BASE_MNT=$MNT
BASE_OUT=$OUT

BS_LIST=("4k" "16k" "64k")
QD_LIST=("2" "4" "8" "16" "32" "64" "128")
P_LIST=("1")

MT_LIST=("2" "4" "8" "10")
for MT in "${MT_LIST[@]}"; do
	echo MT=$MT
	export OUT=$BASE_OUT/MT_$MT/
	mkdir -p $OUT

	# Mount
	for T in $(seq 1 $MT); do
		export MNT=$BASE_MNT\_$T
		sudo mkdir -p $MNT > /dev/null
		sudo umount -A $MNT > /dev/null
		sudo mount -t nfs -o wsize=1048576,rsize=1048576,async 10.100.0.19:/pj $MNT
		sudo -E RUNTIME="10s" RW=randrw BS=4k QD=128 P=4 ./workloads/fio.sh > /dev/null
		echo mounted and warmed up $MNT
	done

	# Synthetic
	for RW in "randread" "randwrite"; do
		for BS in "${BS_LIST[@]}"; do
			for QD in "${QD_LIST[@]}"; do
				for P in "${P_LIST[@]}"; do

					echo fio RW=$RW BS=$BS QD=$QD P=$P
					for T in $(seq 1 $MT); do
						export MNT=$BASE_MNT\_$T

						sudo -E env BS=$BS QD=$QD P=$P RW=$RW \
							./workloads/fio.sh > $OUT/fio_${RW}_${BS}_${QD}_${P}_${T}.out &
					done
					wait

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
					for T in $(seq 1 $MT); do
						export MNT=$BASE_MNT\_$T
						export OUT=$BASE_OUT\_$T
						mkdir -p $OUT

						sudo -E env BS=$BS QD=$QD P=$P RW=$RW \
							./workloads/fio.sh > $OUT/fio_${RW}_${BS}_${QD}_${P}.out &
					done
					wait

				done
			done
		done
	done
	sudo pkill setcpulatency
	sleep 5

done
