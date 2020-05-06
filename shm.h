
#include "proc.h"

#define USERS_COUNT 20
#define FRAMES_COUNT 256

#define FRAME_DIRTY 0x1
struct frame {
	int pa;	//page in frame
	int pr;	//process holding page

	char flags;
};

struct frame_table {
	struct frame  frames[FRAMES_COUNT];
	unsigned char unallocated[(FRAMES_COUNT / 8) + 1];
};

struct memory {
	struct clock clock;
	struct process procs[USERS_COUNT];
	sem_t lock;	/* for locking whole shared region */

	struct frame_table ft;
};

struct memory * shm_attach(const int flags);
						int shm_detach(const int clear);
