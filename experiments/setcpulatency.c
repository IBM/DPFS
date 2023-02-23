#include <stdio.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>

int main(int argc, char **argv) {
        int32_t l;
        int fd;

        if (argc != 2) {
                fprintf(stderr, "Usage: %s <latency in us>\n", argv[0]);
                return 2;
        }
        l = atoi(argv[1]);
        printf("setting latency to %d us\n", l);

        fd = open("/dev/cpu_dma_latency", O_WRONLY);
        if (fd < 0) {
                perror("open /dev/cpu_dma_latency");
                return 1;
        }

        if (write(fd, &l, sizeof(l)) != sizeof(l)) {
                perror("write to /dev/cpu_dma_latency");
                return 1;
        }

        while (1) pause();
}
