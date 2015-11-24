#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include "userprog/gdt.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "userprog/syscall.h"
#include "vm/page.h"

/* Number of page faults processed. */
static long long page_fault_cnt;

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);

/* Registers handlers for interrupts that can be caused by user
   programs.

   In a real Unix-like OS, most of these interrupts would be
   passed along to the user process in the form of signals, as
   described in [SV-386] 3-24 and 3-25, but we don't implement
   signals.  Instead, we'll make them simply kill the user
   process.

   Page faults are an exception.  Here they are treated the same
   way as other exceptions, but this will need to change to
   implement virtual memory.

   Refer to [IA32-v3a] section 5.15 "Exception and Interrupt
   Reference" for a description of each of these exceptions. */
void
exception_init (void) 
{
  /* These exceptions can be raised explicitly by a user program,
     e.g. via the INT, INT3, INTO, and BOUND instructions.  Thus,
     we set DPL==3, meaning that user programs are allowed to
     invoke them via these instructions. */
  intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
  intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
  intr_register_int (5, 3, INTR_ON, kill,
                     "#BR BOUND Range Exceeded Exception");

  /* These exceptions have DPL==0, preventing user processes from
     invoking them via the INT instruction.  They can still be
     caused indirectly, e.g. #DE can be caused by dividing by
     0.  */
  intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
  intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
  intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
  intr_register_int (7, 0, INTR_ON, kill,
                     "#NM Device Not Available Exception");
  intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
  intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
  intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
  intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
  intr_register_int (19, 0, INTR_ON, kill,
                     "#XF SIMD Floating-Point Exception");

  /* Most exceptions can be handled with interrupts turned on.
     We need to disable interrupts for page faults because the
     fault address is stored in CR2 and needs to be preserved. */
  intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/* Prints exception statistics. */
void
exception_print_stats (void) 
{
  printf ("Exception: %lld page faults\n", page_fault_cnt);
}

//analog of sys_exit with hardcoded -1
void
except_exit ()
{
  int status = -1;
  struct thread *cur = thread_current();
  if(cur->parent)
  {
    lock_acquire(&cur->parent->child_list_lock);
    struct list_elem *e;
    for(e = list_begin(&cur->parent->children); e != list_end(&cur->parent->children); e = list_next(e))
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

/* Handler for an exception (probably) caused by a user process. */
static void
kill (struct intr_frame *f) 
{
  /* This interrupt is one (probably) caused by a user process.
     For example, the process might have tried to access unmapped
     virtual memory (a page fault).  For now, we simply kill the
     user process.  Later, we'll want to handle page faults in
     the kernel.  Real Unix-like operating systems pass most
     exceptions back to the process via signals, but we don't
     implement them. */
     
  /* The interrupt frame's code segment value tells us where the
     exception originated. */
  switch (f->cs)
    {
    case SEL_UCSEG:
      /* User's code segment, so it's a user exception, as we
         expected.  Kill the user process.  */
      printf ("%s: dying due to interrupt %#04x (%s).\n",
              thread_name (), f->vec_no, intr_name (f->vec_no));
      intr_dump_frame (f);
      except_exit(); 

    case SEL_KCSEG:
      /* Kernel's code segment, which indicates a kernel bug.
         Kernel code shouldn't throw exceptions.  (Page faults
         may cause kernel exceptions--but they shouldn't arrive
         here.)  Panic the kernel to make the point.  */
      intr_dump_frame (f);
      printf("%s: exit(%d)\n", thread_current()->name, -1);
      PANIC ("Kernel bug - unexpected interrupt in kernel"); 

    default:
      /* Some other code segment?  Shouldn't happen.  Panic the
         kernel. */
      printf ("Interrupt %#04x (%s) in unknown segment %04x\n",
             f->vec_no, intr_name (f->vec_no), f->cs);
      printf("%s: exit(%d)\n", thread_current()->name, -1);
      except_exit ();
    }
}

/* Page fault handler.  This is a skeleton that must be filled in
   to implement virtual memory.  Some solutions to project 2 may
   also require modifying this code.

   At entry, the address that faulted is in CR2 (Control Register
   2) and information about the fault, formatted as described in
   the PF_* macros in exception.h, is in F's error_code member.  The
   example code here shows how to parse that information.  You
   can find more information about both of these in the
   description of "Interrupt 14--Page Fault Exception (#PF)" in
   [IA32-v3a] section 5.15 "Exception and Interrupt Reference". */
static void
page_fault (struct intr_frame *f) 
{
  bool not_present;  /* True: not-present page, false: writing r/o page. */
  bool write;        /* True: access was write, false: access was read. */
  bool user;         /* True: access by user, false: access by kernel. */
  void *fault_addr;  /* Fault address. */

  /* Obtain faulting address, the virtual address that was
     accessed to cause the fault.  It may point to code or to
     data.  It is not necessarily the address of the instruction
     that caused the fault (that's f->eip).
     See [IA32-v2a] "MOV--Move to/from Control Registers" and
     [IA32-v3a] 5.15 "Interrupt 14--Page Fault Exception
     (#PF)". */
  asm ("movl %%cr2, %0" : "=r" (fault_addr));

  /* Turn interrupts back on (they were only off so that we could
     be assured of reading CR2 before it changed). */
  intr_enable ();

  /* Count page faults. */
  page_fault_cnt++;
  if(fault_addr == 0)
  {
    except_exit();
    return;
  }
  /* Determine cause. */
  not_present = (f->error_code & PF_P) == 0;
  write = (f->error_code & PF_W) != 0;
  user = (f->error_code & PF_U) != 0;

  if(not_present)
  {
    struct page *p2 = page_for_addr (fault_addr);
    if(p2 == NULL)
    {
      struct thread *t = thread_current();
      if(t->num_extensions > 2000)
      {
        except_exit();
      }
      void *stack_ptr_swizzle;
      if(is_kernel_vaddr(f->esp))
      {
        stack_ptr_swizzle = t->stack;
      }
      else
      {
        t->stack = f->esp;
        stack_ptr_swizzle = t->stack;
      }

      /* if(fault_addr >= stack_ptr_swizzle-32 || (page_for_addr(pg_round_down(fault_addr)-PGSIZE) && !is_kernel_vaddr(fault_addr))) */
      if(fault_addr >= stack_ptr_swizzle-32 && fault_addr < PHYS_BASE)
      {
        struct page *p = page_allocate(fault_addr, false, PAGET_STACK);
        p->file = NULL;
        p->read_only = false;
        if(p != NULL)
        {
          t->num_extensions++;
          if(!page_in_2(fault_addr))
            PANIC("page_fault: unable to page in successfully");
          return;
        }
      }
      //printf("page_fault: fault_addr %p\n", fault_addr);
      if(user)
      {
        //PANIC("page_fault: user faulting on %p with frame address %p\n", fault_addr, f);
        except_exit();
      }
      else
      {
        void *pg_above_addr = pg_round_down(fault_addr) + PGSIZE;
        struct page *p3 = page_for_addr (pg_above_addr);

        if(p3 == NULL)
        {
          printf("page_fault: hitting except exit\n");
          except_exit();
        } else {
          struct page *p = page_allocate(fault_addr, false, PAGET_STACK);
          p->file = NULL;
          p->read_only = false;
          if(p != NULL)
          {
            t->num_extensions++;
            if(!page_in_2(fault_addr))
              PANIC("page_fault: unable to page in successfully");
            return;
          }
        }


        PANIC("page_fault: kernel faulting on %p with stack_ptr_swizzle: %p current_thread->stack: %p f->esp: %p\n", fault_addr, stack_ptr_swizzle, t->stack, f->esp);
      }
    }
    if(is_user_vaddr(fault_addr) && !page_in_2(fault_addr))
    {
      //PANIC("page_fault: page_in_2 error");
      except_exit ();
    }
    return;
  }
  else /* writing to read only memory should exit every thread */
  {
    //PANIC("page_fault: not_present is false");
    except_exit();
  }
}

