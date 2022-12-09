#include "ftimer.h"
#include <stdio.h>
#include <err.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/statvfs.h>

/* How to compile:
 * gcc lat_statfs.c -O3 -o lat_statfs
 * How to use:
 * ./lat_statfs /mnt/dpu 5000 1
 */

int main(int argc, char **argv)
{
	ftimer ft;
	struct statvfs b;

	int nruns = atoi(argv[2]);
	int skip_first = atoi(argv[3]);

	if (skip_first) {
		// First do it once without measuring the time to skip to the HOT PATH
		// This is only really useful if you do a single run, as then the number
		// is somewhat representative
		int ret = statvfs(argv[1], &b);
		if (ret < 0) {
			err(-1, "stat errored");
		}
		memset(&b, 0, sizeof(b));
	}

	ft_init(&ft);

	for (size_t i = 0; i < nruns; i++) {
		ft_start(&ft);
		int ret = statvfs(argv[1], &b);
		ft_stop(&ft);
		if (ret < 0) {
			err(-1, "stat errored");
		}
		memset(&b, 0, sizeof(b));
	}
	printf("statfs took %lu, averaged over %d runs\n", ft_get_nsec(&ft)/nruns, nruns);
}
