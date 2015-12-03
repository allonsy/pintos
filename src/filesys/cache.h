#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "devices/block.h"

/* Type of block lock. */
enum lock_type 
  {
    NON_EXCLUSIVE,	/* Any number of lockers. */
    EXCLUSIVE		/* Only one locker. */
  };


 struct cache_block 
  {
    struct lock block_lock;
    struct condition no_readers_or_writers;
    struct condition no_writers;           
    int readers, read_waiters;
    int writers, write_waiters;
    block_sector_t sector;
    bool up_to_date;
    bool dirty;
    struct lock data_lock; 
    uint8_t data[BLOCK_SECTOR_SIZE];   
  };

void cache_init (void);
void cache_flush (void);
struct cache_block *cache_lock (block_sector_t, enum lock_type);
void *cache_read (struct cache_block *);
void *cache_zero (struct cache_block *);
void cache_dirty (struct cache_block *);
void cache_unlock (struct cache_block *);
void cache_free (block_sector_t);
void cache_readahead (block_sector_t);

#endif /* filesys/cache.h */