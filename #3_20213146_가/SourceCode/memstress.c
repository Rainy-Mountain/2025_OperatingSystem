#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

static void
usage(void) {
  printf(1, "usage: memstress [-n pages] [-t ticks] [-w]\n");
  exit();
}

int
main(int argc, char *argv[])
{
  int pages = 64;
  int hold_ticks = 200;
  int do_write = 0;

  // 옵션 처리
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-n") == 0) {
      if (i + 1 >= argc)
        usage();
      pages = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-t") == 0) {
      if (i + 1 >= argc)
        usage();
      hold_ticks = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-w") == 0) {
      do_write = 1;
    } else {
      usage();
    }
  }

  int pid = getpid();
  printf(1, "[memstress] pid=%d pages=%d hold=%d ticks write=%d\n", pid, pages, hold_ticks, do_write);

  int inc = pages * 4096; 
  char *base = sbrk(inc);
  if (base == (char*)-1) {
    printf(1, "[memstress] sbrk failed\n");
    exit();
  }

  if (do_write) {
    for (int p = 0; p < pages; p++) {
      base[p*4096] = (char)(p & 0xff);
    }
  }

  sleep(hold_ticks);

  printf(1, "[memstress] pid=%d done\n", pid);
  exit();
}