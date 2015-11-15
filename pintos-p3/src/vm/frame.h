#include "page.h"

/* A physical frame. */
struct frame
{
  struct lock lock;           /* Prevent simultaneous access. */
  void *base;                 /* Kernel virtual base address. */
  struct page *page;          /* Mapped process page, if any. */
};


void frame_lock (struct page *p);
void frame_free (struct frame *f);
void frame_unlock (struct frame *f);