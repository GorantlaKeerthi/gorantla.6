#include <stdlib.h>
#include <strings.h>
#include "proc.h"
#include "oss.h"

static unsigned int bitmap = 0;

static int bitmap_find_unset_bit(){

	int i;
  for(i=0; i < USERS_COUNT; i++){
  	if(((bitmap & (1 << i)) >> i) == 0){	//if bit is unset

			bitmap ^= (1 << i);	//raise the bit
      return i;
    }
  }
  return -1;
}

void process_init(struct process * proc){
	proc->pid = 0;
	proc->id = 0;
	proc->state = 0;

	int i;
	for(i=0; i < PAGES_COUNT; i++){
		struct page *page = &proc->page[i];
		if(page->fr >= 0){
			page->fr = -1;   //not in memory
		}
		page->flags = 0;
	}
}
void process_free(struct process * procs, const unsigned int i){

    bitmap ^= (1 << i); //switch bit

		process_init(&procs[i]);
}

struct process * process_new(struct process * procs, const int id){
	const int i = bitmap_find_unset_bit();
	if(i == -1){
		return NULL;
	}

  procs[i].id	= id;
  procs[i].state = READY;
	return &procs[i];
}
