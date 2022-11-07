#include "ftimer.h"

void ft_init(struct ftimer *ft) {
    ft->sec = 0;
    ft->nsec = 0;
}

void ft_start(struct ftimer *ft) {
    clock_gettime(CLOCK_MONOTONIC, &ft->begin);
}
void ft_stop(struct ftimer *ft) {
    clock_gettime(CLOCK_MONOTONIC, &ft->end);
    ft->sec += (double)(ft->end.tv_sec - ft->begin.tv_sec);
    ft->nsec += (double)(ft->end.tv_nsec - ft->begin.tv_nsec);
}
double ft_get_sec(struct ftimer *ft) {
    return ft->sec + (double)ft->nsec / 1e9;
}
long ft_get_nsec(struct ftimer *ft) {
    return ft->sec * 1e9 + ft->nsec;
}
