#ifndef FTIMER_HEADER
#define FTIMER_HEADER
#include <time.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ftimer {
    struct timespec begin;
    struct timespec end;
    double sec;
    long nsec;
    bool running;
};

void ft_init(struct ftimer *ft);
void ft_start(struct ftimer *ft);
void ft_stop(struct ftimer *ft);
double ft_get_sec(struct ftimer *ft);
long ft_get_nsec(struct ftimer *ft);
void ft_add(struct ftimer *, struct ftimer *);

#ifdef __cplusplus
}
#endif

#endif // FTIMER_HEADER
