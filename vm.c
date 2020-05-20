#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"

extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()
char buffer[PGSIZE];    //Task2, read from swapFile.
// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
  struct cpu *c;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpuid()];
  c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
  lgdt(c->gdt, sizeof(c->gdt));
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if(*pde & PTE_P){
    pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table
    // entries, if necessary.
    *pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
static int
mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
  char *a, *last;
  pte_t *pte;

  a = (char*)PGROUNDDOWN((uint)va);
  last = (char*)PGROUNDDOWN(((uint)va) + size - 1);
  for(;;){
    if((pte = walkpgdir(pgdir, a, 1)) == 0)
      return -1;
    if(*pte & PTE_P)
      panic("remap");
    *pte = pa | perm | PTE_P;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
  void *virt;
  uint phys_start;
  uint phys_end;
  int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    PTE_W}, // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text+rodata
 { (void*)data,     V2P(data),     PHYSTOP,   PTE_W}, // kern data+memory
 { (void*)DEVSPACE, DEVSPACE,      0,         PTE_W}, // more devices
};

// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if((pgdir = (pde_t*)kalloc()) == 0)
    return 0;
  memset(pgdir, 0, PGSIZE);
  if (P2V(PHYSTOP) > (void*)DEVSPACE)
    panic("PHYSTOP too high");
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start,
                (uint)k->phys_start, k->perm) < 0) {
      freevm(pgdir);
      return 0;
    }
  return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
  lcr3(V2P(kpgdir));   // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
  if(p == 0)
    panic("switchuvm: no process");
  if(p->kstack == 0)
    panic("switchuvm: no kstack");
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");

  pushcli();
  mycpu()->gdt[SEG_TSS] = SEG16(STS_T32A, &mycpu()->ts,
                                sizeof(mycpu()->ts)-1, 0);
  mycpu()->gdt[SEG_TSS].s = 0;
  mycpu()->ts.ss0 = SEG_KDATA << 3;
  mycpu()->ts.esp0 = (uint)p->kstack + KSTACKSIZE;
  // setting IOPL=0 in eflags *and* iomb beyond the tss segment limit
  // forbids I/O instructions (e.g., inb and outb) from user space
  mycpu()->ts.iomb = (ushort) 0xFFFF;
  ltr(SEG_TSS << 3);
  lcr3(V2P(p->pgdir));  // switch to process's address space
  popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W|PTE_U);
  memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n;
  pte_t *pte;

  if((uint) addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, P2V(pa), offset+i, n) != n)
      return -1;
  }
  return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
    //cprintf("*******entering allocuvm,    pid = %d\n", myproc()->pid);
  char *mem;
  uint a;

  if(newsz >= KERNBASE)
    return 0;
  if(newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  for(; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pgdir, (char*)a, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
      cprintf("allocuvm out of memory (2)\n");
      deallocuvm(pgdir, newsz, oldsz);
      kfree(mem);
      return 0;
    }
    //////////////////Task 2///////////////////
    if(myproc()->pid > 2 && !nonePolicy()){
        //cprintf("---allocuvm !nonepolicy,   myproc()->numRamPages=%d   pid=%d\n", myproc()->numRamPages, myproc()->pid);
        if(myproc()->numRamPages < MAX_PSYC_PAGES){
            //cprintf("-if\n");
            //int x = findFreeIndexRamArr();
            //cprintf("-  x = %d\n", x);
            insertRamArr(findFreeIndexRamArr(), pgdir, a);
        }
        else{
            //cprintf("-else\n");
            swap(pgdir, a);
        }
    }
    //////////////////////////////////////////
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int
deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
    //cprintf("*******entering deallocuvm,    pid = %d\n", myproc()->pid);
  pte_t *pte;
  uint a, pa;

  if(newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  for(; a  < oldsz; a += PGSIZE){
    pte = walkpgdir(pgdir, (char*)a, 0);
    if(!pte)
      a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
    else if((*pte & PTE_P) != 0){
      pa = PTE_ADDR(*pte);
      if(pa == 0)
        panic("kfree");
      char *v = P2V(pa);
      kfree(v);
      //////Task 2.2/////
      if(!nonePolicy() && myproc()->pid > 2){
          //cprintf("-clear\n");
          removeFromRamArr(pgdir, a);
        /*for(int i = 0; i < MAX_PSYC_PAGES; i++){
          if(myproc()->ramArr[i].va == a && myproc()->ramArr[i].pgdir == pgdir)
            myproc()->ramArr[i].state = UNUSEDP;
            myproc()->ramArr[i].va = 0;
            myproc()->ramArr[i].pgdir = 0;
            myproc()->ramArr[i].offset = 0;
        }*/
      }
      //////////////////
      *pte = 0;
    }
  }
  return newsz;
}

// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir)
{
  uint i;

  if(pgdir == 0)
    panic("freevm: no pgdir");
  deallocuvm(pgdir, KERNBASE, 0);
  for(i = 0; i < NPDENTRIES; i++){
    if(pgdir[i] & PTE_P){
      char * v = P2V(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }
  kfree((char*)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if(pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, uint sz)
{
  pde_t *d;
  pte_t *pte;
  uint pa, i, flags;
  char *mem;

  if((d = setupkvm()) == 0)
    return 0;
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
    ///////Task 2//////
    if(*pte & PTE_PG){
        pte = walkpgdir(d, (void *) i, 0);
        *pte |= PTE_PG;
        *pte &= ~PTE_P;
        lcr3(V2P(myproc()->pgdir)); 
        continue;
    }
    //////////////////
    if(!(*pte & PTE_P))
      panic("copyuvm: page not present");
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto bad;
    memmove(mem, (char*)P2V(pa), PGSIZE);
    if(mappages(d, (void*)i, PGSIZE, V2P(mem), flags) < 0)
      goto bad;
  }
  return d;

bad:
  freevm(d);
  return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if((*pte & PTE_P) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  return (char*)P2V(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uint va, void *p, uint len)
{
  char *buf, *pa0;
  uint n, va0;

  buf = (char*)p;
  while(len > 0){
    va0 = (uint)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char*)va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

//Task1:
int
checkPTE_W(uint va){
    pte_t* pte = walkpgdir(myproc()->pgdir, (void*)va, 0);
    return *pte & PTE_W;
}

//Task 3:
int
checkPTE_PG(uint va){
    pte_t* pte = walkpgdir(myproc()->pgdir, (void*)va, 0);
    return *pte & PTE_PG;
}
int
chooseIndexToSwap()
{
    #ifdef NONE
    //cprintf("~~~chooseIndexToSwap NONE\n");
    return -1;
    #endif
    
    #ifdef LIFO
    //cprintf("~~~chooseIndexToSwap LIFO\n");
    return getLIFO();
    #endif
    
    #ifdef SCFIFO
    //cprintf("~~~chooseIndexToSwap SCFIFO\n");
    return getSCFIFO();
    #endif
}

int
nonePolicy()
{
    #ifdef NONE
        return 1;
    #endif
	return 0;
}

int
getLIFO()
{
    int last = 0;
    int index = -1;
    for(int i = 0; i < MAX_PSYC_PAGES; i++){
        //cprintf("getLIFO:   i = %d, myproc()->ramArr[i].state = %d,  myproc()->ramArr[i].loadOrder = %d,    last = %d\n", i, myproc()->ramArr[i].state, myproc()->ramArr[i].loadOrder, last);
        if(myproc()->ramArr[i].state == USEDP && (myproc()->ramArr[i].loadOrder) > last){
            last = myproc()->ramArr[i].loadOrder;
            index = i;
        }
    //    cprintf("---    index=%d\n", index);
    }
   // cprintf("~ends getLIFO\n");
    return index;
}

int
getSCFIFO()
{
    pte_t* pte;
    int first = -1;
    int index = -1;
    check:
    for(int i = 0; i < MAX_PSYC_PAGES; i++)
        if((myproc()->ramArr[i].state == USEDP && myproc()->ramArr[i].loadOrder < first) || index == -1){
            first = myproc()->ramArr[i].loadOrder;
            index = i;
        }
    pte = walkpgdir(myproc()->pgdir, (void*)myproc()->ramArr[index].va, 0);
    if(*pte & PTE_A){
        *pte &= ~PTE_A;
        myproc()->ramArr[index].loadOrder = myproc()->loadCounter++;
        goto check;
    }
    return index;
}

//Task 2:
void
removeFromRamArr(pde_t *pgdir, uint va){
  for (int i = 0; i < MAX_PSYC_PAGES; i++) {
    if (myproc()->ramArr[i].va == va && myproc()->ramArr[i].pgdir == pgdir){
        //cprintf("       i = %d\n", i);
      myproc()->ramArr[i].state = UNUSEDP;
      /*myproc()->ramArr[i].va = 0;
      myproc()->ramArr[i].pgdir = 0;
      myproc()->ramArr[i].offset = 0;
      myproc()->ramArr[i].loadOrder = 0;*/
      myproc()->numRamPages--;
      return;
    }
  }
}

int
swap(pde_t *pgdir, uint va)
{
    //cprintf("-----ENTERING SWAP \n");
    //cprintf("--------VA is : %d\n");
    pte_t* pte;
    int index = chooseIndexToSwap();
    if(index < 0)
        panic("~~~ swap: chooseIndexToSwap() ===> index = -1\n");
    int swap_index = findFreeIndexSwapArr();
    if(swap_index == -1)
        panic("~~~ cannot swap into full swapFile");
    
    //cprintf("-----before writeToSwapFile with index = %d\n", index);
    writeToSwapFile(myproc(), (char*)myproc()->ramArr[index].va, swap_index*PGSIZE, PGSIZE);
    //cprintf("-----after writeToSwapFile with index = %d\n", index);
    
    insertSwapArr(swap_index, myproc()->ramArr[index].pgdir, myproc()->ramArr[index].va);
     
    pte = walkpgdir(myproc()->ramArr[index].pgdir, (int*)myproc()->ramArr[index].va, 0);
    if(!pte){
        //cprintf("~ !pte\n")
        panic("~~~ swap: pte is null");
        return -1;
    }
    uint pa = PTE_ADDR(*pte);
    kfree(P2V(pa));
    myproc()->ramArr[index].state = UNUSEDP;
    myproc()->numRamPages--; 
    *pte |= PTE_PG;
    *pte &= ~PTE_P;
    lcr3(V2P(myproc()->pgdir));
    insertRamArr(index, pgdir, va);
    //cprintf("------ finished swap\n");
    return 0;
}

/*
int
swap(pde_t *pgdir, uint va)
{
    cprintf("-----ENTERING SWAP \n");
    //cprintf("--------VA is : %d\n");
    pte_t* pte;
    int index = chooseIndexToSwap();
    if(index < 0)
        panic("~~~cannot swap in NONE policy!");
    int swap_index = findFreeIndexSwapArr();
    if(swap_index == -1)
        panic("~~~cannot swap into full swapFile");
    pte = walkpgdir(myproc()->ramArr[index].pgdir, (int*)myproc()->ramArr[index].va, 0);
    if(!pte){
        cprintf("~ !pte\n");
        return -1;
    }
    uint pa = PTE_ADDR(*pte);
    if(writeToSwapFile(myproc(), (char*)myproc()->ramArr[index].va, swap_index*PGSIZE, PGSIZE) == -1){
        cprintf("~ writeToSwapFile = -1\n");
        return -1;
    }
    insertSwapArr(swap_index, myproc()->ramArr[index].pgdir, myproc()->ramArr[index].va);
    kfree(P2V(pa));
    myproc()->ramArr[index].state = UNUSEDP;
    *pte |= PTE_PG;
    *pte &= ~PTE_P;
    lcr3(V2P(myproc()->pgdir));
    insertRamArr(index, pgdir, va);
    cprintf("------ finished swap\n");
    return 0;
}*/

int
findInSwapArr(int va)
{
    for(int i = 0; i < MAX_TOTAL_PAGES - MAX_PSYC_PAGES; i++){
        if(myproc()->swapArr[i].va == va)
            return i;
    }
    return -1;
}

int
findFreeIndexRamArr()
{
    for(int i = 0; i < MAX_PSYC_PAGES; i++){
        if(myproc()->ramArr[i].state == UNUSEDP)
            return i;
    }
    return -1;
}

int
findFreeIndexSwapArr()
{
    for(int i = 0; i < MAX_TOTAL_PAGES - MAX_PSYC_PAGES; i++){
        if(myproc()->swapArr[i].state == UNUSEDP)
            return i;
    }
    return -1;
}

void
insertRamArr(int index, pde_t *pgdir, uint va)
{
    myproc()->ramArr[index].state = USEDP;
    myproc()->ramArr[index].va = va;
    myproc()->ramArr[index].pgdir = pgdir;
    myproc()->ramArr[index].offset = 0;
    myproc()->ramArr[index].loadOrder = myproc()->loadCounter++;
    myproc()->numRamPages++;
}

void
insertSwapArr(int index, pde_t *pgdir, uint va)
{
    myproc()->swapArr[index].state = USEDP;
    myproc()->swapArr[index].va = va;
    myproc()->swapArr[index].pgdir = pgdir;
    myproc()->swapArr[index].offset = index*PGSIZE;
    myproc()->swapArr[index].loadOrder = 0;
    myproc()->numSwapPages++;
    myproc()->totalPageOut++;//total
}

	
int
fromSwapArrToRamArr(int va, uint pa, int ram_index, char* buf)
{
    //cprintf("*** entering fromSwapArrToRamArr   ***\n");
    pte_t* pte = walkpgdir(myproc()->pgdir, (void*)va, 0);
    *pte |= PTE_P | PTE_W | PTE_U;
    *pte &= ~PTE_PG;
    *pte = (*pte & 0x00000fff) | pa;
    //*pte |= pa;
    //*pte |= V2P(ka); //pa = V2P(ka)
    lcr3(V2P(myproc()->pgdir));
    
    int swap_index = findInSwapArr(va);
    if(swap_index == -1)
            return -1;
    readFromSwapFile(myproc(), buf, myproc()->swapArr[swap_index].offset, PGSIZE);
    //memmove((void *)(va), buffer, PGSIZE);
    //readFromSwapFile(myproc(), (char*)va, myproc()->swapArr[swap_index].offset, PGSIZE);
    
    ram_index = findFreeIndexRamArr();
    insertRamArr(ram_index, myproc()->pgdir, va);
    myproc()->swapArr[swap_index].state = UNUSEDP;
    myproc()->numSwapPages--;
    
    return 1;
}


int
backtoRam(uint cr2)
{
    //cprintf("*** entering backtoRam   ***\n");
    int ram_index;
    pte_t* pte;
    int va = PGROUNDDOWN(cr2);
    char* ka = kalloc();
    memset(ka, 0, PGSIZE);
    if(myproc()->numRamPages < MAX_PSYC_PAGES){// available space in ramArr, no need for swapping 
        ram_index = findFreeIndexRamArr();
        fromSwapArrToRamArr(va, V2P(ka), ram_index, (char*)va);
        return 1;
    }
    ram_index = chooseIndexToSwap();
    struct page outPage = myproc()->ramArr[ram_index];
    
    fromSwapArrToRamArr(va, V2P(ka), ram_index, buffer);/////
    
    pte = walkpgdir(myproc()->pgdir, (void*)va, 0);
    int out_page_addrs = PTE_ADDR(*pte);
    memmove((void *)(ka), buffer, PGSIZE);
    int  free_swap_index = findFreeIndexSwapArr();
    insertSwapArr(free_swap_index, outPage.pgdir, outPage.va);

    
    pte = walkpgdir(outPage.pgdir, (void*)outPage.va, 0);
    *pte |= PTE_PG;
    *pte &= ~PTE_P;
    lcr3(V2P(myproc()->pgdir));
    char *v = P2V(out_page_addrs);
    kfree(v);
    
    return 1;
}






/// BEGIN CHANGE TASK 1



// get address for a page (as uint)
// set flags to this address' PDE
// if SET==1 than & set with flags
// if SET==0 then | set with flags
int
set_flags(uint va, int flags, int set) {
    
    pte_t *pte;
    struct proc *currproc;
    currproc = myproc();
    //flush the TLB
    lcr3(V2P(currproc->pgdir));
    pte = walkpgdir(currproc->pgdir, (void *) va, 0);//get phizicall adress
    if (pte) {
        if (!set) { //add the flags using or
            *pte |= flags;
        }
        else { //set the flags using and
            *pte &= flags;
        }
        //upon sucess return 1
        return 1;
    }
    //upon failuire return -1
    return -1;
}


/// this function is in order to determine the cause of the fault when we need to
int
get_flags(uint va) {
    
    pte_t *pte;
    struct proc *currproc;
    currproc = myproc();
    pte = walkpgdir(currproc->pgdir, (void *) va, 0); //get phizicall adress
    if (pte == 0)
        return -1;
    return PTE_FLAGS(*pte);
}

int
incNumProtected(int i){
    
    myproc()->numOfProtected += i;
    return 0;
    
}

 

/// END CHANGE TASK 1

//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.


//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.

