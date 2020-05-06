#ifndef PBTL_H
#define PBTL_H

#include <semaphore.h>

#include "clock.h"
#include "oss.h"

#define PAGE_SIZE	1024
#define PAGES_COUNT	32
#define PAGE_INDEX(address) (address / PAGE_SIZE)

enum process_state { READY=1, IOBLK, TERMINATE, EXITED};
enum request_state { AVAILABLE=0, NOT_AVAILABLE, PENDING, CANCELLED};

#define PAGE_REFERENCED 0x1

struct page {
	int fr;
	char flags;
};

struct request{
	//type is 0 - no request, >0 write, <0 read
	int v,t;	//value, type
	enum request_state state;
	struct clock load_time;
};

// entry in the process control table
struct process {
	int	pid;
	int id;
	int state;
	sem_t lock; /* for locking process data only */
	struct request request;
	struct page	  page[PAGES_COUNT];	//virtual memory pages
};

struct process * process_new(struct process * procs, const int id);
void 						 process_init(struct process * procs);
void 						 process_free(struct process * procs, const unsigned int i);

#endif
