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


static struct frame *try_frame_alloc_and_lock (struct page *page) {}
static struct frame *try_frame_alloc_and_lock (struct page *page) {}
void frame_lock (struct page *p) {}
void frame_free (struct frame *f) {}
void frame_unlock (struct frame *f) {}
