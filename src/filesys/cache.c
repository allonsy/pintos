#include "filesys/cache.h"
#include <debug.h>
#include <string.h>
#include "filesys/filesys.h"
#include "devices/timer.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"

#define INVALID_SECTOR ((block_sector_t) -1)


/* Cache. */
#define CACHE_CNT 64
struct cache_block cache[CACHE_CNT];
struct lock cache_sync;
static int hand = 0;


static void flushd_init (void);
static void readaheadd_init (void);
static void readaheadd_submit (block_sector_t sector);


/* Initializes cache. */
void
cache_init (void) 
{
  int i;
  lock_init(&cache_sync);

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
  lock_acquire(&cache_sync);
  for(i = 0; i < CACHE_CNT; i++)
  {
    struct cache_block *b = &cache[i];
    lock_acquire(&b->block_lock);
    if(b->dirty)
    {
      lock_data(b);
      block_write (fs_device, b->sector, b->data);
      unlock_data(b);
    }
    lock_release(&b->block_lock);
  }
  
  lock_release(&cache_sync);
}

static void
cache_lock_helper(struct cache_block *cb, enum lock_type type)
{
  if(type == EXCLUSIVE) /* I assume this means writing? */
  {
    cb->write_waiters++;
    cb->is_free = false;
    while(cb->writers || cb->readers)
    {
      lock_release(&cache_sync);
      cond_wait(&cb->no_readers_or_writers, &cb->block_lock);
      lock_acquire(&cache_sync);
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
      lock_release(&cache_sync);
      cond_wait(&cb->no_writers, &cb->block_lock);
      lock_acquire(&cache_sync);
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

  /* need this for some inode functions */
  if(sector == INVALID_SECTOR)
    return NULL;

  int i;

  try_again:

  /* Is the block already in-cache? */

  lock_acquire(&cache_sync);
  for(i = 0; i < CACHE_CNT; i++)
  {
    struct cache_block *cb = &cache[i];
    lock_acquire(&cb->block_lock);
    if(cb->sector == sector)
    { 
      cache_lock_helper(cb, type);
      lock_release(&cb->block_lock);
      lock_release(&cache_sync);
      return cb;
    }
    lock_release(&cb->block_lock);
  }

  /* Not in cache.  Find empty slot. */

  PANIC("haven't implemented this yet lololol");

  i = find_free_block();

  if(i != -1)
  {

  }
  else /* No empty slots.  Evict something. */
  {

  }


  /* Wait for cache contention to die down. */

  // sometimes, you might get into a situation where you
  // cannot find a block to evict, or you cannot lock
  // the desired block. If that's the case there might
  // some contention. So the safest way to do this, is to
  // release the cache_sync lock, and sleep for 1 sec, and
  // try again the whole operation.



  lock_release (&cache_sync);
  timer_msleep (1000);
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
  lock_acquire(&b->block_lock);
  lock_data(b);
  block_read (fs_device, b->sector, b->data);
  unlock_data(b);
  b->up_to_date = true;
  lock_release(&b->block_lock);
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
  lock_acquire(&b->block_lock);
  b->dirty = true;
  lock_release(&b->block_lock);
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
  lock_acquire(&cache_sync);
  lock_acquire(&b->block_lock);
  if(type == EXCLUSIVE) /* I assume this means writing? */
  {
    b->writers--; /* should be zero now */
    if(b->writers == 0)
    {
      if(b->readers == 0)
      {
        cond_signal(&b->no_readers_or_writers, &b->block_lock);
        cond_signal(&b->no_writers, &b->block_lock);
      }
      else
      {
        cond_signal(&b->no_writers, &b->block_lock);
      }
    }
  }
  else
  {
    b->readers--;
    if(b->writers == 0)
    {
      if(b->readers == 0)
      {
        cond_signal(&b->no_readers_or_writers, &b->block_lock);
        cond_signal(&b->no_writers, &b->block_lock);
      }
      else
      {
        cond_signal(&b->no_writers, &b->block_lock);
      }
    }
  }
  cache_unlock_freer(b);
  lock_release(&b->block_lock);
  lock_release (&cache_sync);
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
  lock_acquire(&b->data_lock);
}

void unlock_data(struct cache_block *b)
{
  lock_release(&b->data_lock);
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
      timer_msleep (30 * 1000);
      cache_flush ();
    }
}


//returns the index of the first free block
//returns -1 if full
//thread-safe
int
find_free_block()
{
  /* assuming for now that this is called with lock already held */
  //lock_acquire(&cache_sync);
  int i;
  for(i = 0; i<CACHE_CNT; i++)
  {
    if(cache[i].is_free)
    {
      cache[i].is_free=false;
      //lock_release(&cache_sync);
      return i;
    }
  }
  //lock_release(&cache_sync);
  return -1;
}
