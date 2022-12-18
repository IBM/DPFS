#ifndef FTIMER_HEADER
#define FTIMER_HEADER
#include <time.h>

typedef struct {
    struct timespec begin;
    struct timespec end;
    double sec;
    long nsec;
} ftimer;

inline void ft_init(ftimer *ft) {
    ft->sec = 0;
    ft->nsec = 0;
}
inline void ft_start(ftimer *ft) {
    clock_gettime(CLOCK_MONOTONIC, &ft->begin);
}
inline void ft_stop(ftimer *ft) {
    clock_gettime(CLOCK_MONOTONIC, &ft->end);
    ft->sec += (double)(ft->end.tv_sec - ft->begin.tv_sec);
    ft->nsec += (double)(ft->end.tv_nsec - ft->begin.tv_nsec);
}
inline double ft_get_sec(ftimer *ft) {
    return ft->sec + (double)ft->nsec / 1e9;
}
inline long ft_get_nsec(ftimer *ft) {
    return ft->sec * 1e9 + ft->nsec;
}
#endif // FTIMER_HEADER
