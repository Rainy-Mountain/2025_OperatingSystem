// kalloc.c (모든 B/C 과제 버그가 수정된 최종본)

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"
#include "ipt.h"

static inline uint paddr_to_pfn(uint pa) { return pa >> 12; }
static inline uint kva_to_pfn(char *kva) { return paddr_to_pfn(V2P(kva)); }

void freerange(void *vstart, void *vend);
extern char end[]; // first address after kernel loaded from ELF file

struct run {
  struct run *next;
};

struct {
  struct spinlock lock; // B/C 과제를 위한 단일 락
  int use_lock;
  struct run *freelist;
} kmem;

struct physframe_info {
  uint frame_index;
  int  allocated;
  int  pid;
  uint start_tick;
  int  ref_count;  // (사용자님이 사용한 이름 'ref_count' 유지)
};
struct physframe_info pf_info[PFNNUM];

static void
pfinfo_init(void){
  for(int i=0;i<PFNNUM;i++){
    pf_info[i].frame_index = i;
    pf_info[i].allocated   = 0;
    pf_info[i].pid         = -1;
    pf_info[i].start_tick  = 0;
    pf_info[i].ref_count   = 0;
  }
}

void
kinit1(void *vstart, void *vend)
{
  initlock(&kmem.lock, "kmem"); // 락은 kmem.lock 하나만 초기화
  kmem.use_lock = 0;
  pfinfo_init(); // pf_info 테이블 초기화
  freerange(vstart, vend);
  cprintf("kinit1: end=%p vend=%p freelist=%p\n", end, P2V(16*1024*1024), kmem.freelist);
  if (kmem.freelist == 0) panic("kinit1: freelist empty");
}

void
kinit2(void *vstart, void *vend)
{
  freerange(vstart, vend);
  kmem.use_lock = 1;
  ipt_init(); // IPT 초기화
  sw_tlb_init(); // SW TLB 초기화
}

void
freerange(void *vstart, void *vend)
{
  char *p;
  p = (char*)PGROUNDUP((uint)vstart);
  for(; p + PGSIZE <= (char*)vend; p += PGSIZE)
    kfree(p);
}

void
kfree(char *v){
  struct run *r;
  if(((uint)v % PGSIZE) || v < end || V2P(v) >= PHYSTOP) panic("kfree");

  if(kmem.use_lock)
    acquire(&kmem.lock); // [수정] 락은 kmem.lock 하나만 사용

  struct physframe_info *pf = &pf_info[kva_to_pfn(v)];

  if(pf->allocated == 1) {
    if(pf->ref_count <= 0)
      panic("kfree: allocated page ref_count <= 0");
    
    pf->ref_count--; // 참조 카운트 1 감소

    if(pf->ref_count > 0) {
      // 아직 COW로 공유 중. 메모리 해제 안 함.
      if(kmem.use_lock) release(&kmem.lock);
      return; 
    }
    
    // ref_count가 0이 됨. pf_info 정보 초기화.
    pf->allocated = 0;
    pf->pid = -1;
    pf->start_tick = 0;
    
  } else {
    // [Case 2: Initialization (freerange)]
    // 'allocated'가 0인 페이지 (ref_count == 0 이어야 함)
    // [버그 수정] 부팅 시 ref_count가 0이므로 panic을 호출하면 안 됨.
    if(pf->ref_count != 0)
      panic("kfree: unallocated page ref_count != 0");
  }

  // [공통] Case 1(ref_count=0)과 Case 2(freerange) 모두 freelist에 추가
  memset(v, 1, PGSIZE); 
  r = (struct run*)v;
  r->next = kmem.freelist;
  kmem.freelist = r;
  
  if(kmem.use_lock)
    release(&kmem.lock);
}

char*
kalloc(void)
{
  struct run *r;

  // 필요 시 kmem 락 획득
  if (kmem.use_lock)
    acquire(&kmem.lock);

  r = kmem.freelist;
  if (r) {
    kmem.freelist = r->next;

    // pf_info 갱신은 락 잡은 상태에서!
    struct physframe_info *pf = &pf_info[kva_to_pfn((char*)r)];
    pf->allocated = 1;
    pf->ref_count = 1;

    if (kmem.use_lock) {
      // 초기 부팅 직후엔 myproc()가 NULL일 수 있음 → 널 가드
      struct proc *p = myproc();
      if (p) pf->pid = p->pid;
      else   pf->pid = -1;

      // ticks는 tickslock으로 보호해서 읽기
      acquire(&tickslock);
      pf->start_tick = ticks;
      release(&tickslock);
    } else {
      // 부팅 중 커널이 쓰는 페이지
      pf->pid = -1;
      pf->start_tick = 0;
    }
  }

  if (kmem.use_lock)
    release(&kmem.lock);

  return (char*)r;
}

// C를 위해 추가한 ref_count 증가 함수
void
ref_count_inc(uint pa)
{
  if(pa >= PHYSTOP)
    panic("ref_count_inc: pa >= PHYSTOP");

  // [수정] pf_lock 대신 kmem.lock 사용
  if(kmem.use_lock) 
    acquire(&kmem.lock);
    
  pf_info[paddr_to_pfn(pa)].ref_count++;
  
  if(kmem.use_lock)
    release(&kmem.lock);
}

void
ref_count_dec(uint pa)
{
  if(pa >= PHYSTOP)
    panic("ref_count_dec: pa >= PHYSTOP");

  if(kmem.use_lock) acquire(&kmem.lock);

  int pfn = pa >> 12;
  int rc = --pf_info[pfn].ref_count;

  // 마지막 참조일 때만 실제로 free
  if(rc == 0){
    pf_info[pfn].allocated = 0;
    pf_info[pfn].pid = -1;
    pf_info[pfn].start_tick = 0;
    if(kmem.use_lock) release(&kmem.lock);
    kfree(P2V(pa));                 // ← 진짜 물리 해제는 여기서만
    return;
  }

  if(kmem.use_lock) release(&kmem.lock);
}