
#include "types.h"
#include "stat.h"
#include "user.h"

#define PGSIZE 4096
#define LOOPLENGTH 16//MAX_PSYC_PAGES


// policy tests
#define PAGE_NUM(addr) ((uint)(addr) & ~0xFFF)
#define TEST_POOL 500
#define ARR_SIZE 70000

static unsigned long int next = 1;


int protect_page_test() {
  printf(1, "\n*** Protect page test ***\n\n");

  void *mk = malloc(5000);//allocating 2 pages
  void *pm = pmalloc();
   //   printf(1, "pm value: %d\n", pm);

  //void *pm = pmalloc();

  //case protected and start in head of page
  if(protect_page(pm) == -1)
    printf(1, "shouldnt return -1, the page is protected.\n");
  
  //printf(1, "hey\n");
  //case not protected start in the head of page
  if(protect_page(mk) != -1)
    printf(1, "should return -1, the page is not protected.\n");
 
  //printf(1, "hey2\n");
  // case protected start not in head of page
  if(protect_page(pm +3) != -1)
    printf(1, "should return -1, the page is protected but the pointer doesn't point to begining of page.\n");
  
  //printf(1, "hey3\n");
  // case not protected start not in head of page
  if(protect_page(mk + 7) != -1)
    printf(1, "should return -1, the page is not protected and the pointer doesn't point to begining of page.\n");
  
 
  //printf(1, "hey4\n");
  
  if(pfree(mk) == 1){
    printf(1, "pfree shouldn't work for this page, it is not protected \n");
  }
  //printf(1, "hey5\n");

  
 // printf(1, "pm value: %d\n", pm);
  if(pfree(pm) == -1){
    printf(1, "pfree should work for this page, it is protected \n");
  }
    //printf(1, "hey4\n");

  if(pfree(pm) != -1){
    printf(1, " pfree shouldnt work for this page, it was freed already \n");
  }
  //  printf(1, "hey4\n");
   

  printf(1, "\n*** Protect page test end ***\n\n");
    return 0;
}


int getRandNumber() {
  next = next * 1103579545 + 18755;
  return (unsigned int)(next/65792) % ARR_SIZE;
}


/*
Policies Test:
The process has 21 pages - 1 code 1 space 1 stack 18 malloc
Using pseudoRNG to access a single cell in the array and put a number in it.
Idea behind the algorithm:
	Space page will be swapped out sooner or later with scfifo. if selction - NONE the test will fail - or not beeing able to reach place in memory or malloc fails.
	Since no one calls the space page, an extra page is needed to play with swapping (hence the #17).
	We selected a single page and reduced its page calls to see if scfifo will become more efficient.

*/
void policiesTest(){
	char * arr;
	int i;
	int rndm;
	arr = malloc(ARR_SIZE); //allocates 18 pages so that overall there are 19 pages- to allow more then one swapping in scfifo)
	for (i = 0; i < TEST_POOL; i++) {
		rndm = getRandNumber();	//generates a pseudo random number between 0 and ARR_SIZE
		while (PGSIZE*12-8 < rndm && rndm < PGSIZE*12+PGSIZE/2-8)
			rndm = getRandNumber(); //gives page #15 50% less chance of being selected
				//(what the while loop does is to pick a number that isn't in the first half of page #15)
		arr[rndm] = 'X';				//write to memory
	}
	free(arr);
        
        //now press cntrl^P to veiw results
}


/*
	Test for swapping machanism in fork.
	Best tested when LIFO is used (for more swaps)
*/
void forkTest(){
        int i, j, num;
    int pid = 0;
    char * Fmem[LOOPLENGTH];
    char input[5];
    
    
    printf(1,"\n\n Starting myMemTest : \n\n"); 
    printf(1,"\n\n  myMemTest : starting sbrking ,allocating 13 pages\n\n"); 
    
    for (i = 0; i < LOOPLENGTH - 3; i++)
    {
        Fmem[i] = sbrk(PGSIZE);
        //sbrk(PGSIZE)= address of the memory
        *Fmem[i] = i;
    }


    for (i = 100; i < 120; i++)
    {
        //filling the pages
        *Fmem[(i % 3)] = i;
        printf(1, "*Fmem[%d] at adress %d was accessed in tick %d\n", i % 3,Fmem[(i%3)], uptime());
    }

    for (i = LOOPLENGTH - 3; i < LOOPLENGTH; i++)
    {
        printf(1,"\nPress enter to continue \n"); 
        gets(input,5);
        Fmem[i] = sbrk(PGSIZE);
        *Fmem[i] = i;
    }

    printf(1,"\n\nmyMemTest : finishing sbrking \n\n"); 

    printf(1,"\n\nmyMemTest : Performing fork()  \n\n"); 

    if((pid = fork()) < 0)
    {
         printf(1,"Fork operation failed\n");
    }
    else if (pid == 0)
    {
        for(j = 0; j < LOOPLENGTH; j++)
        {
            //filling pages with data when forking
            if(Fmem[j])
            {
                printf(1,"\nPress enter to continue \n");
                gets(input,5);
                num = j + 50;
                *Fmem[j] = num;
                printf(1,"Process %d: Fmem[%d] at adress %d is %d \n", getpid(), j, Fmem[j], (int)*(Fmem[j])); 
            } 
        }
        exit();
    }
    else
    {
        wait();
        for(j = 0; j < LOOPLENGTH; j++)
        {
            if(Fmem[j])
            {
                printf(1,"\n.Press enter to continue \n");
                gets(input,5);
                printf(1,"Process %d: Fmem[%d] at adress %d is %d \n", getpid(), j, Fmem[j], (int)*(Fmem[j])); 
            } 
        }

}
    
}

int main(int argc, char *argv[])
{  
    protect_page_test();
    

    //forkTest();
    //policiesTest();
    
    return 0;
    
   
}
