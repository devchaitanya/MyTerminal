#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

static double now_seconds(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <label> <delay_sec> <count>\n", argv[0]);
        return 1;
    }
    const char* label = argv[1];
    double delay = atof(argv[2]);
    int count = atoi(argv[3]);
    if (delay < 0) delay = 0;
    if (count < 0) count = 0;

    // Optional tiny random jitter so two instances drift differently
    unsigned int seed = (unsigned int)(getpid() ^ time(NULL));
    srand(seed);

    for (int i = 1; i <= count; ++i) {
        double t = now_seconds();
        printf("%s %d at %.6f (pid=%d)\n", label, i, t, (int)getpid());
        fflush(stdout);
        // add 0..10ms jitter to make interleaving obvious
    unsigned int r = (unsigned int)rand();
    unsigned int jitter_us = r % 10000; // microseconds
    unsigned int total_us = (unsigned int)(delay * 1000000.0) + jitter_us;
    struct timespec ts;
    ts.tv_sec = total_us / 1000000u;
    ts.tv_nsec = (long)(total_us % 1000000u) * 1000L;
    (void)clock_nanosleep(CLOCK_REALTIME, 0, &ts, NULL);
    }
    return 0;
}
