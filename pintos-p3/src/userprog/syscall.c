#include "userprog/syscall.h"
#include "userprog/process.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include <string.h>
#include "devices/shutdown.h"
#include "devices/input.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/synch.h"
#include "vm/page.h"

static void syscall_handler (struct intr_frame *f);

static int sys_exec (const char *ufile);
static bool sys_create (char *ufile, unsigned initial_size);
static int sys_write (int fd, void *usrc_, unsigned size);
//static void sys_halt (void);
static void sys_exit (int status);
static int sys_wait (int pid);
static bool sys_remove (const char *file);
static int sys_open (const char *file);
static int sys_filesize (int fd);
static int sys_read (int fd, void *buffer, unsigned length);
static void sys_seek (int fd, unsigned position);
static unsigned sys_tell (int fd);
static void sys_close (int fd);
static int sys_mmap (int handle, void *addr);
static int sys_munmap (int mapping);

static inline bool get_user (uint8_t *dst, const uint8_t *usrc);
static inline bool put_user (uint8_t *udst, uint8_t byte);
static void copy_in (void *dst_, const void *usrc_, size_t size);
static char *copy_in_string (const char *us);




void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

int
is_valid_uptr(void *ptr)
{
  struct thread *t = thread_current();
  if(ptr != NULL)
  {
    if(is_user_vaddr(ptr))
    {
      void *page_number = page_for_addr(ptr);
      if(page_number !=NULL)
      {
        return 1;
      }
      else
      {
        if(ptr >= t->stack-32 && t->num_extensions <= 2000)
        {
          uint32_t *newPageAddr = pg_round_down(ptr);
          struct page *p = page_allocate(ptr, 0);
          p->file = NULL;
          p->read_only = 0;
          if(p != NULL)
          {
            t->num_extensions++;
            page_in(ptr);
            return 1;
          }
        }
      }
    }
  }
  sys_exit (-1);
  return 0;
}

/* System call handler. */
static void
syscall_handler (struct intr_frame *f)
{
  if(!is_valid_uptr(f->esp))
  {
    return;
  }
  thread_current()->stack = f->esp;
  int args[3];
  uint32_t call_nr;
  int arg_cnt = 0;

  copy_in (&call_nr, f->esp, sizeof call_nr);

  // set arg_cnt based on which system call we are handling
  // then set the arguments as necessary
  switch (call_nr)
  {
    /* 0-arg sys call is just halt */
    case SYS_HALT:
      //if halt is called we don't need any fancy bs, 
      // just turn off the machine
      shutdown_power_off();
      break; //strictly speaking not necessary   
    
    /* 1-arg sys calls */
    case SYS_REMOVE:
    case SYS_OPEN:
    case SYS_TELL:
    case SYS_CLOSE:
    case SYS_EXEC:
    case SYS_WAIT:
    case SYS_FILESIZE:
    case SYS_EXIT:
    case SYS_MUNMAP:
      arg_cnt = 1;
      break;

    /* 2-arg sys calls */  
    case SYS_CREATE:
    case SYS_SEEK:
    case SYS_MMAP:
      arg_cnt = 2;
      break;

    /* 3-arg sys calls */  
    case SYS_WRITE:
    case SYS_READ:
      arg_cnt = 3;
      break;
    default:
      // shouldn't reach here
      printf("system call! number: %d\n", call_nr);
      thread_exit ();
      break;
  }

  // copy the args (depends on arg_cnt for every syscall).
  // note that if the arg passed is a pointer (e.g. a string),
  // then we just copy the pointer here, and you still need to
  // call 'copy_in_string' on the pointer to pass the string
  // from user space to kernel space
  memset (args, 0, sizeof args);;
  copy_in (args, ((uint32_t *) f->esp) + 1, (sizeof *args) * arg_cnt);

  // now that args holds the correct arguments, call the functions
  // and sets f->eax to the return value for syscalls that return values
  switch (call_nr)
  {
    /* 0-arg sys call is just halt */
    case SYS_HALT:
      // should not happen since we should already be off
      shutdown_power_off();
      break;  
    
    /* 1-arg sys calls */
    case SYS_REMOVE:
      f->eax = (uint32_t) sys_remove((const char*) args[0]);
      break;

    case SYS_OPEN:
      f->eax = (uint32_t) sys_open((const char *) args[0]);
      break;

    case SYS_TELL:
      f->eax = (uint32_t) sys_tell((int) args[0]);
      break;

    case SYS_CLOSE:
      sys_close((int) args[0]);
      break;

    case SYS_EXEC:
      f->eax = (uint32_t) sys_exec((const char*) args[0]);
      break;

    case SYS_WAIT:
      f->eax = (uint32_t) sys_wait((int) args[0]);
      break;

    case SYS_FILESIZE:
      f->eax = (uint32_t) sys_filesize((int) args[0]);
      break;
    case SYS_EXIT:
      sys_exit((int) args[0]);
      break;
    case SYS_MUNMAP:
      f->eax = sys_munmap((int) args[0]);
      break;

    /* 2-arg sys calls */  
    case SYS_CREATE:
      f->eax = (uint32_t) sys_create((const char *) args[0], (unsigned) args[1]);
      break;
    case SYS_SEEK:
      sys_seek((int) args[0], (unsigned) args[1]);
      break;
    case SYS_MMAP:
      f->eax = sys_mmap((int) args[0], (void*) args[1]);
      break;

    /* 3-arg sys calls */  
    case SYS_WRITE:
      f->eax = (uint32_t) sys_write((int) args[0], (void*) args[1], (unsigned) args[2]);
      break;
    case SYS_READ:
      f->eax = sys_read((int) args[0], (void *) args[1], (unsigned) args[2]);
      break;
    default:
      // shouldn't reach here
      printf("system call! number: %d\n", call_nr);
      thread_exit ();
      break;
  }

  return;
}


/* Binds a mapping id to a region of memory and a file. */
struct mapping
{
  struct list_elem elem;      /* List element. */
  int handle;                 /* Mapping id. */
  struct file *file;          /* File. */
  uint8_t *base;              /* Start of memory mapping. */
  size_t page_cnt;            /* Number of pages mapped. */
};


static struct mapping *lookup_mapping (int handle) 
{
  struct thread *t = thread_current ();
  struct mapping *m = NULL;
  struct list_elem *e;

  lock_acquire(&t->map_lock);

  for(e=list_begin(&t->maps); e != list_end(&t->maps); e= list_next(e))
  {
    struct mapping *map = list_entry(e, struct mapping, elem);
    if(map->handle == handle)
    {
      m = map;
      break;
    }
  }
  lock_release(&t->map_lock);
  return m;
}




static int
sys_mmap (int handle, void *addr)
{
  /* can't mmap stdin, or to non-page aligned memory, or if addr == 0 cuz PINTOS */
  if(handle <= 2 || addr == NULL /* || addr isn't page aligned */)
  {
    return -1;
  }

  struct thread *t = thread_current ();
  struct fdesc *fds = NULL;
  struct list_elem *e;

  for(e=list_begin(&t->files); e != list_end(&t->files); e = list_next(e))
  {
    fds = list_entry(e, struct fdesc, elem);
    if(fds->fd == handle)
      break;
  }

  if(fds == NULL || fds->fd != handle)
    return -1;

  struct file *rfile = file_reopen(fds->fptr);

  if(file_length(rfile) == 0)
  {
    file_close(rfile);
    return -1;
  }

  /* more shit to do here */





  return -1;
}

/* Remove mapping M from the virtual address space,                              
   writing back any pages that have changed. */
static int
sys_munmap (int mapping)
{
  return 0;
}








/* Exec system call. */
static int
sys_exec (const char *ufile)
{
  char *kfile = copy_in_string (ufile);
  tid_t pid = process_execute (kfile);
  free(kfile);
  if(pid == TID_ERROR)
    return -1;
  else
  {
    struct thread *cur = thread_current();
    sema_down(&cur->exec_wait_sema);
    struct list_elem *e;
    lock_acquire(&cur->child_list_lock);
    for(e=list_begin(&cur->children); e != list_end(&cur->children); e= list_next(e))
    {
      struct child *chld = list_entry(e, struct child, elem);
      if(chld->pid == pid)
      {
        if(chld->exec_status != NULL)
        {
          pid = *chld->exec_status;
        }
      }
    }
    lock_release(&cur->child_list_lock);
    return (int) pid;
  }
}

/* Create system call. */
static bool
sys_create (char *file_name, unsigned initial_size)
{
  char *kfile = copy_in_string (file_name);
  lock_acquire(&filesys_lock);
  bool ret = filesys_create(kfile, initial_size);
  lock_release(&filesys_lock);
  free(kfile);
  return ret;
}



/* Write system call. */
static int
sys_write (int fd, void *usrc_, unsigned size)
{
  is_valid_uptr(usrc_);
  char *kernel_buf = malloc(size);
  memcpy(kernel_buf,usrc_, size);
  if(fd == STDOUT_FILENO)
  {
    putbuf(kernel_buf, size);
    free(kernel_buf);
    return size;
  }
  else
  {
    struct thread *t = thread_current();
    struct list_elem *e;
    struct fdesc *fds = NULL;
    for(e=list_begin(&t->files); e != list_end(&t->files); e = list_next(e))
    {
      fds = list_entry(e, struct fdesc, elem);
      if(fds->fd == fd)
      {
        if(fds->deny_write)
        {
          return 0;
        }
        lock_acquire(&filesys_lock);
        int written = file_write(fds->fptr, kernel_buf, size);
        lock_release(&filesys_lock);
        free(kernel_buf);
        return written;
      }
    }
  }
  return -1;
}

static void 
sys_exit (int status)
{
  struct thread *cur = thread_current();
  if(cur->parent)
  {

    lock_acquire(&cur->parent->child_list_lock);
    struct list_elem *e;
    for(e = list_begin(&cur->parent->children); e != list_end(&cur->parent->children); e= list_next(e))
    {
      struct child *chld = list_entry(e, struct child, elem);
      if(chld->pid == cur->tid)
      {
        chld->status= malloc(sizeof(int));
        *chld->status = status;
        sema_up(&chld->wait_sema);
      }
    }
    lock_release(&cur->parent->child_list_lock);
  }
  
  printf ("%s: exit(%d)\n", cur->name, status);
  thread_exit();
}

static int 
sys_wait (int pid)
{
  return process_wait(pid);
}


static bool 
sys_remove (const char *file)
{
  char *kfile = copy_in_string (file);
  lock_acquire(&filesys_lock);
  bool ret = filesys_remove(kfile);
  lock_release(&filesys_lock);
  free(kfile);
  return ret;
}

static int 
sys_open (const char *file)
{
  char *kfile = copy_in_string (file);

  if(strlen(kfile) == 0)
    return -1;

  struct thread *t = thread_current ();

  lock_acquire(&filesys_lock);
  struct file *fptr = filesys_open(kfile);
  lock_release(&filesys_lock);

  if(fptr == NULL)
    return -1;

  struct fdesc *fds = malloc(sizeof (struct fdesc) );
  fds->fptr = fptr;
  
  if(strcmp(kfile, t->name) == 0)
  {
    fds->deny_write = 1;
  }
  else
  {
    fds->deny_write = 0;
  }

  if(list_empty(&t->files))
  {
    fds->fd = 3;
  }
  else
  {
    fds->fd = list_entry(list_back(&t->files), struct fdesc, elem)->fd + 1;
  }

  list_push_back(&t->files, &fds->elem);

  free(kfile);
  return fds->fd;
}

static int 
sys_filesize (int fd)
{
  struct thread *t = thread_current ();
  struct file *file = NULL;
  int size;
  struct list_elem *e;

  for(e = list_begin(&t->files); e != list_end(&t->files); e = list_next(e))
  {
    struct fdesc *fds = list_entry(e, struct fdesc, elem);
    if(fds->fd == fd)
    {
      file = fds->fptr;
      break;
    }
  }
  
  if(file == NULL)
  {
    return -1;
  }
  else
  {
    lock_acquire(&filesys_lock);
    size = file_length(file);
    lock_release(&filesys_lock);
    return size;
  } 
}

static int 
sys_read (int fd, void *buffer, unsigned length)
{
  is_valid_uptr(buffer);
  is_valid_uptr(buffer+length-1);
  char *kbuf = malloc(length);
  int readbytes = 0;
  if(fd == STDIN_FILENO)
  {
    while(readbytes < length)
    {
      kbuf[readbytes] = input_getc();
      readbytes++;
    }
    memcpy(buffer, kbuf, readbytes);
    free(kbuf);
    return readbytes;
  }
  else
  {
    struct thread *t = thread_current();
    struct list_elem *e;
    struct fdesc *fds = NULL;
    for(e=list_begin(&t->files); e != list_end(&t->files); e = list_next(e))
    {
      fds = list_entry(e, struct fdesc, elem);
      if(fds->fd == fd)
      {
        lock_acquire(&filesys_lock);
        readbytes = file_read(fds->fptr, kbuf, length);
        lock_release(&filesys_lock);
        memcpy(buffer, kbuf, readbytes);
        free(kbuf);
        return readbytes;
      }
    }
  }
  return -1;
}

static void 
sys_seek (int fd, unsigned position)
{
  struct thread *t = thread_current();
  struct list_elem *e;

  for(e=list_begin(&t->files); e != list_end(&t->files); e = list_next(e))
  {
    struct fdesc *fds = list_entry(e, struct fdesc, elem);
    if(fds->fd == fd)
    {
      file_seek(fds->fptr, position);
    }
  }
  return;
}

static unsigned int
sys_tell (int fd)
{
  struct thread *t = thread_current();
  struct list_elem *e;

  for(e=list_begin(&t->files); e != list_end(&t->files); e = list_next(e))
  {
    struct fdesc *fds = list_entry(e, struct fdesc, elem);
    if(fds->fd == fd)
    {
      return file_tell(fds->fptr);
    }
  }
  return -1;
}

static void
sys_close (int fd)
{
  struct thread *t = thread_current();
  struct list_elem *e;
  for(e= list_begin(&t->files); e != list_end(&t->files); e = list_next(e))
  {
    struct fdesc *fds = list_entry(e, struct fdesc, elem);
    if(fds->fd == fd)
    {
      list_remove(e);
      free(fds);
      return;
    }
  }
}



/* Copies a byte from user address USRC to kernel address DST.  USRC must
   be below PHYS_BASE.  Returns true if successful, false if a segfault
   occurred. Unlike the one posted on the p2 website, thsi one takes two
   arguments: dst, and usrc */

static inline bool
get_user (uint8_t *dst, const uint8_t *usrc)
{
  int eax;
  asm ("movl $1f, %%eax; movb %2, %%al; movb %%al, %0; 1:"
       : "=m" (*dst), "=&a" (eax) : "m" (*usrc));
  return eax != 0;
}


/* Writes BYTE to user address UDST.  UDST must be below PHYS_BASE.
   Returns true if successful, false if a segfault occurred. */

static inline bool
put_user (uint8_t *udst, uint8_t byte)
{
  int eax;
  asm ("movl $1f, %%eax; movb %b2, %0; 1:"
       : "=m" (*udst), "=&a" (eax) : "q" (byte));
  return eax != 0;
}


/* Copies SIZE bytes from user address USRC to kernel address DST.  Call
   thread_exit() if any of the user accesses are invalid. */ 

static void copy_in (void *dst_, const void *usrc_, size_t size) { 

  uint8_t *dst = dst_; 
  const uint8_t *usrc = usrc_;
  
  for (; size > 0; size--, dst++, usrc++)
  {
    is_valid_uptr(usrc);
    get_user (dst, usrc);
  }
}



/* Creates a copy of user string US in kernel memory and returns it as a
   page that must be freed with palloc_free_page().  Truncates the string
   at PGSIZE bytes in size.  Call thread_exit() if any of the user accesses
   are invalid. */
static char *
copy_in_string (const char *us)
{
  is_valid_uptr(us);
  char *ks = NULL;

  size_t length = strlen(us);

  ks = malloc(length+1);
  if (ks == NULL)
    thread_exit ();

  copy_in(ks, (void *) us, length + 1);

  return ks;
}

