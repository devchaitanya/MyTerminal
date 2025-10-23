#include <stdio.h>
#include <unistd.h>

int main() {
    printf("Starting infinite loop test program...\n");
    while (1) {
        printf("Loop iteration\n");
        fflush(stdout);
        sleep(1);
    }
    return 0;
}