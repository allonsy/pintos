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

static bool allocate_sector(block_sector_t *sectorp);

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
static void init_indirect_sector(block_sector_t *);

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
inode_create (block_sector_t sector, enum inode_type type) 
{

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */

  struct cache_block *block = cache_lock (sector, EXCLUSIVE);
  if(block == NULL)
    return false;

  struct inode_disk *disk_inode = (struct inode_disk *) cache_read(block);

  disk_inode->length = 0;
  disk_inode->magic = INODE_MAGIC;
  disk_inode->type = type;

  int i;
  for(i = 0; i < SECTOR_CNT; i++)
    disk_inode->sectors[i] = INVALID_SECTOR;

  cache_unlock(block);

  return true;

  /* old code */
  // if (disk_inode != NULL)
  //   {
  //     size_t sectors = bytes_to_sectors (length);
  //     disk_inode->length = length;
  //     disk_inode->magic = INODE_MAGIC;
  //     if (free_map_allocate (sectors, &disk_inode->start)) 
  //       {
  //         block_write (fs_device, sector, disk_inode);
  //         if (sectors > 0) 
  //           {
  //             static char zeros[BLOCK_SECTOR_SIZE];
  //             size_t i;
              
  //             for (i = 0; i < sectors; i++) 
  //               block_write (fs_device, disk_inode->start + i, zeros);
  //           }
  //         success = true; 
  //       } 
  //     free (disk_inode);
  //   }
  //return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */

  lock_acquire(&open_inodes_lock);
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes); e = list_next (e)) 
  {
    inode = list_entry (e, struct inode, elem);
    if (inode->sector == sector) 
      {
        inode_reopen (inode);
        lock_release(&open_inodes_lock);
        return inode; 
      }
  }
  lock_release(&open_inodes_lock);

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  lock_acquire(&open_inodes_lock);
  list_push_front (&open_inodes, &inode->elem);
  lock_release(&open_inodes_lock);
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

  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
  {
    lock_acquire (&inode->lock);
    inode->open_cnt++;
    lock_release (&inode->lock);
  }
  return inode;
}

/* Returns the type of INODE. */
enum inode_type
inode_get_type (const struct inode *inode) 
{

  ASSERT(inode != NULL);

  struct cache_block *block = cache_lock(inode->sector, NON_EXCLUSIVE);
  struct inode_disk *data = (struct inode_disk *) cache_read(block);
  enum inode_type type = data->type;
  cache_unlock(block);
  return type;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  /* have to have list lock on the outside to prevent deadlock
    see inode_open for why */
  lock_acquire (&open_inodes_lock);
  lock_acquire(&inode->lock);
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
  }
  lock_release(&inode->lock);
  lock_release (&open_inodes_lock);
}

/* returns true if it allocated the sector, false if it didn't */
static bool
allocate_sector(block_sector_t *sectorp)
{
  if(*sectorp == INVALID_SECTOR && free_map_allocate(sectorp))
  {
    struct cache_block *block = cache_lock(*sectorp, NON_EXCLUSIVE);
    uint8_t *data = (uint8_t *) cache_read(block);
    memset(data, 0, BLOCK_SECTOR_SIZE);
    cache_unlock(block);
    return true;
  }
  return false;
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
          break;
        else
          deallocate_recursive(sectors[i], level - 1);
      }
      break;
    default:
      PANIC("deallocate_recursive: INVALID SECTOR LEVEL %d", level);
  }
  memset(block->data, 0, BLOCK_SECTOR_SIZE);
  /* I think we should free this on the freemap? */
  free_map_release (sector);
  cache_unlock(block); /* should we be evicting here?? */
}

/* Deallocates the blocks allocated for INODE. */
static void
deallocate_inode (const struct inode *inode)
{

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
  free_map_release (inode->sector);
  cache_unlock(block);

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
  /* THIS MATH IS NOT SAFE FOR ANYTHING BUT 1 DBL INDIRECT */
  ASSERT(sector_idx < MAX_DBL_INDIRECT_SECTOR);

  /* Handle direct blocks. */
  if(sector_idx < DIRECT_CNT)
  {
    *offset_cnt = 1;
    offsets[0] = sector_idx;
    return;
  }
  /* Handle indirect blocks. */
  if (sector_idx < MAX_INDIRECT_SECTOR)
  {
    *offset_cnt = 2;
    off_t ind_idx = sector_idx - DIRECT_CNT;
    offsets[0] = DIRECT_CNT + (ind_idx / PTRS_PER_SECTOR);
    offsets[1] = ind_idx % PTRS_PER_SECTOR;
    return;
  }
  /* Handle doubly indirect blocks. */
  if(sector_idx < MAX_DBL_INDIRECT_SECTOR)
  {
    /* this bit in particular assumes that there is one doubly indirect block */
    *offset_cnt = 3;
    offsets[0] = DIRECT_CNT + INDIRECT_CNT;
    off_t dbl_ind_idx = sector_idx - (DIRECT_CNT + INDIRECT_CNT * PTRS_PER_SECTOR);
    offsets[1] = dbl_ind_idx / PTRS_PER_SECTOR;
    offsets[2] = dbl_ind_idx % PTRS_PER_SECTOR;
    return;
  }
}

/* will take a pointer to a sector, allocate a sector on disk and set the
  pointed to value to this sector. 
  then it sets the all the data block bits to 1. This way, all the sectors 
  in it are invalid, waiting to be filled */ 
static void
init_indirect_sector(block_sector_t *sectorp)
{
  if(*sectorp == INVALID_SECTOR && free_map_allocate(sectorp))
  {
    struct cache_block *block = cache_lock(*sectorp, EXCLUSIVE);
    uint8_t *data = (uint8_t *) cache_read(block);
    memset(data, ~0, BLOCK_SECTOR_SIZE);
    cache_unlock(block);
  }
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
                struct cache_block **data_block) 
{
  off_t logical_sector = offset / BLOCK_SECTOR_SIZE;
  size_t offsets[3];
  size_t offset_cnt;
  struct cache_block *block, *indirect_block, *dbl_indirect_block;
  struct inode_disk *data;
  block_sector_t *blocks, *ind_blocks;
  bool success = false;

  calculate_indices(logical_sector, offsets, &offset_cnt);

  block = cache_lock(inode->sector, NON_EXCLUSIVE);
  data = (struct inode_disk *) cache_read(block);

  switch(offset_cnt)
  {
    case 1:
      if(allocate)
      {
        if(allocate_sector(&data->sectors[offsets[0]]))
        {
          *data_block = cache_lock(data->sectors[offsets[0]], EXCLUSIVE);
        }
        else
        {
          *data_block = cache_lock(data->sectors[offsets[0]], NON_EXCLUSIVE);
        }
        success = true;
      }
      else
      {
        *data_block = cache_lock(data->sectors[offsets[0]], NON_EXCLUSIVE);
        success = *data_block != NULL;
      }
      break;
    case 2:
      if(allocate)
      {
        init_indirect_sector(&data->sectors[offsets[0]]);
        indirect_block = cache_lock(data->sectors[offsets[0]], NON_EXCLUSIVE);
        blocks = (block_sector_t *) cache_read(indirect_block);
        if(allocate_sector(&blocks[offsets[1]]))
        {
          *data_block = cache_lock(blocks[offsets[1]], EXCLUSIVE);
        }
        else
        {
          *data_block = cache_lock(blocks[offsets[1]], NON_EXCLUSIVE);
        }
        cache_unlock(indirect_block);
        success = true;
      }
      else
      {
        if(data->sectors[offsets[0]] != INVALID_SECTOR)
        {
          indirect_block = cache_lock(data->sectors[offsets[0]], NON_EXCLUSIVE);
          blocks = (block_sector_t *) cache_read(indirect_block);
          *data_block = cache_lock(blocks[offsets[1]], NON_EXCLUSIVE);
          success = *data_block != NULL;
        } 
      }
    case 3:
      if(allocate)
      {
        init_indirect_sector(&data->sectors[offsets[0]]);
        dbl_indirect_block = cache_lock(data->sectors[offsets[0]], EXCLUSIVE);
        ind_blocks = (block_sector_t *) cache_read(dbl_indirect_block);
        init_indirect_sector(&ind_blocks[offsets[1]]);
        indirect_block = cache_lock(ind_blocks[offsets[1]], EXCLUSIVE);
        blocks = cache_read(indirect_block);
        if(allocate_sector(&blocks[offsets[2]]))
        {
          *data_block = cache_lock(blocks[offsets[2]], EXCLUSIVE);
        }
        else
        {
          *data_block = cache_lock(blocks[offsets[2]], NON_EXCLUSIVE);
        }
        cache_unlock(indirect_block);
        cache_unlock(dbl_indirect_block);
        success = true;
      }
      else
      {
        if(data->sectors[offsets[0]] != INVALID_SECTOR)
        {
          dbl_indirect_block = cache_lock(data->sectors[offsets[0]], NON_EXCLUSIVE);
          ind_blocks = (block_sector_t *) cache_read(dbl_indirect_block);
          if(ind_blocks[offsets[1]] != INVALID_SECTOR)
          {
            indirect_block = cache_lock(ind_blocks[offsets[1]], NON_EXCLUSIVE);
            blocks = (block_sector_t*) cache_read(indirect_block);
            *data_block = cache_lock(blocks[offsets[2]], NON_EXCLUSIVE);
            cache_unlock(indirect_block);
            success = *data_block != NULL;
          }
          cache_unlock(dbl_indirect_block);
        }
      }

      break;
    default:
      break;
  }

  cache_unlock(block);

  return success;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;

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
      if (chunk_size <= 0 || !get_data_block (inode, offset, false, &block))
        break;

      if (block == NULL) 
        memset (buffer + bytes_read, 0, chunk_size);
      else 
        {
          const uint8_t *sector_data = cache_read (block);
          memcpy (buffer + bytes_read, sector_data + sector_ofs, chunk_size);
          cache_unlock (block);
        }
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }

  return bytes_read;
}


/* Extends INODE to be at least LENGTH bytes long. */
static void
extend_file (struct inode *inode, off_t length) 
{

  /* maybe shouldn't be an assertion? */
  ASSERT(length <= INODE_SPAN);

  struct cache_block *block, *indirect_block, *dbl_indirect_block;
  struct inode_disk *data;
  block_sector_t *blocks, *ind_blocks;
  int i = 0;
  int j, k;
  off_t offset = 0;
  bool second_while = false;
  bool third_while = false;


  block = cache_lock(inode->sector, EXCLUSIVE);
  data = (struct inode_disk *) cache_read(block);

  while(offset < length && i < DIRECT_CNT)
  {
    /* only allocates if the sector number is invalid */
    allocate_sector(&data->sectors[i]);
    offset += BLOCK_SECTOR_SIZE;
    i++;
  }

  if(offset < length)
  {  
    /* will only create a new sector if there is no valid indirect sector */
    init_indirect_sector(&data->sectors[DIRECT_CNT]);
    indirect_block = cache_lock(data->sectors[DIRECT_CNT], EXCLUSIVE);
    blocks = (block_sector_t *) cache_read(indirect_block);
    j = 0;
    second_while = true;
  }

  /* will only enter this if the above if happened, so use of j is safe 
    technically the i condition implies the j condition, but let us be safe here */
  while(offset < length && i < MAX_INDIRECT_SECTOR && j < PTRS_PER_SECTOR)
  {
    allocate_sector(&blocks[j]);
    j++;
    i++;
    offset += BLOCK_SECTOR_SIZE;
  }

  if(second_while)
  {
    cache_unlock(indirect_block);
  }

  if(offset < length)
  {
    init_indirect_sector(&data->sectors[DIRECT_CNT+1]);
    dbl_indirect_block = cache_lock(data->sectors[DIRECT_CNT+1], EXCLUSIVE);
    ind_blocks = (block_sector_t *) cache_read(dbl_indirect_block);
    j = 0;
    third_while = true;
  }

  while(offset < length && i < MAX_DBL_INDIRECT_SECTOR && j < PTRS_PER_SECTOR)
  {
    init_indirect_sector(&ind_blocks[j]);
    indirect_block = cache_lock(ind_blocks[j], EXCLUSIVE);
    blocks = (block_sector_t *) cache_read(indirect_block);
    k = 0;
    while(offset < length && k < PTRS_PER_SECTOR)
    {
      allocate_sector(&blocks[j]);
      k++;
      i++;
      offset += BLOCK_SECTOR_SIZE;
    }
    cache_unlock(indirect_block);
    j++;
  }

  if(third_while)
  {
    cache_unlock(dbl_indirect_block);
  }

  data->length = offset;

  cache_unlock(block);
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

      if (chunk_size <= 0 || !get_data_block (inode, offset, true, &block))
        break;

      sector_data = cache_read (block);
      memcpy (sector_data + sector_ofs, buffer + bytes_written, chunk_size);
      cache_dirty (block);
      cache_unlock (block);

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

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  lock_acquire(&inode->deny_write_lock);
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  lock_release(&inode->deny_write_lock);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  lock_acquire(&inode->deny_write_lock);
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
  lock_release(&inode->deny_write_lock);
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  struct cache_block *block = cache_lock(inode->sector, NON_EXCLUSIVE);
  struct inode_disk *data = (struct inode_disk *) cache_read(block);
  off_t length = data->length;
  cache_unlock(block);
  return length;
}

/* Returns the number of openers. */
int
inode_open_cnt (const struct inode *inode) 
{
  int open_cnt;
  
  lock_acquire (&open_inodes_lock);
  open_cnt = inode->open_cnt;
  lock_release (&open_inodes_lock);
  return open_cnt;
}

/* Locks INODE. */
void
inode_lock (struct inode *inode) 
{
  lock_acquire (&inode->lock);
}

/* Releases INODE's lock. */
void
inode_unlock (struct inode *inode) 
{
  lock_release (&inode->lock);
}