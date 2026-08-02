#include "types.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"

extern struct ptable ptable;

struct k_procinfo{
	int pid, ppid, state;
	uint sz;
	char name[16];
};

int
sys_fork(void)
{
  return fork();
}

int
sys_exit(void)
{
  exit();
  return 0;  // not reached
}

int
sys_wait(void)
{
  return wait();
}

int
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

int
sys_getpid(void)
{
  return myproc()->pid;
}

int
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

int
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

// new system call handler
int 
sys_hello_number(void) {
	int n;
	if(argint(0, &n) < 0)
		return -1;
	cprintf("Hello, xv6! Your number is %d\n", n);
	return n*2;
}

// get_procinfo system call handler
int sys_get_procinfo(void){
	int pid;
	char *uaddr;
	struct proc *p, *t;
	struct k_procinfo kinfo;

	if(argint(0, &pid) < 0) return -1;
	if(argptr(1, &uaddr, sizeof(struct k_procinfo)) < 0) return -1;

	acquire(&ptable.lock);
	if(pid <= 0) t = myproc();
	else {
		t = 0;
		for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
			if(p->pid == pid) {t = p; break;}
	}
	if(t == 0 || (t -> state == UNUSED)) {release(&ptable.lock); return -1;}

	// adding the code
	kinfo.pid = t->pid;
	kinfo.ppid = (t->parent) ? t->parent->pid : 0;
	kinfo.state = t->state;
	kinfo.sz = t->sz;
	safestrcpy(kinfo.name, t->name, sizeof(kinfo.name));

	release(&ptable.lock);

	// copy to user space
	if(copyout(myproc() -> pgdir, (uint)uaddr, (void*)&kinfo, sizeof(kinfo)) < 0)
		return -1;
	return 0;
}
