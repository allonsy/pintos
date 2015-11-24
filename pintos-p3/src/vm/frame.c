#include "vm/frame.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "lib/debug.h"
#include "threads/loader.h"
#include "userprog/pagedir.h"
#include "vm/page.h"
#include "threads/vaddr.h"


static struct frame *frames;
static size_t frame_cnt;

static struct lock scan_lock;
static size_t hand;


void
frame_init (void)
{
  void *base;
  int i = 0;
  hand = 0;
  lock_init (&scan_lock);
  random_init (frame_cnt);

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
  //printf("try_frame_alloc_and_lock: entered\n");
  int i;
  struct frame *f;
  lock_acquire(&scan_lock);
  
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
          //printf("try_frame_alloc_and_lock: exiting\n");
          return f;
      }
    }
  }

  /* at this point, we believe that all frames are held */
  /* f is locked when this returns */
  //f = perform_LRU();
  f = randomEvict();

  ASSERT(f != NULL);
  struct page *p = f->page;
  
  if(p == NULL)
  {
    PANIC("try_frame_alloc_and_lock_2: after LRU f->page is NULL. bizarre.\n");
    f->page = page;
    page->frame = f;
    lock_release(&scan_lock);
    //printf("try_frame_alloc_and_lock: exiting\n");
    return f;
  }
  ASSERT(p->frame == f);
  switch(p->type)
  {
    case PAGET_STACK:
    case PAGET_DATA:
      if(p->thread->pagedir != NULL && p->thread->pagedir != 0xcccccccc)
      {
        swap_out(p);
        ASSERT(p->frame == NULL);
      }
      f->page = page;
      page->frame = f;
      lock_release(&scan_lock);
      //printf("try_frame_alloc_and_lock: exiting\n");
      return f;
      break;
    case PAGET_MMAP:
      if(p->thread->pagedir != NULL && p->thread->pagedir != 0xcccccccc)
      {
        if(pagedir_is_dirty(p->thread->pagedir, p->addr))
          file_write_at (p->file, p->frame->base, p->file_bytes, p->file_offset); 
        pagedir_clear_page (p->thread->pagedir, p->addr);
        memset(f->base, 0, PGSIZE);
      }
      p->frame = NULL;
      f->page = page;
      page->frame = f;
      lock_release(&scan_lock);
      //printf("try_frame_alloc_and_lock: exiting\n");
      return f;
      break;
    case PAGET_READONLY: /* file is guaranteed to be nonnull */
      if(p->thread->pagedir != NULL && p->thread->pagedir != 0xcccccccc)
      {
        pagedir_clear_page (p->thread->pagedir, p->addr);
        memset(f->base, 0, PGSIZE);
      }
      p->frame = NULL;
      f->page = page;
      page->frame = f;
      lock_release(&scan_lock);
      //printf("try_frame_alloc_and_lock: exiting\n");
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

void 
frame_lock (struct frame *f) 
{
  lock_acquire(&f->lock);
}

// should this take in a page or a frame? I am not sure. 
// frame might make sense, since we could have multiple read only pages that
// point to this frame? I dunno...
void 
frame_free (struct frame *f)
{
  if(!f)
  {
    return;
  }
  lock_acquire(&scan_lock);
  lock_acquire(&f->lock);
  if(f != NULL)
  {
    if(f->page != NULL)
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
    if(p==NULL /* || is_kernel_vaddr(p->addr) */)
    {
      hand++;
      if(hand >= frame_cnt)
      {
        hand = 0;
      }
      continue;
    }

    uint32_t *cur_pagedir = p->thread->pagedir;
    //printf("acc: %ld dirty: %ld\n", pagedir_is_accessed(cur_pagedir, p->addr), pagedir_is_dirty(cur_pagedir, p->addr));
    if(cur_pagedir && !pagedir_is_accessed(cur_pagedir, p->addr))
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
    else if(cur_pagedir)
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

  if(ret != NULL && !lock_held_by_current_thread(&ret->lock))
  {
    frame_lock(ret);
  }
  return ret;
}


struct frame *randomEvict()
{
  int i = random_ulong () % frame_cnt;
  frame_lock(&frames[i]);
  return &frames[i];
}