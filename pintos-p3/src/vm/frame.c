#include "vm/frame.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "lib/debug.h"
#include "threads/loader.h"
#include "userprog/pagedir.h"
#include "vm/page.h"


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

// struct frame*
// try_frame_alloc_and_lock (struct page *page) 

// temporary to make this a fake palloc
struct frame*
try_frame_alloc_and_lock (struct page *page)
{
  // super simple (ie dumb) method to find a frame
  // does no eviction or smart things and stuff

  int i;

  lock_acquire(&scan_lock);
  
  for(i = 0; i < frame_cnt; i++)
  {
    struct frame *f = &frames[i];
    bool success = lock_try_acquire(&f->lock);
    if(success)
    {
      if(f->page == NULL)
      {
        f->page = page;
        page->frame = f;
        lock_release(&scan_lock);
        return f;
      }
      else
      {
        lock_release(&f->lock);
      }
    }
  }

  lock_release(&scan_lock);


  /* TO DO: Find a frame to evict here 
    then call swap_out (f->page) followed by frame->free 
    then return f */

  PANIC("no more frames :(");

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
    struct page *p = frames[hand].page;
    if(p==NULL)
    {
      hand++;
      if(hand==frame_cnt)
      {
        hand = 0;
      }
      continue;
    }

    uint32_t *cur_pagedir = p->thread->pagedir;
    if(!pagedir_is_accessed(cur_pagedir, p))
    {
      if(!pagedir_is_dirty(cur_pagedir, p))
      {
        ret = &frames[hand];
      }
      else
      {
        if(oneshot)
        {
          ret = &frames[hand];
        }
      }
    }
    else
    {
      pagedir_set_accessed(cur_pagedir, p, 0);
      didChange = 1;
    }
    hand++;
    if(hand==frame_cnt)
    {
      hand=0;
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
  if(ret != NULL)
  {
    frame_lock(ret);
  }
  return ret;
  
}
