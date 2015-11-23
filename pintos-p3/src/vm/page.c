
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
  struct thread *t = thread_current ();
  struct hash_iterator itr;
  struct page *p;

  lock_acquire(&t->supp_pt_lock);
  //printf("page_exit: beginning loop\n");
  while(!hash_empty(&t->supp_pt))
  {
    /* have to initialize the iterator every time, since
      page_deallocate deletes an element from the hash table,
      which invalidates the iterator */
    hash_first(&itr, &t->supp_pt);
    p = hash_entry(itr.elem, struct page, hash_elem);
    page_deallocate(p->addr);
  }
  //printf("page_exit: out of the loop with no memory errors!!\n");
  lock_release(&t->supp_pt_lock);
  return;
}

/* this function expects that fault_addr is found in the supplemental page table
   when this function returns the current thread will own the lock on the newly
   acquired frame. the page will be populated.   */
bool 
page_in (void *fault_addr) 
{
  //printf("page_in: entered with fault_addr %p\n", fault_addr);
  struct page *p = page_for_addr (fault_addr);
  if(p == NULL)
  {
    PANIC("page_in: address %p not in SPT", fault_addr);
  }



  /* try_frame_alloc_and_lock will only return NULL
  if EVERY frame is being used AND the swap space is full
  AND none of the frames are read-only, since those can be read in
  from the file again */

  //printf("page_in: after page_for_addr and before try_frame_alloc_and_lock, fault_addr: %p\n", fault_addr);

  struct frame *f = try_frame_alloc_and_lock (p);

  //printf("page_in: after try_frame_alloc_and_lock with frame addr %p\n", f);
  
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


    //printf("page_in: about to call pagedir_set_page on address %p\n", p->addr);
    if(pagedir_set_page (p->thread->pagedir, p->addr, f->base, !p->read_only))
    {
      frame_unlock(f);
      //printf("page_in: exiting\n");
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

  //printf("page_allocate: entered\n");
  if(!is_user_vaddr(vaddr))
  {
    PANIC("tried to allocate a page for a kernel address %p", vaddr);
  }
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
    //printf("page_allocate: exiting\n");
    return p;
  }
  else 
  {
    free(p);
    //printf("page_allocate: exiting\n");
    return NULL;
  }
}

void 
page_deallocate (void *vaddr) 
{
  //printf("page_deallocate: entered\n");
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

    if(!p->private && p->frame && pagedir_is_dirty(t->pagedir, p->addr))
    {
      lock_acquire(&filesys_lock);
      file_write_at (p->file, p->frame->base, p->file_bytes, p->file_offset);
      lock_release(&filesys_lock);
    }
    if(pagedir_get_page(p->thread->pagedir, p->addr))
    {
      pagedir_clear_page (p->thread->pagedir, p->addr);
    }
    frame_free(p->frame);
    free(p);
  }
  //printf("page_deallocate: exiting\n");
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
