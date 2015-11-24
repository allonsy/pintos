#ifndef VM_FRAME_H
#define VM_FRAME_H


#include "threads/synch.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "page.h"

/* A physical frame. */
struct frame
{
  struct lock lock;           /* Prevent simultaneous access. */
  void *base;                 /* Kernel virtual base address. */
  struct page *page;          /* Mapped process page, if any. */
};

void frame_init(void);
//struct frame* try_frame_alloc_and_lock (struct page *page);
struct frame* try_frame_alloc_and_lock_2 (struct page *page);
void frame_lock (struct frame *f);
void frame_free (struct frame *f);
void frame_unlock (struct frame *f);
void lock_scan(void);
void unlock_scan(void);
struct frame *perform_LRU(void);

#endif /* VM_FRAME_H */
