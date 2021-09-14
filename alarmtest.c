#include "types.h"
#include "stat.h"
#include "user.h"

void periodic();

int
main(int argc, char *argv[])
{
    long i;
    printf(1, "alarmtest starting\n");
    if(alarm(10, periodic) < 0){
        printf(1, "alarm failed");
    }
    for(i = 0; i < 25*5000000; i++){
        if((i % 250000) == 0)
        write(2, ".", 1);
    }
    exit();
}

void
periodic()
{
    int a = 0;
    int b = 1;
    int c = 2;
    int d = 3;

    printf(1, "alarm! a=%d b =%d c=%d c=%d\n", a, b, c, d);
}