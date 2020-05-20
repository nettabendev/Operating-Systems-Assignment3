#include "types.h"
#include "stat.h"
#include "user.h"
#include "param.h"
#include "mmu.h"

// Memory allocator by Kernighan and Ritchie,
// The C programming Language, 2nd ed.  Section 8.7.

typedef long Align;

union header {
  struct {
    union header *ptr;
    uint size;
  } s;
  Align x;
};

typedef union header Header;

static Header base;
static Header *freep;


////// BEGIN TASK 1 defenitions

// like in the regular malloc we need a list to keep track of our assignments and make sure it is page alligned

typedef struct protectedList {
    uint va;
    int used;
    struct protectedList* next;
} protectedList;


static protectedList* protectedList_head;


////// END TASK 1 defenitions

void
free(void *ap)
{
  Header *bp, *p;

  bp = (Header*)ap - 1;
  for(p = freep; !(bp > p && bp < p->s.ptr); p = p->s.ptr)
    if(p >= p->s.ptr && (bp > p || bp < p->s.ptr))
      break;
  if(bp + bp->s.size == p->s.ptr){
    bp->s.size += p->s.ptr->s.size;
    bp->s.ptr = p->s.ptr->s.ptr;
  } else
    bp->s.ptr = p->s.ptr;
  if(p + p->s.size == bp){
    p->s.size += bp->s.size;
    p->s.ptr = bp->s.ptr;
  } else
    p->s.ptr = bp;
  freep = p;
}

static Header*
morecore(uint nu)
{
  char *p;
  Header *hp;

  if(nu < 4096)
    nu = 4096;
  p = sbrk(nu * sizeof(Header));
  if(p == (char*)-1)
    return 0;
  hp = (Header*)p;
  hp->s.size = nu;
  free((void*)(hp + 1));
  return freep;
}

void*
malloc(uint nbytes)
{
  Header *p, *prevp;
  uint nunits;

  nunits = (nbytes + sizeof(Header) - 1)/sizeof(Header) + 1;
  if((prevp = freep) == 0){
    base.s.ptr = freep = prevp = &base;
    base.s.size = 0;
  }
  for(p = prevp->s.ptr; ; prevp = p, p = p->s.ptr){
    if(p->s.size >= nunits){
      if(p->s.size == nunits)
        prevp->s.ptr = p->s.ptr;
      else {
        p->s.size -= nunits;
        p += p->s.size;
        p->s.size = nunits;
      }
      freep = prevp;
      return (void*)(p + 1);
    }
    if(p == freep)
      if((p = morecore(nunits)) == 0)
        return 0;
  }
}





//////////BEGIN CHANGE TASK 1

void *pmalloc(void) {
   // printf(1, "hello pmalloc\n");
    // Allocate first link if missing
    if(protectedList_head == 0){
        //printf(1, " pmalloc should enter here\n");

        protectedList_head = (protectedList*) malloc(sizeof(struct protectedList)); //allocate space for head
        protectedList_head->va = (uint)sbrk(PGSIZE); //allocate virtual space
        protectedList_head->used = 0;// initialize
        protectedList_head->next = 0; //initialize
        //printf(1, " protectedList_head is: %d\n", protectedList_head);
        //printf(1, " protectedList_head next is: %d\n", protectedList_head->next);
    }

    struct protectedList* new_node = protectedList_head;

    // Now look for an unused node.
    // If not found, need to allocate a new node and page for it.
    while(new_node->used != 0){
        if(new_node->used == 1 && new_node->next == 0){ //reached the end of the list, need to allocate and add to list
                printf(1, "adding page\n");

            new_node->next = (protectedList*) malloc(sizeof(struct protectedList));
            // Skip to the newly crreated node
            new_node = new_node->next;
            new_node->used = 0;
            new_node->va = (uint)sbrk(PGSIZE);
            break;
        }
        else{//move up in the list
            new_node = new_node->next;
        }
    }
    

    // changing used status to 1
    new_node->used = 1;
    if(!set_flags(new_node->va, PTE_PM | PTE_P | PTE_U | PTE_W, 0)){ //setting flags
        // If failed, mark as free and turn off PRESENT flag
      new_node->used = 0;
      set_flags(new_node->va, ~PTE_P, 1);
    }

    incNumProtected(1);
    return (void*) new_node->va;
}


//This function will verify that the address of ap has been allocated 
//using pmalloc and that it points to the start of the page. 
//If the above condition holds, it will protect the page and return 1 (any attempts to write to the page will result in a failure). 
// If the above condition does not hold, it returns –1.

int protect_page(void* ap){
   //printf(1, "hello protected page\n");
    struct protectedList* pageNode = protectedList_head;
    //printf(1, " ap is %d\n",ap);
    while(pageNode != 0){ //run through the list of allocated pages until either you are done or you found your pointer
        if(pageNode->va == (uint) ap){
            //printf(1, "found it\n");

            break;
        }
        else
            pageNode = pageNode->next;
    }
    
        //printf(1, "got here\n");
       // printf(1, "pageNode: %d\n" , pageNode);
     //printf(1, "pageNode->va : %d\n", pageNode->va);
    if(pageNode==0 || pageNode->va != (uint) ap) {
        return -1; // didnt find one
    }
    if((get_flags((uint)ap) & PTE_PM)){ // if flag is set as allocated with PMALLOC, then protect the page
       return set_flags((uint) ap, ~PTE_W, 1);//make sure it won't write into it
    }
    return -1;
}

//must called on protected pages
int pfree(void* ap){
    
    struct protectedList* node = protectedList_head;
    while(node != 0){ //go throght page list and search for a page with the right phizical adress
        if(node->va == (uint) ap)
            break;
        else
            node = node->next;
    }

    if(node==0 || node->va != (uint) ap) {// case: reached to the end of page list
        return -1;
    }
    if(node->used ==0){ //case already freed
        return -1;
    }
        
    
    if(!(get_flags((uint)ap) & PTE_PM)){// || (uint)ap != PGROUNDUP((uint) ap)){// if not created by pmalloc or not start of page
        return -1;
    }
    if((uint)ap != PGROUNDUP((uint) ap)){
        return -1;
    }


    //clear flags,  Set writable flag to ON
    set_flags((uint)ap, PTE_W, 0);
    node->used = 0;
      
    incNumProtected(-1);

    return 1;
}


