// snap_delete.c
#include "types.h"
#include "stat.h"
#include "user.h"

int
main(int argc, char *argv[])
{
  if(argc != 2){
    printf(2, "usage: snap_delete id\n");
    exit();
  }
  int id = atoi(argv[1]);
  if(snapshot_delete(id) < 0)
    printf(2, "snap_delete: failed for id %d\n", id);
  exit();
}
