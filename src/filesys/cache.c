#include "filesys/cache.h"
#include <debug.h>
#include <string.h>
#include "filesys/filesys.h"
#include "devices/timer.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include <random.h>

#define INVALID_SECTOR ((block_sector_t) -1)


/* Cache. */
#define CACHE_CNT 64
struct cache_block cache[CACHE_CNT];
struct lock cache_sync;
static int hand = 0;


static void flushd_init (void);
static void readaheadd_init (void);
static void readaheadd_submit (block_sector_t sector);

static void
lock_cache(void)
{
  //printf("lock_cache: entered\n");
  lock_acquire(&cache_sync);
  //printf("lock_cache: exiting\n");
}
static void
unlock_cache(void)
{
  //printf("unlock_cache: entered\n");
  lock_release(&cache_sync);
  //printf("unlock_cache: exiting\n");
}
/* Initializes cache. */
void
cache_init (void) 
{
  int i;
  lock_init(&cache_sync);
  random_init(0xf1c183acc);
  for(i = 0; i < CACHE_CNT; i++)
  {
    struct cache_block *cb = &cache[i];
    lock_init(&cb->block_lock);
    cond_init(&cb->no_readers_or_writers);
    cond_init(&cb->no_writers);
    cb->readers = 0;
    cb->read_waiters = 0;
    cb->writers = 0;
    cb->write_waiters = 0;
    cb->sector = INVALID_SECTOR;
    cb->up_to_date = false;
    cb->dirty = false;
    cb->is_free = true;
    lock_init(&cb->data_lock);
  }
}

/* Flushes cache to disk. */
void
cache_flush (void) 
{
  int i;
  lock_cache();
  for(i = 0; i < CACHE_CNT; i++)
  {
    struct cache_block *b = &cache[i];
    //lock_acquire(&b->block_lock);
    if(b->dirty)
    {
      lock_data(b);
      block_write (fs_device, b->sector, b->data);
      unlock_data(b);
    }
    //lock_release(&b->block_lock);
  }
  
  unlock_cache();
}


/* block lock is assumed to be held */
static void
cache_lock_helper(struct cache_block *cb, enum lock_type type)
{
  if(type == EXCLUSIVE) /* I assume this means writing? */
  {
    cb->write_waiters++;
    cb->is_free = false;
    while(cb->writers || cb->readers)
    {
      unlock_cache();
      cond_wait(&cb->no_readers_or_writers, &cb->block_lock);
      lock_cache();
    }
    cb->write_waiters--;
    cb->writers++;
  }
  else
  {
    cb->read_waiters++;
    cb->is_free = false; 
    while(cb->writers)
    {
      unlock_cache();
      cond_wait(&cb->no_writers, &cb->block_lock);
      lock_cache();
    }
    cb->readers++;
    cb->read_waiters--;
  }
}

/* Locks the given SECTOR into the cache and returns the cache
   block.
   If TYPE is EXCLUSIVE, then the block returned will be locked
   only by the caller.  The calling thread must not already
   have any lock on the block.
   If TYPE is NON_EXCLUSIVE, then block returned may be locked by
   any number of other callers.  The calling thread may already
   have any number of non-exclusive locks on the block. */
struct cache_block *
cache_lock (block_sector_t sector, enum lock_type type) 
{

  //printf("cache_lock: entered with type %s\n", type == EXCLUSIVE ? "EXCLUSIVE" : "NON_EXCLUSIVE");

  /* need this for some inode functions */
  if(sector == INVALID_SECTOR)
  {
    PANIC("cache_lock: INVALID_SECTOR passed in");
    return NULL;
  }

  int i;

  try_again:

  /* Is the block already in-cache? */

  //printf("cache_lock: checking to see if sector %d is in cache\n", sector);

  lock_cache();
  for(i = 0; i < CACHE_CNT; i++)
  {
    struct cache_block *cb = &cache[i];
    //lock_acquire(&cb->block_lock);
    if(cb->sector == dsector)
    { 
      //cache_lock_helper(cb, type);
      if(!lock_held_by_current_thread(&cb->data_lock))
        lock_acquire(&cb->data_lock);
      //lock_release(&cb->block_lock);
      unlock_cache();
      //printf("cache_lock: returning the in cache case\n");
      return cb;
    }
    //lock_release(&cb->block_lock);
  }

  /* Not in cache.  Find empty slot. */

  //printf("cache_lock: sector %d not in cache\n", sector);

  /* cache[i] is locked when i is returned */
  i = find_free_block();

  if(i != -1)
  {
    struct cache_block *cb = &cache[i];
    cb->sector = sector;
    cb->readers = type == EXCLUSIVE ? 0 : 1;
    cb->read_waiters = 0;
    cb->writers = type == EXCLUSIVE ? 1 : 0;
    cb->write_waiters = 0;
    cb->up_to_date = false;
    cb->dirty = false;
    //lock_release(&cb->block_lock);
    lock_acquire(&cb->data_lock);
    unlock_cache();
    //PANIC("found a free block");
    //printf("cache_lock: returning the free block case\n");
    return &cache[i];
  }
  else /* No empty slots.  Evict something. */
  {
    int rand;

    rand_segment:
    random_bytes(&rand, 1);
    if(rand < 0)
    {
      rand = rand * (-1);
    }
    rand = rand % CACHE_CNT;
    struct cache_block *chosen_one = &cache[rand];

    if(chosen_one->readers || chosen_one->writers || 
      chosen_one->write_waiters || chosen_one->read_waiters)
    {
      goto rand_segment:
    }

    if(chosen_one->dirty)
    {
      lock_acquire(&chosen_one->data_lock);
      block_write (fs_device, chosen_one->sector, chosen_one->data);
      lock_release(&chosen_one->data_lock);
    }
    chosen_one->sector = sector;
    chosen_one->readers = type == EXCLUSIVE ? 0 : 1;
    chosen_one->read_waiters = 0;
    chosen_one->writers = type == EXCLUSIVE ? 1 : 0;
    chosen_one->write_waiters = 0;
    chosen_one->up_to_date = false;
    chosen_one->dirty = false;
    //lock_release(&cb->block_lock);
    //lock_init(&chosen_one->data_lock);
    lock_acquire(&chosen_one->data_lock);
    unlock_cache();
    //PANIC("found a free block");
    //printf("cache_lock: returning the free block case\n");
    return &cache[i];
  }

  unlock_cache();

  //PANIC("no free blocks");

  /* Wait for cache contention to die down. */

  // sometimes, you might get into a situation where you
  // cannot find a block to evict, or you cannot lock
  // the desired block. If that's the case there might
  // some contention. So the safest way to do this, is to
  // release the cache_sync lock, and sleep for 1 sec, and
  // try again the whole operation.




  PANIC("about to sleep");
  timer_msleep (100);
  goto try_again;

  return NULL;
}

/* Bring block B up_to_date, by reading it from disk if
   necessary, and return a pointer to its data.
   The caller must have an exclusive or non-exclusive lock on
   B. */
void *
cache_read (struct cache_block *b) 
{
  /* do we need anything else here?? */
  //lock_acquire(&b->block_lock);
  //lock_data(b);
  if(!b->up_to_date)
  {
    block_read (fs_device, b->sector, b->data);
    //unlock_data(b);
    b->up_to_date = true;
  }
  //lock_release(&b->block_lock);
  return (void *) b->data;
}

/* Zero out block B, without reading it from disk, and return a
   pointer to the zeroed data.
   The caller must have an exclusive lock on B. */
void *
cache_zero (struct cache_block *b) 
{
  // ...
  //  memset (b->data, 0, BLOCK_SECTOR_SIZE);
  // ...

  return NULL;
}

/* Marks block B as dirty, so that it will be written back to
   disk before eviction.
   The caller must have a read or write lock on B,
   and B must be up_to_date. */
void
cache_dirty (struct cache_block *b) 
{
  /* may need more here? */
  //lock_acquire(&b->block_lock);
  b->dirty = true;
  //lock_release(&b->block_lock);
}

/* marks block as free if there are no readers or writers or waiters 
  assumes that the block lock is held
*/
void cache_unlock_freer(struct cache_block *b)
{
  if(!(b->writers || b->write_waiters || b->readers || b->read_waiters))
  {
    b->is_free = true;
  }
}
/* Unlocks block B.
   If B is no longer locked by any thread, then it becomes a
   candidate for immediate eviction. */
void
cache_unlock (struct cache_block *b, enum lock_type type) 
{
  /* may not be necessary to hold cache sync lock */
  lock_cache();
  //lock_acquire(&b->block_lock);
  // if(type == EXCLUSIVE) /* I assume this means writing? */
  // {
  //   b->writers--; /* should be zero now */
  //   if(b->writers == 0)
  //   {
  //     if(b->readers == 0)
  //     {
  //       cond_signal(&b->no_readers_or_writers, &b->block_lock);
  //       cond_signal(&b->no_writers, &b->block_lock);
  //     }
  //     else
  //     {
  //       cond_signal(&b->no_writers, &b->block_lock);
  //     }
  //   }
  // }
  // else
  // {
  //   b->readers--;
  //   if(b->writers == 0)
  //   {
  //     if(b->readers == 0)
  //     {
  //       cond_signal(&b->no_readers_or_writers, &b->block_lock);
  //       cond_signal(&b->no_writers, &b->block_lock);
  //     }
  //     else
  //     {
  //       cond_signal(&b->no_writers, &b->block_lock);
  //     }
  //   }
  // }
  if(lock_held_by_current_thread(&b->data_lock))
    lock_release(&b->data_lock);
  //cache_unlock_freer(b);
  //lock_release(&b->block_lock);
  unlock_cache();
  return;
}

/* If SECTOR is in the cache, evicts it immediately without
   writing it back to disk (even if dirty).
   The block must be entirely unused. */
void
cache_free (block_sector_t sector) 
{
  // ...
}

void lock_data(struct cache_block *b)
{
  //lock_acquire(&b->data_lock);
}

void unlock_data(struct cache_block *b)
{
  //lock_release(&b->data_lock);
}


/* Flush daemon. */

static void flushd (void *aux);

/* Initializes flush daemon. */
static void
flushd_init (void) 
{
  thread_create ("flushd", PRI_MIN, flushd, NULL);
}

/* Flush daemon thread. */
static void
flushd (void *aux UNUSED) 
{
  for (;;) 
    {
      printf("for some reason this function is running\n");
      //timer_msleep (30 * 1000);
      cache_flush ();
    }
}


//returns the index of the first free block
//returns -1 if full
//thread-safe
int
find_free_block()
{
  /* assuming for now that this is called with cache_sync already held */
  //lock_cache();
  int i;
  for(i = 0; i<CACHE_CNT; i++)
  {
    if(cache[i].is_free)
    {
      cache[i].is_free=false;
      //lock_acquire(&cache[i].block_lock);
      //unlock_cache();
      return i;
    }
  }
  //unlock_cache();
  return -1;
}
