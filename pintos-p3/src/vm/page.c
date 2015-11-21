
#ifndef _PAGEH_
#define _PAGEH_
#include "vm/page.h"
#endif

#include "vm/frame.h"
#include "lib/kernel/hash.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
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
struct
page *page_for_addr (const void *address) 
{
  struct thread *t = thread_current ();

  struct page p;
  struct hash_elem *e;

  p.addr = pg_round_down (address);

  bool lockheld = lock_held_by_current_thread(&t->supp_pt_lock);

  if(!lockheld)
  {
    lock_acquire(&t->supp_pt_lock);
  }

  e = hash_find (&t->supp_pt, &p.hash_elem);

  if(!lockheld)
  {
    lock_release(&t->supp_pt_lock);
  }

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
  struct page *p = page_for_addr (fault_addr);
  if(p == NULL)
  {
    PANIC("page_in: address %p not in SPT", fault_addr);
  }


  /* try_frame_alloc_and_lock will only return NULL
  if EVERY frame is being used AND the swap space is full
  AND none of the frames are read-only, since those can be read in
  from the file again */
  struct frame *f = try_frame_alloc_and_lock (p);
  
  if(f != NULL)
  {
    off_t read;
    /* TO DO: NEED THE CASE WHERE p IS MMAP'd (I think)
       AND THE CASE WHERE p IS IN SWAP SPACE */
    if(p->swap)
    {
      swap_in(p);
    }
    else if(p->file != NULL)
    {
      read = file_read_at (p->file, f->base, p->file_bytes, p->file_offset);
      if(read != p->file_bytes)
      {
        frame_unlock(f);
        frame_free(f);

        PANIC("page_in: unable to read correct number of bytes from file %p, read %d, expected %d", p->file, read, p->file_bytes);
        return false;
      }
      memset (f->base + read, 0, PGSIZE - read);
    }
    else //page has no file, probably a stack page
    {
      memset (f->base, 0, PGSIZE);
    }

    if(pagedir_set_page (p->thread->pagedir, p->addr, f->base, !p->read_only))
    {
      frame_unlock(f);
      return true;
    }
    else
    {
      frame_unlock(f);
      frame_free(f);
      PANIC("page_in: failed to set page table entry");
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
  p->swap = false;
  p->private=true;
  if(p == NULL)
  {
    PANIC("no memory for struct page allocation");
    return NULL;
  }

  p->addr = pg_round_down (vaddr);
  p->read_only = read_only;
  p->thread = t;

  p->frame = NULL;

  bool lockheld = lock_held_by_current_thread(&t->supp_pt_lock);

  if(!lockheld)
  {
    lock_acquire(&t->supp_pt_lock);
  }

  void *success = hash_insert (&t->supp_pt, &p->hash_elem);

  if(!lockheld)
  {
    lock_release(&t->supp_pt_lock);
  }


  // note hash_insert returns null on success
  if(!success)
  {
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
  struct thread *t = thread_current ();
  if((p = page_for_addr (vaddr)) != NULL)
  {

    bool lockheld = lock_held_by_current_thread(&t->supp_pt_lock);

    if(!lockheld)
    {
      lock_acquire(&t->supp_pt_lock);
    }

    hash_delete (&p->thread->supp_pt, &p->hash_elem);

    if(!lockheld)
    {
      lock_release(&t->supp_pt_lock);
    }

    /* do writeback for mmap'd files */
    
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
  struct page *p = page_for_addr (addr);
  if(!will_write || !p->read_only)
  {
    frame_lock(p->frame);
    return true;
  }
  return false;
}

/* safe whether or not you hold the lock! */
void 
page_unlock (const void *addr) 
{
  struct page *p = page_for_addr (addr);

  if(lock_held_by_current_thread(&p->frame->lock))
  {
    frame_unlock(p->frame);
  }
}
