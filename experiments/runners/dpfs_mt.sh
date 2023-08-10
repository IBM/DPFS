#!/bin/bash

if [[ -z $OUT || -z $MNT ]]; then
	echo OUT and MNT must be defined!
	exit 1
fi

if [[ -z $MT ]]; then
	echo You must define the number of tenants using 
	exit 1
fi

export MOUNT_COMMAND="sudo mount -t virtiofs dpfs-@T @MNT"
MT=$MT MODULE=virtiofs ./runners/multi-tenancy.sh

