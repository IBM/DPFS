#!/bin/bash
REPS=5

echo "RUNNING: gitclone"
RESULTS_GITCLONE=$OUT/gitclone
sudo mkdir -p $RESULTS_GITCLONE

for i in $(seq 1 $REPS); do
	sleep 1
	sudo sh -c "time -o $RESULTS_GITCLONE/gitclone_$i git clone https://github.com/mit-pdos/xv6-riscv.git $MNT/xv6$i"
	sleep 1
	sudo rm -rf $MNT/xv6$i
done

sudo sh -c "git clone https://github.com/torvalds/linux.git $MNT/linux"

echo "RUNNING: grep"
RESULTS_GREP=$OUT/results/grep
sudo mkdir -p $RESULTS_GREP

for i in $(seq 1 $REPS); do
	sleep 1
	sudo sh -c "time -o $RESULTS_GREP/grep_$i grep -r 'jbd2_journal_start' $MNT/linux > $MNT/grep$i"
	sleep 1
	sudo rm -rf $MNT/grep$i $MNT/xv6fs$i $MNT/linux$i
done

echo "RUNNING: tar"
RESULTS_TAR=$OUT/results/tar
sudo mkdir -p $RESULTS_TAR

for i in $(seq 1 $REPS); do
	sleep 1
	sudo sh -c "time -o $RESULTS_TAR/tar_$i tar -cf $MNT/linux.tar $MNT/linux"
	sleep 1
	sudo rm -rf $MNT/linux.tar
done


echo "RUNNING: untar"
RESULTS_UNTAR=$OUT/results/untar
sudo mkdir -p $RESULTS_UNTAR

sudo tar -cf $MNT/linuxTARR.tar $MNT/linux
# We don't need it anymore
sudo rm -rf $MNT/linux

for i in $(seq 1 $REPS); do
	sleep 1
	sudo sh -c "time -o $RESULTS_UNTAR/untar_$i tar -xf $MNT/linuxTARR.tar --one-top-level -C $MNT"
	sleep 1
	sudo rm -rf $MNT/linuxTARR
done
