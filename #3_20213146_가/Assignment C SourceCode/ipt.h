// ipt.h (간단 헤더)
#ifndef __IPT_H__
#define __IPT_H__

#include "spinlock.h"

enum ipt_flags {
  IPT_VALID = 0b00000001,   // 유효한 엔트리
  IPT_DIRTY = 0b00000010,   // 수정된 페이지
};

struct ipt_entry {
  unsigned pfn;            // physical frame number
  unsigned pid;            // owning process id
  unsigned va;             // user virtual address (page-aligned)
  enum ipt_flags flags;    // 상태 플래그 (사용자 정의)

  struct ipt_entry *next_hash;  // 해시 체인
  struct ipt_entry *next_pfn;   // pfn 체인
};

extern struct spinlock ipt_lock;
extern struct ipt_entry *ipt_by_pfn[PFNNUM]; // 해시 테이블

void ipt_init(void);
struct ipt_entry *ipt_lookup(unsigned pid, unsigned va);
int ipt_map(unsigned pfn, unsigned pid, unsigned va, enum ipt_flags flags);
int ipt_unmap(unsigned pid, unsigned va);

#endif
