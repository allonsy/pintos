
#ifndef _PAGEH_
#define _PAGEH_
#include "vm/page.h"
#endif

#include "vm/frame.h"
#include "lib/kernel/hash.h"
#include "threads/thread.h"

#define STACK_MAX (1024 * 1024)


bool
page_init (struct hash *h)
{
	return hash_init(h, page_hash, page_less, NULL);
}


static void 
destroy_page (struct hash_elem *p_, void *aux UNUSED)  
{
	return;
}

/* Returns the page containing the given virtual address,
   or a null pointer if no such page exists.
   NOTE THIS CODE COPIED FROM PINTOS DOCUMENTATION */
static struct 
page *page_for_addr (const void *address) 
{
  struct thread *t = thread_current ();
  struct page p;
  struct hash_elem *e;

  p.addr = address;
  e = hash_find (&t->supp_pt, &p.hash_elem);
  return e != NULL ? hash_entry (e, struct page, hash_elem) : NULL;
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
  struct page *p;
  p = malloc(sizeof *p);

	if(p == NULL)
	{
		PANIC("no memory for struct page allocation");
		return NULL;
	}

	p->addr = vaddr;
	p->read_only = read_only;
	p->thread = thread_current ();

	struct frame *f = try_frame_alloc_and_lock (p);

	if(f == NULL)
	{	
		free(p);
		return NULL;
	}

	p->frame = f;

	return p;
}

void 
page_deallocate (void *vaddr) 
{
  struct page *p;
  if((p = page_for_addr (vaddr)) != NULL)
  {
    frame_free(p->frame);
    free(p);
  }
}

/* Returns a hash value for page p. 
   Note, this code copied from the pintos documentation */
unsigned
page_hash (const struct hash_elem *p_, void *aux UNUSED)
{
  const struct page *p = hash_entry (p_, struct page, hash_elem);
  return hash_bytes (&p->addr, sizeof p->addr);
}

/* Returns true if page a precedes page b. 
   Note, this code copied from the pintos documentation */
bool
page_less (const struct hash_elem *a_, const struct hash_elem *b_,
           void *aux UNUSED)
{
  const struct page *a = hash_entry (a_, struct page, hash_elem);
  const struct page *b = hash_entry (b_, struct page, hash_elem);
  return a->addr < b->addr;
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
