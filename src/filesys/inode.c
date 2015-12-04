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
    unsigned magic;                     /* Magic number. */
    uint32_t unused[125];               /* Not used. */
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

  struct cache_block *b = cache_lock(inode->sector, EXCLUSIVE);
  struct inode_disk *data = (struct inode_disk *) cache_read(b);
  cache_unlock(b, EXCLUSIVE);

  ASSERT (inode != NULL);
  if (pos < data->length)
    return data->start + pos / BLOCK_SECTOR_SIZE;
  else
    return -1;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Controls access to open_inodes list. */
static struct lock open_inodes_lock;


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
inode_create (block_sector_t sector, off_t length)
{
  struct inode_disk *disk_inode = malloc(BLOCK_SECTOR_SIZE);
  bool success = false;
  void *data;

  //printf("inode_create: sector %d length %d\n", sector, length);

  ASSERT (length >= 0);

  size_t sectors = bytes_to_sectors (length);

  ASSERT(sizeof *disk_inode == BLOCK_SECTOR_SIZE);

 /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */

  struct cache_block *block = cache_lock (sector, EXCLUSIVE);
  if(block == NULL)
  {
    return false;
  }

  data = cache_read(block);

  disk_inode->length = length;
  disk_inode->magic = INODE_MAGIC;

  if (free_map_allocate (sectors, &disk_inode->start)) 
  {
    success = true;
  }

  //printf("inode_create: before memcpy\n");
  if(success)
    memcpy(data, disk_inode, BLOCK_SECTOR_SIZE);
  //disk_inode->type = type;
  //printf("inode_create: after memcpy\n");

  cache_dirty(block);
  cache_unlock(block, EXCLUSIVE);

  return success;
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
  cache_unlock(b, EXCLUSIVE);

  //printf("inode_open: sector %d length %d ptr %p\n", sector, inode->length, inode);
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
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Read full sector directly into caller's buffer. */
          b = cache_lock(sector_idx, EXCLUSIVE);
          data = (char*) cache_read(b);
          memcpy(buffer + bytes_read, data, BLOCK_SECTOR_SIZE);
          //cache_dirty(b);
          cache_unlock(b, EXCLUSIVE);
        }
      else 
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          b = cache_lock(sector_idx, EXCLUSIVE);
          data = (char*) cache_read(b);
          memcpy (buffer + bytes_read, data + sector_ofs, chunk_size);
          cache_unlock(b, EXCLUSIVE);
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

  //printf("inode ptr: %p sector: %d\n", inode, inode->sector);

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