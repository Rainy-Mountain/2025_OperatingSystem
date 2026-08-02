#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

#define MAX_FRINFO 60000

static void
usage(void)
{
  printf(1, "usage: memdump [-a] [-p PID]\n");
  exit();
}

int main(int argc, char *argv[])
{
  int do_alloc = 0;
  int target_pid = -1;

  if (argc == 1)
    usage();

  // 옵션 처리
  for (int i = 1; i < argc; i++)
  {
    if (strcmp(argv[i], "-a") == 0)
      do_alloc = 1;
    else if (strcmp(argv[i], "-p") == 0)
    {
      if (i + 1 >= argc)
        usage();
      target_pid = atoi(argv[++i]);
      if (target_pid <= 0)
        usage();
    }
    else
      usage();
  }

  // 물리 메모리 정보 덤프
  static struct physframe_info buf[MAX_FRINFO];
  int n = dump_physmem_info((void *)buf, MAX_FRINFO);
  if (n < 0)
  {
    printf(1, "memdump: dump_physmem_info failed\n");
    exit();
  }

  printf(1, "[memdump] pid=%d\n", getpid());
  printf(1, "[frame#]\t[alloc]\t[pid]\t[start_tick]\n");

  int count = 0;
  for (int i = 0; i < n; i++) {
    struct physframe_info *pf = &buf[i];
    if (do_alloc && pf->allocated == 0)
      continue;
    if (target_pid != -1 && pf->pid != target_pid)
      continue;
    printf(1, "%d\t\t%d\t%d\t%d\n",
        pf->frame_index, pf->allocated,
        pf->pid, pf->start_tick);
    count++;
  }

  exit();
}