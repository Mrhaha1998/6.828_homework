#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];    // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

#define FEC_US 0x004
#define FEC_WR 0x002
#define FEC_P  0x001
#define FEC_ALL 0x007



void
tlb_invalidate(pde_t *pgdir, void *va)
{
	// Flush the entry only if we're modifying the current address space.
    struct proc *curproc = myproc();
	if (!curproc || curproc->pgdir == pgdir)
		invlpg(va);
}

void
tvinit(void)
{
    int i;

    for(i = 0; i < 256; i++)
        SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
    SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);

    initlock(&tickslock, "time");
}

void
idtinit(void)
{
    lidt(idt, sizeof(idt));
}

//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
    if(tf->trapno == T_SYSCALL){
        if(myproc()->killed)
            exit();
        myproc()->tf = tf;
        syscall();
        if(myproc()->killed)
            exit();
        return;
    }

    switch(tf->trapno){
    case T_IRQ0 + IRQ_TIMER:
        if(cpuid() == 0){
            acquire(&tickslock);
            ticks++;    
            wakeup(&ticks);
            release(&tickslock);
        }
        if(myproc() != 0 && (tf->cs & 3) == 3 
            && myproc()->alarmhandler && --myproc()->alarmticksleft == 0){
            myproc()->alarmticksleft = myproc()->alarmticks;
            // tf->esp -= 4;
            // *(uint*)(tf->esp) = tf->eip;
            // tf->eip = (uint)myproc()->alarmhandler;
            if(!myproc()->inalarmhandler){
                myproc()->inalarmhandler = 1;

                /*  |栈上注入的代码      |
                    ----------------------
                    |保存的寄存器值       |
                    ----------------------
                    |保存的寄存器的值的地址|
                    ----------------------
                    |注入代码地址         |
                    ---------------------- <--esp
                */
                *(uint*)(tf->esp - 4) = 0xc340cd00;    // int 0x49;  第二条指令
                *(uint*)(tf->esp - 8) = 0x000019b8;   // mov  0x19, %eax; 第一条指令
                *(uint*)(tf->esp - 12) = tf->edx;
                *(uint*)(tf->esp - 16) = tf->ecx;
                *(uint*)(tf->esp - 20) = tf->eax;  // 保存的寄存器的值
                *(uint*)(tf->esp - 24) = tf->esp - 20; // 保存的寄存器的值的地址
                *(uint*)(tf->esp - 28) = tf->eip;
                *(uint*)(tf->esp - 32) = tf->esp - 8 ;   // 注入的代码的地址
                tf->esp -= 32;
                tf->eip = (uint)myproc()->alarmhandler;
            }
        }
        lapiceoi();
        break;
    case T_IRQ0 + IRQ_IDE:
        ideintr();
        lapiceoi();
        break;
    case T_IRQ0 + IRQ_IDE+1:
        // Bochs generates spurious IDE1 interrupts.
        break;
    case T_IRQ0 + IRQ_KBD:
        kbdintr();
        lapiceoi();
        break;
    case T_IRQ0 + IRQ_COM1:
        uartintr();
        lapiceoi();
        break;
    case T_IRQ0 + 7:
    case T_IRQ0 + IRQ_SPURIOUS:
        cprintf("cpu%d: spurious interrupt at %x:%x\n",
                        cpuid(), tf->cs, tf->eip);
        lapiceoi();
        break;
    case T_PGFLT:
        if(myproc() != 0 && (tf->err & FEC_US)) {
            struct proc *curproc;
            uint error, faddr, a;
            pte_t *pte;
            char *mem;

            mem = 0;
            error = tf->err;
            faddr = PGROUNDDOWN(rcr2());
            curproc = myproc();
            if((error & FEC_ALL) == FEC_ALL){   
                // cow casue the fault
                pte = walkpgdir(curproc->pgdir, (void*)faddr, 0);
                if(pte == 0 || !((*pte & PTE_P) && (*pte & PTE_COW)))
                    goto bad;
                if((mem = kalloc()) == 0){
                    cprintf("trap out of memory\n");
                    goto bad;
                }    
                a = PTE_ADDR(*pte);
                memmove(mem, P2V(a), PGSIZE);
                goto buildmap;
            }
            if(curproc->stack.end <= faddr && faddr < curproc->heap.end) {
                // lazy allocation
                if((mem = kalloc()) == 0){
                    cprintf("trap out of memory\n");
                    goto bad;
                }    
                memset(mem, 0, PGSIZE);
                goto buildmap;
            }
        buildmap:
            if(mappage(curproc->pgdir, (void*)faddr, V2P(mem), PTE_W|PTE_U) < 0){
                cprintf("trap out of memory (2)\n");
                kfree(mem);
                goto bad;
            }
            tlb_invalidate(curproc->pgdir, (void *)faddr);
            break;    
        bad:    
            cprintf("pid %d %s: trap %d err %d on cpu %d "
                "eip 0x%x addr 0x%x--kill proc\n",
                myproc()->pid, myproc()->name, tf->trapno,
                tf->err, cpuid(), tf->eip, rcr2());
            myproc()->killed = 1;
            break;
        }    

    //PAGEBREAK: 13
    default:
        if(myproc() == 0 || (tf->cs&3) == 0){
            // In kernel, it must be our mistake.
            cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
                            tf->trapno, cpuid(), tf->eip, rcr2());
            panic("trap");
        }
    }

    // Force process exit if it has been killed and is in user space.
    // (If it is still executing in the kernel, let it keep running
    // until it gets to the regular system call return.)
    if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
        exit();

    // Force process to give up CPU on clock tick.
    // If interrupts were on while locks held, would need to check nlock.
    if(myproc() && myproc()->state == RUNNING &&
        tf->trapno == T_IRQ0+IRQ_TIMER)
        yield();

    // Check if the process has been killed since we yielded
    if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
        exit();
}
