#ifndef TYPES_H
#define TYPES_H

typedef unsigned int   uint;
typedef unsigned short ushort;
typedef unsigned char  uchar;
typedef uint pde_t;
typedef unsigned char bool;

struct vlist {
  uint pid;       // 프로세스 ID
  uint va_page;   // 매핑된 가상 주소 (페이지 정렬)
  uint flags;     // IPT 엔트리에 저장된 플래그 (예: IPT_VALID)
};

#endif // TYPES_H