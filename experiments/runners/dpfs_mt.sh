#!/bin/bash

if [[ -z $OUT || -z $MNT ]]; then
	echo OUT and MNT must be defined!
	exit 1
fi

MT_LIST=("2" "4" "8" "10")
for MT in "${MT_LIST[@]}"; do
	export MOUNT_COMMAND="sudo mount -t virtiofs dpfs-@T @MNT"
	MT=$MT MODULE=virtiofs ./runners/multi-tenancy.sh
done

