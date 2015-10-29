#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include <string.h>

static void syscall_handler (struct intr_frame *);
static void write_handler (struct intr_frame *);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f) 
{
  uint32_t intr_num = *(int *)(f->esp);
  if(intr_num ==SYS_WRITE)
  {
  	write_handler(f);
  }
  else if(intr_num==SYS_EXIT)
  {
  	int *stack_ptr= (int *)(f->esp);
  	int exit_num= *(stack_ptr+1);
  	struct thread *cur = thread_current();
  	printf ("%s: exit(%d)\n", cur->name,exit_num);
  	thread_exit();
  }
  else
  {
    case SYS_WRITE:
      write_handler(f);
      break;
    case SYS_EXIT:
      int *stack_ptr= (int *)(f->esp);
      int exit_num= *(stack_ptr+1);
      struct thread *cur = thread_current();
      //printf ("%s: exit(%d)\n", cur->tid_name, exit_num);
      thread_exit();
      break;
    case SYS_HALT:
      shutdown_power_off();
      break;
    case SYS_CREATE:

    case SYS_REMOVE:

    case SYS_OPEN:

    case SYS_READ:

    case SYS_SEEK:

    case SYS_TELL:

    case SYS_CLOSE:

    case SYS_EXEC:

    case SYS_WAIT:

    default:
      printf("system call! number: %d\n", intr_num);
      thread_exit ();
      break;
  }
}

static void write_handler(struct intr_frame *f)
{
	int *stack_ptr = (int *)(f->esp);
  int size = *(stack_ptr+3);
	int fd = *(stack_ptr+1);
  char kernel_buf[size];
	void *buf = (void *)(*(stack_ptr+2));
  memcpy(kernel_buf,buf, size);
	if(fd == 1)
	{
		putbuf(kernel_buf, size);
		return;
	}
}

static void create_handler(struct intr_frame *f)
{

  filesys_create (const char *name, off_t initial_size);
}
