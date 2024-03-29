#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

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
  	//printf ("%s: exit(%d)\n", cur->tid_name, exit_num);
  	thread_exit();
  }
  else
  {
  	printf("system call! number: %d\n", intr_num);
  	thread_exit ();
  }
}

static void write_handler(struct intr_frame *f)
{
	int *stack_ptr = (int *)(f->esp);
	int fd = *(stack_ptr+1);
	void *buf = (void *)(*(stack_ptr+2));
	int size = *(stack_ptr+3);
	if(fd == 1)
	{
		printf("writing\n");
		putbuf((char *)buf, size);
		return;
	}
}
