#include <strings.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/types.h>

#include "shm.h"

static int wait_request(struct process * pe){
	int rv = 0;
	while(rv == 0){

		usleep(10);

		if(sem_wait(&pe->lock) < 0){
			return -1;
		}

		if(pe->state == TERMINATE){
			sem_post(&pe->lock);
			return -1;
		}

		switch(pe->request.state){
			case AVAILABLE:
				rv = 1;
				pe->request.v = 0;
				pe->request.t = 0;	//clear request
				break;
			case CANCELLED:	//we get here only when a deadlock has occured
				rv = -1;
				pe->request.t = 0;	//clear request
				break;
			default:
				break;
		}

		if(sem_post(&pe->lock) < 0){
			return -1;
		}
	}
	return rv;
}

static int make_request(struct process * pe, int v, int t){
	if(sem_wait(&pe->lock) < 0){
		return -1;
	}

		pe->request.v   = v;	//address value
		pe->request.t		= t;	//type - read or write
		pe->request.state  = PENDING;

	if(sem_post(&pe->lock) < 0){
		return -1;
	}
	return 0;
}

int main(const int argc, char * const argv[]){

	const int my_index = atoi(argv[1]);
	const int my_scheme = atoi(argv[2]);
	float weights[PAGES_COUNT];

	//fprintf(stderr, "INDEX %d\n", my_index);

	struct memory *mem = shm_attach(0);
	if(mem == NULL){
		fprintf(stderr, "Error: %d Can't attach \n", my_index);
		return -1;
	}

	struct process * pe = &mem->procs[my_index];

	srand(getpid());

	if(my_scheme == 1){
		//initialize the weighting
		int i;
		for(i=0; i < PAGES_COUNT; i++){
			 weights[i] =  1.0f / (float)(i + 1);
		}
	}

	if(sem_wait(&mem->lock) < 0){
		fprintf(stderr, "Error: A %d\n", my_index);
		return -1;
	}

	struct clock terminator = mem->clock;
	terminator.s += USER_RUNTIME;	//how much we run


	if(sem_post(&mem->lock) < 0){
		fprintf(stderr, "Error: B %d\n", my_index);
		return -1;
	}

	int stop = 0;
  while(!stop){
		//check if we should terminated
		if(sem_wait(&mem->lock) < 0){
			break;
		}
		//make sure we run at least USER_RUNTIME seconds
		const int terminate = (mem->clock.s <= terminator.s) ? pe->state : TERMINATE;
		if(terminate == TERMINATE){
			stop = 1;
		}

		if(sem_post(&mem->lock) < 0){
			break;
		}

		//generate page address
		int i, page=-1;
		if(my_scheme == 0){
			page = rand() % PAGES_COUNT;

		}else if(my_scheme == 1){

			const float max_val = weights[PAGES_COUNT-1];
		  const float val = ((float) rand() / (float) RAND_MAX) * max_val;

		  for(i=0; i < PAGES_COUNT; i++){
		    if(weights[i] > val){	//if weight is higher than random weight
					page = i;	//this is our page
		      break;
		    }
		  }

			//update page weight
			for(i=1; i < PAGES_COUNT; i++){
				 weights[i] += weights[i-1];
			}
		}

		//calculate address and read or write
		const int addr = (page*1024) + (rand() % 1024);
		const int type = ((rand() % 100) < READ_PROB) ? -1 : 1;

		//make the request
		make_request(pe, addr, type);

		if(wait_request(pe) < 0){	//if request is denied, or we have to terminate
				break;
		}
  }

	sem_wait(&mem->lock);
	pe->state = EXITED;
	sem_post(&mem->lock);
	//fprintf(stderr, "Done %d\n", my_index);
	shm_detach(0);

  return 0;
}
