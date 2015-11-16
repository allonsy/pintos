
#ifndef _PAGEH_
#define _PAGEH_
#include "vm/page.h"
#endif

#include "vm/frame.h"
#include "lib/kernel/hash.h"
#include "threads/thread.h"

#define STACK_MAX (1024 * 1024)

// supplemental page table
static struct hash spt;

void 
page_init (void)
{
	// no guarantees this is correct
	hash_init(&spt, page_hash, page_less, NULL);
}


static void 
destroy_page (struct hash_elem *p_, void *aux UNUSED)  
{
	return;
}

static struct 
page *page_for_addr (const void *address) 
{
	return NULL;
}

static bool do_page_in (struct page *p) 
{
	return false;
}

void 
page_exit (void)  
{
	return;
}

bool 
page_in (void *fault_addr) 
{
	return false;
}

bool 
page_out (struct page *p) 
{
	return false;
}

bool 
page_accessed_recently (struct page *p) 
{
	return false;
}

struct page * 
page_allocate (void *vaddr, bool read_only) 
{
	struct page *p = malloc(sizeof struct page);

	if(p == NULL)
	{
		PANIC("no memory for struct page allocation");
		return NULL;
	}

	p->addr = vaddr;
	p->read_only = read_only;
	p->thread = thread_current ();



	return false;
}

void 
page_deallocate (void *vaddr) 
{
	return false;
}

unsigned 
page_hash (const struct hash_elem *e, void *aux UNUSED) 
{
	return 0;
}

bool 
page_less (const struct hash_elem *a_, const struct hash_elem *b_, void *aux UNUSED) 
{
	return false;
}

bool 
page_lock (const void *addr, bool will_write) 
{
	return false;
}

void 
page_unlock (const void *addr) 
{
	return;
}