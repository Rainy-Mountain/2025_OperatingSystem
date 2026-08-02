// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"

static inline uint paddr_to_pfn(uint pa) { return pa >> 12; }   // 4KB
static inline uint kva_to_pfn(char *kva) { return paddr_to_pfn(V2P(kva)); }

void freerange(void *vstart, void *vend);
extern char end[]; // first address after kernel loaded from ELF file
                   // defined by the kernel linker script in kernel.ld

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  int use_lock;
  struct run *freelist;
} kmem;

struct physframe_info pf_info[PFNNUM];

static void
pfinfo_init(void){
  for(int i=0;i<PFNNUM;i++){
    pf_info[i].frame_index = i;
    pf_info[i].allocated   = 0;
    pf_info[i].pid         = -1;
    pf_info[i].start_tick  = 0;
  }
}

// Initialization happens in two phases.
// 1. main() calls kinit1() while still using entrypgdir to place just
// the pages mapped by entrypgdir on free list.
// 2. main() calls kinit2() with the rest of the physical pages
// after installing a full page table that maps them on all cores.
void
kinit1(void *vstart, void *vend)
{
  initlock(&kmem.lock, "kmem");
  kmem.use_lock = 0;
  freerange(vstart, vend);
}

void
kinit2(void *vstart, void *vend)
{
  freerange(vstart, vend);
  kmem.use_lock = 1;
  pfinfo_init();
}

void
freerange(void *vstart, void *vend)
{
  char *p;
  p = (char*)PGROUNDUP((uint)vstart);
  for(; p + PGSIZE <= (char*)vend; p += PGSIZE)
    kfree(p);
}
//PAGEBREAK: 21
// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(char *v){
  if(((uint)v % PGSIZE) || v < end || V2P(v) >= PHYSTOP) panic("kfree");
  memset(v,1,PGSIZE);

  if(kmem.use_lock) acquire(&kmem.lock);
  if(kmem.use_lock){
    uint pfn = V2P(v) >> 12;
    if(pfn < PFNNUM){
      pf_info[pfn].allocated = 0;
      pf_info[pfn].pid = -1;
      pf_info[pfn].start_tick = 0;
    }
  }
  struct run *r=(struct run*)v;
  r->next = kmem.freelist;
  kmem.freelist = r;
  if(kmem.use_lock) release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
char* 
kalloc(void){
  struct run *r;
  if(kmem.use_lock) acquire(&kmem.lock);
  r = kmem.freelist;
  if(r) kmem.freelist = r->next;

  if(kmem.use_lock && r){
    uint pfn = kva_to_pfn((char*)r);
    if(pfn < PFNNUM){
      int owner = myproc() ? myproc()->pid : -1;
      pf_info[pfn].allocated = 1;
      pf_info[pfn].pid = owner;
      acquire(&tickslock);
      pf_info[pfn].start_tick = ticks;
      release(&tickslock);
    }
  }
  if(kmem.use_lock) release(&kmem.lock);

  if(r) memset((char*)r, 5, PGSIZE);
  return (char*)r;
}

