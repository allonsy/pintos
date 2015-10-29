#include "userprog/syscall.h"
#include "userprog/process.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include <string.h>
#include "devices/shutdown.h"

// static void syscall_handler (struct intr_frame *);
// static void write_handler (struct intr_frame *);

static void syscall_handler (struct intr_frame *f);

static int sys_exec (const char *ufile);
static bool sys_create (const char *ufile, unsigned initial_size);
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

/* System call handler. */
static void
syscall_handler (struct intr_frame *f)
{
  
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
      // int *stack_ptr= (int *)(f->esp);
      // int exit_num= *(stack_ptr+1);
      // struct thread *cur = thread_current();
      // //printf ("%s: exit(%d)\n", cur->tid_name, exit_num);
      // thread_exit();
      // break;
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
  memset (args, 0, sizeof args);
  copy_in (args, (uint32_t *) f->esp + 1, (sizeof *args) * arg_cnt);

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
sys_create (const char *ufile, unsigned initial_size)
{
  // ...; 
  // char *kfile = copy_in_string (ufile);
  // ...;
  // bool ok = filesys_create (kfile, initial_size);
  // ...;
  return false;
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


  char kernel_buf[size];
  memcpy(kernel_buf,usrc_, size);
  if(fd == 1)
  {
    putbuf(kernel_buf, size);
    return size;
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
  return -1;
}

static int 
sys_filesize (int fd)
{
  return 0;
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
  return;
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
    if (usrc >= (uint8_t *) PHYS_BASE || !get_user (dst, usrc))
      thread_exit ();
}



/* Creates a copy of user string US in kernel memory and returns it as a
   page that must be freed with palloc_free_page().  Truncates the string
   at PGSIZE bytes in size.  Call thread_exit() if any of the user accesses
   are invalid. */
static char *
copy_in_string (const char *us)
{
  char *ks = NULL;

  size_t length = strlen(us);

  ks = malloc(length)
  if (ks == NULL)
    thread_exit ();

  copy_in(ks, (void *) us, sizeof char * (length + 1));

  return ks;

  // don't forget to call palloc_free_page(..) when you're done
  // with this page, before you return to user from syscall
}

