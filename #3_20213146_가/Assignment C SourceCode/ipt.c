// ipt.c
#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"
#include "ipt.h"

#define PGSHIFT 12  // 페이지 오프셋 비트 수

// --- 설정값: 시스템에 맞춰 조정하세요 ---
#define IPT_HASH_SZ  128    // 해시 버킷 수 (power-of-two 권장)

// --- 내부 스토리지 ---
// pfn 인덱스용 배열(빠른 pfn->entry)
struct ipt_entry *ipt_by_pfn[PFNNUM];

// 전역 락
struct spinlock ipt_lock;

// 해시 버킷
static struct ipt_entry *ipt_buckets[IPT_HASH_SZ];

// 풀(정적 할당). xv6에 kmalloc이 없을 경우를 대비해 정적 블록 사용.
// 실제환경에서는 필요하면 동적 할당(kalloc)로 변경 가능.
static struct ipt_entry ipt_pool[PFNNUM];
static struct ipt_entry *ipt_freelist;


// 해시 함수: pid + va 로 해시
static unsigned ipt_hash(unsigned pid, unsigned va) {
  // va는 페이지 정렬된 주소라고 가정
  unsigned key = (pid * 1103515245u) ^ (va >> PGSHIFT);
  return (key ^ (key >> 16)) & (IPT_HASH_SZ - 1);
}

// 내부: 새 엔트리 할당
static struct ipt_entry *ipt_alloc_entry(void) {
  struct ipt_entry *e = ipt_freelist;
  if(e)
    ipt_freelist = e->next_hash; // next_hash 포인터를 재활용
  return e;
}

// 내부: 엔트리를 프리리스트로 반환
static void ipt_free_entry(struct ipt_entry *e) {
  e->pfn = 0; e->pid = 0; e->va = 0; // 디버깅용 초기화
  e->next_hash = ipt_freelist;
  ipt_freelist = e;
}

// 초기화: ipt_init()을 부팅 초기화 루틴에서 호출
void ipt_init(void) {
  int i;
  initlock(&ipt_lock, "ipt");
  acquire(&ipt_lock);

  // 해시 버킷 초기화
  for (i = 0; i < IPT_HASH_SZ; i++)
    ipt_buckets[i] = 0;

  // pfn 배열 초기화
  for (i = 0; i < PFNNUM; i++)
    ipt_by_pfn[i] = 0;

  ipt_freelist = 0;
  for (i = PFNNUM - 1; i >= 0; i--) {
    ipt_free_entry(&ipt_pool[i]);
  }

  release(&ipt_lock);
}

// lookup: pid+va -> entry (NULL if none)
struct ipt_entry *ipt_lookup(unsigned pid, unsigned va) {
  unsigned h;
  struct ipt_entry *e;

  // va must be page-aligned in this table's semantics
  va = va & ~((1<<PGSHIFT) - 1);

  acquire(&ipt_lock);
  h = ipt_hash(pid, va);
  for (e = ipt_buckets[h]; e; e = e->next_hash) {
    if (e->pid == pid && e->va == va) {
      // found
      release(&ipt_lock);
      return e;
    }
  }
  release(&ipt_lock);
  return 0;
}

// map: pfn에 (pid,va,flags) 맵핑을 저장 (신규 또는 리맵)
// mappages() 등에서 호출 [cite: 325]
int ipt_map(unsigned pfn, unsigned pid, unsigned va, enum ipt_flags flags) {
  unsigned h;
  struct ipt_entry *e;

  if (pfn >= PFNNUM)
    return 0;
  va = va & ~((1<<PGSHIFT) - 1);

  acquire(&ipt_lock);

  // 1. (pid, va) 매핑이 이미 존재하는지 확인 (리맵 시나리오)
  h = ipt_hash(pid, va);
  for (e = ipt_buckets[h]; e; e = e->next_hash) {
    if (e->pid == pid && e->va == va) {
      // 찾음 (Remap).
      if (e->pfn == pfn) {
        // pfn도 동일. flags만 업데이트
        e->flags = flags;
        release(&ipt_lock);
        return 1;
      }
      
      // pfn이 변경됨. old_pfn 리스트에서 'e'를 제거
      unsigned old_pfn = e->pfn;
      struct ipt_entry *cur = ipt_by_pfn[old_pfn], *prev = 0;
      while (cur) {
        if (cur == e) {
          if (prev) prev->next_pfn = cur->next_pfn;
          else ipt_by_pfn[old_pfn] = cur->next_pfn;
          break;
        }
        prev = cur; cur = cur->next_pfn;
      }
      
      // 새 정보로 'e' 업데이트
      e->pfn = pfn;
      e->flags = flags;
      
      // 새 pfn 리스트에 'e'를 추가 (맨 앞)
      e->next_pfn = ipt_by_pfn[pfn];
      ipt_by_pfn[pfn] = e;
      
      release(&ipt_lock);
      return 1;
    }
  }

  // 2. 새 매핑 (New mapping)
  e = ipt_alloc_entry();
  if (!e) {
    release(&ipt_lock);
    return 0; // 풀(pool) 부족
  }

  // 새 엔트리 정보 채우기
  e->pfn = pfn;
  e->pid = pid;
  e->va = va;
  e->flags = flags;

  // (pid, va) 해시 리스트에 추가 [cite: 324]
  h = ipt_hash(pid, va);
  e->next_hash = ipt_buckets[h];
  ipt_buckets[h] = e;

  // (pfn) 리스트에 추가 (COW 지원) 
  e->next_pfn = ipt_by_pfn[pfn];
  ipt_by_pfn[pfn] = e;

  release(&ipt_lock);
  return 1;
}

// unmap: (pid, va) 매핑을 해제
// uvmdealloc() 등에서 호출 [cite: 325]
int ipt_unmap(unsigned pid, unsigned va) {
  unsigned h;
  struct ipt_entry *e, *prev;

  va = va & ~((1<<PGSHIFT) - 1);

  acquire(&ipt_lock);

  // (pid, va) 해시 리스트에서 엔트리 'e' 찾기 및 제거
  h = ipt_hash(pid, va);
  prev = 0;
  e = ipt_buckets[h];
  while (e) {
    if (e->pid == pid && e->va == va) {
      // 찾아서 해시 리스트에서 제거
      if (prev) prev->next_hash = e->next_hash;
      else ipt_buckets[h] = e->next_hash;
      break;
    }
    prev = e;
    e = e->next_hash;
  }

  if (e == 0) {
    release(&ipt_lock);
    return 0;
  }

  // (pfn) 리스트에서 엔트리 'e' 제거 
  unsigned pfn = e->pfn;
  prev = 0;
  struct ipt_entry *cur = ipt_by_pfn[pfn];
  while (cur) {
    if (cur == e) {
      // 찾아서 pfn 리스트에서 제거
      if (prev) prev->next_pfn = cur->next_pfn;
      else ipt_by_pfn[pfn] = cur->next_pfn;
      break;
    }
    prev = cur;
    cur = cur->next_pfn;
  }
  
  ipt_free_entry(e);  // 엔트리를 프리 리스트로 반환
  release(&ipt_lock); // 락 해제
  sw_tlb_invalidate(pid, va); // SW TLB 캐시 무효화
  return 1;
}