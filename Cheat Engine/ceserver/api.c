/*
 * api.c
 *
 *  Created on: Jul 20, 2011
 *      Author: erich
 *
 *      This unit will implement the api's CE uses
 *      This will be the main point of interest when porting to another system
 */

//#define _XOPEN_SOURCE 500

//todo for in the far future: Hook syscalls


#define TRACEPTRACE

#define _FILE_OFFSET_BITS 64
#define _LARGEFILE64_SOURCE

#include <stdio.h>
#include <pthread.h>

#include <sys/mman.h>


#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#include <stdint.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>

#include <strings.h>

#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <signal.h>

#ifndef __x86_64__
//#include <asm/signal.h>
#endif


#include <sys/eventfd.h>

#include <errno.h>

#include <semaphore.h>
#include <sys/queue.h>
#include <limits.h>

#include <sys/ptrace.h>

//#ifndef __x86_64__
#include <linux/elf.h>
//#include <linux/uio.h>
//#endif


#ifdef __arm__
  #ifndef __ANDROID__
    #include <linux/user.h>
  #endif
#endif


#include <dlfcn.h>

//blatantly stolen from the kernel source
#define PTRACE_GETHBPREGS 29
#define PTRACE_SETHBPREGS 30

/* Breakpoint */
#define ARM_BREAKPOINT_EXECUTE  0

/* Watchpoints */
#define ARM_BREAKPOINT_LOAD     1
#define ARM_BREAKPOINT_STORE    2

/* Privilege Levels */
#define ARM_BREAKPOINT_PRIV     1
#define ARM_BREAKPOINT_USER     2

/* Lengths */
#define ARM_BREAKPOINT_LEN_1    0x1
#define ARM_BREAKPOINT_LEN_2    0x3
#define ARM_BREAKPOINT_LEN_4    0xf
#define ARM_BREAKPOINT_LEN_8    0xff

static inline unsigned int encode_ctrl_reg(int mismatch, int len, int type, int privilege, int enabled)
{
        return (mismatch << 22) | (len << 5) | (type << 3) | (privilege << 1) | enabled;
}

#ifndef __ANDROID__
  #if defined(__i386__) || defined(__x86_64__)
    #include <sys/user.h>
  #endif
#endif



#include "api.h"
#include "porthelp.h"
#include "ceserver.h"
#include "threads.h"
#include "symbols.h"
#include "context.h"


PROCESS_VM_WRITEV process_vm_writev=NULL;
PROCESS_VM_READV process_vm_readv=NULL;

//#include <vector>
sem_t sem_DebugThreadEvent;

pthread_mutex_t memorymutex;
pthread_mutex_t debugsocketmutex;
//pthread_mutex_t mut_RPM;

typedef struct
{
  int ReferenceCount;
  int processListIterator;
  int processCount;
  PProcessListEntry processList;
} ProcessList, *PProcessList;

typedef struct
{
  int ReferenceCount;
  int moduleListIterator;
  int moduleCount;
  PModuleListEntry moduleList;
} ModuleList, *PModuleList;

int VerboseLevel=0;

int MEMORY_SEARCH_OPTION = 2;
int ATTACH_PID = 0;
int ATTACH_TO_ACCESS_MEMORY = 0;
int ATTACH_TO_WRITE_MEMORY = 0;
unsigned char SPECIFIED_ARCH = 9;

//Implementation for shared library version ceserver.
int debug_log(const char * format , ...)
{
  va_list list;
  va_start(list,format);
  int ret = vprintf(format,list);

  #ifdef __ANDROID__
    LOGD(format,list);
  #endif
  va_end(list);
  return ret;
}

#ifdef TRACEPTRACE
char *PTraceToString(int request)
{
  switch (request)
  {
    case PTRACE_ATTACH: return "PTRACE_ATTACH";
    case PTRACE_DETACH: return "PTRACE_DETACH";
    case PTRACE_PEEKDATA: return "PTRACE_PEEKDATA";
    case PTRACE_POKEDATA: return "PTRACE_POKEDATA";
    case PTRACE_CONT: return "PTRACE_CONT";
    case PTRACE_GETSIGINFO: return "PTRACE_GETSIGINFO";
    default:
      return "";

  }
}
#endif

//Implementation for consistency with Android Studio.
uintptr_t safe_ptrace(int request, pid_t pid, void * addr, void * data)
{
#ifdef TRACEPTRACE

  debug_log("ATTACH_TO_ACCESS_MEMORY=%d\n", ATTACH_TO_ACCESS_MEMORY);
  if (threadname)
    debug_log("%s: ptrace called (%s(%x), %d, %p, %p)\n",threadname, PTraceToString(request),request, pid, addr, data);
  else
    debug_log("ptrace called (%s(%x), %d, %p, %p)\n",PTraceToString(request),request, pid, addr, data);
#endif
  uintptr_t result;
  errno = 0;
  result = ptrace(request, pid, addr, data);
  if(errno != 0)
  {
    debug_log("ptrace error(%s (%d))!\n",strerror(errno), errno);
  }
  return result;
}

int WakeDebuggerThread()
{
  return sem_post(&sem_DebugThreadEvent);
}

void mychildhandler(int signal, struct siginfo *info, void *context)
{
  //only call re-entrant functions

  int orig_errno = errno;
  WakeDebuggerThread();
  errno = orig_errno;
}

int getArchitecture(HANDLE hProcess)
{
  if (GetHandleType(hProcess) == htProcesHandle )
  {
    PProcessData p=(PProcessData)GetPointerFromHandle(hProcess);
    if (p->is64bit)
#if defined (__arm__) || defined(__aarch64__)
      return 3;
    else
      return 2;
#else
      return 1;
    else
      return 0;
#endif
  }

  return -1;
}

int GetDebugPort(HANDLE hProcess)
//return the debugserver fd
{
  if (GetHandleType(hProcess) == htProcesHandle )
  {
    PProcessData p=(PProcessData)GetPointerFromHandle(hProcess);
    if (p->isDebugged)
    {
      return p->debuggerServer;
    }
  }

  return -1;

}

int FindPausedThread(PProcessData p)
/*
 * Scan the threadlist and return the threadid of the thread that is paused. Return 0 if no thread is paused
 */
{
  int i;
  for (i=0; i<p->threadlistpos; i++)
    if (p->threadlist[i].isPaused)
      return p->threadlist[i].tid;

  return 0;
}

int getBreakpointCapabilities(int tid, uint8_t *maxBreakpointCount, uint8_t *maxWatchpointCount, uint8_t *maxSharedBreakpoints)
//Only called when the thread is suspended
{

  *maxBreakpointCount=0;
  *maxWatchpointCount=0;
  *maxSharedBreakpoints=0;


#ifdef __arm__
  HBP_RESOURCE_INFO hwbpcap;

  memset(&hwbpcap, 0, sizeof(HBP_RESOURCE_INFO));
  if (safe_ptrace(PTRACE_GETHBPREGS, tid, 0, &hwbpcap)==0)
  {
    debug_log("hwbpcap:\n");
    debug_log("debug architecture:                %d\n", hwbpcap.debug_arch);
    debug_log("number of instruction breakpoints: %d\n", hwbpcap.num_brps);
    debug_log("number of data breakpoints:        %d\n", hwbpcap.num_wrps);
    debug_log("max length of a data breakpoint:   %d\n", hwbpcap.wp_len);

    *maxBreakpointCount=hwbpcap.num_brps;
    *maxWatchpointCount=hwbpcap.num_wrps;
    *maxSharedBreakpoints=0;

    return 1;
  }
  else
    return 0;
#endif

#ifdef __aarch64__
  struct iovec iov;
  struct user_hwdebug_state hwd;
  memset(&hwd, 0, sizeof(hwd));

  iov.iov_base=&hwd;
  iov.iov_len=sizeof(hwd);

  if (safe_ptrace(PTRACE_GETREGSET, tid, (void*)NT_ARM_HW_WATCH, &iov)==0)
  {
    debug_log("NT_ARM_HW_WATCH: dbg_info=%x:\n", hwd.dbg_info);
    *maxWatchpointCount=hwd.dbg_info & 0xf;
  }
  else
  {
    debug_log("NT_ARM_HW_WATCH: Failure getting watch breakpoint information\n");
    *maxWatchpointCount=0;
  }

  iov.iov_base=&hwd;
  iov.iov_len=sizeof(hwd);
  if (safe_ptrace(PTRACE_GETREGSET, tid, (void*)NT_ARM_HW_BREAK, &iov)==0)
  {
    debug_log("NT_ARM_HW_BREAK: dbg_info=%x:\n", hwd.dbg_info);
    *maxBreakpointCount=hwd.dbg_info & 0xf;
  }
  else
  {
    debug_log("NT_ARM_HW_BREAK: Failure getting breakpoint information\n");
    *maxBreakpointCount=0;
  }

  return 1;
#endif

#if defined(__i386__) || defined(__x86_64__)
  *maxBreakpointCount=0;
  *maxWatchpointCount=0;
  *maxSharedBreakpoints=4;
  return 1;
#endif
}

int StartDebug(HANDLE hProcess)
{
  if (GetHandleType(hProcess) == htProcesHandle )
  {
    PProcessData p=(PProcessData)GetPointerFromHandle(hProcess);

    struct sigaction childactionhandler;
    if (p->isDebugged)
    {
      debug_log("Trying to start debugging a process that is already debugged\n");
      return FALSE;
    }

    //attach to each task
    //first get a threadlist
    InitializeProcessThreadlist(p);

    //setup an event
    memset(&childactionhandler, 0, sizeof(childactionhandler));
    childactionhandler.sa_handler=(void *)mychildhandler;
    childactionhandler.sa_flags=SA_SIGINFO;



    sigaction(SIGCHLD, &childactionhandler, NULL);




    //read the taskid's from /proc/pid/task
    char _taskdir[255];
    DIR *taskdir;

    sprintf(_taskdir, "/proc/%d/task", p->pid);

    taskdir=opendir(_taskdir);

    if (taskdir)
    {
      struct dirent *d;

      d=readdir(taskdir);
      while (d)
      {
        int tid=atoi(d->d_name);

        if (tid)
        {
          ThreadData td;
          memset(&td, 0, sizeof(td));
          td.tid=tid;
          td.isPaused=0;
          AddThreadToProcess(p, &td);

          pthread_mutex_lock(&memorymutex); //so there's no ptrace_attach busy when attaching after opening and reading memory


          if (safe_ptrace(PTRACE_ATTACH, tid,0,0)<0)
            debug_log("Failed to attach to thread %d\n", tid);
          else
          {
            DebugEvent createThreadEvent;



            if (p->isDebugged==0)
            {
              p->isDebugged=1; //at least one made it...
              p->debuggedThreadEvent.threadid=0; //none yet
              p->debuggerThreadID=pthread_self();

              socketpair(PF_LOCAL, SOCK_STREAM, 0, &p->debuggerServer);

              //first event, create process
              DebugEvent createProcessEvent;

#if defined(__arm__) || defined (__aarch64__)

              if (WaitForDebugEventNative(p, &createProcessEvent, tid, -1))
              {
                //get the debug capabilities

                getBreakpointCapabilities(tid, &createProcessEvent.maxBreakpointCount, &createProcessEvent.maxWatchpointCount, &createProcessEvent.maxSharedBreakpoints);

                safe_ptrace(PTRACE_CONT, createProcessEvent.threadid, 0,0);

                PThreadData _td=GetThreadData(p, createProcessEvent.threadid);

                if (_td)
                  _td->isPaused=0;
                else
                  debug_log("Invalid first debug thread\n");
              }
              else
              {
                debug_log("Failure waiting for create event");
                createProcessEvent.maxBreakpointCount=0;
                createProcessEvent.maxWatchpointCount=0;
                createProcessEvent.maxSharedBreakpoints=4;
              }
#endif

#if defined(__i386__) || defined(__x86_64__)
              //4 breakpoints, hybrid
              createProcessEvent.maxBreakpointCount=0;
              createProcessEvent.maxWatchpointCount=0;
              createProcessEvent.maxSharedBreakpoints=4;
#endif

              createProcessEvent.debugevent=-2; //create process
              createProcessEvent.threadid=p->pid;

              AddDebugEventToQueue(p, &createProcessEvent);

            }

            createThreadEvent.debugevent=-1; //create thread event (virtual event)
            createThreadEvent.threadid=tid;
            AddDebugEventToQueue(p, &createThreadEvent);


           // p->debuggerThreadID=syscall(SYS_gettid);
          }

          pthread_mutex_unlock(&memorymutex);

        }

        d=readdir(taskdir);
      }

      closedir(taskdir);

    }
    else
    {
      debug_log("Failure opening %s",_taskdir);
    }


    return p->isDebugged;

  }
  else
  {
    //printf("Invalid handle\n");
    return FALSE;
  }

}

int SetBreakpoint(HANDLE hProcess, int tid, int debugreg, void *address, int bptype, int bpsize)
/*
 * Sets a breakpoint of the specifed type at the given address
 * tid of -1 means ALL threads
 * bptype: 0 = Execute
 * bptype: 1 = write
 * bptype: 2 = read (only)
 * bptype: 3 = Read/Write
 *
 * returns TRUE if the breakpoint was set *
 */
{
  int result=FALSE;


  debug_log("SetBreakpoint(%d, %d, %d, %p, %d, %d)\n", hProcess, tid, debugreg, address, bptype, bpsize);
  if (GetHandleType(hProcess) == htProcesHandle )
  {
    PProcessData p=(PProcessData)GetPointerFromHandle(hProcess);



    if (p->debuggerThreadID==pthread_self())
    {

      int isdebugged=FindPausedThread(p);
      int wtid;
      DebugEvent de;



      debug_log("SetBreakpoint from debuggerthread\n");

      if (tid==-1)
      {
        int i,r;
        debug_log("Calling SetBreakpoint for all threads\n");

        for (i=0; i<p->threadlistpos; i++)
        {
          r=SetBreakpoint(hProcess, p->threadlist[i].tid, debugreg, address, bptype, bpsize);
          if (r)
            result=TRUE; //at least one thread succeeded
        }

      }
      else
      {
        PThreadData td=GetThreadData(p, tid);
        int wasPaused;

        if (td==NULL) //should NEVER happen
          return FALSE;

        wasPaused=td->isPaused;

        debug_log("Calling setbreakpoint for thread :%d\n", tid);

        debug_log("isdebugged=%d\n", isdebugged);

        if (wasPaused==0)
        {
          //manual
          debug_log("Target thread wasn't stopped yet\n");

          int k=0;

          debug_log("td=%p\n", td);
          debug_log("td->isPaused=%d\n", td->isPaused);




          wtid=tid;
          while ((td) && (td->isPaused==0) && (k<10))
          {
            debug_log("Not yet paused\n");
            syscall(__NR_tkill, tid, SIGSTOP);

            if (WaitForDebugEventNative(p, &de, tid, 100))
            {
              wtid=de.threadid;
              break;
            }
            k++;
          }



          if (wtid!=tid)
          {
            debug_log("<<<================UNEXPECTED TID (wtid=%d tid=%d)================>>>\n", wtid, tid);
          }

          debug_log("k=%d (number of tries)\n", k);

          if (k==10)
          {
            debug_log("Timeout when waiting for thread\n");
          }
        }
        else
        {
          debug_log("The thread I wanted to break was already broken. Yeeeh\n");
          wtid=isdebugged;
        }

        //debugging the given tid
        debug_log("Setting breakpoint in thread %d\n", wtid);
#ifdef __aarch64__
        struct user_pt_regs regset;

        struct iovec iov;
        int maxWatchCount=0;
        int maxBreakCount=0;

        debug_log("aarch64\n");




        memset(&regset, 0, sizeof(regset));
        memset(&iov, 0, sizeof(iov));
        iov.iov_base=&regset;
        iov.iov_len=sizeof(regset);
        int i=safe_ptrace(PTRACE_GETREGSET, wtid, (void*)NT_PRSTATUS, &iov);

        debug_log("iov.iov_len=%d\n", (int)iov.iov_len);  //272=64 bit app. 72=32 bit app
        debug_log("PTRACE_GETREGSET returned %d\n", i);

        debug_log("pc=%lx\n", regset.pc);

        if (iov.iov_len==72)
        {
          debug_log("This is a 32 bit target. Most likely debugging will fail\n");
        }

        debug_log("r0=%llx\n", regset.regs[0]);
        debug_log("r1=%llx\n", regset.regs[1]);
        debug_log("r2=%llx\n", regset.regs[2]);
        debug_log("r3=%llx\n", regset.regs[3]);




        struct user_hwdebug_state hwd;
        memset(&hwd, 0, sizeof(hwd));


        iov.iov_base=&hwd;
        iov.iov_len=sizeof(hwd);
        i=safe_ptrace(PTRACE_GETREGSET, wtid, (void*)NT_ARM_HW_WATCH, &iov);

        if (i==0)
          maxWatchCount=hwd.dbg_info & 0xf;

        debug_log("iov.iov_len=%d\n", (int)iov.iov_len);  //272=64 bit app. 72=32 bit app
        debug_log("PTRACE_GETREGSET for NT_ARM_HW_WATCH returned %d %d\n", i,errno);

        debug_log("hwd.dbg_info=%x\n", hwd.dbg_info);
        debug_log("hwd.dbg_regs[0].addr=%llx\n", hwd.dbg_regs[0].addr);
        debug_log("hwd.dbg_regs[0].ctrl=%x\n", hwd.dbg_regs[0].ctrl);

        iov.iov_base=&hwd;
        iov.iov_len=sizeof(hwd);
        i=safe_ptrace(PTRACE_GETREGSET, wtid, (void*)NT_ARM_HW_BREAK, &iov);
        if (i==0)
          maxBreakCount=hwd.dbg_info & 0xf;

        debug_log("PTRACE_GETREGSET for NT_ARM_HW_BREAK returned %d %d\n", i,errno);



        debug_log("iov.iov_len=%d\n", (int)iov.iov_len);  //272=64 bit app. 72=32 bit app
        debug_log("i=%d (%d)\n", i,errno);

        debug_log("hwd.dbg_info=%x\n", hwd.dbg_info);
        debug_log("hwd.dbg_regs[0].addr=%llx\n", hwd.dbg_regs[0].addr);
        debug_log("hwd.dbg_regs[0].ctrl=%x\n", hwd.dbg_regs[0].ctrl);



        int btype=0;
        int bplist=NT_ARM_HW_BREAK;

        if (bptype==0)
        {
          //execute bp
          bplist=NT_ARM_HW_BREAK;

          btype=ARM_BREAKPOINT_EXECUTE;
        }
        else
        {
          //watchpoint
          bplist=NT_ARM_HW_WATCH;
          if (bptype==1)
            btype=ARM_BREAKPOINT_STORE;
          else
          if (bptype==2)
            btype=ARM_BREAKPOINT_LOAD;
          else
          if (bptype==3)
            btype=ARM_BREAKPOINT_STORE | ARM_BREAKPOINT_LOAD;

        }


        debug_log("Caling PTRACE_GETREGSET for bplist %d\n", bplist);
        debug_log("iov.iov_len=%d\n", iov.iov_len);
        i=safe_ptrace(PTRACE_GETREGSET, wtid, (void*)(size_t)bplist, &iov);

        debug_log("PTRACE_GETREGSET returned %d\n", i);

        hwd.dbg_regs[debugreg].addr=(uintptr_t)address;
        hwd.dbg_regs[debugreg].ctrl=encode_ctrl_reg(0, ARM_BREAKPOINT_LEN_4, btype, 0, 1);
        //iov.iov_len=8+16*(debugreg+1); //sizeof(hwd);

        if (bplist==NT_ARM_HW_BREAK)
          iov.iov_len=8+16*maxBreakCount;
        else
          iov.iov_len=8+16*maxWatchCount;

        debug_log("iov.iov_len=%d\n", iov.iov_len);
        debug_log("hwd.dbg_regs[0].addr=%llx\n", hwd.dbg_regs[0].addr);
        debug_log("hwd.dbg_regs[0].ctrl=%x\n", hwd.dbg_regs[0].ctrl);


        debug_log("Caling PTRACE_SETREGSET\n", bplist);


        i=safe_ptrace(PTRACE_SETREGSET, wtid, (void*)(size_t)bplist, &iov);

        debug_log("set=%d",i);
        if (i==-1)
        {
          debug_log(" Error=%s",strerror(errno));

        }
        else
          result=TRUE; //success at least once

        debug_log("\n");

        memset(&hwd, 0, sizeof(hwd));

        i=safe_ptrace(PTRACE_GETREGSET, wtid, (void*)NT_ARM_HW_WATCH, &iov);

       // debug_log("get: iov.iov_len=%d\n", (int)iov.iov_len);  //272=64 bit app. 72=32 bit app
       // debug_log("i=%d\n", i);

        debug_log("hwd.dbg_info=%x\n", hwd.dbg_info);
        debug_log("hwd.dbg_regs[0].addr=%llx\n", hwd.dbg_regs[0].addr);
        debug_log("hwd.dbg_regs[0].ctrl=%x\n", hwd.dbg_regs[0].ctrl);

#endif

#ifdef __arm__
    //hwbps
        unsigned long long val;
        int bpindex=1+(2*debugreg);



        debug_log("PTRACE_GETHBPREGS=%d\n",PTRACE_GETHBPREGS);


        val=0;

        if (safe_ptrace(PTRACE_GETHBPREGS, wtid, 0, &val)==0)
        {
          int i;
          unsigned int hwbpreg;
          debug_log("BPREG0 (Info)=%x\n", val);

          debug_log("Setting bp address\n");



          if (bptype==0)
          {
            //execute
            void *rv=NULL;
           // safe_ptrace(PTRACE_SETHBPREGS, wtid, bpindex, &rv);
           // safe_ptrace(PTRACE_SETHBPREGS, wtid, bpindex+1, &rv);


            i=safe_ptrace(PTRACE_GETHBPREGS, wtid, bpindex, &rv);
            debug_log("%d: Before: %d=%p\n", i, bpindex, rv);

            i=safe_ptrace(PTRACE_SETHBPREGS, wtid, bpindex, &address);
            debug_log("i1=%d\n", i, hwbpreg);

            i=safe_ptrace(PTRACE_GETHBPREGS, wtid, bpindex, &rv);
            debug_log("%d: After: %d=%p\n", i, bpindex, rv);


            //right now i'm not really sure how the breakpoint len is set and why it works in some cases and why not in other cases
            result=i==0;

            hwbpreg=encode_ctrl_reg(0, ARM_BREAKPOINT_LEN_4, ARM_BREAKPOINT_EXECUTE, 2, 1);
            if (safe_ptrace(PTRACE_SETHBPREGS, wtid, bpindex+1, &hwbpreg)<0) //according to my guess, this should usually work, but just in case...
            {
              debug_log("f1\n");
              hwbpreg=encode_ctrl_reg(0, ARM_BREAKPOINT_LEN_2, ARM_BREAKPOINT_EXECUTE, 2, 1);
              if (safe_ptrace(PTRACE_SETHBPREGS, wtid, bpindex+1, &hwbpreg)<0)
              {
                debug_log("f2\n");
                hwbpreg=encode_ctrl_reg(0, ARM_BREAKPOINT_LEN_1, ARM_BREAKPOINT_EXECUTE, 2, 1);
                if (safe_ptrace(PTRACE_SETHBPREGS, wtid, bpindex+1, &hwbpreg)<0)
                {
                  debug_log("f3\n");
                  //last try, 8 ?
                  hwbpreg=encode_ctrl_reg(0, ARM_BREAKPOINT_LEN_8, ARM_BREAKPOINT_EXECUTE, 2, 1);
                  if (safe_ptrace(PTRACE_SETHBPREGS, wtid, bpindex+1, &hwbpreg)<0)
                  {
                    debug_log("Failure to set breakpoint\n");
                    result=FALSE;
                  }
                }

              }
            }

            debug_log("bpindex=%d bpindex+1=%d\n", bpindex, bpindex+1);

            debug_log("hwbpreg=%x\n", hwbpreg);

            i=safe_ptrace(PTRACE_GETHBPREGS, wtid, bpindex+1, &hwbpreg);
            debug_log("after=%x\n", hwbpreg);

          }
          else
          {
            //watchpoint
            //(negative)
            int btype;


            debug_log("watchpoint\n");

            i=safe_ptrace(PTRACE_SETHBPREGS, wtid, -bpindex, &address);
            debug_log("i1=%d\n", i, hwbpreg);

            btype=0;
            if (bptype==1)
              btype=ARM_BREAKPOINT_STORE;
            else
            if (bptype==2)
              btype=ARM_BREAKPOINT_LOAD;
            else
            if (bptype==3)
              btype=ARM_BREAKPOINT_STORE | ARM_BREAKPOINT_LOAD;

            hwbpreg=encode_ctrl_reg(0, ARM_BREAKPOINT_LEN_4, btype, 0, 1);
            i=safe_ptrace(PTRACE_SETHBPREGS, wtid, -(bpindex+1), &hwbpreg);

            debug_log("-bpindex=%d -(bpindex+1)=%d\n", -bpindex, -(bpindex+1));
          //  debug_log("i=%d  (hwbpreg=%x)\n", i, hwbpreg);
            result=i==0;

          }

        }
        else
          debug_log("Failure getting the debug capability register for thread %d (%d)\n", wtid, errno);


#endif

#if defined(__i386__) || defined(__x86_64__)
        //debug regs
        //PTRACE_SETREGS
        int r,r2;

        uintptr_t newdr7=safe_ptrace(PTRACE_PEEKUSER, wtid, offsetof(struct user, u_debugreg[7]), 0);


        newdr7=newdr7 | (1<<debugreg*2);

        if (bptype==2) //x86 does not support read onlyhw bps
          bptype=3;

        newdr7=newdr7 | (bptype << (16+(debugreg*4))); //bptype

        debug_log("Setting DR7 to %x\n", newdr7);

        //bplen
        if (bpsize<=1)
          newdr7=newdr7 | (0 << (18+(debugreg*4)));
        else
        if (bpsize<=2)
          newdr7=newdr7 | (1 << (18+(debugreg*4)));
        else
          newdr7=newdr7 | (3 << (18+(debugreg*4)));


        r=safe_ptrace(PTRACE_POKEUSER, wtid, offsetof(struct user, u_debugreg[debugreg]), address);
        r2=safe_ptrace(PTRACE_POKEUSER, wtid, offsetof(struct user, u_debugreg[7]), newdr7);

        result=(r==0) && (r2==0);
        if (!result)
        {
          debug_log("Failure setting breakpoint\n");
        }

        debug_log("result=%d  (r=%d r2=%d)\n", result, r, r2);


#endif

        //store this breakpoint in the list


        if (wasPaused==0)
        {
          int r;

          debug_log("Continue self broken thread\n");

          if (de.debugevent!=SIGSTOP) //in case a breakpoint or something else happened before sigstop happened
          {


            debug_log("Not a SIGSTOP. Adding to queue and leave suspended\n");
            AddDebugEventToQueue(p, &de);
            td->isPaused=1; //mark as paused for other api's
          }
          else
          {

            r=safe_ptrace(PTRACE_CONT, wtid, 0,0);
            debug_log("PTRACE_CONT=%d\n", r);

            td->isPaused=0;
          }
        }
      }

    //

      debug_log("end of SetBreakpoint reached. result=%d\n", result);


    }
    else
    {
      //not the debugger thread. Send a message to the debuggerthread to execute this command
      debug_log("SetBreakpoint from outside the debuggerthread. Waking debuggerthread\n");
      //setup a sb command
  #pragma pack(1)
      struct
      {
        char command;
        HANDLE hProcess;
        int tid;
        int debugreg;
        uint64_t address;
        int bptype;
        int bpsize;
      } sb;
  #pragma pack()

      sb.command=CMD_SETBREAKPOINT;
      sb.hProcess=hProcess;
      sb.tid=tid;
      sb.debugreg=debugreg;
      sb.address=(uintptr_t)address;
      sb.bptype=bptype;
      sb.bpsize=bpsize;

      if (pthread_mutex_lock(&debugsocketmutex) == 0)
      {
        debug_log("Sending message to the debuggerthread\n");

        sendall(p->debuggerClient, &sb, sizeof(sb), 0);
        WakeDebuggerThread();
        recvall(p->debuggerClient, &result, sizeof(result), MSG_WAITALL);

        debug_log("Received reply from debugger thread: %d\n", result);


        pthread_mutex_unlock(&debugsocketmutex);
      }

    }

  }
  else
  {
   // debug_log("Invalid handle\n");
  }

  return result;

}

int RemoveBreakpoint(HANDLE hProcess, int tid, int debugreg,int wasWatchpoint)
/*
 * Removes the breakpoint with the provided address
 */
{
  int result=FALSE;

  debug_log("RemoveBreakpoint(%d, %d, %d, %d)\n", hProcess, tid, debugreg, wasWatchpoint);
  if (GetHandleType(hProcess) == htProcesHandle )
  {
    PProcessData p=(PProcessData)GetPointerFromHandle(hProcess);

    if (p->debuggerThreadID==pthread_self())
    {
      int isdebugged=p->debuggedThreadEvent.threadid;
      int wtid;
      DebugEvent de;

      debug_log("Called from the debuggerthread itself\n");

      if (tid==-1)
      {
        int i;
        debug_log("Calling RemoveBreakpoint for all threads\n");
        for (i=0; i<p->threadlistpos; i++)
        {
          if (RemoveBreakpoint(hProcess, p->threadlist[i].tid, debugreg, wasWatchpoint)==TRUE)
            result=TRUE;
        }
      }
      else
      {
        debug_log("specific thread\n");

        PThreadData td=GetThreadData(p, tid);
        int wasPaused=td->isPaused;

        if (wasPaused==0)
        {
          //manual
          debug_log("Not currently paused\n");
          debug_log("Going to kill and wait for this thread\n");

          int k;


          k=0;
          while ((td) && (td->isPaused==0) && (k<10))
          {
            syscall(__NR_tkill, tid, SIGSTOP);
            if (WaitForDebugEventNative(p, &de, tid, 100))
              break;
            k++;
          }

          wtid=de.threadid;


          debug_log("----AFTER WAIT----\n");

          debug_log("after wtid=%d\n", wtid);
          debug_log("^^^^AFTER WAIT^^^^\n");
        }
        else
        {
          debug_log("The thread I wanted to break was already broken. Yeeeh\n");
          wtid=isdebugged;
        }

        //debugging the given tid
        debug_log("Removing breakpoint from thread %d\n", wtid);


#ifdef __arm__
        int bpreg=0;
        int i,i2,i3;
        void *a=NULL;

        int bpIndex=1+(2*debugreg);

        debug_log("arm\n");

        if (wasWatchpoint)
        {
          i=safe_ptrace(PTRACE_SETHBPREGS, wtid, -bpIndex, &bpreg);
          i2=safe_ptrace(PTRACE_SETHBPREGS, wtid, -(bpIndex+1), &bpreg);
        }
        else
        {
          i=safe_ptrace(PTRACE_SETHBPREGS, wtid, bpIndex, &bpreg);
          i2=safe_ptrace(PTRACE_SETHBPREGS, wtid, bpIndex+1, &bpreg);
        }



        debug_log("i1=%d\n", i);

        debug_log("i2=%d\n", i2);



        i3=safe_ptrace(PTRACE_SETHBPREGS, wtid, 1, &a);

        result=(i==0) && (i2==0) && (i3==0);
#endif

#ifdef __aarch64__
        int i;
        struct user_hwdebug_state hwd;
        struct iovec iov;

        memset(&hwd, 0, sizeof(hwd));
        memset(&iov, 0, sizeof(iov));
        iov.iov_base=&hwd;
        iov.iov_len=sizeof(hwd);

        int bplist;

        if (wasWatchpoint)
          bplist=NT_ARM_HW_WATCH;
        else
          bplist=NT_ARM_HW_BREAK;

        i=safe_ptrace(PTRACE_GETREGSET, wtid, (void*)(size_t)bplist, &iov);
        if (i!=0)
          debug_log("PTRACE_GETREGSET failed\n");

        int listsize=hwd.dbg_info & 0xf;

        if (debugreg<listsize)
        {
          hwd.dbg_regs[debugreg].addr=0;
          hwd.dbg_regs[debugreg].ctrl=0;

          iov.iov_len=8+16*(hwd.dbg_info & 0xf);

          i=safe_ptrace(PTRACE_SETREGSET, wtid, (void*)(size_t)bplist, (void*)&iov);
          if (i!=0)
            debug_log("PTRACE_SETREGSET failed :%s\n", strerror(errno));
        }
        else
        {
          debug_log("Error: Tried to remove a breakpoint index out of range\n");
          i=1000;
        }



        result=i;
#endif

#if defined(__i386__) || defined(__x86_64__)
        int r;
        uintptr_t dr7=0;
        debug_log("x86\n");

        dr7=safe_ptrace(PTRACE_PEEKUSER, wtid, offsetof(struct user, u_debugreg[7]), 0);

        dr7&=~(3 << (debugreg*2)); //disable G# and L#
        dr7&=~(15 << (16+debugreg*4)); //set len and type for this debugreg to 0


        r=safe_ptrace(PTRACE_POKEUSER, wtid, offsetof(struct user, u_debugreg[debugreg]), 0);


        r=safe_ptrace(PTRACE_POKEUSER, wtid, offsetof(struct user, u_debugreg[7]), dr7);
        if (r==0)
          result=TRUE;
        else
          debug_log("Failure removing breakpoint from thread %d\n", wtid);


#endif

        if (wasPaused==0)
        {
          int r;
          PThreadData td=GetThreadData(p, tid);

          debug_log("Continue self broken thread\n");

          if (de.debugevent!=SIGSTOP) //in case a breakpoint or something else happened before sigstop happened
          {
            debug_log("Not a SIGSTOP. Adding to queue and leave suspended\n");
            AddDebugEventToQueue(p, &de);


            td->isPaused=1;
          }
          else
          {
            r=safe_ptrace(PTRACE_CONT, wtid, 0,0);
            debug_log("PTRACE_CONT=%d\n", r);

            td->isPaused=0;
          }
        }


      }

    }
    else
    {
      debug_log("Called from a secondary thread\n");
#pragma pack(1)
      struct
      {
        char command;
        HANDLE hProcess;
        int tid;
        int debugreg;
        int wasWatchpoint;
      } rb;
#pragma pack()

      rb.command=CMD_REMOVEBREAKPOINT;
      rb.hProcess=hProcess;
      rb.tid=tid;
      rb.debugreg=debugreg;
      rb.wasWatchpoint=wasWatchpoint;


      if (pthread_mutex_lock(&debugsocketmutex) == 0)
      {

        debug_log("Sending message to the debuggerthread\n");

        sendall(p->debuggerClient, &rb, sizeof(rb), 0);
        WakeDebuggerThread();
        recvall(p->debuggerClient, &result, sizeof(result), MSG_WAITALL);


        pthread_mutex_unlock(&debugsocketmutex);
      }

    }

  }
  else
    debug_log("Invalid handle\n");

  return result;
}

int GetThreadContext(HANDLE hProcess, int tid, PCONTEXT Context, int type)
/*
 * Gets the context of the given thread
 * Freezes/Resumes the thread for you if it isn't suspended yet
 * type is the data to be gathered (currently ignored but may be used in the future for specific data)
 */
{
  int r=FALSE;
  debug_log("GetThreadContext(%d)\n", tid);



  if (tid<=0)
  {
    debug_log("Invalid tid\n");
    return FALSE;
  }

  if (GetHandleType(hProcess) == htProcesHandle )
  {
    PProcessData p=(PProcessData)GetPointerFromHandle(hProcess);



    if (p->debuggerThreadID==pthread_self())
    {
      PThreadData td=GetThreadData(p, tid);

      debug_log("Inside debuggerthread\n");

      if (td)
      {
        DebugEvent de;
        int wasPaused=td->isPaused;
        int k=0;


        while ((td->isPaused==0) && (k<10))
        {
          debug_log("This thread was not paused. Pausing it\n");
          syscall(__NR_tkill, tid, SIGSTOP);
          if (WaitForDebugEventNative(p, &de, tid, 100))
            break;

          k++;
        }

        //the thread is paused, so fetch the data

        k=getRegisters(tid, &Context->regs);


        //k=safe_ptrace(PTRACE_GETREGS, tid, 0, &Context->regs);
        debug_log("getRegisters() returned %d\n", k);

        if (k==0)
          r=TRUE;
        else
          r=FALSE;


        if (!wasPaused)
        {
          //continue if sigstop
          PThreadData td=GetThreadData(p, tid);

          debug_log("The thread was not paused, so resuming it now\n");

          if (de.debugevent!=SIGSTOP) //in case a breakpoint or something else happened before sigstop happened
          {
            debug_log("Not a SIGSTOP. Adding to queue and leave suspended\n");
            AddDebugEventToQueue(p, &de);
            td->isPaused=1;
          }
          else
          {
            r=(r && (safe_ptrace(PTRACE_CONT, de.threadid, 0,0)==0));


            td->isPaused=0;
            debug_log("r=%d\n", r);
          }
        }


      }
      else
        debug_log("Invalid tid\n");

    }
    else
    {
      debug_log("Not the debugger thread. Pass to serverthread");
#pragma pack(1)
      struct
      {
        char command;
        HANDLE hProcess;
        int tid;
        int type;
      } gtc;
#pragma pack()

      gtc.command=CMD_GETTHREADCONTEXT;
      gtc.hProcess=hProcess;
      gtc.tid=tid;
      gtc.type=type;


      if (pthread_mutex_lock(&debugsocketmutex) == 0)
      {
        debug_log("Sending message to the debuggerthread\n");

        sendall(p->debuggerClient, &gtc, sizeof(gtc), 0);
        WakeDebuggerThread();
        recvall(p->debuggerClient, &r, sizeof(r), MSG_WAITALL);

        if (r)
        {
          //followed by the contextsize
          uint32_t structsize;

          recvall(p->debuggerClient, &structsize, sizeof(structsize), MSG_WAITALL);
          recvall(p->debuggerClient, &Context->regs, structsize, MSG_WAITALL); //and context
        }


        pthread_mutex_unlock(&debugsocketmutex);
      }

    }
  }
  else
    debug_log("invalid handle\n");


  return r;
}

/*
 * Sets the context of the given thread
 * Fails if the thread is not suspended first
 */
int SetThreadContext(HANDLE hProcess, int tid, PCONTEXT Context, int type)
{
  int r=FALSE;
  debug_log("SetThreadContext(%d)\n", tid);



  if (tid<=0)
  {
    debug_log("Invalid tid\n");
    return FALSE;
  }

  if (GetHandleType(hProcess) == htProcesHandle )
  {
    PProcessData p=(PProcessData)GetPointerFromHandle(hProcess);



    if (p->debuggerThreadID==pthread_self())
    {
        PThreadData td=GetThreadData(p, tid);

        debug_log("Inside debuggerthread\n");

        if (td)
        {
          DebugEvent de;
          int wasPaused=td->isPaused;
          int k=0;


          while ((td->isPaused==0) && (k<10))
          {
            debug_log("This thread was not paused. Pausing it\n");
            syscall(__NR_tkill, tid, SIGSTOP);
            if (WaitForDebugEventNative(p, &de, tid, 100))
              break;

            k++;
          }

          //the thread is paused, so fetch the data

          k=setRegisters(tid, &Context->regs);


          //k=safe_ptrace(PTRACE_SETREGS, tid, 0, &Context->regs);
          debug_log("setRegisters() returned %d\n", k);

          if (k==0)
            r=TRUE;
          else
            r=FALSE;


          if (!wasPaused)
          {
            //continue if sigstop
            PThreadData td=GetThreadData(p, tid);

            debug_log("The thread was not paused, so resuming it now\n");

            if (de.debugevent!=SIGSTOP) //in case a breakpoint or something else happened before sigstop happened
            {
              debug_log("Not a SIGSTOP. Adding to queue and leave suspended\n");
              AddDebugEventToQueue(p, &de);
              td->isPaused=1;
            }
            else
            {
              r=(r && (safe_ptrace(PTRACE_CONT, de.threadid, 0,0)==0));


              td->isPaused=0;
              debug_log("r=%d\n", r);
            }
          }


        }
        else
          debug_log("Invalid tid\n");

      }
    } 
    else
    debug_log("invalid handle\n");

    return r;
}

int SuspendThread(HANDLE hProcess, int tid)
/*
 * Increase the suspendcount. If it was 0, stop the given thread if it isn't stopped yet
 * If the thread was already stopped, look up this event in the queue and remove it
 * if it's the currently debugged thread it will not be in the queue, but continueFromDebugEvent
 * will see it is suspended, and as a result, NOT continue it, but store the debug event into the thread
 * If it wasn't stopped, stop it, and fetch the debug event
 */
{
  int result=-1;

  debug_log("SuspendThread(%d)\n", tid);
  if (GetHandleType(hProcess) == htProcesHandle )
  {
    PProcessData p=(PProcessData)GetPointerFromHandle(hProcess);
    PThreadData t=GetThreadData(p, tid);

    if (t==NULL)
    {
      debug_log("Invalid thread\n");
      return -1;
    }

    if (p->debuggerThreadID==pthread_self())
    {
      //inside the debuggerthrad
      debug_log("Inside the debugger thread.\n");

      if (t->isPaused)
      {
        debug_log("Already paused\n");

        if (t->suspendCount==0)
        {
          //first time suspend. Gather the debug event and remove it from the queue (hardly ever, most of the time it's just the currently broken thread)

          if (p->debuggedThreadEvent.threadid==tid) //it's the currently suspended thread
            t->suspendedDevent=p->debuggedThreadEvent;
          else
          {
            //go through the queuelist to find the debug event and remove it
            t->suspendedDevent=*FindThreadDebugEventInQueue(p,tid);
            RemoveThreadDebugEventFromQueue(p, tid);
          }
        }

        t->suspendCount++;


      }
      else
      {
        debug_log("Not yet paused\n");

        while (t->isPaused==0)
        {
          syscall(__NR_tkill, tid, SIGSTOP);
          if (WaitForDebugEventNative(p, &t->suspendedDevent, tid, 100))
            break;

        }

        t->suspendCount++;

      }

      return t->suspendCount;


      //PThreadData t=GetThreadData(tid);
    }
    else
    {
      debug_log("Not from the debugger thread. Switching...\n");
#pragma pack(1)
      struct
      {
        char command;
        HANDLE hProcess;
        int tid;
      } st;
#pragma pack()

      st.command=CMD_SUSPENDTHREAD;
      st.hProcess=hProcess;
      st.tid=tid;

      if (pthread_mutex_lock(&debugsocketmutex) == 0)
      {
        sendall(p->debuggerClient, &st, sizeof(st), 0);
        WakeDebuggerThread();
        recvall(p->debuggerClient, &result, sizeof(result), MSG_WAITALL);

        pthread_mutex_unlock(&debugsocketmutex);


      }
    }
  }
  else
  {
    debug_log("invalid handle\n");
    result=-1;
  }

  return result;

}

int ResumeThread(HANDLE hProcess, int tid)
/*
 * Decrease suspendcount. If 0, resume the thread by adding the stored debug event back to the queue
 */
{
  int result;

  debug_log("ResumeThread(%d)\n", tid);
  if (GetHandleType(hProcess) == htProcesHandle )
  {
    PProcessData p=(PProcessData)GetPointerFromHandle(hProcess);
    PThreadData t=GetThreadData(p, tid);

    if (t==NULL)
    {
      debug_log("Invalid thread\n");
      return -1;
    }

    if (p->debuggerThreadID==pthread_self())
    {
      //inside the debuggerthread
      debug_log("Inside the debugger thread.\n");

      if ((t->isPaused) && (t->suspendCount>0))
      {

        t->suspendCount--;

        result=t->suspendCount;


        if (t->suspendCount==0)
        {
          //reached 0, continue process if sigstop, else add to queue
          PThreadData td=GetThreadData(p, tid);
          debug_log("suspeneCount==0\n");

          if (t->suspendedDevent.debugevent==SIGSTOP)
          {
            debug_log("SIGSTOP: Continue thread without queing\n");
            safe_ptrace(PTRACE_CONT, t->suspendedDevent.threadid, 0,0);
            td->isPaused=0;
          }
          else
          {
            debug_log("Not a SIGSTOP Add to the event queue\n");
            td->isPaused=1;
            AddDebugEventToQueue(p, &t->suspendedDevent);
            WakeDebuggerThread();
          }
        }
      }
      else
      {
        debug_log("Failure resuming this thread\n");

      }


      //PThreadData t=GetThreadData(tid);
    }
    else
    {
      debug_log("Not from the debugger thread. Switching...\n");
#pragma pack(1)
      struct
      {
        char command;
        HANDLE hProcess;
        int tid;
      } rt;
#pragma pack()

      rt.command=CMD_RESUMETHREAD;
      rt.hProcess=hProcess;
      rt.tid=tid;

      if (pthread_mutex_lock(&debugsocketmutex) == 0)
      {
        sendall(p->debuggerClient, &rt, sizeof(rt), 0);
        WakeDebuggerThread();
        recvall(p->debuggerClient, &result, sizeof(result), MSG_WAITALL);

        pthread_mutex_unlock(&debugsocketmutex);
      }
    }
  }
  else
  {
    debug_log("invalid handle\n");
    result=-1;
  }

  return result;
}

int RemoveThreadDebugEventFromQueue(PProcessData p, int tid)
/*
 * removes the debug event from the queue
 */
{
  int result=FALSE;
  struct DebugEventQueueElement *deqe;

  pthread_mutex_lock(&p->debugEventQueueMutex);

 // debug_log("RemoveThreadDebugEventFromQueue(%d)\n", tid);

  deqe=p->debugEventQueue.tqh_first;
  while (deqe)
  {
   // debug_log("deqe->de.threadid=%d  (looking for %d)\n", deqe->de.threadid, tid);
    if (deqe->de.threadid==tid)
    {
      //printf("Found. Removing it\n");
      TAILQ_REMOVE(&p->debugEventQueue, deqe, entries);

      free(deqe);
      result=TRUE;
      break;
    }

   // debug_log("Not what I wanted. Check next\n");
    deqe=deqe->entries.tqe_next;
  }


  pthread_mutex_unlock(&p->debugEventQueueMutex);
  return result;
}

PDebugEvent FindThreadDebugEventInQueue(PProcessData p, int tid)
/*
 * Finds the DebugEvent for this specific thread if there is one
 * Note that it returns a pointer to it. If you plan on removing it from the queue, copy the results manually
 */
{
  PDebugEvent result=NULL;
  struct DebugEventQueueElement *deqe;
  pthread_mutex_lock(&p->debugEventQueueMutex);

  deqe=p->debugEventQueue.tqh_first;
  while (deqe)
  {
    if ((tid==-1) || (deqe->de.threadid==tid))
    {
      result=&deqe->de;
      break;
    }

    deqe=deqe->entries.tqe_next;
  }


  pthread_mutex_unlock(&p->debugEventQueueMutex);
  return result;
}

void AddDebugEventToQueue(PProcessData p, PDebugEvent devent)
{
  struct DebugEventQueueElement *deqe;

  if (devent->debugevent==SIGSTOP)
  {
    debug_log("<<<<<--------------------SIGSTOP ADDED TO THE QUEUE!\n");
  }

  pthread_mutex_lock(&p->debugEventQueueMutex);

  deqe=malloc(sizeof(struct DebugEventQueueElement));
  deqe->de=*devent;

  TAILQ_INSERT_TAIL(&p->debugEventQueue, deqe, entries);

  pthread_mutex_unlock(&p->debugEventQueueMutex);
}

int GetStopSignalFromThread(int tid)
{
  siginfo_t si;
  if (safe_ptrace(PTRACE_GETSIGINFO, tid, NULL, &si)==0)
    return si.si_signo;
  else
    return -1;
}

int WaitForDebugEventNative(PProcessData p, PDebugEvent devent, int tid, int timeout)
/*
 * Waits for a debug event for a specific thread, queues the devent if not the expected tid
 * Only call this from a(the) debugger thread (NO OTHER THREAD MAY CALL THIS)
 */
{
  int currentTID;
  int status;
  int r;





  //printf("WaitForDebugEventNative (p=%p, devent=%p, tid=%d timeout=%d)\n", p, devent, tid, timeout);


  //first check if there is already a thread waiting
  currentTID=1;
  while (currentTID>0)
  {
    currentTID=waitpid(tid, &status, __WALL | WNOHANG);

    //printf("First waitid=%d\n", currentTID);

    if (currentTID>0)
    {
      devent->threadid=currentTID;
      devent->debugevent=GetStopSignalFromThread(devent->threadid);

      PThreadData td=GetThreadData(p, currentTID);
      if (td)
        td->isPaused=1;


      if ((tid==-1) || (currentTID==tid))
        return TRUE;

      //still here, this wasn't what I was looking for...
      //add it to the queue

      //printf("Unexpected event from thread %d while waiting for %d\n", currentTID, tid);



      AddDebugEventToQueue(p, devent);
    }
    //try again, perhaps there is another one available right now
  }

 // debug_log("Checking for debug server command\n");

//  fflush(stdout);

  //still here
  //CheckForAndDispatchCommand(p->debuggerServer);


 // debug_log("After check and dispatch\n");
 // fflush(stdout);


  if (timeout>=0)
  {
    //wait till there is an event

    if (timeout>0)
    {
      struct timespec abstime;
      struct timeval current,wanted, diff;
      int timedwait;

     // debug_log("timed wait\n");

      memset(&abstime, 0, sizeof(abstime));
      gettimeofday(&current,NULL);

      diff.tv_sec=(timeout / 1000);
      diff.tv_usec=(timeout % 1000)*1000;

      timeradd(&current, &diff, &wanted);

      abstime.tv_sec=wanted.tv_sec;
      abstime.tv_nsec=wanted.tv_usec*1000;

      currentTID=-1;
      while (currentTID<=0)
      {
        timedwait=sem_timedwait(&sem_DebugThreadEvent, &abstime);
       // if (log==1)
      //    debug_log("log=1: sem_timedwait=%d\n", timedwait);

        if (timedwait==0)
        {
          //it got signaled
          //check if there is a debugger thread message waiting
         // if (log==1)
         //   debug_log("Checking for dispatch command\n");

          CheckForAndDispatchCommand(p->debuggerServer);
          if (VerboseLevel>10)
            debug_log("CheckForAndDispatchCommand returned\n");

          //check if an event got queued for this thread by the dispatcher
          PDebugEvent e=FindThreadDebugEventInQueue(p, tid);
          if (e)
          {
            debug_log("There was a queued event after CheckForAndDispatchCommand. TID=%ld (wanted %d)\n", e->threadid, tid);
            currentTID=e->threadid;

            r=RemoveThreadDebugEventFromQueue(p, currentTID);
            debug_log("RemoveThreadDebugEventFromQueue returned %d\n", r);
          }
          else
            currentTID=waitpid(tid, &status, __WALL | WNOHANG);

          if (VerboseLevel>10)
            debug_log("currentTID = %d\n", currentTID);

          if (currentTID>0)
          {
            devent->threadid=currentTID;
            devent->debugevent=GetStopSignalFromThread(devent->threadid);

            PThreadData td=GetThreadData(p, currentTID);
            if (td)
              td->isPaused=1;

            if ((tid==-1) || (currentTID==tid))
              return TRUE;

            //still here
            debug_log("Still here so currentTID(%d) is not the same as tid (%d)\n", currentTID, tid);
            AddDebugEventToQueue(p, devent);
          }
          currentTID=-1; //retry

        }
        else
        {
          if (errno==ETIMEDOUT)
          {
            //printf("timeout\n");
            return FALSE;
          }
          //else
          //  debug_log("Not a timeout. Retry\n");
        }
      }
    }
    else
    {
      //no wait
      //printf("no wait\n");


    }

  }
  else
  {
    //printf("Infinite wait\n");
    currentTID=-1;
    while (currentTID<0)
    {
      currentTID=waitpid(tid, &status, __WALL);
      if (currentTID>0)
      {
        devent->threadid=currentTID;
        devent->debugevent=GetStopSignalFromThread(devent->threadid);

        PThreadData td=GetThreadData(p, currentTID);
        if (td)
          td->isPaused=1;

        if ((tid==-1) || (currentTID==tid))
          return TRUE;

        //still here
        AddDebugEventToQueue(p, devent);
      }

      if ((currentTID==-1) && (errno!=EINTR))
      {
        debug_log("WaitForDebugEventNative: Infinite wait: Could not wait for tid %d (errno=%d)\n", tid, errno);
        return FALSE; //something bad happened
      }

      currentTID=-1;
    }

  }

  //printf("Returning false\n");


  return FALSE;

}


int WaitForDebugEvent(HANDLE hProcess, PDebugEvent devent, int timeout)
/*
 *Waits for the specified timeout in milliseconds, called by the ce gui
 *Does not care about which thread to wait for
 */
{
  if (GetHandleType(hProcess) == htProcesHandle )
  {
    PProcessData p=(PProcessData)GetPointerFromHandle(hProcess);

    if (p->debuggedThreadEvent.threadid==0)
    {
      int r=0;

      int status;
      int tid;
      struct DebugEventQueueElement *de=NULL;

      //check the queue (first one in the list)
      pthread_mutex_lock(&p->debugEventQueueMutex);
      de=TAILQ_FIRST(&p->debugEventQueue);

      if (de)
        TAILQ_REMOVE(&p->debugEventQueue, de, entries);


      pthread_mutex_unlock(&p->debugEventQueueMutex);

      if (de) //there was a queued event, return it
      {
        debug_log("Returning queued event (sig=%d,  thread=%ld)\n", de->de.debugevent, de->de.threadid);
        if (de->de.debugevent==SIGSTOP)
        {
          debug_log("<---Something queued a SIGSTOP--->\n");
        }

        *devent=de->de;
        p->debuggedThreadEvent=*devent;
        free(de);

        r=1; //mark that a queued event happened
      }

      if (!r) //not a queued event
        r=WaitForDebugEventNative(p, devent, -1, timeout);


      if (r)
      {
        p->debuggedThreadEvent=*devent;


        if (p->debuggedThreadEvent.debugevent==SIGTRAP)
        {
          siginfo_t si;
          debug_log("SIGTRAP\n");

          //fill in the address


#if (defined(__arm__) || defined(__aarch64__))
          //return si_addr of siginfo
          if (safe_ptrace(PTRACE_GETSIGINFO, p->debuggedThreadEvent.threadid, NULL, &si)==0)
          {

            p->debuggedThreadEvent.address=(uintptr_t)si.si_addr;
            debug_log("si.si_addr=%p\n", si.si_addr);
          }
          else
            debug_log("Failure getting siginfo for this trap\n");
#endif

#if defined __i386__ || defined __x86_64__
          //use DR6 to determine which bp (if possible)
          uintptr_t DR0,DR1,DR2,DR3,DR7, IP, SP;
          regDR6 DR6;
#if defined __i386__
          IP=safe_ptrace(PTRACE_PEEKUSER, p->debuggedThreadEvent.threadid, offsetof(struct user, regs.eip), 0);
          SP=safe_ptrace(PTRACE_PEEKUSER, p->debuggedThreadEvent.threadid, offsetof(struct user, regs.esp), 0);
#else
          IP=safe_ptrace(PTRACE_PEEKUSER, p->debuggedThreadEvent.threadid, offsetof(struct user, regs.rip), 0);
          SP=safe_ptrace(PTRACE_PEEKUSER, p->debuggedThreadEvent.threadid, offsetof(struct user, regs.rsp), 0);
#endif
          DR0=safe_ptrace(PTRACE_PEEKUSER, p->debuggedThreadEvent.threadid, offsetof(struct user, u_debugreg[0]), 0);
          DR1=safe_ptrace(PTRACE_PEEKUSER, p->debuggedThreadEvent.threadid, offsetof(struct user, u_debugreg[1]), 0);
          DR2=safe_ptrace(PTRACE_PEEKUSER, p->debuggedThreadEvent.threadid, offsetof(struct user, u_debugreg[2]), 0);
          DR3=safe_ptrace(PTRACE_PEEKUSER, p->debuggedThreadEvent.threadid, offsetof(struct user, u_debugreg[3]), 0);

          DR6.value=safe_ptrace(PTRACE_PEEKUSER, p->debuggedThreadEvent.threadid, offsetof(struct user, u_debugreg[6]), 0);
          DR7=safe_ptrace(PTRACE_PEEKUSER, p->debuggedThreadEvent.threadid, offsetof(struct user, u_debugreg[7]), 0);

          debug_log("sizeof(dr0)=%d\n", sizeof(DR0));
          debug_log("sizeof(long)=%d\n", sizeof(long));
          debug_log("DR0=%p\n",(void*)DR0);
          debug_log("DR1=%p\n",(void*)DR1);
          debug_log("DR2=%p\n",(void*)DR2);
          debug_log("DR3=%p\n",(void*)DR3);
          debug_log("DR6=%p\n",(void*)(DR6.value));
          debug_log("DR7=%p\n",(void*)DR7);
          debug_log("IP=%p\n",(void*)IP);
          debug_log("SP=%p\n",(void*)SP);



          p->debuggedThreadEvent.address=0; //something unexpected
          if (DR6.B0)
            p->debuggedThreadEvent.address=DR0;
          else
          if (DR6.B1)
            p->debuggedThreadEvent.address=DR1;
          else
          if (DR6.B2)
            p->debuggedThreadEvent.address=DR2;
          else
          if (DR6.B3)
            p->debuggedThreadEvent.address=DR3;
          else
          if (DR6.BS) //single step
            p->debuggedThreadEvent.address=1;



#endif
          debug_log("p->debuggedThreadEvent.address=%lx\n", p->debuggedThreadEvent.address);

          devent->address=p->debuggedThreadEvent.address;
        }
      }

      return r;
    }
    else
    {
      *devent=p->debuggedThreadEvent;
      debug_log("Can not wait for a debug event when a thread is still paused\n");
      debug_log("tid=%d  debugevent=%d\n", (int)devent->threadid, devent->debugevent);
      return 1; //success and just handle this event
    }

  }
  return 0;
}

int ContinueFromDebugEvent(HANDLE hProcess, int tid, int ignoresignal)
{
  //printf("ContinueFromDebugEvent called\n");
  if (GetHandleType(hProcess) == htProcesHandle )
  {
    PProcessData p=(PProcessData)GetPointerFromHandle(hProcess);
    PThreadData td=GetThreadData(p, tid);
    siginfo_t si;

   //printf("td==%p\n", td);

    if (p->debuggedThreadEvent.debugevent<0) //virtual debug event. Just ignore
    {
      debug_log("Virtual event. Ignore\n");
      p->debuggedThreadEvent.threadid=0;
      p->debuggedThreadEvent.debugevent=0;
      return 1; //ignore it
    }


    if (td==NULL)
    {
      debug_log("Invalid thread\n");
      p->debuggedThreadEvent.threadid=0;

      return 0; //invalid thread
    }

   // debug_log("td->suspendcount=%d\n", td->suspendCount);

    if (td->suspendCount>0)
    {
      debug_log("Tried to continue a suspended thread (suspendcount=%d)\n", td->suspendCount);
      return 1; //keep it suspended
    }

   // debug_log("p->debuggedThreadEvent.debugevent=%d\n", p->debuggedThreadEvent.debugevent);

    //if (p->debuggedThreadEvent.threadid!=tid)
    //  debug_log("Unexpected thread continue. Expected %d got %d\n", p->debuggedThreadEvent.threadid, tid);




    if (safe_ptrace(PTRACE_GETSIGINFO, tid, NULL, &si)==0)
    {
      int signal=ignoresignal?0:si.si_signo;

     // debug_log("si.si_signo=%d\n", si.si_signo);
     // debug_log("si.si_code=%d\n", si.si_code);



      if ((signal==19) || (signal==21)) //STOPPED
      {
        signal=0;
      }

#if defined __i386__ || defined __x86_64__
      safe_ptrace(PTRACE_POKEUSER, tid, offsetof(struct user, u_debugreg[6]), 0);
#endif


      //printf("Continue %d with signal %d\n", tid, signal);

      int result;
      if (ignoresignal==2)
      {
        debug_log("ContinueFromDebugEvent(): Single step\n");


#ifdef __aarch64__
        /*
        //test: removing old breakpoints (In case CE hadn't done this beforehand...)
        int i;
        int maxWatchCount=0;
        int maxBreakCount=0;
        struct user_pt_regs regset;

        struct iovec iov;

        struct user_hwdebug_state watchhwd;
        struct user_hwdebug_state breakhwd;
        struct user_hwdebug_state temphwd;
        memset(&watchhwd, 0, sizeof(watchhwd));
        memset(&breakhwd, 0, sizeof(breakhwd));

        iov.iov_base=&watchhwd;
        iov.iov_len=sizeof(watchhwd);

        result=safe_ptrace(PTRACE_GETREGSET, tid, (void*)NT_ARM_HW_WATCH, &iov);
        if (result==0)
        {

          maxWatchCount=watchhwd.dbg_info & 0xf;
          temphwd=watchhwd;

          debug_log("Current watchlist:\n");


          for (i=0; i<maxWatchCount; i++)
          {
            debug_log("%d: %x - %p:\n", i, watchhwd.dbg_regs[i].ctrl, (void*)watchhwd.dbg_regs[i].addr);
            temphwd.dbg_regs[i].addr=0;
            temphwd.dbg_regs[i].ctrl=0;
          }
          debug_log("Disabling watches for this thread:");
          iov.iov_base=&temphwd;
          iov.iov_len=8+16*maxWatchCount;
          result=safe_ptrace(PTRACE_SETREGSET, tid, (void*)NT_ARM_HW_WATCH, &iov);

          if (result==0)
            debug_log("Success\n");
          else
            debug_log("Error: %s\n", strerror(errno));

        }

        iov.iov_base=&breakhwd;
        iov.iov_len=sizeof(breakhwd);
        i=safe_ptrace(PTRACE_GETREGSET, tid, (void*)NT_ARM_HW_BREAK, &iov);
        if (i==0)
        {
          maxBreakCount=breakhwd.dbg_info & 0xf;
          temphwd=breakhwd;
          debug_log("Current breaklist:\n");
          for (i=0; i<maxBreakCount; i++)
          {
            debug_log("%d: %x - %p:\n", i, breakhwd.dbg_regs[i].ctrl, (void*)breakhwd.dbg_regs[i].addr);
            temphwd.dbg_regs[i].addr=0;
            temphwd.dbg_regs[i].ctrl=0;
          }

          debug_log("Disabling breaks for this thread:");
          iov.iov_len=8+16*maxBreakCount;
          result=safe_ptrace(PTRACE_SETREGSET, tid, (void*)NT_ARM_HW_BREAK, &iov);

          if (result==0)
            debug_log("Success\n");
          else
            debug_log("Error: %s\n", strerror(errno));
        }*/

        //test2: use the Debug mask flag at bit 9 of PSTATE

#endif

        result=safe_ptrace(PTRACE_SINGLESTEP, tid, 0,0);

#ifdef __aarch64__

        if (result==0)
          debug_log("PTRACE_SINGLESTEP returned success\n");


#endif
        if (result!=0)
        {
          debug_log("PTRACE_SINGLESTEP failed (%d). Shit happens\n", errno);
          result=safe_ptrace(PTRACE_CONT, tid, 0,signal);
        }

      }
      else
      {
        result=safe_ptrace(PTRACE_CONT, tid, 0,signal);
      }



     // debug_log("Continue result=%d\n", result);

      if (td)
        td->isPaused=0;


      if (result<0)
      {
        debug_log("Failure to continue thread %d with signal %d\n", tid, signal);
        RemoveThreadFromProcess(p, tid);
        p->debuggedThreadEvent.threadid=0;
        return 0;
      }
      else
      {
        p->debuggedThreadEvent.threadid=0;
        return 1;
      }
    }
    else
      debug_log("Failure getting sig info\n");

  }
  else
    debug_log("Invalid handle\n");

  return 0;
}

int StopDebug(HANDLE hProcess)
{
  if (GetHandleType(hProcess) == htProcesHandle )
  {
    PProcessData p=(PProcessData)GetPointerFromHandle(hProcess);
    int i;
    for (i=0; i<p->threadlistpos;i++)
      if (safe_ptrace(PTRACE_DETACH, p->threadlist[i].tid,0,0)<0)
        debug_log("Failed to detach from %ld\n", p->threadlist[i].tid);
  }

  return 1;

}

int WriteProcessMemoryDebug(HANDLE hProcess, PProcessData p, void *lpAddress, void *buffer, int size)
{
  int byteswritten=0;
  int i;

  debug_log("WriteProcessMemoryDebug:");
  for (i=0; i<size; i++)
  {
    debug_log("%.2x ", ((unsigned char *)buffer)[i]);
  }

  debug_log("\n");




  if (p->debuggerThreadID==pthread_self()) //this is the debugger thread
  {
    int isdebugged=FindPausedThread(p); //find a paused thread if there is one
    DebugEvent event;

    event.threadid=isdebugged;

    if (!isdebugged)
    {
     // debug_log("Not currently debugging a thread. Suspending a random thread\n");
      kill(p->pid, SIGSTOP);

      //printf("Going to wait for debug event\n");
      WaitForDebugEventNative(p, &event, -1, -1); //wait for it myself

     // debug_log("After WaitForDebugEventNative (tid=%d)\n", event.threadid);
    }



    //write the bytes
    {
      int offset=0;
      int max=size-sizeof(long int);

      long int *address=(long int *)buffer;


      while (offset<max)
      {
        debug_log("offset=%d max=%d\n", offset, max);
        safe_ptrace(PTRACE_POKEDATA, p->pid, (void*)((uintptr_t)lpAddress+offset), (void *)*address);

        address++;
        offset+=sizeof(long int);
        byteswritten+=sizeof(long int);
      }

      if (offset<size)
      {
        debug_log("WPMD: Still some bytes left: %d\n", size-offset);
        //still a few bytes left
        uintptr_t oldvalue=0;
        oldvalue=safe_ptrace(PTRACE_PEEKDATA, p->pid,  (void *)(uintptr_t)lpAddress+offset, (void*)0);
        #ifdef __x86_64__
          //Even with 64 bits, peek_data can read only 4 bytes.
          debug_log("64-bit: oldvalue=%lx\n", oldvalue);
          //oldvalue += safe_ptrace(PTRACE_PEEKDATA, p->pid,  (void *)(uintptr_t)lpAddress+offset+4, (void*)0)*0x100000000;

          //debug_log("64-bit: oldvalue with full read=%lx\n", oldvalue);
        #endif
        unsigned char *oldbuf=(unsigned char *)&oldvalue;
        unsigned char *newmem=(unsigned char *)address;
        int i;

        debug_log("oldvalue=%lx\n", oldvalue);

        for (i=0; i< (size-offset); i++)
          oldbuf[i]=newmem[i];

        debug_log("newvalue=%lx\n", oldvalue);


        i=safe_ptrace(PTRACE_POKEDATA, p->pid, (void*)((uintptr_t)lpAddress+offset), (void *)oldvalue);

        debug_log("ptrace poke returned %d\n", i);
        if (i>=0)
          byteswritten+=size-offset;

      }

    }



    if (!isdebugged)
    {
      PThreadData td=GetThreadData(p, event.threadid);

      //if a SIGSTOP happened just continue it, else the next time WaitForDebugEvent() runs it will get this event from the queue
      if (event.debugevent==SIGSTOP)
      {

      //  debug_log("Continue from sigstop\n");

        safe_ptrace(PTRACE_CONT, event.threadid, 0,0);

        if (td)
          td->isPaused=0;


      }
      else
      {
        debug_log("WriteProcessMemoryDebug: Adding unexpected signal to eventqueue (event.debugevent=%d event.threadid)\n", event.debugevent, event.threadid);

        AddDebugEventToQueue(p, &event);
        if (td)
          td->isPaused=1;
      }
    }

  }
  else
  {
    debug_log("WriteProcessMemoryDebug from outside the debuggerthread. Waking debuggerthread\n");

    //setup a rpm command
#pragma pack(1)
    struct
    {
      uint8_t command;   //1
      uint32_t pHandle;  //5
      uint64_t address;  //13
      uint32_t size;     //17
    } wpm;
#pragma pack()

    unsigned char data [size];
    debug_log("sizeof wpm=%d\n", sizeof(wpm));
    wpm.command=CMD_WRITEPROCESSMEMORY;
    wpm.pHandle=hProcess;
    wpm.address=(uintptr_t)lpAddress;
    wpm.size=size;
    memcpy(data, buffer, size);



    //and write it to the p->debuggerthreadfd and read out the data
    //aquire lock (I don't want other threads messing with the client socket)
    if (pthread_mutex_lock(&debugsocketmutex) == 0)
    {
      sendall(p->debuggerClient, &wpm, sizeof(wpm), 0);
      sendall (p-> debuggerClient, &data, size, 0);
      WakeDebuggerThread();

      recvall(p->debuggerClient, &byteswritten, sizeof(byteswritten), MSG_WAITALL);

      pthread_mutex_unlock(&debugsocketmutex);

      debug_log("after recvall byteswritten=%d\n", byteswritten);
    }
  }


  return byteswritten;
}

int WriteProcessMemory(HANDLE hProcess, void *lpAddress, void *buffer, int size)
{
  int written=0;

  debug_log("WriteProcessMemory(%d, %p, %p, %d)\n", hProcess, lpAddress, buffer, size);

  if (GetHandleType(hProcess) == htProcesHandle )
  {
    PProcessData p=(PProcessData)GetPointerFromHandle(hProcess);

    if (p->isDebugged) //&& cannotdealwithotherthreads
    {
      debug_log("This process is being debugged\n");
      //use the debugger specific writeProcessMemory implementation
      return WriteProcessMemoryDebug(hProcess, p, lpAddress, buffer, size);
    }

    debug_log("aquiring memorymutex\n");
    if (pthread_mutex_lock(&memorymutex) == 0)
    {
      debug_log("aquired memorymutex\n");

      if ((MEMORY_SEARCH_OPTION == 2) && (process_vm_writev))
      {
        struct iovec local;
        struct iovec remote;

        debug_log("WPM: MEMORY_SEARCH_OPTION == 2\n");

        local.iov_base=buffer;
        local.iov_len=size;

        remote.iov_base=lpAddress;
        remote.iov_len=size;

        written=process_vm_writev(p->pid,&local,1,&remote,1,0);
        if (written==-1)
        {
          debug_log("process_vm_writev(%p, %d) failed: %s\n", lpAddress, size, strerror(errno));
          written=0;
        }
        else
          debug_log("Write successful\n");

        pthread_mutex_unlock(&memorymutex);
        return written;
      }

      if (MEMORY_SEARCH_OPTION==2)
        debug_log("process_vm_writev==NULL. Using other method\n");

      debug_log("WPM: ATTACH_TO_WRITE_MEMORY == %d\n",ATTACH_TO_WRITE_MEMORY);
      debug_log("p->memrw=%d\n", p->memrw);
      debug_log("p->mem=%d\n", p->mem);

      if ((p->memrw==0) && (ATTACH_TO_WRITE_MEMORY==0))
      {
        debug_log("p->memrw is 0. Overriding ATTACH_TO_WRITE_MEMORY from 0 to 1\n");
        ATTACH_TO_WRITE_MEMORY=1;
      }


      if ((ATTACH_TO_WRITE_MEMORY==0) || (safe_ptrace(PTRACE_ATTACH, p->pid,0,0)==0))
      {
        int status;



        pid_t pid=ATTACH_TO_WRITE_MEMORY ? wait(&status) : p->pid;

        if ((MEMORY_SEARCH_OPTION == 0) && (p->memrw))
        {
          debug_log("WPM: MEMORY_SEARCH_OPTION == 0\n");

          lseek64(p->mem, (uintptr_t)lpAddress, SEEK_SET);
          written=write(p->mem, buffer, size);
          if (written==-1)
          {
            debug_log("write() to address %p failed with error: %s\n", lpAddress, strerror(errno));
            written=0;
          }


        }
        else
        {
          debug_log("WPM: MEMORY_SEARCH_OPTION == %d p->memrw=%d\n", MEMORY_SEARCH_OPTION, p->memrw);

          int offset=0;
          int max=size-sizeof(long int);

          long int *address=(long int *)buffer;

          debug_log("start: offset=%d  max=%d\n", offset, max);

          while (offset<max)
          {
            debug_log("offset=%d max=%d\n", offset, max);
            safe_ptrace(PTRACE_POKEDATA, pid, (void*)((uintptr_t)lpAddress+offset), (void *)*address);

            address++;
            offset+=sizeof(long int);

            written+=sizeof(long int);
          }

          debug_log("after loop: offset=%d  max=%d\n", offset, max);

          if (offset<size)
          {
            debug_log("WPM: Still some bytes left: %d\n", size-offset);
            //still a few bytes left
            uintptr_t oldvalue=0;
            oldvalue=safe_ptrace(PTRACE_PEEKDATA, pid,  (void *)(uintptr_t)lpAddress+offset, (void*)0);

            unsigned char *oldbuf=(unsigned char *)&oldvalue;
            unsigned char *newmem=(unsigned char *)address;
            int i;

            debug_log("oldvalue=%lx\n", oldvalue);

            for (i=0; i< (size-offset); i++)
              oldbuf[i]=newmem[i];

            debug_log("newvalue=%lx\n", oldvalue);


            i=safe_ptrace(PTRACE_POKEDATA, pid, (void*)((uintptr_t)lpAddress+offset), (void *)oldvalue);

            debug_log("ptrace poke returned %d\n", i);
            if (i>=0)
              written+=size-offset;
          }
        }

        if (ATTACH_TO_WRITE_MEMORY)
          safe_ptrace(PTRACE_DETACH, pid,0,0);
      }
      //else
      //  debug_log("PTRACE ATTACH FAILED\n");


      pthread_mutex_unlock(&memorymutex);
    }



  }

  return written;
}

int ReadProcessMemoryDebug(HANDLE hProcess, PProcessData p, void *lpAddress, void *buffer, int size)
{
  //problem: Only the thread that is currently debugging a stopped thread has read access
  //work around: add a que to the debugger thread that fills in this
  //que a ReadProcessMemory command with the debugger

  //problem 2: If the debugger thread has broken on a breakpoint and another thread wants to read memory it can't
  //e.g: t1:WaitForDebugEvent  t1:Returned. T1: recv  t2: ReadProcessMemory...
  //solution, let the debuggerthread wait for a socket event with select on 2 fd's. One from the connected socket, and one from the que.
  //the que will contain normal rpm command


  int bytesread=0;

 // debug_log("ReadProcessMemoryDebug");
//  debug_log("lpAddress=%p\n", lpAddress);



  if (p->debuggerThreadID==pthread_self()) //this is the debugger thread
  {
    int isdebugged=FindPausedThread(p); //find a paused thread if there is one
    DebugEvent event;

    event.threadid=isdebugged;


    //printf("ReadProcessMemoryDebug inside debuggerthread (thread debbugged=%d)\n", isdebugged);

    if (!isdebugged)
    {
      // debug_log("Not currently debugging a thread. Suspending a random thread\n");
      kill(p->pid, SIGSTOP);

      //printf("Going to wait for debug event\n");
      if (WaitForDebugEventNative(p, &event, -1, -1)==FALSE) //wait for it myself
      {
        debug_log("WaitForDebugEventNative returned FALSE for a wait without timeout\n");
        return 0;
      }

     // debug_log("After WaitForDebugEventNative (tid=%d)\n", event.threadid);
    }

    if(MEMORY_SEARCH_OPTION== 0)
    {
      
      int inflooptest=0;

      bytesread=-1;
      while (bytesread==-1)
      {
        inflooptest++;

        if (inflooptest>10)
          debug_log("FUUU");



        //bytesread=pread(p->mem, buffer, size, (uintptr_t)lpAddress);

        lseek64(p->mem, (uintptr_t)lpAddress, SEEK_SET);
        bytesread=read(p->mem, buffer, size);


        if ((bytesread<0) && (errno!=EINTR))
        {
          /*
          debug_log("pread failed and not due to a signal: %d  (isdebugged=%d)\n", errno, isdebugged);
          if (isdebugged)
          {
            debug_log("event.threadid=%d devent=%d\n", (int)event.threadid, event.debugevent);
          }
          debug_log("lpAddress=%p\n", lpAddress);
          debug_log("size=%d\n", size);
          */

          bytesread=0;

          if (isdebugged)
          {
          // debug_log("trying to read from specific task\n");

            int f;
            char mempath[255];

            sprintf(mempath,"/proc/%d/task/%d/mem", p->pid, (int)event.threadid);
          // debug_log("Opening %s\n", mempath);
            f=open(mempath, O_RDONLY);
            debug_log("f=%d\n", f);
            if (f>=0)
            {
              //bytesread=pread(f, buffer, size, (uintptr_t)lpAddress);
              lseek64(p->mem, (uintptr_t)lpAddress, SEEK_SET);
              bytesread=read(p->mem, buffer, size);

              if ((bytesread<0) && (errno!=EINTR))
              {
              //  debug_log("Also failed on second try\n");
                bytesread=0;
              }
              close(f);
            }
          }


          break;
        }
      }
    }
    else
    {

      int offset=0;
      int max=size-sizeof(long int);

      long int *address = (long int *)buffer;

      long int value = 0;

      int is_readable = 1;

      while(offset<max)
      {
        errno = 0;
        value =  safe_ptrace(PTRACE_PEEKDATA, p->pid, (void*)((uintptr_t)lpAddress+offset), (void *)0);

        if(errno == 0)
        {
          *address = value;

          address++;
          offset+=sizeof(long int);

          bytesread+=sizeof(long int);
        }
        else
        {
          is_readable = 0;
          break;
        }
          
      }

      if(offset < size && is_readable)
      {
        errno = 0;
        value =  safe_ptrace(PTRACE_PEEKDATA, p->pid, (void*)((uintptr_t)lpAddress+offset), (void *)0);
        
        if(errno == 0)
        {
          memcpy(address,&value,size-offset);
          
          int i = size-offset;
          if(i>=0)
            bytesread+=size-offset;
        }     

      }
    }

    if (!isdebugged)
    {
      PThreadData td=GetThreadData(p, event.threadid);

      //if a SIGSTOP happened just continue it, else the next time WaitForDebugEvent() runs it will get this event from the queue
      if (event.debugevent==SIGSTOP)
      {

      //  debug_log("Continue from sigstop\n");

        safe_ptrace(PTRACE_CONT, event.threadid, 0,0);

        if (td)
          td->isPaused=0;


      }
      else
      {
        debug_log("ReadProcessMemoryDebug: Adding unexpected signal to eventqueue (event.debugevent=%d event.threadid=%d)\n", event.debugevent, event.threadid);

        if (td)
          td->isPaused=1;

        AddDebugEventToQueue(p, &event);

        debug_log("After add\n");

        VerboseLevel=1000000000;

      }
    }


  }
  else
  {

    int tid=p->pid; //p->threadlist[p->threadlistpos-1];
   // debug_log("ReadProcessMemoryDebug from outside the debuggerthread. Waking debuggerthread\n");

    //setup a rpm command
#pragma pack(1)
    struct
    {
      uint8_t command;
      uint32_t pHandle;
      uint64_t address;
      uint32_t size;
      uint8_t compressed;
    } rpm;
#pragma pack()


    rpm.command=CMD_READPROCESSMEMORY;
    rpm.pHandle=hProcess;
    rpm.address=(uintptr_t)lpAddress;
    rpm.size=size;
    rpm.compressed=0;

    //and write it to the p->debuggerthreadfd and read out the data
    //aquire lock (I don't want other threads messing with the client socket)
    if (pthread_mutex_lock(&debugsocketmutex) == 0)
    {
    //  debug_log("Sending message to the debuggerthread\n");

      sendall(p->debuggerClient, &rpm, sizeof(rpm), 0);


      //log=1;
      //printf("Waking debugger thread and waiting for result\n");
      WakeDebuggerThread();

      recvall(p->debuggerClient, &bytesread, sizeof(bytesread), MSG_WAITALL);

      if (VerboseLevel>10)
        debug_log("After waiting for debugger thread: bytesread=%d\n", bytesread);

      if (bytesread>0)
        recvall(p->debuggerClient, buffer, bytesread, MSG_WAITALL);


      pthread_mutex_unlock(&debugsocketmutex);
    }

    //bytesread=read...
  }


  if (VerboseLevel>10)
    debug_log("ReadProcessMemoryDebug returns %d\n", bytesread);

  return bytesread;
}

int ReadProcessMemory(HANDLE hProcess, void *lpAddress, void *buffer, int size)
{
  //idea in case this is too slow. always read a full page and keep the last 16 accessed pages.
  //only on cache miss, or if the cache is older than 1000 milliseconds fetch the page.
  //keep in mind that this routine can get called by multiple threads at the same time

  if (lpAddress==NULL) //don't even bother
    return 0;



  //printf("ReadProcessMemory\n");
  int bread=0;


  if (GetHandleType(hProcess) == htProcesHandle )
  { //valid handle
    //debug_log("ReadProcessMemory(%d, %p, %p, %d)  ATTACH_TO_ACCESS_MEMORY=%d MEMORY_SEARCH_OPTION=%d \n", (int)hProcess, lpAddress, buffer, size, ATTACH_TO_ACCESS_MEMORY, MEMORY_SEARCH_OPTION);

    PProcessData p=(PProcessData)GetPointerFromHandle(hProcess);

    //printf("hProcess=%d, lpAddress=%p, buffer=%p, size=%d\n", hProcess, lpAddress, buffer, size);

    if (p->isDebugged) //&& cannotdealwithotherthreads
    {
      //printf("This process is being debugged\n");
      //use the debugger specific readProcessMemory implementation
      return ReadProcessMemoryDebug(hProcess, p, lpAddress, buffer, size);
    }

    //printf("Read without debug\n");

    if ((MEMORY_SEARCH_OPTION == 2) && (process_vm_readv==NULL)) //user explicitly wants to use process_vm_readv but it's not available
      MEMORY_SEARCH_OPTION=0; //fallback to 0


    if (MEMORY_SEARCH_OPTION == 2)
    {
      struct iovec local;
      struct iovec remote;

      local.iov_base=buffer;
      local.iov_len=size;

      remote.iov_base=lpAddress;
      remote.iov_len=size;




      bread=process_vm_readv(p->pid,&local,1,&remote,1,0);
      if (bread==-1)
      {
       // debug_log("process_vm_readv(%x, %d) failed: %s\n", lpAddress, size, strerror(errno));
        bread=0;
      }

      return bread;
    }

    if (pthread_mutex_lock(&memorymutex) == 0)
    {

      {
        if ((ATTACH_TO_ACCESS_MEMORY==0) || (safe_ptrace(PTRACE_ATTACH, p->pid,0,0)==0))
        {
          int status;

        //  debug_log("Attach is 0 or attach is succesfull\n");

          pid_t pid=ATTACH_TO_ACCESS_MEMORY ? wait(&status) : p->pid;

        //  debug_log("after wait or skip\n");

          if (MEMORY_SEARCH_OPTION == 0)
          {
           // debug_log("MEMORY_SEARCH_OPTION == 0\n");
          //  debug_log("Reading p->mem\n");

            lseek64(p->mem, (uintptr_t)lpAddress, SEEK_SET);

            bread=read(p->mem, buffer, size);

           // debug_log("bread=%d\n",bread);
            if (bread==-1)
            {
              bread=0;
              //debug_log("pread error for address %p (error=%s) ", lpAddress, strerror(errno));
            }

          }
          else
          {
            
            //debug_log("MEMORY_SEARCH_OPTION != 0\n");

            int offset=0;
            int max=size-sizeof(long int);

            long int *address = (long int *)buffer;

            long int value = 0;

            int is_readable = 1;

            while(offset<max)
            {
              errno = 0;
              value =  safe_ptrace(PTRACE_PEEKDATA, pid, (void*)((uintptr_t)lpAddress+offset), (void *)0);

              if(errno == 0)
              {
                *address = value;

                address++;
                offset+=sizeof(long int);

                bread+=sizeof(long int);
              }
              else
              {
                is_readable = 0;
                break;
              }
                
            }

            if(offset < size && is_readable)
            {
              errno = 0;
              value =  safe_ptrace(PTRACE_PEEKDATA, pid, (void*)((uintptr_t)lpAddress+offset), (void *)0);
              
              if(errno == 0)
              {
                memcpy(address,&value,size-offset);
                
                int i = size-offset;
                if(i>=0)
                  bread+=size-offset;
              }     

            }
          
          }
          
          //printf("bread=%d size=%d\n", bread, size);
          
          if (ATTACH_TO_ACCESS_MEMORY)
            safe_ptrace(PTRACE_DETACH, pid,0,0);

        }
        else
          debug_log("ptrace attach failed (pid=%d). This system might not be properly rooted\n", p->pid);

      }

      pthread_mutex_unlock(&memorymutex);
    }
    else
      debug_log("For some reason I failed to obtain a lock\n");
  }
  //else
  //  debug_log("RPM: invalid handle\n");

 // debug_log("Returned from rpm\n");

  fflush(stdout);

  return bread;
}


DWORD ProtectionStringToType(char *protectionstring)
{
  if (strchr(protectionstring, 's'))
    return MEM_MAPPED;
  else
    return MEM_PRIVATE;
}

uint32_t ProtectionStringToProtection(char *protectionstring)
{
  int w,x;

  if (strchr(protectionstring, 'x'))
    x=1;
  else
    x=0;

  if (strchr(protectionstring, 'w'))
    w=1;
  else
    w=0;

  if (x)
  {
    //executable
    if (w)
      return PAGE_EXECUTE_READWRITE;
    else
      return PAGE_EXECUTE_READ;
  }
  else
  {
    //not executable
    if (w)
      return PAGE_READWRITE;
    else
      return PAGE_READONLY;
  }
}

void AddToRegionList(uint64_t base, uint64_t size, uint32_t type, uint32_t protection, RegionInfo **list, int *pos, int *max)
//helper function for VirtualQueryExFull to add new entries to the list
{
  //printf("Calling AddToRegionList\n");

  debug_log("++>%llx->%llx : (%llx)  - %d\n", (unsigned long long)base, (unsigned long long)base+size, (unsigned long long)size, type);

  (*list)[*pos].baseaddress=base;
  (*list)[*pos].size=size;
  (*list)[*pos].type=type;
  (*list)[*pos].protection=protection;

  (*pos)++;

  if (*pos>=*max)
  {
    debug_log("resize list\n");
    *max=(*max)*2;
    *list=(RegionInfo *)realloc(*list, sizeof(RegionInfo)*(*max));
  }

  //printf("Returning from AddToRegionList\n");
}

int VirtualQueryExFull(HANDLE hProcess, uint32_t flags, RegionInfo **rinfo, uint32_t *count)
/*
 * creates a full list of the maps file (less seeking)
 */
{
  int pagedonly=flags & VQE_PAGEDONLY;
  int dirtyonly=flags & VQE_DIRTYONLY;
  int noshared=flags & VQE_NOSHARED;

  debug_log("VirtualQueryExFull:\n");

  if (GetHandleType(hProcess) == htProcesHandle )
  {
    PProcessData p=(PProcessData)GetPointerFromHandle(hProcess);

    char smaps_name[64];
    char pagemap_name[64];

    sprintf(smaps_name,"/proc/%d/smaps", p->pid);
    sprintf(pagemap_name,"/proc/%d/pagemap", p->pid);

    FILE *maps=fopen(smaps_name, "r");
    int pagemap=-1;

    uint64_t *pagemap_entries=NULL;

    if (pagedonly)
    {
      debug_log("pagedonly\n");
      pagemap=open(pagemap_name, O_RDONLY);

     // debug_log("pagemap=%p\n", pagemap);


      pagemap_entries=(uint64_t *)malloc(512*8);

      debug_log("allocated pagemap_entries at %p\n", pagemap_entries);
    }


    if (maps && (!pagedonly || (pagemap>=0)))
    {


      unsigned long long start=0, stop=0;
      char protectionstring[25];
      char x[200];
      int pos=0, max=pagedonly?64:128;
      RegionInfo *r;

      debug_log("going to allocate r\n");
      r=(RegionInfo *)malloc(sizeof(RegionInfo)*max);

      debug_log("Allocated r at %p\n", r);

      int isdirty=0;
      int end=0;


      while (!end)
      {
        unsigned long long _start, _stop;
        char temp[25];

        end=fgets(x, 200, maps)==0;


        //make sure the stream gets to tne next line. (long modulepaths can cause an issue)

        //printf("%s\n",x);

        if (x[strlen(x)-1]!='\n')
        {
          //need to go to the end of line first

          char discard[100];

          do
          {
            discard[99]=0;
            fgets(discard, 99, maps);
          } while (discard[99]!=0);
        }

        temp[0]=0;
        sscanf(x, "%llx-%llx %24s", &_start, &_stop, temp);

        if ((end) || (temp[0]!=0))
        {
          //new entry
          if (start)
          {
            int passed=1;
            DWORD type=ProtectionStringToType(protectionstring);
            DWORD protection=ProtectionStringToProtection(protectionstring);

            //a block was being processed


            //some checks to see if it passed
            if (noshared && (type==MEM_MAPPED)) //
              passed=0;

            if (dirtyonly && !isdirty)
              passed=0;

            //writable only as well ?

            if (passed)
            {
              //printf("passed the initial check\n");
              if (pagedonly)
              {
                //only add the pages
                //todo: Add a dirtyonlyPlus which works in conjunction with softdirty pages (only works on newer kernels and requires a forced clear)
                uint64_t current=start;
                int pagesize;
#ifndef PAGESIZE
                pagesize=getpagesize();
#else
                pagesize=PAGESIZE;
#endif
                while (current<stop)
                {
                  int i;
                  int currentstart=-1;
                  off_t offset=(current / pagesize)*8;
                  size_t pagecount=(stop-current) / pagesize;
                  if (pagecount>512)
                    pagecount=512;

                  debug_log("-->%llx->%llx : %s (%llx)\n", start, stop, protectionstring, stop-start);

                  i=pread(pagemap, pagemap_entries, pagecount*8, offset);
                  if (i==-1)
                  {
                    debug_log("offset=%llx, pagecount=%d read=%d (%d) \n", (unsigned long long)offset, (int)pagecount, i/8, i);
                    //exit(12);
                  }
                  else
                  {
                    for (i=0; i<pagecount; i++)
                    {
                      if (pagemap_entries[i] >> 63) //present
                      {
                        if (currentstart==-1) //new entry
                          currentstart=i;

                        //total3+=pagesize;
                      }
                      else
                      {
                        if (currentstart!=-1)
                        {
                          //store this section (from currentstart to i-1)
                          AddToRegionList((uint64_t)(current+currentstart*pagesize), (i-currentstart)*pagesize, type, protection, &r, &pos, &max);
                        }

                        currentstart=-1;
                      }
                    }

                    if (currentstart!=-1)
                    {
                      //store this section (from currentstart to pagecount)
                      int count=pagecount-currentstart;
                      AddToRegionList((uint64_t)(current+currentstart*pagesize), count*pagesize, type, protection, &r, &pos, &max);
                    }

                    current+=pagecount*pagesize;
                  }


                }

              }
              else
                AddToRegionList(start, stop-start, type, protection, &r, &pos, &max);
            }



          }


          //declare a new clean block for the next iteration
          isdirty=0;
          strcpy(protectionstring, temp);
          start=_start;
          stop=_stop;
        }
        else
        {
          //one of the info lines

          if (dirtyonly && (start!=0))
          {
            //figure out if it's something I am interested in (dirty pages)
            int i;
            int number;
            char name[32];
            i=sscanf(x, "%31[^:]: %d", name, &number);
            if (i==2)
            {
              if ((number>0) && ((strcmp(name,"Shared_Dirty")==0) || (strcmp(name,"Private_Dirty")==0))) //the value is >0 and it's information about the dirty state
                isdirty=1;
            }


          }

        }


      }

     // debug_log("End of loop\n");

      if (maps)
        fclose(maps);

      if (pagemap>=0)
        close(pagemap);


      *count=pos;
      *rinfo=r;

      if (pagemap_entries)
        free(pagemap_entries);

      fflush(stdout);

      return 1;
    }
    else
    {
      debug_log("Failure maps=%p pagemap=%d\n", maps, pagemap);


      if (maps)
        fclose(maps);
      else
        debug_log("Failure opening /proc/%d/smaps", p->pid);

      if (pagemap>=0)
        close(pagemap);
      else
        debug_log("Failure opening /proc/%d/pagemap", p->pid);

      fflush(stdout);

      return 0;
    }

  }
  else
    return 0;

}

int VirtualQueryEx(HANDLE hProcess, void *lpAddress, PRegionInfo rinfo, char *mapsline)
{
  /*
   * Alternate method: read pagemaps and look up the pfn in /proc/kpageflags (needs to 2 files open and random seeks through both files, so not sure if slow or painfully slow...)
   */

  //VirtualQueryEx stub port. Not a real port, and returns true if successful and false on error
  int found=0;

  //printf("VirtualQueryEx(%p)", lpAddress);

  if (GetHandleType(hProcess) == htProcesHandle )
  {
    PProcessData p=(PProcessData)GetPointerFromHandle(hProcess);
    FILE *maps=fopen(p->maps, "r");
    if (maps)
    {
      char x[200];
      //printf("Opened %s\n", p->maps);


      rinfo->protection=0;
      rinfo->baseaddress=(uintptr_t)lpAddress & ~0xfff;
      lpAddress=(void *)(uintptr_t)(rinfo->baseaddress);

      while (fgets(x, 200, maps) )
      {
        unsigned long long start=0, stop=0;
        char protectionstring[25];

        x[199]=0;

        if (x[strlen(x)-1]!='\n')
        {
          char discard[100];

          do
          {
            discard[99]=0;
            fgets(discard, 99, maps);
          } while (discard[99]!=0);
        }




        sscanf(x, "%llx-%llx %s", &start, &stop, protectionstring);
       // debug_log("%llx - %llx : %s\n", start,stop, protectionstring);

        if (stop > ((uintptr_t)lpAddress) ) //we passed it
        {
          found=1;

          if (((uintptr_t)lpAddress) >= start )
          {
            //it's inside the region, so useable

            rinfo->protection=ProtectionStringToProtection(protectionstring);
            rinfo->type=ProtectionStringToType(protectionstring);
            rinfo->size=stop-rinfo->baseaddress;
          }
          else
          {
            rinfo->size=start-rinfo->baseaddress;
            rinfo->protection=PAGE_NOACCESS;
            rinfo->type=0;
          }

          if (mapsline!=NULL)
            strcpy(mapsline, x);

          break;
        }
      }

      fclose(maps);

      return found;
    }
    else
    {
      debug_log("failed opening %s\n", p->maps);
    }
  }

  return 0;
}

int SearchHandleListProcessCallback(PProcessData data, int *pid)
/*
 * Callback. Called during the call to SearchHandleList
 * Searchdata is a pointer to the processid to be looked for
 */
{
  return (data->pid==*pid);
}

HANDLE OpenProcess(DWORD pid)
{
  //check if the process exists
  char processpath[100];
  int handle;
  sprintf(processpath, "/proc/%d/", pid);


  //check if this process has already been opened
  handle=SearchHandleList(htProcesHandle, SearchHandleListProcessCallback, &pid);
  if (handle)
  {
   // debug_log("Already opened. Returning same handle\n");
    PProcessData p=(PProcessData)GetPointerFromHandle(handle);
    p->ReferenceCount++;
    return handle;
  }

  //still here, so not opened yet


  if (chdir(processpath)==0)
  {
    //success

    //create a process info structure and return a handle to it
    PProcessData p=(PProcessData)malloc(sizeof(ProcessData));

    memset(p, 0, sizeof(ProcessData));

    p->ReferenceCount=1;
    p->pid=pid;
    p->path=strdup(processpath);

    sprintf(processpath,"/proc/%d/maps", pid);
    p->maps=strdup(processpath);

    if(MEMORY_SEARCH_OPTION == 0)
    {
      debug_log("Opening memory access\n");
      sprintf(processpath,"/proc/%d/mem", pid);

      p->memrw=1;
      p->mem=open(processpath, O_RDWR);
      if (p->mem==-1)
      {
        debug_log("Failure opening %s for RW access because of :%s\nTrying RO: ",processpath, strerror(errno));
        p->mem=open(processpath, O_RDONLY);
        p->memrw=0;

        if (p->mem==-1)
        {
          debug_log("Also failed\n");
        }
        else
        {
          debug_log("Success. ReadOnly access.  Use an alternate write access routine\n");
          ATTACH_TO_WRITE_MEMORY=1;
        }
      }
    }
    

    pthread_mutex_init(&p->extensionMutex, NULL);
    p->hasLoadedExtension=0;
    p->extensionFD=0;


    p->isDebugged=0;

    p->threadlistmax=0;
    p->threadlistpos=0;
    p->threadlist=NULL;

    p->debuggedThreadEvent.threadid=0;

    pthread_mutex_init(&p->debugEventQueueMutex, NULL);

    /*
    p->queuedDebugEventList=NULL;
    p->queuedDebugEventListPos=0;
    p->queuedDebugEventListMax=0;
    */

    TAILQ_INIT(&p->debugEventQueue);

    HANDLE result=CreateHandleFromPointer(p, htProcesHandle);

    debug_log("Getting processdata for TH32CS_SNAPFIRSTMODULE\n");

    HANDLE ths=CreateToolhelp32Snapshot(TH32CS_SNAPFIRSTMODULE, p->pid);
    if (ths)
    {
      ModuleListEntry mle;
      if (Module32First(ths, &mle))
      {
        p->is64bit=mle.is64bit;

        if (p->is64bit)
          debug_log("The opened process is 64-bit\n");
        else
          debug_log("The opened process is 32-bit\n");
      }
      else
      {
        debug_log("Module32First returned false\n"); //fall back on what ceserver is
#if defined(__aarch64__) || defined(__x86_64__)
        p->is64bit=1;
#else
        p->is64bit=0;
#endif
      }

      CloseHandle(ths);
    }
    else
    {
      debug_log("Failed creating toolhelp snapshot for firstmodule\n");

    }



    return result;
  }
  else
    return 0; //could not find the process

}


BOOL Process32Next(HANDLE hSnapshot, PProcessListEntry processentry)
{
  //get the current iterator of the list and increase it. If the max has been reached, return false
 // debug_log("Process32Next\n");

  if (GetHandleType(hSnapshot) == htTHSProcess)
  {
    PProcessList pl=(PProcessList)GetPointerFromHandle(hSnapshot);

    if (pl->processListIterator<pl->processCount)
    {
      processentry->PID=pl->processList[pl->processListIterator].PID;
      processentry->ProcessName=pl->processList[pl->processListIterator].ProcessName; //no need to copy
      pl->processListIterator++;

      return TRUE;
    }
    else
      return FALSE;
  }
  else
    return FALSE;
}




BOOL Process32First(HANDLE hSnapshot, PProcessListEntry processentry)
{
  //Get a processentry from the processlist snapshot. fill the given processentry with the data.

 // debug_log("Process32First\n");
  if (GetHandleType(hSnapshot) == htTHSProcess)
  {
    PProcessList pl=(PProcessList)GetPointerFromHandle(hSnapshot);
    pl->processListIterator=0;
    return Process32Next(hSnapshot, processentry);
  }
  else
    return FALSE;
}


BOOL Module32Next(HANDLE hSnapshot, PModuleListEntry moduleentry)
{
  //get the current iterator of the list and increase it. If the max has been reached, return false
 // debug_log("Module32First/Next(%d)\n", hSnapshot);

  if (GetHandleType(hSnapshot) == htTHSModule)
  {
    PModuleList ml=(PModuleList)GetPointerFromHandle(hSnapshot);

    if (ml->moduleListIterator<ml->moduleCount)
    {
      moduleentry->baseAddress=ml->moduleList[ml->moduleListIterator].baseAddress;
      moduleentry->moduleName=ml->moduleList[ml->moduleListIterator].moduleName;
      moduleentry->moduleSize=ml->moduleList[ml->moduleListIterator].moduleSize;
      moduleentry->part=ml->moduleList[ml->moduleListIterator].part;
      moduleentry->is64bit=ml->moduleList[ml->moduleListIterator].is64bit;

      ml->moduleListIterator++;

      //printf("Module32First/Next: Returning %s size %x\n", moduleentry->moduleName, moduleentry->moduleSize);

      return TRUE;
    }
    else
    {
      debug_log("Module32First/Next: Returning false because ml->moduleListIterator=%d and ml->moduleCount=%d\n", ml->moduleListIterator, ml->moduleCount);
      return FALSE;
    }
  }
  else
  {
    //debug_log("Module32First/Next: GetHandleType(hSnapshot)=%d\n",GetHandleType(hSnapshot));
    return FALSE;
  }
}


BOOL Module32First(HANDLE hSnapshot, PModuleListEntry moduleentry)
{

  //printf("Module32First\n");
  if (GetHandleType(hSnapshot) == htTHSModule)
  {
    PModuleList ml=(PModuleList)GetPointerFromHandle(hSnapshot);
    ml->moduleListIterator=0;
    return Module32Next(hSnapshot, moduleentry);
  }
  else
  {
    debug_log("Module32First error. Handle is not a THSModule handle\n");
    return FALSE;
  }
}


HANDLE CreateToolhelp32Snapshot(DWORD dwFlags, DWORD th32ProcessID)
{

  if (dwFlags & TH32CS_SNAPPROCESS)
  {
    //create a processlist which process32first/process32next will make use of. Not often called so you may make it as slow as you wish
    int max=2048;
    PProcessList pl=(PProcessList)malloc(sizeof(ProcessList));

    //printf("Creating processlist\n");

    pl->ReferenceCount=1;
    pl->processCount=0;
    pl->processList=(PProcessListEntry)malloc(sizeof(ProcessListEntry)* max);

    DIR *procfolder=opendir("/proc/");

    struct dirent *currentfile;

    while ((currentfile=readdir(procfolder)) != NULL)
    {

      if (strspn(currentfile->d_name, "1234567890")==strlen(currentfile->d_name))
      {
        int pid;
        char exepath[200];
        char processpath[512];
        snprintf(exepath, 200, "/proc/%s/exe", currentfile->d_name);
        exepath[199]=0; //'should' not be needed in linux, but I read that microsoft is an asshole with this function

        int i=readlink(exepath, processpath, 254);
        if (i != -1)
        {
          char extrafile[255];
          int f;

          if (i>254)
            i=254;

          processpath[i]=0;

          snprintf(extrafile, 255, "/proc/%s/cmdline", currentfile->d_name);
          extrafile[254]=0;

          f=open(extrafile, O_RDONLY);
          if (f!=-1)
          {
            i=read(f, extrafile, 255);
            if (i>=0)
              extrafile[i]=0;
            else
              extrafile[0]=0;

            strcat(processpath," ");
            strcat(processpath,extrafile);

            close(f);
          }


          sscanf(currentfile->d_name, "%d", &pid);
         // debug_log("%d - %s\n", pid, processpath);

          //add this process to the list
          pl->processList[pl->processCount].PID=pid;
          pl->processList[pl->processCount].ProcessName=strdup(processpath);

          pl->processCount++;

          if (pl->processCount>=max)
          {
            max=max*2;
            pl->processList=(PProcessListEntry)realloc(pl->processList, max*sizeof(ProcessListEntry));
          }
        }


      }

    }

    closedir(procfolder);

    return CreateHandleFromPointer(pl, htTHSProcess);
  }
  else
  if (((dwFlags & TH32CS_SNAPMODULE) || (dwFlags & TH32CS_SNAPFIRSTMODULE) ) && (ATTACH_PID == 0 ||(ATTACH_PID != 0 && (th32ProcessID == ATTACH_PID))))
  {
    //make a list of all the modules loaded by processid th32ProcessID
    //the module list
    int max=64;
    char mapfile[255];
    FILE *f=NULL;
    snprintf(mapfile, 255, "/proc/%d/maps", th32ProcessID);

    PModuleList ml=(PModuleList)malloc(sizeof(ModuleList));

    if (dwFlags & TH32CS_SNAPFIRSTMODULE)
      debug_log("Creating module list for process %d\n", th32ProcessID);


    ml->ReferenceCount=1;
    ml->moduleCount=0;
    ml->moduleList=(PModuleListEntry)malloc(sizeof(ModuleListEntry)*max);


    f=fopen(mapfile, "r");


    if (f)
    {
      char s[512];
      memset(s, 0, 512);

      PModuleListEntry mle=NULL;
      int phandle=OpenProcess(th32ProcessID);



      while (fgets(s, 511, f)) //read a line into s
      {

        char *currentModule;
        unsigned long long start, stop;
        char memoryrange[64],protectionstring[32],modulepath[511];
        unsigned char elfident[8];

        modulepath[0]='\0';
        memset(modulepath, 0, 255);


        sscanf(s, "%llx-%llx %s %*s %*s %*s %[^\t\n]\n", &start, &stop, protectionstring, modulepath);

        if (ProtectionStringToType(protectionstring)==MEM_MAPPED)
          continue;

        if (modulepath[0]) //it's something
        {
          int i;
          if (strcmp(modulepath, "[heap]")==0)  //not static enough to mark as a 'module'
            continue;

         // debug_log("%s\n", modulepath);

          if (strcmp(modulepath, "[vdso]")!=0)  //temporary patch as to not rename vdso, because it is treated differently by the ce symbol loader
          {
            for (i=0; modulepath[i]; i++) //strip square brackets from the name (conflicts with pointer notations)
            {
              if ((modulepath[i]=='[') || (modulepath[i]==']'))
                modulepath[i]='_';
            }
          }

          //new module, or not linkable

          mle=NULL;


          //check if it's readable
          i=ReadProcessMemory(phandle, (void *)start, elfident, 8); //only the first few bytes
          if (i==0)
          {
            //printf("%s is unreadable(%llx)\n", modulepath, start);
            continue; //unreadable
          }

          //check if this module is in the list. If so, mark it with a part tag
          int part=0;

          //this is going to be slower than the original implementation. But lets assume cpu's have gotten faster by now

          for (i=ml->moduleCount-1; i>=0; i--)
          {
            if (strcmp(ml->moduleList[i].moduleName, modulepath)==0)
            {
              part=ml->moduleList[i].part+1;
              break;
            }
          }

          if ((part==0) && ((elfident[0]!=ELFMAG0) || (elfident[1]!=ELFMAG1) || (elfident[2]!=ELFMAG2) || (elfident[3]!=ELFMAG3) ) )  //  7f 45 4c 46 , not yet in the list, and not an ELF
          {
            //printf("%s is not an ELF(%llx).  tempbuf=%s\n", modulepath, start, tempbuf);
            continue; //not an ELF
          }

          //it's either an ELF, or there is another entry with this name in the list that is an ELF

          if (dwFlags & TH32CS_SNAPFIRSTMODULE)
            debug_log("Adding %s as a module\n", modulepath);

          mle=&ml->moduleList[ml->moduleCount];
          mle->moduleName=strdup(modulepath);
          mle->baseAddress=start;
          mle->moduleSize=stop-start; //GetModuleSize(modulepath, 0); GetModuleSize is not a good idea as some modules have gaps in them, and alloc will use those gaps (e.g ld*.so)
          mle->part=part;

          if (part==0)
            mle->is64bit=elfident[EI_CLASS]==ELFCLASS64; //else is64bit is invalid


        //  debug_log("Setting size of %s to %x\n", modulepath, mle->moduleSize);

          ml->moduleCount++;

          if (ml->moduleCount>=max)
          {
            //printf("reallocate modulelist\n");
            max=max*2;
            ml->moduleList=(PModuleListEntry)realloc(ml->moduleList, max* sizeof(ModuleListEntry));
          }

          if (dwFlags & TH32CS_SNAPFIRSTMODULE)
            break;


        }
        else
          mle=NULL;



      }

      CloseHandle(phandle);

      fclose(f);

      return CreateHandleFromPointer(ml, htTHSModule);
    }
    else
    {
      if (dwFlags & TH32CS_SNAPFIRSTMODULE)
        debug_log("Failed opening %s\n", mapfile);

      return 0;
    }



  }


  return 0;
}

void CloseHandle(HANDLE h)
{
  int i;
  handleType ht=GetHandleType(h);

 // debug_log("CloseHandle(%d)\n", h);
  if (ht==htTHSModule)
  {
    ModuleList *ml=(PModuleList)GetPointerFromHandle(h);
    ml->ReferenceCount--;
    if (ml->ReferenceCount<=0)
    {
      //free all the processnames in the list
      for (i=0; i<ml->moduleCount; i++)
        free(ml->moduleList[i].moduleName);

      free(ml->moduleList); //free the list
      free(ml); //free the descriptor

      RemoveHandle(h);
    }

  }
  if (ht==htTHSProcess)
  {
    ProcessList *pl=(PProcessList)GetPointerFromHandle(h);


    pl->ReferenceCount--;

    if (pl->ReferenceCount<=0)
    {
      //free all the processnames in the list
      for (i=0; i<pl->processCount; i++)
        free(pl->processList[i].ProcessName);

      free(pl->processList); //free the list
      free(pl); //free the descriptor

      RemoveHandle(h);
    }
  }
  else
  if (ht==htProcesHandle)
  {
    PProcessData pd=(PProcessData)GetPointerFromHandle(h);

    pd->ReferenceCount--;
    if (pd->ReferenceCount<=0)
    {
      free(pd->maps);
      free(pd->path);
      close(pd->mem);
      free(pd);

      RemoveHandle(h);
    }
  }
  else
  if (ht==htNativeThreadHandle)
  {
    uint64_t *th=GetPointerFromHandle(h);
    debug_log("Closing thread handle\n");

    free(th);
    RemoveHandle(h);
  }
  else
    RemoveHandle(h); //no idea what it is...


}

void initAPI()
{
  pthread_mutex_init(&memorymutex, NULL);
  pthread_mutex_init(&debugsocketmutex, NULL);

  sem_init(&sem_DebugThreadEvent, 0, 0); //locked by default

  void *libc=dlopen("libc.so",RTLD_NOW);

  if (libc)
  {
    process_vm_readv=dlsym(libc,"process_vm_readv");
    process_vm_writev=dlsym(libc,"process_vm_writev");
  }

  debug_log("process_vm_readv=%p\n",process_vm_readv);
  debug_log("process_vm_writev=%p\n",process_vm_writev);

}
