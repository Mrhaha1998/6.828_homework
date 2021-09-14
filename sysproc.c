#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"

struct callerregs {
    uint eax;
    uint ecx;
    uint edx;
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
    return 0;    // not reached
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
    int a, stack, n;
    struct proc *curproc;

    if(argint(0, &n) < 0)
        return -1;
    curproc = myproc();    
    a = curproc->heap.start + curproc->heap.sz;
    stack = curproc->stack.end;
    if (a + n < stack || a + n > KERNBASE) {
        return -1;
    }
    if(n < 0)
        return shrinkheap(curproc->pgdir, &curproc->heap, -n);
    if(expandheap(curproc->pgdir, &curproc->heap, n) < 0)
        return -1;
    return a;
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

int
sys_date(void)
{
    char *date;
    if(argptr(0, &date, sizeof(struct rtcdate)) < 0) {
        return -1;
    }
    cmostime((struct rtcdate*)date);
    return 0;
}

int
sys_alarm(void)
{
    int ticks;
    void (*handler)();

    if(argint(0, &ticks) < 0)
        return -1;
    if(argptr(1, (char**)&handler, 1) < 0)
        return -1;
    if((uint)handler >= KERNBASE)
        return -1;
    myproc()->alarmticks = ticks;
    myproc()->alarmticksleft = ticks;
    myproc()->alarmhandler = handler;
    return 0;
}

int
sys_rstoregs(void)
{

    /*  ----------------------
        |栈上注入的代码      |
        ----------------------
        |保存的寄存器值       |
        ----------------------
        |保存的寄存器的值的地址|
        ----------------------  <--  tf->esp
        |注入代码地址         |
        ---------------------- 
                */
    struct callerregs *regs;
    struct trapframe *tf;

    if(argptr(0, (char **)&regs, 3*4) < 0)
        return -1;
    tf = myproc()->tf;
    tf->eax = regs->eax;
    tf->ecx = regs->ecx;
    tf->edx = regs->edx;
    tf->eip = *(uint*)(tf->esp);
    tf->esp +=  28;
    myproc()->inalarmhandler = 0;
    return 0;
}
