#ifndef FILESYS_FILESYS_H
#define FILESYS_FILESYS_H

#include <stdio.h>
#include "filesys/off_t.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include <stdbool.h>

/* Sectors of system file inodes. */
#define FREE_MAP_SECTOR 0       /* Free map file inode sector. */
#define ROOT_DIR_SECTOR 1       /* Root directory file inode sector. */

/* Block device that contains the file system. */
struct block *fs_device;

void filesys_init (bool format);
void filesys_done (void);
bool filesys_create (const char *name, off_t initial_size, enum inode_type);
struct file *filesys_open (const char *name);
bool filesys_remove (const char *name);
bool resolve_name_to_entry (const char *name, struct dir **dirp, char base_name[NAME_MAX+1]);
bool readdir_by_inode(struct inode *inode, char *dst);
bool filesys_hasinit();

#endif /* filesys/filesys.h */
