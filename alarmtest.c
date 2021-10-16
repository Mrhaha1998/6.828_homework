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
    int e, f, g, h;
    e = a + b;
    f = a + c;
    g = c + d;
    h = b + c;
    printf(1, "alarm! %d %d %d %d %d %d %d %d\n", a, b, c, d, e, f, g, h);
}