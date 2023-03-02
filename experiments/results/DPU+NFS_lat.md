BS=4k IODEPTH=1 P=1 RW=randread or RW=randwrite ./workloads/fio.sh

NFS Read 100.5
NFS write 101.91

nulldev Read: 38.58
nulldev Write: 43.27

libnfs Read: 114.59 - 38.58         = 76.01
libnfs Read: 117.43 - 43.27         = 74.16usec

libnfs+XLIO Read: 91.49 - 38.58     = 52.91usec
libnfs+XLIO Write: 95.46 - 43.27656 = 52.18usec