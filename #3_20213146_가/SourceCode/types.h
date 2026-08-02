#ifndef TYPES_H
#define TYPES_H

typedef unsigned int   uint;
typedef unsigned short ushort;
typedef unsigned char  uchar;
typedef uint pde_t;
typedef unsigned char bool;

struct physframe_info {
  uint frame_index;   // PFN
  int  allocated;     // 0|1
  int  pid;           // 소유 PID, 없으면 -1
  uint start_tick;    // 할당 시점 ticks
};

#endif // TYPES_H