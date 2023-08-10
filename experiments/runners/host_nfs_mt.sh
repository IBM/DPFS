#!/bin/bash

if [ -z $NFS_URI ]; then
	echo "You must define the NFS remote URI for example `10.100.0.19:/pj`!"
	exit 1
fi

if [[ -z $OUT || -z $MNT ]]; then
	echo OUT and MNT must be defined!
	exit 1
fi

MT_LIST=("2" "4" "8" "10")
for MT in "${MT_LIST[@]}"; do
	export MOUNT_COMMAND="sudo mount -t nfs -o wsize=1048576,rsize=1048576,async $NFS_URI @MNT"
	METADATA=1 MT=$MT ./runners/multi-tenancy.sh
done

