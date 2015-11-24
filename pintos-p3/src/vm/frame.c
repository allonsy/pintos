#include "vm/frame.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "lib/debug.h"
#include "threads/loader.h"
#include "userprog/pagedir.h"
#include "vm/page.h"
#include "threads/vaddr.h"
#include "vm/swap.h"


static struct frame *frames;
static size_t frame_cnt;

static struct lock scan_lock;
static size_t hand;


void lock_scan(void)
{
  lock_acquire(&scan_lock);
}

void unlock_scan(void)
{
  lock_release(&scan_lock);
}


void
frame_init (void)
{
  void *base;
  int i = 0;
  hand = 0;
  lock_init (&scan_lock);

  frames = malloc (sizeof *frames * init_ram_pages);
  if (frames == NULL)
    PANIC ("out of memory allocating page frames");

  while ((base = palloc_get_page (PAL_USER)) != NULL && i < init_ram_pages)
  {

    struct frame *f = &frames[frame_cnt++];
    lock_init (&f->lock);
    f->base = base;
    f->page = NULL;
    i++;
  }
}

/* when this function returns we have a page that was empty and
   we hold its lock 
   the implementation below does not do any eviction or swapping
   it only tries to get a free frame */
struct frame*
try_frame_alloc_and_lock (struct page *page)
{

  printf("try_frame_alloc_and_lock: entered\n");

  int i;
  struct frame *f;
  //bool success;
  //if(!lock_held_by_current_thread(&scan_lock))
  //{
    lock_acquire(&scan_lock);
  //}
  
  for(i = 0; i < frame_cnt; i++)
  {
    f = &frames[i];
    if(f->page == NULL)
    {  
      if(!lock_held_by_current_thread(&f->lock) && f->page == NULL)
      {
        if(lock_try_acquire(&f->lock))
        {
          f->page = page;
          page->frame = f;
          lock_release(&scan_lock);
          //printf("try_frame_alloc_and_lock: exiting empty frame case\n");
          return f;
        }
      }
    }
  }

  //printf("try_frame_alloc_and_lock: after first for loop\n");

  /* at this point, we believe that all frames are held */

  /* f is locked when this returns */
  f = perform_LRU();
  printf("try_frame_alloc_and_lock: after LRU\n");

  ASSERT(f != NULL);
  struct page *p = f->page;
  
  if(p == NULL)
    return f;

  //printf("try_frame_alloc_and_lock: past assertions, about to try second print\n");
  /* p should not be NULL since we held the scan lock above
    and no page had NULL */

  printf("try_frame_alloc_and_lock: p: %p f: %p p->thread: %p p->thread->name: %s\n\t\t&p->thread->pagedir %p p->thread->pagedir %p\n", p, f, p->thread, p->thread->name, &p->thread->pagedir, p->thread->pagedir);
  if(p->thread->pagedir && pagedir_is_dirty(p->thread->pagedir, p->addr))
  {

    printf("try_frame_alloc_and_lock: after pagedir_is_dirty\n");
    if(/* p->private */ 1)
    {

      printf("try_frame_alloc_and_lock: entered non-mmap case\n");
      if(!swap_out(p))
      {
        PANIC("try_frame_alloc_and_lock: SWAP SPACE FULL");
      }
      /* swapout clears from pagedir */
      ASSERT(p->frame ==NULL);
      f->page = page;
      page->frame = f;
      lock_release(&scan_lock);
      printf("try_frame_alloc_and_lock: exiting non-mmap case\n");
      return f;
    }
    else // mmap case
    {
      printf("try_frame_alloc_and_lock: entered mmap case\n");
      /* if filesize isn't fixed, this could be problematic */
      /* could also be problematic if we can't get the file thing to work like vm segment had */
      file_write_at (p->file, p->frame->base, p->file_bytes, p->file_offset); 
      pagedir_clear_page (p->thread->pagedir, p->addr);
      p->frame = NULL;
      f->page = page;
      page->frame = f;
      lock_release(&scan_lock);
      printf("try_frame_alloc_and_lock: exiting mmap case\n");
      return f;
    }
  }
  else if (p->read_only && p->file != NULL)
  {
    //printf("try_frame_alloc_and_lock: after pagedir_is_dirty\n");
    printf("try_frame_alloc_and_lock: entered read_only from file case\n");
    p->frame == NULL; /* safe to do since scan lock and frame lock are held */
    if(p->thread->pagedir)
    {
      pagedir_clear_page (p->thread->pagedir, p->addr);
    }
    else
    {
      printf("try_frame_alloc_and_lock: null pagedir in huh\n");
    }
    f->page = page;
    page->frame = f;
    lock_release(&scan_lock);
    printf("try_frame_alloc_and_lock: exiting read_only from file case\n");
    return f;
  }
  else
  {
    //printf("try_frame_alloc_and_lock: after pagedir_is_dirty\n");
    printf("try_frame_alloc_and_lock: entered catch-all case\n");

    if(!swap_out(p))
    {
      PANIC("try_frame_alloc_and_lock: SWAP SPACE FULL");
    }
    ASSERT(p->frame ==NULL);
    f->page = page;
    page->frame = f;
    lock_release(&scan_lock);
    printf("try_frame_alloc_and_lock: exiting catch-all case\n");
    return f;
  }
  printf("try_frame_alloc_and_lock: huh\n");

  lock_release(&f->lock);

  lock_release(&scan_lock);

  PANIC("try_frame_alloc_and_lock: WE NEED ADDITIONAL LOGIC AFTER LRU");
  return NULL;
}

void 
frame_lock (struct frame *f) 
{
  lock_acquire(&f->lock);
}

void 
frame_free (struct frame *f)
{
  if(!f)
  {
    return;
  }
  lock_acquire(&scan_lock);
  lock_acquire(&f->lock);
  if(f->page != NULL)
  {
    f->page->frame = NULL;
    f->page = NULL;
  }
  lock_release(&f->lock);
  lock_release(&scan_lock);
}

void 
frame_unlock (struct frame *f) 
{
  lock_release(&f->lock);
}

struct frame *perform_LRU()
{
  struct frame *ret=NULL;
  int didChange = 0;
  int top = hand;
  int oneshot = 0;
  while(ret == NULL)
  {
    //printf("loop\n");
    struct page *p = frames[hand].page;
    if( p == NULL || p->thread->pagedir == NULL /* || is_kernel_vaddr(p->addr) */)
    {
      //printf("LRU: page is null case\n");
      hand++;
      if(hand >= frame_cnt)
      {
        hand = 0;
      }
      /* if page is NULL, we should return this frame */
      if(hand == 0)
    	{
    	  frame_lock(&frames[frame_cnt-1]);
    	  return &frames[frame_cnt-1];
    	}
      else
    	{
    	  frame_lock(&frames[hand-1]);
    	  return &frames[hand-1];
    	}
      
      continue;
    }

    uint32_t *cur_pagedir = p->thread->pagedir;
    if(cur_pagedir == NULL)
    {
      /* assumpetion here is htat an invalid pagedir 
	 means that the page can be freed */
      //printf("LRU: pagedir is null\n");
      ret = &frames[hand];
      ret->page = NULL;
    }
    else if(!pagedir_is_accessed(cur_pagedir, p->addr))
    {
      // if(!pagedir_is_dirty(cur_pagedir, p->addr))
      // {
       ret = &frames[hand];
     //  }
     //  else
     //  {
     //   if(oneshot)
     //   {
     //     ret = &frames[hand];
     //   }
     // }
    }
    else
    {
      pagedir_set_accessed(cur_pagedir, p->addr, 0);
      didChange = 1;
    }
    hand++;
    if(hand >= frame_cnt)
    {
      hand = 0;
    }
    if(hand == top)
    {
      if(didChange)
      {
        didChange = 0;
      }
      else
      {
        oneshot = 1;
      }
    }
  }

  if(ret != NULL /* && !lock_held_by_current_thread(&ret->lock) */)
  {
    frame_lock(ret);
  }
  return ret;
}









struct frame*
try_frame_alloc_and_lock_2 (struct page *page)
{

  //printf("try_frame_alloc_and_lock_2: entered\n");

  int i;
  struct frame *f;
  //bool success;
  //if(!lock_held_by_current_thread(&scan_lock))
  //{
    lock_acquire(&scan_lock);
  //}
  
  for(i = 0; i < frame_cnt; i++)
  {
    f = &frames[i];
    if(f->page == NULL)
    {  
      if(!lock_held_by_current_thread(&f->lock) && lock_try_acquire(&f->lock))
      {
          f->page = page;
          page->frame = f;
          lock_release(&scan_lock);
          return f;
      }
    }
  }

  /* at this point, we believe that all frames are held */

  /* f is locked when this returns */
  f = perform_LRU();
  //printf("try_frame_alloc_and_lock_2: after LRU\n");

  ASSERT(f != NULL);
  struct page *p = f->page;
  
  if(p == NULL)
  {
    PANIC("try_frame_alloc_and_lock_2: after LRU f->page is NULL. bizarre.\n");
    f->page = page;
    page->frame = f;
    lock_release(&scan_lock);
    return f;
  }
  ASSERT(p->frame == f);
  //printf("try_frame_alloc_and_lock_2: p: %p f: %p p->thread: %p p->thread->name: %s\n\t\t&p->thread->pagedir %p p->thread->pagedir %p\n", p, f, p->thread, p->thread->name, &p->thread->pagedir, p->thread->pagedir);

  ASSERT(p->thread->pagedir != NULL);
  switch(p->type)
  {
    case PAGET_STACK:
    case PAGET_DATA:
      swap_out(p);
      ASSERT(p->frame == NULL);
      f->page = page;
      page->frame = f;
      lock_release(&scan_lock);
      return f;
      break;
    case PAGET_MMAP:
      if(pagedir_is_dirty(p->thread->pagedir, p->addr))
        file_write_at (p->file, p->frame->base, p->file_bytes, p->file_offset); 
      pagedir_clear_page (p->thread->pagedir, p->addr);
      memset(f->base, 0, PGSIZE);
      p->frame = NULL;
      f->page = page;
      page->frame = f;
      lock_release(&scan_lock);
      return f;
      break;
    case PAGET_READONLY: /* file is guaranteed to be nonnull */
      pagedir_clear_page (p->thread->pagedir, p->addr);
      memset(f->base, 0, PGSIZE);
      p->frame = NULL;
      f->page = page;
      page->frame = f;
      lock_release(&scan_lock);
      return f;
      break;
    default:
      PANIC("try_frame_alloc_and_lock_2: reached an undefined page type...");
      break;
  }
  

  lock_release(&f->lock);
  lock_release(&scan_lock);

  PANIC("try_frame_alloc_and_lock_2: WE NEED ADDITIONAL LOGIC AFTER LRU");
  return NULL;
}
