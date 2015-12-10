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
struct lock *cache_bak;
struct lock *debug;
int debug_cnt;
static int hand = 0;

#define DEBUG_VAR_CACHE 0


static void flushd_init (void);
static void readaheadd_init (void);
static void readaheadd_submit (block_sector_t sector);

static void
lock_cache(void)
{
  if(!lock_held_by_current_thread(cache_bak))
    lock_acquire(cache_bak);
}

static void debugCount()
{
  debug_cnt++;
  if(debug_cnt ==799)
  {
    if(DEBUG_VAR_CACHE)
    {
      printf("we hit it\n");
    }
  }
  if(DEBUG_VAR_CACHE)
  {
    printf("debug_cnt is: %d\n", debug_cnt);
  }
}

static void
unlock_cache(void)
{
  if(DEBUG_VAR_CACHE)
  {
    printf("unlock_cache: entered\n");
  }
  if(lock_held_by_current_thread(cache_bak))
    lock_release(cache_bak);
  if(DEBUG_VAR_CACHE)
  {
    printf("unlock_cache: exiting\n");
  }
}
/* Initializes cache. */
void
cache_init (void) 
{
  int i;
  debug_cnt = 0;
  lock_init(&cache_sync);
  cache_bak = &cache_sync;
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
    lock_init(&cb->read_write_lock);
    cb->cache_back = cache_bak;
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
    if(b->dirty)
    {
      lock_data(b);
      block_write (fs_device, b->sector, b->data);
      unlock_data(b);
    }
  }
  unlock_cache();
}


/* block lock is assumed to be held */
static void
cache_lock_helper(struct cache_block *cb, enum lock_type type)
{
  PANIC("someone called me");
  if(DEBUG_VAR_CACHE)
  {
    printf("in helper\n");
  }
  if(type == EXCLUSIVE) /* I assume this means writing? */
  {
    if(!lock_held_by_current_thread(&cb->block_lock))
    {
      lock_acquire(&cb->block_lock);
    }
    cb->write_waiters++;
    cb->is_free = false;
    while(cb->writers>0 || cb->readers>0)
    {
      unlock_cache();
      if(DEBUG_VAR_CACHE)
      {
        printf("going in with writers: %d, readers: %d\n", cb->writers, cb->readers);
      }
      cond_wait(&cb->no_readers_or_writers, &cb->block_lock);
      lock_cache();
    }
    cb->write_waiters--;
    cb->writers++;
    lock_release(&cb->block_lock);
  }
  else
  {
    cb->read_waiters++;
    cb->is_free = false;
    if(!lock_held_by_current_thread(&cb->block_lock))
    {
      lock_acquire(&cb->block_lock);
    }
    while(cb->writers>0)
    {
      unlock_cache();
      if(DEBUG_VAR_CACHE)
      {
        printf("going in with writers: %d, readers: %d\n", cb->writers, cb->readers);
      }
      cond_wait(&cb->no_writers, &cb->block_lock);
      lock_cache();
    }
    cb->read_waiters--;
    cb->readers++;
    if(lock_held_by_current_thread(&cb->block_lock))
    {
      lock_release(&cb->block_lock);
    }
    if(cb->readers==1)
    {
      debug_cnt++;
      if(debug_cnt==131)
      {
        if(DEBUG_VAR_CACHE)
        {
          printf("readers is one with cnt %d\n", debug_cnt);
        }
      }
    }
  }
  if(DEBUG_VAR_CACHE)
  {
    printf("out helper\n");
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
  if(DEBUG_VAR_CACHE)
  {
    printf("cache_lock: entered\n");
  }
  if(sector == INVALID_SECTOR)
  {
    debug_backtrace ();
    PANIC("cache_lock: INVALID_SECTOR passed in");
    return NULL;
  }

  int i;

  try_again:

  /* Is the block already in-cache? */

  lock_cache();
  if(DEBUG_VAR_CACHE)
  {
    printf("wat the devil\n");
  }
  for(i = 0; i < CACHE_CNT; i++)
  {
    struct cache_block *cb = &cache[i];     
    if(cb->sector == sector)
    {
      if(DEBUG_VAR_CACHE)
      {
        printf("index is: %d\n", i);
      }
      //cache_lock_helper(cb, type);
      debugCount();
      unlock_cache();
      if(!lock_held_by_current_thread(&cb->read_write_lock))
      {
        lock_acquire(&cb->read_write_lock);
      }
      if(DEBUG_VAR_CACHE)
      {
        printf("out lock\n");
      }
      return cb;
    }
  }

  /* Not in cache.  Find empty slot. */
  if(DEBUG_VAR_CACHE)
  {
    printf("cache_lock: going to fiund free\n");
  }
  i = find_free_block();
  if(DEBUG_VAR_CACHE)
  {
    printf("cache_lock: found free\n");
  }
  if(i != -1)
  {
    if(DEBUG_VAR_CACHE)
    {
      printf("free block with index %d\n", i);
    }
    struct cache_block *cb = &cache[i];
    cb->sector = sector;
    cb->readers = 0;
    cb->read_waiters = 0;
    cb->writers = 0;
    cb->write_waiters = 0;
    cb->up_to_date = true;
    cb->dirty = false;
    block_read (fs_device, cb->sector, cb->data);
    //cache_lock_helper(cb, type);
    debugCount();
    unlock_cache();
    if(!lock_held_by_current_thread(&cb->read_write_lock))
    {
      lock_acquire(&cb->read_write_lock);
    }
    if(DEBUG_VAR_CACHE)
    {
      printf("out lock\n");
    }
    return &cache[i];
  }
  else /* No empty slots.  Evict something. */
  {
    if(DEBUG_VAR_CACHE)
    {
      printf("begin eviction\n");
    }
    int rand;

    rand_segment:
    random_bytes(&rand, 1);
    if(rand < 0)
    {
      rand = rand * (-1);
    }
    rand = rand % CACHE_CNT;
    struct cache_block *chosen_one = &cache[rand];
    /*if(!lock_held_by_current_thread(&chosen_one->block_lock))
    {
      lock_acquire(&chosen_one->block_lock);
    }
    while(chosen_one->writers>0 || chosen_one->readers>0)
    {
      if(DEBUG_VAR_CACHE)
      {
        printf("writers are: %d, readers are: %d\n", chosen_one->writers, chosen_one->readers);
      }
      cond_wait(&chosen_one->no_readers_or_writers, &chosen_one->block_lock);
    }
    lock_release(&chosen_one->block_lock);*/
    debugCount();

    if(!lock_held_by_current_thread(&chosen_one->read_write_lock))
    {
      lock_acquire(&chosen_one->read_write_lock);
    }
    if(chosen_one->dirty)
    {
      // if(!lock_held_by_current_thread(&chosen_one->data_lock))
      // {
      //   lock_acquire(&chosen_one->data_lock);
      // }
      block_write (fs_device, chosen_one->sector, chosen_one->data);
      //lock_release(&chosen_one->data_lock);
    }
    chosen_one->sector = sector;
    chosen_one->readers = 0;
    chosen_one->read_waiters = 0;
    chosen_one->writers = 0;
    chosen_one->write_waiters = 0;
    chosen_one->up_to_date = false;
    chosen_one->dirty = false;
    //lock_release(&cb->block_lock);
    //lock_init(&chosen_one->data_lock);
    unlock_cache();
    if(!lock_held_by_current_thread(&chosen_one->data_lock))
    {
      lock_acquire(&chosen_one->data_lock);
    }
    block_read (fs_device, chosen_one->sector, chosen_one->data);
      //cache_lock_helper(chosen_one, type);

    //PANIC("found a free block");
    if(DEBUG_VAR_CACHE)
    {
      printf("cache_lock: returning the eviction case\n");
    }
    return chosen_one;
  }

  unlock_cache();

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
  if(DEBUG_VAR_CACHE)
  {
    //printf("in read\n");
  }
  /* do we need anything else here?? */
  lock_cache();
  if(true)
  {
    if(DEBUG_VAR_CACHE)
    {
      //printf("cache_read: reading from disk\n");
    }
    block_read (fs_device, b->sector, b->data);
    b->up_to_date = true;
  }
  unlock_cache();
  if(DEBUG_VAR_CACHE)
  {
    //printf("out read\n");
  }
  return (void *) b->data;
}

cache_write(struct cache_block *b)
{
  if(true)
  {
    block_write(fs_device, b->sector, b->data);
  }
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
  b->dirty = true;
}

/* marks block as free if there are no readers or writers or waiters 
  assumes that the block lock is held
*/
void cache_unlock_freer(struct cache_block *b)
{
  if(!(b->writers || b->write_waiters || b->readers || b->read_waiters))
  {
    if(b->dirty)
    {
      block_write (fs_device, b->sector, b->data);
      //b->is_free = true;
    }
  }
}
/* Unlocks block B.
   If B is no longer locked by any thread, then it becomes a
   candidate for immediate eviction. */
void
cache_unlock (struct cache_block *b, enum lock_type type) 
{
  /* may not be necessary to hold cache sync lock */
  //lock_cache();
  /*lock_acquire(&b->block_lock);
  if(type == EXCLUSIVE) // I assume this means writing?
  {
    if(b->writers==0)
    {
      if(DEBUG_VAR_CACHE)
      {
        printf("writers are 0\n");
      }
    }
    b->writers--;
     // should be zero now
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
  //cache_unlock_freer(b);
  if(lock_held_by_current_thread(&b->block_lock))
    lock_release(&b->block_lock);*/
  if(lock_held_by_current_thread(&b->read_write_lock))
  {
    lock_release(&b->read_write_lock);
  }
  //unlock_cache();
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
      if(DEBUG_VAR_CACHE)
      {
        printf("for some reason this function is running\n");
      }
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
      cache[i].is_free = false;
      return i;
    }
  }
  return -1;
}
