#include "userprog/syscall.h"
#include "userprog/process.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include <string.h>
#include "devices/shutdown.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"

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
      void *page_number = pagedir_get_page(t->pagedir, ptr);
      if(page_number !=NULL)
      {
        return 1;
      }
    }
  }
  printf ("%s: exit(%d)\n", t->name, -1);
  thread_exit ();
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
      arg_cnt = 1;
      break;

    /* 2-arg sys calls */  
    case SYS_CREATE:
    case SYS_SEEK:
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

    /* 2-arg sys calls */  
    case SYS_CREATE:
      f->eax = (uint32_t) sys_create((const char *) args[0], (unsigned) args[1]);
      break;
    case SYS_SEEK:
      sys_seek((int) args[0], (unsigned) args[1]);
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



/* Exec system call. */
static int
sys_exec (const char *ufile)
{
  return -1;
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
  // ...;
  // struct file_descriptor *fd = lookup_fd(handle);
  // int sizeToWrite = size;

  // while (sizeToWrite > 0) {

  //   ...;

  //   if (handle == STDOUT_FILENO)
  //     {
  // putbuf (usrc, write_amount);
  // retval = write_amt;
  //     }
  //   else
  //     {
  // retval = file_write (fd->file, usrc, write_amount);
  //     }

  //   ...;

  //   sizeToWrite -= retval;
    
  // }

  // ...;
  is_valid_uptr(usrc_);
  char *kernel_buf=malloc(size);
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
        lock_acquire(&filesys_lock);
        int written = file_write(fds->fptr, kernel_buf, size);
        lock_release(&filesys_lock);
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
  printf ("%s: exit(%d)\n", cur->name, status);
  thread_exit();
}

static int 
sys_wait (int pid)
{
  return -1;
}


static bool 
sys_remove (const char *file)
{

  return false;
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
  return 0;
}

static void 
sys_seek (int fd, unsigned position)
{
  return;
}

static unsigned 
sys_tell (int fd)
{
  return;
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

  // don't forget to call palloc_free_page(..) when you're done
  // with this page, before you return to user from syscall
}

