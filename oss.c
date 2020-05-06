#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/msg.h>
#include <sys/ipc.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <semaphore.h>

#include "shm.h"
#include "proc.h"

// clock step
#define CLOCK_NS 1000

// total processes to start
#define USERS_GENERATED 100

static int started = 0; //started users
static int exited = 0;  //exited users

static struct memory *mem = NULL;   /* shared memory region pointer */

/* counters for request type - read and writes */
static unsigned int addr_reads=0, addr_writes=0;
static unsigned int addr_faults=0, addr_refs=0;

static unsigned int line_count=0;

static int interrupted = 0;
static int m = 0;	//for option -m

//Convert integer number to string
static char * num_arg(const unsigned int number){
	size_t len = snprintf(NULL, 0, "%d", number) + 1;
	char * str = (char*) malloc(len);
	snprintf(str, len, "%d", number);
	return str;
}

static int exec_user(void){

	struct process *pe;

  if((pe = process_new(mem->procs, started)) == NULL){
    return 0; //no free processes
  }
  started++;
  const int pa = pe - mem->procs; //process index

  char * my_id = num_arg(pa);
	char * my_m  = num_arg(m);

  const pid_t pid = fork();
  if(pid == -1){
    perror("fork");
    return -1;
  }else if(pid == 0){
    execl("./user", "./user", my_id, my_m, NULL);
    perror("execl");
    exit(-1);
  }else{
    pe->pid = pid;
  }
  free(my_id);
	free(my_m);

  printf("[%i:%i] Master: Generating process with PID %u\n", mem->clock.s, mem->clock.ns, pe->id);

  return 0;
}

static int forktime(struct clock *forktimer){

  struct clock inc;

  //advance time
  inc.s = 1;
  inc.ns = rand() % CLOCK_NS;

  sem_wait(&mem->lock);
  add_clocks(&mem->clock, &inc);
  sem_post(&mem->lock);

  if(started < USERS_GENERATED){  //if we can fork more

    // if its time to fork
    if(cmp_clocks(&mem->clock, forktimer)){

      //next fork time
      forktimer->s = mem->clock.s + 1;
      forktimer->ns = 0;

      return 1;
    }
  }
  return 0; //not time to fokk
}

//Send mesage to users to quit
static void final_msg(){
  int i;

  for(i=0; i < USERS_COUNT; i++){
    if(mem->procs[i].pid > 0){

      sem_wait(&mem->procs[i].lock);

      mem->procs[i].state = TERMINATE;
      mem->procs[i].request.state = CANCELLED;

      sem_post(&mem->procs[i].lock);
    }
  }
}

static void signal_handler(const int sig){
  printf("[%i:%i] Master: Signaled with %d\n", mem->clock.s, mem->clock.ns, sig);
  interrupted = 1;
}

static void print_results(){

	stdout = freopen("output.txt", "a", stdout);

  printf("Runtime: %u:%u\n", mem->clock.s, mem->clock.ns);
  printf("Started: %i\n", started);
  printf("Exited: %i\n", exited);

	printf("Requests: %u\n", addr_refs);
	printf("Addresses read: %u\n", addr_reads);
	printf("Addresses written: %u\n", addr_writes);
	printf("Refs per second: %.2f\n", (float) addr_refs / mem->clock.s);
	printf("Faults per reference: %.2f\n", (float)addr_faults / addr_refs);
}

static int frame_unallocated(unsigned char *map){
	int i;
  for(i=0; i < FRAMES_COUNT; i++){

    const int p = i / 8, q = i % 8;

		if(	((map[p] & (1 << q)) >> q) == 0){
      map[p] |= (1 << q);
      return i;
    }
  }
  return -1;
}

static void frame_clear(const int f){
	struct frame *fr = &mem->ft.frames[f];
  fr->pa = fr->pr = -1;
	fr->flags = 0;

  const int p = f / 8, q = f % 8;
  mem->ft.unallocated[p] &= ~(1 << q);	//clear
}

static void frame_allocated(const int f){
	const int p = f / 8, q = f % 8;
  mem->ft.unallocated[p] |= (1 << q);	//raise
}

static void frames_clear_pages(struct page *pt){
  int i;
	for(i=0; i < PAGES_COUNT; i++){
		struct page *page = &pt[i];
		if(page->fr >= 0){
			frame_clear(page->fr);
		}
	}
}

static void current_memory(){

	printf("[%i:%i] Master: Current Frames Usage:\n", mem->clock.s, mem->clock.ns);

	printf("%10s\t\t%10s\t\t%10s\t%10s\n", "#", "Occupied", "RefByte", "DirtyBit");
	line_count += 2;

	int i;
	for(i=0; i < FRAMES_COUNT; i++){

    struct frame * fr = &mem->ft.frames[i];

    int flags[2];
    if(fr->pa > 0){
      flags[0] = mem->procs[fr->pr].page[fr->pa].flags & PAGE_REFERENCED;
      flags[1] = mem->ft.frames[i].flags & FRAME_DIRTY;
			printf("Frame %4d\t\t%10s\t\t%10d\t%10d\n", i, "Yes", flags[0], flags[1]);
    }else{
			printf("Frame %4d\t\t%10s\t\t%10d\t%10d\n", i, "No", 0, 0);
		}


	}
	printf("\n");
  line_count += FRAMES_COUNT;
}

static int second_chance_clock(struct frame_table * ft){
  static int clock_pointer = -1;	//shows which page will be checked next

	int i;
  for(i=0; i < FRAMES_COUNT; i++){

    clock_pointer = (clock_pointer + 1) % FRAMES_COUNT;

		//skip free frames
		if(ft->frames[clock_pointer].pa >= 0){

		  struct frame * frame = &ft->frames[clock_pointer];
	    struct page * page = &mem->procs[frame->pr].page[frame->pa];

	    if((page->flags & PAGE_REFERENCED) == 0){

	      line_count++;
	      printf("[%i:%i] Master: CLOCK evicted P%d page %d\n",
	        mem->clock.s, mem->clock.ns, frame->pa, frame->pr);

				break;
			}else{
				page->flags ^= PAGE_REFERENCED;	//mark page as referenced
	    }
		}
  }

  return clock_pointer;
}

static int mem_fault(struct process * pe){

	addr_faults++;
  const int pa = PAGE_INDEX(pe->request.v);

  int free_frame = frame_unallocated(mem->ft.unallocated);
  if(free_frame >= 0){
    line_count++;
    printf("[%i:%i] Master: Using free frame %d for P%d page %d\n",
      mem->clock.s, mem->clock.ns, free_frame, pe->id, pa);

  }else{

	  free_frame = second_chance_clock(&mem->ft); //choose a frame, using CLOCK
	  struct frame * frame = &mem->ft.frames[free_frame]; //frame that holds the evicted page
	  struct page * page = &mem->procs[frame->pr].page[frame->pa];
	  struct process * evicted = &mem->procs[frame->pr];

		if(frame->flags & FRAME_DIRTY){
			line_count++;
			printf("[%i:%i] Master: Dirty bit of frame %d set, adding additional time to the clock\n",
				mem->clock.s, mem->clock.ns, page->fr);

			struct clock save_time;
			save_time.s = 0;
			save_time.ns = 14* 1000;	//since its dirty we have save and load disk time
			add_clocks(&mem->clock, &save_time);
		}

	  line_count++;
	  printf("[%i:%i] Master: Cleared frame %d and swap P%d page %d for P%d page %d\n",
	    mem->clock.s, mem->clock.ns, page->fr, evicted->id, frame->pa, pe->id, pa);

	  free_frame = page->fr;
	  frame_clear(free_frame);
	  page->fr = -1;
	}

  return free_frame;
}

static int load_to_memory(struct process * pe){

  const int pa = PAGE_INDEX(pe->request.v);
  if(pa >= PAGES_COUNT){
		printf("[%i:%i] Master: P%d referenced beyond page table\n",
			mem->clock.s, mem->clock.ns, pe->id);
		return CANCELLED;
	}

  struct page * page = &pe->page[pa];

	int rv;
  if(page->fr < 0){

		line_count++;
    printf("[%i:%i] Master: Page fault on address %d\n",
      mem->clock.s, mem->clock.ns, pe->request.v);

		//find frame for the page
  	page->fr = mem_fault(pe);
		if(page->fr < 0){
			return CANCELLED;
		}
		//page ref flag is on, since we read/write to page
  	page->flags ^= PAGE_REFERENCED;

		//save which page is inside the frame
    frame_allocated(page->fr);
		struct frame * frame = &mem->ft.frames[page->fr];
  	frame->pa = pa;
  	frame->pr = pe - mem->procs;

    //14 ms load time
    pe->request.load_time.s = 0; pe->request.load_time.ns = 14 * 1000;
    add_clocks(&pe->request.load_time, &mem->clock);

		//since we are loading, put pending status
		rv = PENDING;

  }else{	//page is in memory

		//add access time to clock
		struct clock access_time;
		access_time.s = 0;
    access_time.ns = 10;
    add_clocks(&mem->clock, &access_time);

		rv = AVAILABLE;	//page is available to read/write
  }

  return rv;
}

static int new_request(struct process * pe, enum request_state state){

	int rv = CANCELLED;

  addr_refs++;

	switch(pe->request.t){
		case -1:
			addr_reads++;
			printf("[%i:%i] Master: P%d reading %d\n", mem->clock.s, mem->clock.ns, pe->id, pe->request.v);
			break;
		case 1:
			addr_writes++;
			printf("[%i:%i] Master: P%d writing %d\n", mem->clock.s, mem->clock.ns, pe->id, pe->request.v);
			break;
		default:
			fprintf(stderr, "Error: Invalid type in process request\n");
			pe->request.state = CANCELLED;
			break;
	}
  line_count++;

	rv = load_to_memory(pe);
  if(rv == AVAILABLE){
    const int pa = PAGE_INDEX(pe->request.v);
    struct page * page = &pe->page[pa];

		switch(pe->request.t){
			case -1:
				line_count++;
				printf("[%i:%i] Master: Address %d in frame %d, P%d reading data\n", mem->clock.s, mem->clock.ns,
					pe->request.v, page->fr, pe->id);
				break;

			case 1:
				line_count++;
				printf("[%i:%i] Master: Address %d in frame %d, P%d writing data\n",
					mem->clock.s, mem->clock.ns, pe->request.v, page->fr, pe->id);

				//after frame is written, it gets dirty
				mem->ft.frames[page->fr].flags |= FRAME_DIRTY;
				break;

			default:
				break;
		}
  }
	return rv;
}

static int dispatching(enum request_state state){
	int i, count=0;
  struct clock tdisp;

  tdisp.s = 0;

	for(i=0; i < USERS_COUNT; i++){

		struct process * pe = &mem->procs[i];

		sem_wait(&mem->procs[i].lock);

		if(pe->state == EXITED){
			exited++;
			//process exited, we have to clean up his memory
			printf("[%i:%i] Master: P%d terminates. Clearing frames ", mem->clock.s, mem->clock.ns, pe->id);
			int i;
			for(i=0; i < PAGES_COUNT; i++){
				struct page *page = &pe->page[i];
				if(page->fr >= 0){
					printf("%d ", page->fr);
				}
			}
			printf("\n");

			frames_clear_pages(pe->page);	//clear all frames used
			process_free(mem->procs, pe - mem->procs);


		}else if((pe->pid > 0) &&
					(pe->request.state == state)){

			pe->request.state = new_request(pe, state);
			if(pe->request.state == AVAILABLE){
				count++;
			}

    	if((addr_refs % 100) == 0)
    		current_memory();

		}
    sem_post(&mem->procs[i].lock);

    //add request processing to clock
    tdisp.ns = rand() % 100;
    add_clocks(&mem->clock, &tdisp);
	}

	return count;	//return number of dispatched procs
}

static int alloc_memory(){
  //initialize the frames and page tables

	bzero(mem->ft.unallocated, sizeof(char)*((FRAMES_COUNT / 8) + 1));

	int i;
	//init memory
	for(i=0; i < FRAMES_COUNT; i++){
		frame_clear(i);
	}

	//init each child page table
	for(i=0; i < USERS_COUNT; i++){	//for each pe
		process_init(&mem->procs[i]);
	}
	return 0;
}

int main(const int argc, char * argv[]){

  signal(SIGINT,  signal_handler);
  signal(SIGTERM, signal_handler);
  signal(SIGALRM, signal_handler);
  signal(SIGCHLD, SIG_IGN);

  mem = shm_attach(0600 | IPC_CREAT);
  if(mem == NULL){
    return -1;
  }

	if(argc == 3){
		m = atoi(argv[2]);
	}

  stdout = freopen("output.txt", "w", stdout);
  if(stdout == NULL){
		perror("freopen");
		return -1;
	}

  alloc_memory();

  srand(getpid());

  alarm(2);

  struct clock forktimer = {0,0};

  while((exited < USERS_GENERATED) &&
        (!interrupted)){

    if(forktime(&forktimer) && (exec_user() < 0)){
      fprintf(stderr, "exec_user failed\n");
      break;
    }

		/* process blocked and pending requests */
    dispatching(NOT_AVAILABLE);
		dispatching(PENDING);

  	if(line_count >= MAX_OUTPUT_LINES){
  		printf("Master: Log full. Stopping ...\n");
  		stdout = freopen("/dev/null", "w", stdout);
			//break;
  	}
  }

	current_memory();
  print_results();

  final_msg();
  shm_detach(1);

  return 0;
}
