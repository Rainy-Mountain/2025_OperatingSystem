#include "types.h"
#include "stat.h"
#include "user.h"

int
main(int argc, char *argv[])
{
    int id = snapshot_create();
    printf(1, "snapshot id: %d\n", id);
    exit();
}
