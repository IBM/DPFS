#include "ftimer.h"
#include <stdio.h>
#include <err.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

/* How to compile:
 * gcc lat_stat.c -O3 -o lat_stat
 * How to use:
 * ./lat_stat /mnt/dpu/test 5000 1
 */

int main(int argc, char **argv)
{
	ftimer ft;
	struct stat statbuf;

	int nruns = atoi(argv[2]);
	int skip_first = atoi(argv[3]);

	if (skip_first) {
		// First do it once without measuring the time to skip to the HOT PATH
		// This is only really useful if you do a single run, as then the number
		// is somewhat representative
		int ret = stat(argv[1], &statbuf);
		if (ret < 0) {
			err(-1, "stat errored");
		}
		memset(&statbuf, 0, sizeof(statbuf));
	}

	ft_init(&ft);

	for (size_t i = 0; i < nruns; i++) {
		ft_start(&ft);
		int ret = stat(argv[1], &statbuf);
		ft_stop(&ft);
		if (ret < 0) {
			err(-1, "stat errored");
		}
		memset(&statbuf, 0, sizeof(statbuf));
	}
	printf("stat took %lu, averaged over %d runs\n", ft_get_nsec(&ft)/nruns, nruns);
}
