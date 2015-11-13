#include "frame.h"
#include "thread.h"
#include "synch.h"


static struct frame *frames;
static size_t frame_cnt;

static struct lock scan_lock;
static size_t hand;


void
frame_init (void)
{
  void *base;

  lock_init (&scan_lock);

  frames = malloc (sizeof *frames * init_ram_pages);
  if (frames == NULL)
    PANIC ("out of memory allocating page frames");

  while ((base = palloc_get_page (PAL_USER)) != NULL)
    {
      struct frame *f = &frames[frame_cnt++];
      lock_init (&f->lock);
      f->base = base;
      f->page = NULL;
    }
}


static struct frame*
try_frame_alloc_and_lock (struct page *page) 
{
  // super simple (ie dumb) method to find a frame
  // I ASSUME that the frame->lock is for editing the frame
  // not sure if we need the scan lock. I ma not actually sure
  // what hte scan lock is for... maybe it is for this though.

  //lock_acquire(&scan_lock);
  int i;

  for(i = 0; i < frame_cnt; i++)
  {
    struct frame *f = &frames[i];
    bool success = lock_try_acquire(&f->lock);
    if(success && f->page != NULL)
    {
      f->page = page;
      lock_release(&f->lock);
      //lock_release(&scan_lock);
      return f;
    }
  }

  //lock_release(&scan_lock);
  return NULL;
}

void 
frame_lock (struct page *p) 
{
  // not sure if the scan_lock acquisition is necessary
  lock_acquire(&scan_lock);
  lock_acquire(&page->frame->lock);
  lock_release(&scan_lock);
}

// should this take in a page or a frame? I am not sure. 
// frame might make sense, since we could have multiple read only pages that
// point to this frame? I dunno...
void 
frame_free (struct frame *f)
{
  //lock_acquire(&scan_lock);
  lock_acquire(&f->lock);
  if(f->page != NULL)
  {
    f->page->frame = NULL; // should we write this page back to swap here?
    f->page = NULL;
  }
  lock_release(&f->lock);
  //lock_release(&scan_lock);
}

void 
frame_unlock (struct frame *f) 
{
  //lock_acquire(&scan_lock);
  lock_release(&f->lock);
  //lock_release(&scan_lock);
}
