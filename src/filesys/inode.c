#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/cache.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44
#define INVALID_SECTOR ((block_sector_t) -1)

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t start;               /* First data sector. */
    off_t length;                       /* File size in bytes. */
    enum inode_type type;                     /* Magic number. */
    unsigned magic;                     /* Magic number. */
    uint32_t unused[124];               /* Not used. */
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
    off_t length;
    block_sector_t start;

    /* Denying writes. */
    struct lock deny_write_lock;        /* Protects members below. */
    struct condition no_writers_cond;   /* Signaled when no writers. */ 
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    int writer_cnt;                     /* Number of writers. */
  };
/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);
  if (pos < inode->length)
    return inode->start + pos / BLOCK_SECTOR_SIZE;
  else
  {
    //printf("byte_to_sector: failed. pos %d length %d\n", pos, inode->length);
    return -1;
  }
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Controls access to open_inodes list. */
static struct lock open_inodes_lock;


bool is_directory(struct inode *in)
{
  struct cache_block *cb = cache_lock(in->sector, NON_EXCLUSIVE);
  struct inode_disk *idisk = (struct inode_disk *)cache_read(cb);
  cache_unlock(cb, NON_EXCLUSIVE);
  if(idisk->type == DIR_INODE)
  {
    return true;
  }
  else
  {
    return false;
  }
}


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

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, enum inode_type type)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
  {
    size_t sectors = bytes_to_sectors (length);
    disk_inode->length = length;
    disk_inode->type = type;
    disk_inode->magic = INODE_MAGIC;
    if (free_map_allocate (sectors, &disk_inode->start)) 
      {
        block_write (fs_device, sector, disk_inode);
        if (sectors > 0) 
          {
            static char zeros[BLOCK_SECTOR_SIZE];
            size_t i;
            
            for (i = 0; i < sectors; i++) 
              block_write (fs_device, disk_inode->start + i, zeros);
          }
        success = true; 
      } 
    free (disk_inode);
  }
  return success;
}

bool create_dir(char *name)
{
  struct dir *parent;
  char base[NAME_MAX+1];
  int ret = resolve_name_to_entry(name, &parent, base);
  if(ret ==false)
  {
    if(parent == NULL)
    {
      return false;
    }
    else
    {
      block_sector_t sector;
      int ret = free_map_allocate(1, &sector);
      //synch primitives?
      if(ret == false)
      {
        return false;
      }
      struct inode *child = dir_create(sector, dir_get_inode(parent)->sector);
      if(child == NULL)
      {
        return false;
      }
      else
      {
        ret = dir_add(parent, name, sector);
        return ret;
      }
    }
  }
  return false;
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

  lock_list();
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes); e = list_next (e)) 
  {
    inode = list_entry (e, struct inode, elem);
    if (inode->sector == sector) 
      {
        inode_reopen (inode);
        unlock_list();
        return inode; 
      }
  }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
  {
    unlock_list();
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
  struct cache_block *b = cache_lock(inode->sector, EXCLUSIVE);
  struct inode_disk *data = (struct inode_disk*) cache_read(b);
  inode->length = data->length;
  inode->start = data->start;
  cache_unlock(b, EXCLUSIVE);

  //printf("inode_open: sector %d length %d start %d ptr %p\n", sector, inode->length, inode->start, inode);
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
    inode->open_cnt++;
  return inode;
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
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);

      struct cache_block *b = cache_lock(inode->sector, EXCLUSIVE);
      struct inode_disk *data = (struct inode_disk *) cache_read(b);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
      {
        free_map_release (inode->sector, 1);
        free_map_release (data->start, bytes_to_sectors (data->length)); 
      }

      cache_unlock(b, EXCLUSIVE);

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  //uint8_t *bounce = NULL;
  struct cache_block *b;
  char *data;

  while (size > 0) 
  {
    /* Disk sector to read, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector (inode, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length (inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually copy out of this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
    {
      //printf("loop break\n");
      break;
    }

    if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
    {
      //printf("in sectos 1\n");
      /* Read full sector directly into caller's buffer. */
      b = cache_lock(sector_idx, EXCLUSIVE);
      data = (char*) cache_read(b);
      memcpy(buffer + bytes_read, data, BLOCK_SECTOR_SIZE);
      //cache_dirty(b);
      cache_unlock(b, EXCLUSIVE);
      //printf("end sectos 1\n");
    }
    else 
      {
        //printf("sectos 2\n");
        /* Read sector into bounce buffer, then partially copy
           into caller's buffer. */
        b = cache_lock(sector_idx, EXCLUSIVE);
        data = (char*) cache_read(b);
        memcpy (buffer + bytes_read, data + sector_ofs, chunk_size);
        cache_unlock(b, EXCLUSIVE);
        //printf("ern\n");
      }
    
    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_read += chunk_size;
  }
  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;
  char *data;
  struct cache_block *b;

  //printf("inode ptr: %p sector: %d\n", inode, inode->sector, inode->length);

  ASSERT(inode != NULL);
  off_t length = inode_length (inode);

  //printf("inode_write_at: sector %u length %d\n", inode->sector, length);
  // if (inode->deny_write_cnt)
  //   return 0;

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = length - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
      {
        //printf("inode_write_at: chunk_size <= 0, bytes_written %d\n", bytes_written);
        break;
      }

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Write full sector directly to disk. */
          b = cache_lock(sector_idx, EXCLUSIVE);
          data = (char*) cache_read(b);
          memcpy(data, buffer + bytes_written, BLOCK_SECTOR_SIZE);
          cache_dirty(b);
          cache_unlock(b, EXCLUSIVE);
        }
      else 
        {
          /* We need a bounce buffer. */
          b = cache_lock(sector_idx, EXCLUSIVE);
          data = (char*) cache_read(b);
          memcpy(data + sector_ofs, buffer + bytes_written, chunk_size);
          cache_dirty(b);
          cache_unlock(b, EXCLUSIVE);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  free (bounce);

  return bytes_written;
}

void inode_flush(struct inode *in)
{
  struct cache_block *cb=cache_lock(in->start, NON_EXCLUSIVE);
  cache_inode_flush(cb);
  cache_unlock(cb, NON_EXCLUSIVE);
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  // struct cache_block *b = cache_lock(inode->sector, EXCLUSIVE);
  // struct inode_disk *data = (struct inode_disk*) cache_read(b);
  // off_t length = data->length;
  // cache_unlock(b, EXCLUSIVE);
  return inode->length;
}

/*FILE CHANGED TO SUPPORT FILE EXTENSION:



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


#define MAX_INDIRECT_SECTOR (DIRECT_CNT + PTRS_PER_SECTOR * INDIRECT_CNT)
#define MAX_DBL_INDIRECT_SECTOR (DIRECT_CNT + PTRS_PER_SECTOR * INDIRECT_CNT + PTRS_PER_SECTOR * PTRS_PER_SECTOR * DBL_INDIRECT_CNT)

#define DEBUG_VAR_INODE 0

static void
dprint(const char *str, bool exitr)
{
  if(DEBUG_VAR_INODE)
  {
    printf("%s: %s\n", str, exitr ? "exiting" : "entered");
  }
}


struct inode_disk
  {
    block_sector_t sectors[SECTOR_CNT]; 
    enum inode_type type;               
    off_t length;                       
    unsigned magic;                    
  };


static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

struct inode 
  {
    struct list_elem elem;              
    block_sector_t sector;              
    int open_cnt;                       
    bool removed;                       
    struct lock lock;                   


    struct lock deny_write_lock;        
    struct condition no_writers_cond;   
    int deny_write_cnt;                 
    int writer_cnt;                     
  };


static struct list open_inodes;


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


void
inode_init (void) 
{
  list_init (&open_inodes);
  lock_init (&open_inodes_lock);
}


bool
inode_create (block_sector_t sector, off_t size, enum inode_type type) 
{
  
  if(DEBUG_VAR_INODE)
  {
    //printf("break\n");
  }
  struct cache_block *block = cache_lock (sector, EXCLUSIVE);
  if(DEBUG_VAR_INODE)
  {
    //printf("break\n");
  }
  if(block == NULL)
  {
    cache_unlock(block, EXCLUSIVE);
    return false;
  }

  struct inode_disk *disk_inode = (struct inode_disk *) cache_read(block);

  disk_inode->length = size;
  if(DEBUG_VAR_INODE)
  {
    //printf("creating with: %d for sector %d\n", disk_inode->length, sector);
  }

  disk_inode->magic = INODE_MAGIC;
  disk_inode->type = type;

  int i;
  for(i = 0; i < SECTOR_CNT; i++)
  {
    disk_inode->sectors[i] = INVALID_SECTOR;
  }
  allocate_sector(&disk_inode->sectors[0], 0);
  cache_write(block);
  cache_dirty(block);
  cache_unlock(block, EXCLUSIVE);

  return true;
}


struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  dprint("inode_open", 0);

  

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

  
  inode = malloc (sizeof *inode);
  if (inode == NULL)
  {
    unlock_list();
    dprint("inode_open", 1);
    return NULL;
  }

  
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
  

  dprint("inode_open", 1);
  return inode;
}


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


block_sector_t
inode_get_inumber (const struct inode *inode)
{
  dprint("inode_get_inumber", 0);
  dprint("inode_get_inumber", 1);
  return inode->sector;
}


void
inode_close (struct inode *inode) 
{
  dprint("inode_close", 0);
 
  if (inode == NULL)
  {
    dprint("inode_close", 1);
    return;
  }

  lock_list();
  inode_lock(inode);
  if (--inode->open_cnt == 0)
  {
    
    list_remove (&inode->elem);


    if (inode->removed) 
    {
      deallocate_inode(inode);
    }

    free (inode); 
    unlock_list();
    dprint("inode_close", 1);
    return; 
  }
  inode_unlock(inode);
  unlock_list();
  dprint("inode_close", 1);
}



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
   
    case 1: 
    case 2: 
      sectors = (block_sector_t *) block->data;
      for(i = 0; i < PTRS_PER_SECTOR; i++)
      {
   
        if(sectors[i] != INVALID_SECTOR)
        {
          deallocate_recursive(sectors[i], level - 1);
        }
      }
      break;
    default:
      PANIC("deallocate_recursive: INVALID SECTOR LEVEL %d", level);
  }
  memset(block->data, 0, BLOCK_SECTOR_SIZE);
  cache_dirty(block);

  free_map_release (sector, 1);
  cache_unlock(block, EXCLUSIVE); 
}


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
 
  for(i = DIRECT_CNT; i < DIRECT_CNT + INDIRECT_CNT; i++)
  {
    deallocate_recursive(data->sectors[i], 1);
  }

  for(i = DIRECT_CNT + INDIRECT_CNT; i < SECTOR_CNT; i++)
  {
    deallocate_recursive(data->sectors[i], 2);
  }

  memset(data, 0, BLOCK_SECTOR_SIZE);
  free_map_release (inode->sector, 1);
  cache_unlock(block, EXCLUSIVE);
  dprint("deallocate_inode", 1);

}


void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}


static void
calculate_indices (off_t sector_idx, size_t offsets[], size_t *offset_cnt)
{

  dprint("calculate_indices", 0);
  
  ASSERT(sector_idx < MAX_DBL_INDIRECT_SECTOR);


  if(sector_idx < DIRECT_CNT)
  {
    *offset_cnt = 1;
    offsets[0] = sector_idx;
    dprint("calculate_indices", 1);
    return;
  }

  if (sector_idx < MAX_INDIRECT_SECTOR)
  {
    *offset_cnt = 2;
    off_t ind_idx = sector_idx - DIRECT_CNT;
    offsets[0] = DIRECT_CNT + (ind_idx / PTRS_PER_SECTOR);
    offsets[1] = ind_idx % PTRS_PER_SECTOR;
    dprint("calculate_indices", 1);
    return;
  }

  if(sector_idx < MAX_DBL_INDIRECT_SECTOR)
  {
    
    *offset_cnt = 3;
    offsets[0] = DIRECT_CNT + INDIRECT_CNT;
    off_t dbl_ind_idx = sector_idx - (DIRECT_CNT + INDIRECT_CNT * PTRS_PER_SECTOR);
    offsets[1] = dbl_ind_idx / PTRS_PER_SECTOR;
    offsets[2] = dbl_ind_idx % PTRS_PER_SECTOR;
    dprint("calculate_indices", 1);
    return;
  }
}


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
    cache_write(block);
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
  struct inode_disk *temp;

 
  dprint("get_data_block", 0);
  calculate_indices(logical_sector, offsets, &offset_cnt);

  ASSERT(1 <= offset_cnt && offset_cnt <= 3);

  for(i = 0; i < offset_cnt; i++)
  {
    ASSERT(0 <= offsets[i]);
    ASSERT(offsets[i] < (i == 0 ? DIRECT_CNT : PTRS_PER_SECTOR));
  } 



  

  // if(DEBUG_VAR_INODE)
  // {
  //   printf("get_data_block: after calculate_indices, offset_cnt is %d\n", offset_cnt);
  // }

  for(i = 0; i < offset_cnt; i++)
  {
    block = cache_lock(cur_sector, EXCLUSIVE);
    cur_off = offsets[i];
    if(block != NULL)
    {
      if(i == 0)
      {
        temp = (struct inode_disk *) cache_read(block);
        data = temp->sectors;
      }
      else
      {
        data = (block_sector_t *) cache_read(block);
      }
      if(DEBUG_VAR_INODE)
      {
        printf("get_data_block: cur_sector: %u and data[cur_off] %u allocate: %s iter: %u\n", 
            cur_sector, data[cur_off], allocate ? "TRUE" : "FALSE", i);
      }
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
        PANIC("here is a clue\n");
        cache_unlock(block, EXCLUSIVE);
        *data_block = NULL;
        if(DEBUG_VAR_INODE)
        {
          printf("get_data_block: missing block with allocate false on loop inter %d\n", i);
        }
        return true;
      }
      cache_unlock(block, EXCLUSIVE);
    }
    else
    {
      PANIC("get_data_block: cache_lock returned NULL on sector %u. Sector was invalid: %d", 
          cur_sector, cur_sector == INVALID_SECTOR);
      return false;
    }
  } 

  *data_block = cache_lock(cur_sector, EXCLUSIVE);
  dprint("get_data_block", 1);

  return true;
}


off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  bool excl;
  dprint("inode_read_at", 0);

  while (size > 0) 
  {

    int sector_ofs = offset % BLOCK_SECTOR_SIZE;
    struct cache_block *block;

   
    off_t inode_left = inode_length (inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

  
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0 || !get_data_block (inode, offset, false, &block, &excl))
    {
      if(DEBUG_VAR_INODE)
      {
        //printf("pointer to block is: %p\n", block);
      }
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
      

      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }

  dprint("inode_read_at", 1);
  return bytes_read;
}



static void
extend_file (struct inode *inode, off_t length) 
{
  dprint("extend_file", 0);
  
  ASSERT(length <= INODE_SPAN);

  struct cache_block *block;
  off_t offset = 0; 

  
  while(offset < length)
  {
    get_data_block (inode, offset, true, &block, NULL);
    cache_unlock(block, EXCLUSIVE);
    offset += BLOCK_SECTOR_SIZE;
  }
}


off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  bool excl;

  dprint("inode_write_at", 0);


  lock_acquire (&inode->deny_write_lock);
  while(inode->deny_write_cnt) 
  {
    cond_wait (&inode->no_writers_cond, &inode->deny_write_lock);
  }
  inode->writer_cnt++;
  lock_release (&inode->deny_write_lock);

  while (size > 0) 
    {
  
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;
      struct cache_block *block;
      uint8_t *sector_data;

      
      off_t inode_left = INODE_SPAN - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

     
      int chunk_size = size < min_left ? size : min_left;

      if (chunk_size <= 0 || !get_data_block (inode, offset, true, &block, &excl))
        break;

      sector_data = cache_read (block);
      memcpy (sector_data + sector_ofs, buffer + bytes_written, chunk_size);
      cache_write(block);
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


off_t
inode_length (const struct inode *inode)
{
  dprint("inode_length", 0);
  if(DEBUG_VAR_INODE)
  {
    //printf("pass read\n");
  }
  struct cache_block *block = cache_lock(inode->sector, NON_EXCLUSIVE);
  struct inode_disk *data = (struct inode_disk *) cache_read(block);
  off_t length = data->length;
  cache_unlock(block, NON_EXCLUSIVE);
  dprint("inode_length", 1);
  if(DEBUG_VAR_INODE)
  {
    //printf("returning for sector: %d of %d\n", inode->sector, length);
  }
  return length;
}


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


void
inode_lock (struct inode *inode) 
{
  dprint("inode_lock", 0);
  lock_acquire (&inode->lock);
  dprint("inode_lock", 1);
}


void
inode_unlock (struct inode *inode) 
{
  dprint("inode_unlock", 0);
  lock_release (&inode->lock);
  dprint("inode_unlock", 1);
}
*/