#include "swap.h"
#include "page.h"
#include "frame.h"
#include "threads/vaddr.h"
#include "devices/block.h"
#include "lib/kernel/bitmap.h"
#include "userprog/pagedir.h"

/*

Managing the swap table

You should handle picking an unused swap slot for evicting a page from its
frame to the swap partition. And handle freeing a swap slot which its page
is read back.

You can use the BLOCK_SWAP block device for swapping, obtaining the struct
block that represents it by calling block_get_role(). Also to attach a swap
disk, please see the documentation.

and to attach a swap disk for a single run, use this option â€˜--swap-size=nâ€™

*/




// we just provide swap_init() for swap.c
// the rest is your responsibility

/* The swap device. */
static struct block *swap_device;

/* Used swap pages. */
static struct bitmap *swap_bitmap;

/* Protects swap_bitmap. */
static struct lock swap_lock;

/* index in the bitmap of the first free bit */
static size_t idx_first_free;

/* Number of sectors per page. */
#define PAGE_SECTORS (PGSIZE / BLOCK_SECTOR_SIZE)


void
swap_init (void)
{
  swap_device = block_get_role (BLOCK_SWAP);
  if (swap_device == NULL)
    {
      PANIC ("no swap device--swap disabled\n");
      swap_bitmap = bitmap_create (0);
    }
  else
    swap_bitmap = bitmap_create (block_size (swap_device)
                                 / PAGE_SECTORS);
  if (swap_bitmap == NULL)
    PANIC ("couldn't create swap bitmap");

  idx_first_free = 0;
  lock_init (&swap_lock);
}

/* assumes that the page is in a valid block sector
  AND that the page's frame field holds a free frame
  and that we currently hold the lock to said frame.
  This function should only be called from page_in
   */
bool
swap_in (struct page *p)
{
  //printf("swap_in: entered\n");
  int i;
  for(i = 0; i < PAGE_SECTORS; i++)
  {
    block_read (swap_device, p->sector + i, p->frame->base + (BLOCK_SECTOR_SIZE * i));
  }

  size_t bit_idx = p->sector / PAGE_SECTORS;

  lock_acquire(&swap_lock);
  bitmap_reset (swap_bitmap, bit_idx);
  idx_first_free = bit_idx < idx_first_free ? bit_idx : idx_first_free;
  p->sector = -1;
  p->swap = false;
  lock_release(&swap_lock);

  /* NOTE, we do not need to set the pagedir entry here, as that is handled in page_in */

  //printf("swap_in: exiting\n");
  return true;
}

void remove_from_swap(struct page *p)
{
  if(p->sector != -1)
  {
    size_t bit_idx = p->sector / PAGE_SECTORS;

    lock_acquire(&swap_lock);
    bitmap_reset (swap_bitmap, bit_idx);
    idx_first_free = bit_idx < idx_first_free ? bit_idx : idx_first_free;
    p->swap = false;
    lock_release(&swap_lock);
    p->sector = -1;    
    return true;
  }
}

/* this function will be called from try_frame_alloc_and_lock.
  it is solely for the actual eviction of a page, not the
  algorithm that finds it 
  this function assumes that the frame associated with the page is locked
  frame_free must be called AFTER this function.*/
bool 
swap_out (struct page *p) 
{

  size_t bit_idx;
  int i;

  //printf("swap_out: entered\n");

  /* don't swap out if the page is clean */
  if(!pagedir_is_dirty(p->thread->pagedir, p->addr))
  {
    memset(p->frame->base, 0, PGSIZE);
    p->frame->page = NULL;
    p->frame = NULL;
    lock_acquire(&swap_lock);
    p->swap = false;
    p->sector = -1;
    lock_release(&swap_lock);
    pagedir_clear_page (p->thread->pagedir, p->addr);
    //printf("swap_out: exiting clean case\n");
    return true;
  }
  /* this lock on the outside since it has more contexts
    in which it can be locked */
  lock_acquire(&swap_lock);

  //PANIC("swap_out: past lock acquisition");

  if(idx_first_free != BITMAP_ERROR && !bitmap_test (swap_bitmap, idx_first_free))
  {
    /* by the above test, idx_first_free is valid AND is free */
    bit_idx = idx_first_free;
    bitmap_mark (swap_bitmap, bit_idx);

    /* in theory, idx_first_free is the first free bit, so search for the next free bit */
    idx_first_free = bitmap_scan (swap_bitmap, idx_first_free+1, 1, false);

     /* if idx_first_free is a BITMAP_ERROR (ie, previous scan found no free bits after
      the previous value of idx_first_free), scan the whole bitmap to reset idx_first_free 
      if it is still BITMAP_ERROR, then we confirm that there are no more free bits */
    if(idx_first_free == BITMAP_ERROR)
    {
      idx_first_free = bitmap_scan (swap_bitmap, 0, 1, false);
    }
  }
  else
  {
    /* if idx_first_free is BITMAP_ERROR or is not actually free, 
    this likely means that the bitmap is full. Just to verify, we scan from the beginning again */
    bit_idx = bitmap_scan (swap_bitmap, 0, 1, false);
    if(bit_idx != BITMAP_ERROR)
    {
      bitmap_mark (swap_bitmap, bit_idx);
    }
    idx_first_free = bitmap_scan (swap_bitmap, 0, 1, false);
  } 

  /* no free things in the swap sector */
  if(bit_idx == BITMAP_ERROR)
  {
    PANIC("swap_out: exiting with no swap space available");
    frame_unlock(p->frame); 
    return false;
  }




  block_sector_t start = PAGE_SECTORS * bit_idx;

  p->sector = start;
  p->swap = true;
  lock_release(&swap_lock);


  for(i = 0; i < PAGE_SECTORS; i++)
  {
    /* DEFINITELY DOUBLE CHECK MY ARITHMETIC!! */
    block_write (swap_device, start + i, p->frame->base + (i * BLOCK_SECTOR_SIZE));
  }

  p->frame->page = NULL;
  p->frame = NULL; /* safe since we are calling this with frame lock and scan lock held */

  if(p->thread->pagedir)
  {
    pagedir_clear_page (p->thread->pagedir, p->addr);
  }
  else
  {
    PANIC("swap_out: page: %p has NULL pagedir\n", p->addr);
  }


  //printf("swap_out: exiting dirty case\n");
  return true;
}

