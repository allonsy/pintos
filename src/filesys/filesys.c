#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "threads/thread.h"


/* Partition that contains the file system. */
struct block *fs_device;

static bool hasinit = false;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();
  cache_init ();

  if (format) 
    do_format ();

  free_map_open ();
  thread_current()->current_dir=dir_open_root();
  hasinit = true;
}

bool filesys_hasinit()
{
  return hasinit;
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */

/* Extracts a file name part from *SRCP into PART,
   and updates *SRCP so that the next call will return the next
   file name part.
   Returns 1 if successful, 0 at end of string, -1 for a too-long
   file name part. */
static int
get_next_part (char part[NAME_MAX], const char **srcp)
{
  const char *src = *srcp;
  char *dst = part;

  /* Skip leading slashes.
     If it's all slashes, we're done. */
  while (*src == '/')
    src++;
  if (*src == '\0')
    return 0;

  /* Copy up to NAME_MAX character from SRC to DST.
     Add null terminator. */
  while (*src != '/' && *src != '\0') 
    {
      if (dst < part + NAME_MAX)
        *dst++ = *src;
      else
        return -1;
      src++; 
    }
  *dst = '\0';

  /* Advance source pointer. */
  *srcp = src;
  return 1;
}

/* Resolves relative or absolute file NAME.
   Returns true if successful, false on failure.
   Stores the directory corresponding to the name into *DIRP,
   and the file name part into BASE_NAME. */
bool
resolve_name_to_entry (const char *name,
                       struct dir **dirp, char base_name[NAME_MAX + 1]) 
{
  struct dir *start;
  if(name[0]!='/')
  {
    start = thread_current()->current_dir;
  }
  else
  {
    start = dir_open_root();
  }
  char string_part[NAME_MAX+1];

  int ret = get_next_part(string_part, &name);
  while(ret == 1)
  {
    struct inode *hop;
    int success = dir_lookup(start, string_part, &hop);
    if(success == false)
    {
      *dirp=start;
      return false;
    }
    if(is_directory(hop))
    {
      start = dir_open(hop);
      //possibly close parent here;
    }
    else
    {
      ret = get_next_part(string_part, &name);
      if(ret == 0)
      {
        memcpy(base_name, string_part, NAME_MAX+1);
        *dirp = start;
        return true;
      }
    }
  }
  if(ret == -1)
  {
    *dirp = NULL;
    return false;
  }
  if(ret == 0)
  {
    memcpy(base_name, string_part, NAME_MAX+1);
    *dirp = start;
    return true;
  }
  *dirp = NULL;
  return false;
}

/* Resolves relative or absolute file NAME to an inode.
   Returns an inode if successful, or a null pointer on failure.
   The caller is responsible for closing the returned inode. */
struct inode *
resolve_name_to_inode (const char *name)
{
  struct dir *parent_dir;
  char base[NAME_MAX+1];
  struct inode *file_node;
  bool ret = resolve_name_to_entry(name, &parent_dir, base);
  if(ret == false)
  {
    return NULL;
  }
  else
  {
    dir_lookup(parent_dir, base, &file_node);
    return file_node;
  }
}

//given a file path of only dirs, returns the dir* of the last directory
//returns false if the last part isn't a directory
bool
get_directory_from_name(const char *name, struct dir **dirp)
{
  char base[NAME_MAX +1];
  struct dir *parent;
  bool ret = resolve_name_to_entry(name, &parent, base);
  if(ret == false)
  {
    return false;
  }
  struct inode *in;
  ret = dir_lookup(parent, name, &in);
  if(ret == true)
  {
    if(is_directory(in))
    {
      *dirp = dir_open(in);
    }
    else
    {
      return false;
    }
  }
  return false;
}

bool
readdir_by_inode(struct inode *inode, char *dst)
{
  struct dir *dirp;
  if(is_directory(inode))
  {
    dirp = dir_open(inode);
    return dir_readdir(dirp, dst);
  }
  else
  {
    return false;
  }
}

bool change_directory(char *name)
{
  struct dir *d;
  bool ret = get_directory_from_name(name, &d);
  if(ret == true)
  {
    thread_current()->current_dir = dir_get_inode(d);
    return true;
  }
  return false;
}

bool
filesys_create (const char *name, off_t initial_size, enum inode_type type) 
{
  block_sector_t inode_sector = 0;
  struct dir *dir = dir_open_root ();
  bool success = dir != NULL;
  if(!success)
    PANIC("filesys_create: dir is null");
  if(!(success &= free_map_allocate (1, &inode_sector)))
    PANIC("filesys_create: free_map_allocate failed");

  if(!(success &= inode_create (inode_sector, initial_size, type)))
    PANIC("filesys_create: inode_create failed");

  if(!(success &= dir_add (dir, name, inode_sector)))
    PANIC("filesys_create: dir_add failed");


  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  /*struct dir *dir = dir_open_root ();
  struct inode *inode = NULL;

  if (dir != NULL)
    dir_lookup (dir, name, &inode);
  dir_close (dir);
  */
  struct inode *in = resolve_name_to_inode(name);
  if(in == NULL)
  {
    return NULL;
  }

  return file_open (in);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  struct dir *d;
  char base[NAME_MAX + 1];
  bool success = resolve_name_to_entry(name, &d, base);
  if(success == false)
  {
    return false;
  }
  success = d != NULL && dir_remove (d, name);
  dir_close (d); 

  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, -1))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}
