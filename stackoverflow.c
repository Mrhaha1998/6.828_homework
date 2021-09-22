#include "types.h"
#include "user.h"
#include "date.h"


void endlessrecursion()
{
    endlessrecursion();
}

int
main(int argc, char *argv[])
{
    endlessrecursion();
    exit();
}