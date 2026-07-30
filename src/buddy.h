#ifndef _BUDDY_H_
#define _BUDDY_H_

#include <linux/vmalloc.h>
#include <linux/highmem.h>
#include <linux/slab.h>

#define printb(string, args...) printk(KERN_INFO "%s: " string, "BUDDY", ##args)

/* Node can only be in one of 3 states */
enum node_state { FREE, SPLIT, ALLOC };

typedef struct node_t node_t;

struct node_t {
	size_t idx; /* index into the pool */
	enum node_state state; /* state of the node */
	size_t size; /* how many bytes in the pool */
	size_t act_size; /* how many bytes actually allocated by host */
	node_t *left; /* if split we make two buddies below us */
	node_t *right;
};

/* Pool is global so it can be read and written */
extern char *buddy_pool;

int buddy_init(size_t size);
size_t buddy_alloc(size_t size, void *args);
int buddy_free(size_t idx);
size_t buddy_size(size_t idx);
size_t buddy_act_size(size_t idx);
void buddy_print(void);
void buddy_kill(void);
int buddy_is_allocated(void);

#endif
