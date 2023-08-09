#!/bin/bash

if [ -z $NFS_URI ]; then
	echo "You must define the NFS remote URI for example `10.100.0.19:/pj`!"
	exit 1
fi

if [[ -z $OUT || -z $MNT ]]; then
	echo OUT and MNT must be defined!
	exit 1
fi

if [[ -z $MT ]]; then
	echo The number of tenants must be defined using MT!
	exit 1
fi

# NUMA defaults, based on ZRL:zac15
NUMA_NODE="${NUMA_NODE:-1}"
NUMA_CORE="${NUMA_CORE:-27}"
REPS="${REPS:-5}"

BASE_MNT=$MNT
BASE_OUT=$OUT

BS_LIST=("4k" "16k" "64k")
QD_LIST=("2" "4" "8" "16" "32" "64" "128")
P_LIST=("1")

TIME=$(python3 -c "
import datetime
t = 2*3*8*1*70 + 2*3*1*1*70 + 3*${REPS}*60
print(datetime.timedelta(seconds=t))
")
echo "Running: synthetic and metadata multi-tenancy fio experiments which will take $TIME"

echo MT=$MT
export OUT=$BASE_OUT/MT_$MT/
mkdir -p $OUT

sudo modprobe virtio_pci;

# Mount
sudo umount -A $BASE_MNT* 2&> /dev/null
for T in $(seq 1 $MT); do
	# Each tenant has their own mount point
	export MNT=$BASE_MNT\_$T
	sudo mkdir -p $MNT 2&> /dev/null
	sudo mount -t virtiofs dpfs-$T $MNT
	sudo -E RUNTIME="10s" RW=randrw BS=4k QD=128 P=4 ./workloads/fio.sh > /dev/null
	# Each tenant works on their own subtree
	sudo mkdir -p $MNT/$T 2&> /dev/null
	echo mounted and warmed up $MNT
done

echo Synthetic
for RW in "randread" "randwrite"; do
	for BS in "${BS_LIST[@]}"; do
		for QD in "${QD_LIST[@]}"; do
			for P in "${P_LIST[@]}"; do

				echo fio RW=$RW BS=$BS QD=$QD P=$P
				for T in $(seq 1 $MT); do
					export MNT=$BASE_MNT\_$T/$T

					sudo -E env BS=$BS QD=$QD P=$P RW=$RW \
						./workloads/fio.sh > $OUT/fio_${RW}_${BS}_${QD}_${P}_${T}.out &
				done
				wait

			done
		done
	done
done

sudo sh -c "./setcpulatency 0 > /dev/null" & disown
sleep 5
echo "Running: fio latency experiments"
for RW in "randread" "randwrite"; do
	for BS in "${BS_LIST[@]}"; do
		for QD in 1; do
			for P in "${P_LIST[@]}"; do

				echo fio RW=$RW BS=$BS QD=$QD P=$P
				for T in $(seq 1 $MT); do
					export MNT=$BASE_MNT\_$T/$T

					sudo -E env BS=$BS QD=$QD P=$P RW=$RW \
						./workloads/fio.sh > $OUT/fio_${RW}_${BS}_${QD}_${P}_${T}.out &
				done
				wait

			done
		done
	done
done
sudo pkill setcpulatency
sleep 5

FILEBENCHES=("fileserver" "varmail" "webserver")
FILEBENCHES_FOLDER=./workloads/filebenches
sudo sh -c 'echo 0 > /proc/sys/kernel/randomize_va_space'

echo Metadata
export OUT=$BASE_OUT/MT_$MT/filebench
mkdir -p $OUT

for f in "${FILEBENCHES[@]}"; do
	echo "Running $f"
	for i in $(seq 1 $REPS); do
		echo "i=$i"

	# First prepare the filebench files for all the tenants
	for T in $(seq 1 $MT); do
		export MNT=$BASE_MNT\_$T/$T
				sudo cp $FILEBENCHES_FOLDER/$f.f $MNT/
				sudo sed -i -e "s#set \$dir=.*#set \$dir=$MNT#g" $MNT/$f.f
	done

	for T in $(seq 1 $MT); do
		export MNT=$BASE_MNT\_$T/$T
		  		sudo numactl -m $NUMA_NODE ~/filebench/filebench -f $MNT/$f.f > $OUT/$f\_$T\_$i &
	done
	wait

	done
done

sudo sh -c 'echo 1 > /proc/sys/kernel/randomize_va_space'