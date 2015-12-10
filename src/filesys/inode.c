#include "filesys/inode.h"
#include <bitmap.h>
#include <list.h>
#include <debug.h>
#include <round.h>
#include <stdio.h>
#include <string.h>
#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/synch.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define INVALID_SECTOR ((block_sector_t) -1)

#define DIRECT_CNT 123
#define INDIRECT_CNT 1
#define DBL_INDIRECT_CNT 1
#define SECTOR_CNT (DIRECT_CNT + INDIRECT_CNT + DBL_INDIRECT_CNT)

#define PTRS_PER_SECTOR ((off_t) (BLOCK_SECTOR_SIZE / sizeof (block_sector_t)))
#define INODE_SPAN ((DIRECT_CNT                                              \
                     + PTRS_PER_SECTOR * INDIRECT_CNT                        \
                     + PTRS_PER_SECTOR * PTRS_PER_SECTOR * DBL_INDIRECT_CNT) \
                    * BLOCK_SECTOR_SIZE)

/* technically this is 1 higher */
#define MAX_INDIRECT_SECTOR (DIRECT_CNT + PTRS_PER_SECTOR * INDIRECT_CNT)
#define MAX_DBL_INDIRECT_SECTOR (DIRECT_CNT + PTRS_PER_SECTOR * INDIRECT_CNT + PTRS_PER_SECTOR * PTRS_PER_SECTOR * DBL_INDIRECT_CNT)

#define DEBUG_VAR 0

static void
dprint(const char *str, bool exitr)
{
  if(DEBUG_VAR)
  {
    printf("%s: %s\n", str, exitr ? "exiting" : "entered");
  }
}

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t sectors[SECTOR_CNT]; /* sectors. */
    enum inode_type type;               /* FILE_INODE or DIR_INODE. */
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
  };

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}


/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    struct lock lock;                   /* Protects the inode. */

    /* Denying writes. */
    struct lock deny_write_lock;        /* Protects members below. */
    struct condition no_writers_cond;   /* Signaled when no writers. */ 
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    int writer_cnt;                     /* Number of writers. */
  };

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Controls access to open_inodes list. */
static struct lock open_inodes_lock;

static void deallocate_inode (const struct inode *);
static bool allocate_sector(block_sector_t *, bool);



static void
lock_list(void)
{
  lock_acquire(&open_inodes_lock);
}

static void
unlock_list(void)
{
  lock_release(&open_inodes_lock);
}

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
  lock_init (&open_inodes_lock);
}

/* Initializes an inode of the given TYPE, writes the new inode
   to sector SECTOR on the file system device, and returns the
   inode thus created.  Returns a null pointer if unsuccessful,
   in which case SECTOR is released in the free map.

  I assume that the sector has been allocated in the freemap already
    */  
bool
inode_create (block_sector_t sector, off_t size, enum inode_type type) 
{
  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  //printf("break\n");
  struct cache_block *block = cache_lock (sector, EXCLUSIVE);
  //printf("break\n");
  if(block == NULL)
  {
    cache_unlock(block, EXCLUSIVE);
    return false;
  }

  struct inode_disk *disk_inode = (struct inode_disk *) cache_read(block);

  disk_inode->length = size;
  //printf("creating with: %d for sector %d\n", disk_inode->length, sector);

  disk_inode->magic = INODE_MAGIC;
  disk_inode->type = type;

  int i;
  for(i = 0; i < SECTOR_CNT; i++)
  {
    disk_inode->sectors[i] = INVALID_SECTOR;
  }
  allocate_sector(&disk_inode->sectors[0], 1);

  cache_dirty(block);
  cache_unlock(block, EXCLUSIVE);

  return true;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  dprint("inode_open", 0);

  /* Check whether this inode is already open. */

  lock_list();
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes); e = list_next (e)) 
  {
    inode = list_entry (e, struct inode, elem);
    if (inode->sector == sector) 
      {
        inode_reopen (inode);
        unlock_list();
        dprint("inode_open", 1);
        return inode; 
      }
  }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
  {
    unlock_list();
    dprint("inode_open", 1);
    return NULL;
  }

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  unlock_list();
  lock_init(&inode->deny_write_lock);
  lock_init(&inode->lock);
  cond_init (&inode->no_writers_cond);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->writer_cnt = 0;
  inode->removed = false;
  /* I am not sure we need to do anything else here. I think we can lazily
    allocate a cache block when we are reading from the inode 

    IF WE NEED THIS:
    I think this should be nonexclusive since we are just opening the file 
   */
  //struct cache_block *block = cache_lock (sector, NON_EXCLUSIVE);
  //struct inode_disk *data = cache_read(block);

  dprint("inode_open", 1);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  dprint("inode_reopen", 0);
  if (inode != NULL)
  {
    inode_lock(inode);
    inode->open_cnt++;
    inode_unlock (inode);
  }
  dprint("inode_reopen", 1);
  return inode;
}

/* Returns the type of INODE. */
enum inode_type
inode_get_type (const struct inode *inode) 
{

  dprint("inode_get_type", 0);
  ASSERT(inode != NULL);

  struct cache_block *block = cache_lock(inode->sector, NON_EXCLUSIVE);
  struct inode_disk *data = (struct inode_disk *) cache_read(block);
  enum inode_type type = data->type;
  cache_unlock(block, NON_EXCLUSIVE);
  dprint("inode_get_type", 1);
  return type;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  dprint("inode_get_inumber", 0);
  dprint("inode_get_inumber", 1);
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  dprint("inode_close", 0);
  /* Ignore null pointer. */
  if (inode == NULL)
  {
    dprint("inode_close", 1);
    return;
  }

  /* Release resources if this was the last opener. */
  /* have to have list lock on the outside to prevent deadlock
    see inode_open for why */
  lock_list();
  inode_lock(inode);
  if (--inode->open_cnt == 0)
  {
    /* Remove from inode list and release lock. */
    list_remove (&inode->elem);

    /* Deallocate blocks if removed. */
    if (inode->removed) 
    {
      deallocate_inode(inode);
    }

    free (inode); 
    unlock_list();
    dprint("inode_close", 1);
    return; /* can't release inode lock after freeing */
  }
  inode_unlock(inode);
  unlock_list();
  dprint("inode_close", 1);
}


/* Deallocates SECTOR and anything it points to recursively.
   LEVEL is 2 if SECTOR is doubly indirect,
   or 1 if SECTOR is indirect,
   or 0 if SECTOR is a data sector. */
static void
deallocate_recursive (block_sector_t sector, int level) 
{
  if(sector == INVALID_SECTOR)
    return;

  struct cache_block *block = cache_lock(sector, EXCLUSIVE);
  block_sector_t *sectors;
  int i;

  switch(level)
  {
    case 0:
      break;
    /* this is funky, but it works */
    case 1: /* indirect */
    case 2: /* double indirect */
      sectors = (block_sector_t *) block->data;
      for(i = 0; i < PTRS_PER_SECTOR; i++)
      {
        /* deallocate the indirect sectors pointed to by the block */
        if(sectors[i] == INVALID_SECTOR)
          continue;
        else
          deallocate_recursive(sectors[i], level - 1);
      }
      break;
    default:
      PANIC("deallocate_recursive: INVALID SECTOR LEVEL %d", level);
  }
  memset(block->data, 0, BLOCK_SECTOR_SIZE);
  cache_dirty(block);
  /* I think we should free this on the freemap? */
  free_map_release (sector, 1);
  cache_unlock(block, EXCLUSIVE); /* should we be evicting here?? */
}

/* Deallocates the blocks allocated for INODE. */
static void
deallocate_inode (const struct inode *inode)
{
  dprint("deallocate_inode", 0);
  struct cache_block *block;
  struct inode_disk *data;
  int i;

  block = cache_lock(inode->sector, EXCLUSIVE);
  data = (struct inode_disk *) cache_read(block);

  for(i = 0; i < DIRECT_CNT; i++)
  {
    deallocate_recursive (data->sectors[i], 0);
  }
  /* more modular, technically, deallocate indirect sectors */
  for(i = DIRECT_CNT; i < DIRECT_CNT + INDIRECT_CNT; i++)
  {
    deallocate_recursive(data->sectors[i], 1);
  }
  /* deallocate dbl indirect sectors */
  for(i = DIRECT_CNT + INDIRECT_CNT; i < SECTOR_CNT; i++)
  {
    deallocate_recursive(data->sectors[i], 2);
  }

  memset(data, 0, BLOCK_SECTOR_SIZE);
  free_map_release (inode->sector, 1);
  cache_unlock(block, EXCLUSIVE);
  dprint("deallocate_inode", 1);

}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Translates SECTOR_IDX into a sequence of block indexes in
   OFFSETS and sets *OFFSET_CNT to the number of offsets. */
static void
calculate_indices (off_t sector_idx, size_t offsets[], size_t *offset_cnt)
{

  dprint("calculate_indices", 0);
  /* THIS MATH IS NOT SAFE FOR ANYTHING BUT 1 DBL INDIRECT */
  ASSERT(sector_idx <= MAX_DBL_INDIRECT_SECTOR);

  /* Handle direct blocks. */
  if(sector_idx < DIRECT_CNT)
  {
    *offset_cnt = 1;
    offsets[0] = sector_idx;
    dprint("calculate_indices", 1);
    return;
  }
  /* Handle indirect blocks. */
  if (sector_idx <= MAX_INDIRECT_SECTOR)
  {
    *offset_cnt = 2;
    off_t ind_idx = sector_idx - DIRECT_CNT;
    offsets[0] = DIRECT_CNT + (ind_idx / PTRS_PER_SECTOR);
    offsets[1] = ind_idx % PTRS_PER_SECTOR;
    dprint("calculate_indices", 1);
    return;
  }
  /* Handle doubly indirect blocks. */
  if(sector_idx <= MAX_DBL_INDIRECT_SECTOR)
  {
    /* this bit in particular assumes that there is one doubly indirect block */
    *offset_cnt = 3;
    offsets[0] = DIRECT_CNT + INDIRECT_CNT;
    off_t dbl_ind_idx = sector_idx - (DIRECT_CNT + INDIRECT_CNT * PTRS_PER_SECTOR);
    offsets[1] = dbl_ind_idx / PTRS_PER_SECTOR;
    offsets[2] = dbl_ind_idx % PTRS_PER_SECTOR;
    dprint("calculate_indices", 1);
    return;
  }
}

/* returns true if it allocated the sector, false if it didn't */
static bool
allocate_sector(block_sector_t *sectorp, bool direct)
{
  dprint("allocate_sector", 0);
  if(*sectorp == INVALID_SECTOR) 
  {
    free_map_allocate(1, sectorp);
    struct cache_block *block = cache_lock(*sectorp, EXCLUSIVE);
    uint8_t *data = (uint8_t *) cache_read(block);
    memset(data, (direct ? 0 : ~0), BLOCK_SECTOR_SIZE);
    cache_dirty(block);
    cache_unlock(block, EXCLUSIVE);
    dprint("allocate_sector TRUE", 1);
    return true;
  }

  if(*sectorp == INVALID_SECTOR)
  {
    PANIC("allocate_sector: free_map_allocate failed");
  }

  dprint("allocate_sector FALSE", 1);
  return false;
}

/* Retrieves the data block for the given byte OFFSET in INODE,
   setting *DATA_BLOCK to the block.
   Returns true if successful, false on failure.
   If ALLOCATE is false, then missing blocks will be successful
   with *DATA_BLOCk set to a null pointer.
   If ALLOCATE is true, then missing blocks will be allocated.
   The block returned will be locked, normally non-exclusively,
   but a newly allocated block will have an exclusive lock. */
static bool
get_data_block (struct inode *inode, off_t offset, bool allocate,
                struct cache_block **data_block, bool *excl UNUSED) 
{
  off_t logical_sector = offset / BLOCK_SECTOR_SIZE;
  size_t offsets[3];
  size_t offset_cnt;
  struct cache_block *block;
  block_sector_t *data;
  block_sector_t cur_sector = inode->sector;
  size_t cur_off;
  size_t i;

  /* whoohoo */

  dprint("get_data_block", 0);
  calculate_indices(logical_sector, offsets, &offset_cnt);

  ASSERT(1 <= offset_cnt && offset_cnt <= 3);

  //printf("get_data_block: after calculate_indices, offset_cnt is %d\n", offset_cnt);

  for(i = 0; i < offset_cnt; i++)
  {
    block = cache_lock(cur_sector, EXCLUSIVE);
    cur_off = offsets[i];
    if(block != NULL)
    {
      data = (block_sector_t *) cache_read(block);
      if(data[cur_off] != INVALID_SECTOR)
      {
        cur_sector = data[cur_off];
      }
      else if (allocate)
      {
        allocate_sector(&data[cur_off], (i == offset_cnt - 1));
        cur_sector = data[cur_off];
      }
      else
      {
        cache_unlock(block, EXCLUSIVE);
        *data_block = NULL;
        //printf("get_data_block: missing block with allocate false on loop inter %d\n", i);
        return true;
      }
      cache_unlock(block, EXCLUSIVE);
    }
    else /* block == NULL */
    {
      PANIC("get_data_block: cache_lock returned NULL on sector %u. Sector was invalid: %d", 
          cur_sector, cur_sector == INVALID_SECTOR);
      return false;
    }
  } /* end for */

  *data_block = cache_lock(cur_sector, EXCLUSIVE);

  return true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  bool excl;
  dprint("inode_read_at", 0);

  while (size > 0) 
    {
      /* Sector to read, starting byte offset within sector, sector data. */
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;
      struct cache_block *block;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0 || !get_data_block (inode, offset, false, &block, &excl))
      {
        //printf("pointer to block is: %p\n", block);
        cache_unlock(block, excl);
        //PANIC("Whhoopsie");
        break;
      }

      if (block == NULL) 
        memset (buffer + bytes_read, 0, chunk_size);
      else 
        {
          const uint8_t *sector_data = cache_read (block);
          memcpy (buffer + bytes_read, sector_data + sector_ofs, chunk_size);
          // if(excl)
          // {
          //   cache_unlock (block, EXCLUSIVE);
          // }
          // else
          // {
          //   cache_unlock (block, NON_EXCLUSIVE);
          // }
          cache_unlock (block, EXCLUSIVE);
        }
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }

  dprint("inode_read_at", 1);
  return bytes_read;
}


/* Extends INODE to be at least LENGTH bytes long. 
*/
static void
extend_file (struct inode *inode, off_t length) 
{
  dprint("extend_file", 0);
  /* maybe shouldn't be an assertion? */
  ASSERT(length <= INODE_SPAN);

  struct cache_block *block;
  off_t offset = 0; /* MAYBE WE SHOULD SET THIS TO THE CURRENT INODE LENGTH ROUNDED DOWN? */

  /* THIS IS CURRENTLY A VERY SLOW AND INEFFICIENT IMPLEMENATION */
  while(offset < length)
  {
    get_data_block (inode, offset, true, &block, NULL /* unused */);
    cache_unlock(block, EXCLUSIVE);
    offset += BLOCK_SECTOR_SIZE;
  }
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if an error occurs. */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  bool excl;

  dprint("inode_write_at", 0);

  /* Don't write if writes are denied. */
  lock_acquire (&inode->deny_write_lock);
  while(inode->deny_write_cnt) 
  {
    cond_wait (&inode->no_writers_cond, &inode->deny_write_lock);
  }
  inode->writer_cnt++;
  lock_release (&inode->deny_write_lock);

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector, sector data. */
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;
      struct cache_block *block;
      uint8_t *sector_data;

      /* Bytes to max inode size, bytes left in sector, lesser of the two. */
      off_t inode_left = INODE_SPAN - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;

      if (chunk_size <= 0 || !get_data_block (inode, offset, true, &block, &excl))
        break;

      sector_data = cache_read (block);
      memcpy (sector_data + sector_ofs, buffer + bytes_written, chunk_size);
      cache_dirty (block);
      // if(excl)
      // {
      //   cache_unlock (block, EXCLUSIVE);
      // }
      // else
      // {
      //   cache_unlock (block, NON_EXCLUSIVE);
      // }
      cache_unlock (block, EXCLUSIVE);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }

  extend_file (inode, offset);

  lock_acquire (&inode->deny_write_lock);
  if (--inode->writer_cnt == 0)
    cond_signal (&inode->no_writers_cond, &inode->deny_write_lock);
  lock_release (&inode->deny_write_lock);

  dprint("inode_write_at", 1);
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  dprint("inode_deny_write", 0);
  lock_acquire(&inode->deny_write_lock);
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  lock_release(&inode->deny_write_lock);
  dprint("inode_allow_write", 1);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  dprint("inode_allow_write", 0);
  lock_acquire(&inode->deny_write_lock);
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
  lock_release(&inode->deny_write_lock);
  dprint("inode_allow_write", 1);
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  dprint("inode_length", 0);
  //printf("pass read\n");
  struct cache_block *block = cache_lock(inode->sector, NON_EXCLUSIVE);
  struct inode_disk *data = (struct inode_disk *) cache_read(block);
  off_t length = data->length;
  cache_unlock(block, NON_EXCLUSIVE);
  dprint("inode_length", 1);
  //printf("returning for sector: %d of %d\n", inode->sector, length);
  return length;
}

/* Returns the number of openers. */
int
inode_open_cnt (const struct inode *inode) 
{
  int open_cnt;
  dprint("inode_open_cnt", 0);
  lock_list();
  open_cnt = inode->open_cnt;
  unlock_list();
  dprint("inode_open_cnt", 1);
  return open_cnt;
}

/* Locks INODE. */
void
inode_lock (struct inode *inode) 
{
  dprint("inode_lock", 0);
  lock_acquire (&inode->lock);
  dprint("inode_lock", 1);
}

/* Releases INODE's lock. */
void
inode_unlock (struct inode *inode) 
{
  dprint("inode_unlock", 0);
  lock_release (&inode->lock);
  dprint("inode_unlock", 1);
}
