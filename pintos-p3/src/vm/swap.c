#include "swap.h"
#include "page.h"
#include "threads/vaddr.h"
#include "block.h"


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
      printf ("no swap device--swap disabled\n");
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

/* assumes that the page is held in a valid block sector 
  and that frame f is available, the hard bits are handled
  elsewhere. This function just writes into the frame */
bool
swap_in (struct page *p, struct frame *f)
{
  // might want to use these functions:
  // - lock_held_by_current_thread()
  // - block_read()
  // - bitmap_reset()

  int i;

  /* this lock on the outside since it has more contexts
    in which it can be locked */
  frame_lock(f);

  for(i = 0; i < PAGE_SECTORS; i++)
  {
    block_read (swap_device, p->sector + i, f->base + (BLOCK_SECTOR_SIZE * i));
  }

  size_t bit_idx = p->sector / PAGE_SECTORS;

  lock_acquire(&swap_lock);
  bitmap_reset (swap_bitmap, bit_idx);
  idx_first_free = bit_idx < idx_first_free ? bit_idx : idx_first_free;
  lock_release(&swap_lock);

  frame_unlock(f);
	// vestigial
  return false;
}

bool 
swap_out (struct page *p) 
{

  size_t bit_idx;

  /* this lock on the outside since it has more contexts
    in which it can be locked */
  frame_lock(&p->frame);
  lock_acquire(&swap_lock);

  // make sure idx_first_free is in fact free
  if(!bitmap_test (swap_bitmap, idx_first_free))
  {
    bit_idx = idx_first_free;
    bitmap_mark (swap_bitmap, bit_idx);
    idx_first_free = bitmap_scan (swap_bitmap, idx_first_free+1, 1, false);
  }
  else
  {
    // theoretically idx_first_free is handled correctly and this never happens
    // but otherwise we spend time scanning the bitmap to make sure it 
    // gets reset correctly
    bit_idx = bitmap_scan (swap_bitmap, 0, 1, false);
    bitmap_mark (swap_bitmap, bit_idx);
    idx_first_free = bitmap_scan (swap_bitmap, 0, 1, false);
  }
  block_sector_t start = PAGE_SECTORS * bit_idx;
  lock_release(&swap_lock);

  for(i = 0; i < PAGE_SECTORS; i++)
  {
    /* DEFINITELY DOUBLE CHECK MY ARITHMETIC!! */
    block_write (swap_device, start + i, p->frame->base + (i * BLOCK_SECTOR_SIZE));
  }

  p->sector = start;

  frame_unlock(&p->frame);

  frame_free(&p->frame);
  //vestigial
  return false;
}
