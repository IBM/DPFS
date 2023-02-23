#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static int pm_qos_fd = -1;

int start_low_latency(void)  
{  
    int32_t latency = 0;

    if (pm_qos_fd >= 0)  
        return -EALREADY;  

    pm_qos_fd = open("/dev/cpu_dma_latency", O_WRONLY);  
    if (pm_qos_fd < 0) {  
        perror("open /dev/cpu_dma_latency");
        return 1;
    }  

    int ret = write(pm_qos_fd, &latency, sizeof(latency));  
    if (ret != sizeof(latency)) {  
        perror("write to /dev/cpu_dma_latency");
        return 1;
    }

    printf("cpu/dma latency has been set been set to %d\n", latency);

    return 0;
}

void stop_low_latency(void)  
{  
    if (pm_qos_fd >= 0)  
        close(pm_qos_fd);  

    printf("cpu/dma latency has been reset to default\n");
}

