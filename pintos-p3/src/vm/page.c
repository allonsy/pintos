
#ifndef _PAGEH_
#define _PAGEH_
#include "vm/page.h"
#endif

#include "vm/frame.h"
#include "lib/kernel/hash.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "filesys/file.h"
#include "userprog/pagedir.h"
#include "lib/string.h"

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
//static struct
struct
page *page_for_addr (const void *address) 
{
  struct thread *t = thread_current ();
  struct page p;
  struct hash_elem *e;

  p.addr = pg_round_down (address);
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

/* this function expects that fault_addr is found in the supplemental page table
   when this function returns the current thread will own the lock on the newly
   acquired frame. the page will be populated.   */
bool 
page_in (void *fault_addr) 
{
  struct page *p = page_for_addr(fault_addr);

  if(p == NULL)
    PANIC("address passed into page_in not in SPT");

  struct frame *f = try_frame_alloc_and_lock (p);


  // should only hold a lock if f != NULL
  if(f != NULL)
  {
    
    if(file_read_at (p->file, f->base, p->file_bytes, p->file_offset) != p->file_bytes)
    {
      frame_unlock(f);
      frame_free(f);
      return false;
    }

    memset (f->base + p->file_bytes, 0, PGSIZE - p->file_bytes);

    frame_unlock(f);

    struct thread *t = thread_current ();

    if(pagedir_set_page (t->pagedir, p->addr, f->base, !p->read_only))
      return true;
    else
    {
      frame_free(f);
      return false;
    }
  }



  PANIC("page_in: no frames left :(");
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
  struct thread *t = thread_current ();
  p = malloc(sizeof *p);

  if(p == NULL)
  {
    PANIC("no memory for struct page allocation");
    return NULL;
  }

  p->addr = pg_round_down (vaddr);
  p->read_only = read_only;
  p->thread = t;

  p->frame = NULL;

  // note hash_insert returns null on success
  if(!hash_insert (&t->supp_pt, &p->hash_elem))
  {

    struct page *p2 = page_for_addr(p->addr);
    if(p2 == NULL)
      PANIC("page_allocate: something funny is happening with hash_insert...");
    return p;
  }
  else 
  {
    free(p);
    return NULL;
  }
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
page_less (const struct hash_elem *a_, const struct hash_elem *b_, void *aux UNUSED)
{
  const struct page *a = hash_entry (a_, struct page, hash_elem);
  const struct page *b = hash_entry (b_, struct page, hash_elem);
  return a->addr < b->addr;
}

bool 
page_lock (const void *addr, bool will_write) 
{
  // not sure what will_write is for, why we return a bool
  struct page *p = page_for_addr (addr);
  frame_lock(p->frame);
  return true;
}

void 
page_unlock (const void *addr) 
{
  struct page *p = page_for_addr (addr);
  frame_unlock(p->frame);
}
