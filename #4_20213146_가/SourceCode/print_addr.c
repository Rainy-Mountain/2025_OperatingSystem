// print_addr.c
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "fs.h"
#include "param.h"

// xv6의 inode, dinode 구조를 직접 참조하려면 kernel 구조와 맞춰야 함.
// fs.h 에 있는 NDIRECT, NINDIRECT, IPB 등 상수 이용 가능

int
main(int argc, char *argv[])
{
  int fd;
  struct stat st;
  int i;
  int addrs[NDIRECT + 1]; // 직접 + 간접
  int indirect[NINDIRECT];

  if (argc != 2) {
    printf(2, "Usage: print_addr <filename>\n");
    exit();
  }

  // 파일 열기
  fd = open(argv[1], O_RDONLY);
  if (fd < 0) {
    printf(2, "print_addr: cannot open %s\n", argv[1]);
    exit();
  }

  // 파일 상태(stat) 가져오기
  if (fstat(fd, &st) < 0) {
    printf(2, "print_addr: cannot stat %s\n", argv[1]);
    close(fd);
    exit();
  }

  // fstat 구조체에는 직접 블록 주소가 없기 때문에
  // 실제 xv6에서는 커널이 전달해주는 inode 블록 주소를 얻기 위한
  // 별도의 시스템콜을 추가해두는 경우가 많다.
  // 하지만 과제에서는 ‘print_addr.c’가 커널 내부 구조를 보여주는 용도로
  // 테스트용으로 제공되거나, 아래처럼 fake로 가져오기도 함.

  // 이 부분은 실제 kernel/sysproc.c에서 구현한
  // sys_print_addr() 같은 커널 함수가 있어야 한다.
  // 즉, print_addr() 시스템콜을 만들어서 inode의 addrs[]를 유저로 복사하게끔 하는 방식.

  if (print_addr(argv[1], addrs, indirect) < 0) {
    printf(2, "print_addr: system call failed\n");
    close(fd);
    exit();
  }

  close(fd);

  // 이제 결과 출력
  for (i = 0; i < NDIRECT; i++) {
    if (addrs[i] == 0)
      break;
    printf(1, "addr[%d] : %x\n", i, addrs[i]);
  }

  if (addrs[NDIRECT]) {
    printf(1, "addr[%d] : %x (INDIRECT POINTER)\n", NDIRECT, addrs[NDIRECT]);
    for (i = 0; i < NINDIRECT; i++) {
      if (indirect[i] == 0)
        break;
      printf(1, "addr[%d] -> [%d] (bn : %d) : %x\n", NDIRECT, i, NDIRECT + i, indirect[i]);
    }
  }

  exit();
}
