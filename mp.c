// Multiprocessor support
// Search memory for MP description structures.
// http://developer.intel.com/design/pentium/datashts/24201606.pdf

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mp.h"
#include "x86.h"
#include "mmu.h"
#include "proc.h"

struct cpu cpus[NCPU];
int ncpu;
uchar ioapicid;


// 检查 checksum
static uchar
sum(uchar *addr, int len)
{
    int i, sum;

    sum = 0;
    for(i=0; i<len; i++)
        sum += addr[i];
    return sum;
}

// Look for an MP structure in the len bytes at addr.
static struct mp*
mpsearch1(uint a, int len)
{
    uchar *e, *p, *addr;

    addr = P2V(a);
    e = addr+len;
    for(p = addr; p < e; p += sizeof(struct mp))
        if(memcmp(p, "_MP_", 4) == 0 && sum(p, sizeof(struct mp)) == 0)
            return (struct mp*)p;
    return 0;
}

// Search for the MP Floating Pointer Structure, which according to the
// spec is in one of the following three locations:
// 1) in the first KB of the EBDA;
// 2) in the last KB of system base memory;
// 3) in the BIOS ROM between 0xE0000 and 0xFFFFF.
// https://stanislavs.org/helppc/bios_data_area.html
static struct mp*
mpsearch(void)
{
    uchar *bda;
    uint p;
    struct mp *mp;

    bda = (uchar *) P2V(0x400);  // DS = 40
    if((p = ((bda[0x0F]<<8)| bda[0x0E]) << 4)){  // 40:0e 40:0f    存放的是 ebda的地址
        if((mp = mpsearch1(p, 1024)))
            return mp;
    } else {
        p = ((bda[0x14]<<8)|bda[0x13])*1024;  // 40:13 40:14 存放的是 base memeory 地址 以1k为单位，所以乘上1024
        if((mp = mpsearch1(p-1024, 1024)))
            return mp;
    }
    return mpsearch1(0xF0000, 0x10000);   
}

// Search for an MP configuration table.    For now,
// don't accept the default configurations (physaddr == 0).
// Check for correct signature, calculate the checksum and,
// if correct, check the version.
// To do: check extended table checksum.
static struct mpconf*
mpconfig(struct mp **pmp)
{
    struct mpconf *conf;
    struct mp *mp;

    if((mp = mpsearch()) == 0 || mp->physaddr == 0)
        return 0;
    conf = (struct mpconf*) P2V((uint) mp->physaddr);
    if(memcmp(conf, "PCMP", 4) != 0)
        return 0;
    if(conf->version != 1 && conf->version != 4)
        return 0;
    if(sum((uchar*)conf, conf->length) != 0)
        return 0;
    *pmp = mp;
    return conf;
}

void
mpinit(void)
{
    uchar *p, *e;
    int ismp;
    struct mp *mp;
    struct mpconf *conf;
    struct mpproc *proc;
    struct mpioapic *ioapic;

    if((conf = mpconfig(&mp)) == 0)
        panic("Expect to run on an SMP");
    ismp = 1;
    lapic = (uint*)conf->lapicaddr;
    for(p=(uchar*)(conf+1), e=(uchar*)conf+conf->length; p<e; ){    // conf + 1 跳过header
        switch(*p){
        case MPPROC:
            proc = (struct mpproc*)p;
            if(ncpu < NCPU) {
                cpus[ncpu].apicid = proc->apicid;    // apicid may differ from ncpu
                ncpu++;
            }
            p += sizeof(struct mpproc);
            continue;
        case MPIOAPIC:
            ioapic = (struct mpioapic*)p;
            ioapicid = ioapic->apicno;
            p += sizeof(struct mpioapic);
            continue;
        case MPBUS:
        case MPIOINTR:
        case MPLINTR:
            p += 8;
            continue;
        default:
            ismp = 0;
            break;
        }
    }
    if(!ismp)
        panic("Didn't find a suitable machine");

    // if imcrp bit is set， the imcr is present and pic mode is implemented.
    // This register controls whether the interrupt signals that reach the BSP 
    // come from the master PIC or from the local APIC.
    // Before entering Symmetric I/O Mode, either the BIOS or the operating 
    // system must switch out of PIC Mode by changing the IMCR.
    if(mp->imcrp){
        // Bochs doesn't support IMCR, so this doesn't run on Bochs.
        // But it would on real hardware.

        // The IMCR is supported by two read/writable or write-only I/O ports, 22h and 23h, which receive
        // address and data respectively.  To access the IMCR, write a value of 70h to I/O port 22h, which
        // selects the IMCR.  Then write the data to I/O port 23h.  The power-on default value is zero, which
        // connects the NMI and 8259 INTR lines directly to the BSP.  Writing a value of 01h forces the
        // NMI and 8259 INTR signals to pass through the APIC LINT1 and LINT0 .
        outb(0x22, 0x70);     // Select IMCR
        outb(0x23, inb(0x23) | 1);    // Mask external interrupts.
    }
}
