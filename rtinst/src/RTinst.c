/*
 * Copyright (c) 1996 Barton P. Miller
 * 
 * We provide the Paradyn Parallel Performance Tools (below
 * described as Paradyn") on an AS IS basis, and do not warrant its
 * validity or performance.  We reserve the right to update, modify,
 * or discontinue this software at any time.  We shall have no
 * obligation to supply such updates or modifications or any other
 * form of support to you.
 * 
 * This license is for research uses.  For such uses, there is no
 * charge. We define "research use" to mean you may freely use it
 * inside your organization for whatever purposes you see fit. But you
 * may not re-distribute Paradyn or parts of Paradyn, in any form
 * source or binary (including derivatives), electronic or otherwise,
 * to any other organization or entity without our permission.
 * 
 * (for other uses, please contact us at paradyn@cs.wisc.edu)
 * 
 * All warranties, including without limitation, any warranty of
 * merchantability or fitness for a particular purpose, are hereby
 * excluded.
 * 
 * By your use of Paradyn, you understand and agree that we (or any
 * other person or entity with proprietary rights in Paradyn) are
 * under no obligation to provide either maintenance services,
 * update services, notices of latent defects, or correction of
 * defects for Paradyn.
 * 
 * Even if advised of the possibility of such damages, under no
 * circumstances shall we (or any other person or entity with
 * proprietary rights in the software licensed hereunder) be liable
 * to you or any third party for direct, indirect, or consequential
 * damages of any character regardless of type of action, including,
 * without limitation, loss of profits, loss of use, loss of good
 * will, or computer failure or malfunction.  You agree to indemnify
 * us (and any other person or entity with proprietary rights in the
 * software licensed hereunder) for any and all liability it may
 * incur to third parties resulting from your use of Paradyn.
 */


/************************************************************************
 *
 * RTinst.c: platform independent runtime instrumentation functions
 *
 ************************************************************************/

#ifdef SHM_SAMPLING
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#endif

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <memory.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "kludges.h"
#include "rtinst/h/rtinst.h"
#include "rtinst/h/trace.h"
#include "util/h/sys.h"

#if defined(sparc_sun_solaris2_4)
#include <thread.h>
#endif

#ifdef PARADYN_MPI
#include "/usr/lpp/ppe.poe/include/mpi.h"
//#include <mpi.h>
#endif

#if defined(SHM_SAMPLING)
extern void makeNewShmSegCopy();
#endif

#if defined(SHM_SAMPLING) && defined(MT_THREAD)
void DYNINST_initialize_hash(unsigned total);
void DYNINST_initialize_free(unsigned total);
unsigned DYNINST_hash_insert(unsigned k);
unsigned DYNINST_hash_lookup(unsigned k);
unsigned DYNINST_initialize_done=0;
#endif

/* sunos's header files don't have these: */
#ifdef sparc_sun_sunos4_1_3
extern int socket(int, int, int);
extern int connect(int, struct sockaddr *, int);
extern int fwrite(void *, int, int, FILE *);
extern int setvbuf(FILE *, char *, int, int);
#ifdef SHM_SAMPLING
extern int shmget(key_t, unsigned, unsigned);
extern void *shmat(int, void *, int);
extern int shmdt(void*);
extern int shmctl(int, int, struct shmid_ds *);
#endif
#endif

extern void   DYNINSTos_init(int calledByFork, int calledByAttach);
extern time64 DYNINSTgetCPUtime(void);
extern time64 DYNINSTgetWalltime(void);

/* platform dependent functions */
extern void DYNINSTbreakPoint(void);
extern void DYNINST_install_ualarm(unsigned interval);
extern void DYNINSTinitTrace(int);
extern void DYNINSTflushTrace(void);
extern int DYNINSTwriteTrace(void *, unsigned);
extern void DYNINSTcloseTrace(void);
extern void *DYNINST_shm_init(int, int, int *);

void DYNINSTprintCost(void);
int DYNINSTTGroup_CreateUniqueId(int);

/************************************************************************/

static float  DYNINSTsamplingRate   = 0;
static int    DYNINSTtotalSamples   = 0;
static tTimer DYNINSTelapsedCPUTime;
static tTimer DYNINSTelapsedTime;

static int DYNINSTnumReported = 0;
static time64 startWall = 0;
static float DYNINSTcyclesToUsec  = 0;
static time64 DYNINSTtotalSampleTime = 0;

/************************************************************************/

#define DYNINSTTagsLimit      1000
#define DYNINSTTagGroupsLimit 100
#define DYNINSTNewTagsLimit   50 // don't want to overload the system

typedef struct DynInstTag_st {
  int   TagGroupId;
  int   TGUniqueId;
  int   NumTags;
  int   TagTable[DYNINSTTagsLimit];

  struct DynInstTag_st* Next;
} DynInstTagSt;

typedef struct {
  int            TagHierarchy; /* True if hierarchy, false single level */
  int            NumGroups;     /* Number of groups, tag arrays */
  /* Group table, each index pointing to an array of tags */
  DynInstTagSt*  GroupTable[DYNINSTTagGroupsLimit]; 

  /* [0] is tagId, [1] is groupId, [2] is 1 if it is a new group else 0 */
  int            NewTags[DYNINSTNewTagsLimit][3];
  int            NumNewTags;
} DynInstTagGroupSt;

static DynInstTagGroupSt  TagGroupInfo;

/************************************************************************/

#ifdef SHM_SAMPLING
/* these vrbles are global so that fork() knows the attributes of the
   shm segs we've been using */
int DYNINST_shmSegKey;
int DYNINST_shmSegNumBytes;
int DYNINST_shmSegShmId; /* needed? */
void *DYNINST_shmSegAttachedPtr;
#endif
static int the_paradyndPid; /* set in DYNINSTinit(); pass to connectToDaemon();
			       needed if we fork */
#ifndef SHM_SAMPLING
static int DYNINSTin_sample = 0;
#endif

static const double MILLION = 1000000.0;

#ifdef USE_PROF
int DYNINSTbufsiz;
int DYNINSTprofile;
int DYNINSTprofScale;
int DYNINSTtoAddr;
short *DYNINSTprofBuffer;
#endif


#ifdef COSTTEST
time64 DYNINSTtest[10]={0,0,0,0,0,0,0,0,0,0};
int DYNINSTtestN[10]={0,0,0,0,0,0,0,0,0,0};
#endif

double DYNINSTdata[SYN_INST_BUF_SIZE/sizeof(double)];
/* As DYNINSTinit() completes, it has information to pass back
   to paradynd.  The data can differ; more stuff is needed
   when SHM_SAMPLING is defined, for example.  But in any event,
   the following gives us a general framework.  When DYNINSTinit()
   is about to complete, we used to send a TR_START trace record back and then
   DYNINSTbreakPoint().  But we've seen strange behavior; sometimes
   the breakPoint is received by paradynd first.  The following should
   work all the time:  DYNINSTinit() writes to the following vrble
   and does a DYNINSTbreakPoint(), instead of sending a trace
   record.  Paradynd reads this vrble using ptrace(), and thus has
   the info that it needs.
   Note: we use this framework for DYNINSTfork(), too -- so the TR_FORK
   record is now obsolete, along with the TR_START record.
   Additionally, we use this framework for DYNINSTexec() -- so the
   TR_EXEC record is now obsolete, too */
struct DYNINST_bootstrapStruct DYNINST_bootstrap_info;


#ifndef SHM_SAMPLING
/* This could be static, but gdb has can't find them if they are.  
   jkh 5/8/95 */
int64    DYNINSTvalue = 0;
unsigned DYNINSTlastLow;
unsigned DYNINSTobsCostLow;
#endif

#define N_FP_REGS 33

volatile int DYNINSTsampleMultiple    = 1;
   /* written to by dynRPC::setSampleRate() (paradynd/dynrpc.C)
      (presumably, upon a fold) */

#ifndef SHM_SAMPLING
static int          DYNINSTnumSampled        = 0;
static int          DYNINSTtotalAlarmExpires = 0;
#endif

/************************************************************************
 * void DYNINSTstartProcessTimer(tTimer* timer)
************************************************************************/
void
DYNINSTstartProcessTimer(tTimer* timer) {
    /* WARNING: write() could be instrumented to call this routine, so to avoid
       some serious infinite recursion, avoid calling anything that might directly
       or indirectly call write() in this routine; e.g. printf()!!!!! */

#ifdef COSTTEST
    time64 startT,endT;
#endif

#ifndef SHM_SAMPLING
    /* if "write" is instrumented to start/stop timers, then a timer could be
       (incorrectly) started/stopped every time a sample is written back */

    if (DYNINSTin_sample)
       return;
#endif

#ifdef COSTTEST
    startT=DYNINSTgetCPUtime();
#endif

/* For shared-mem sampling only: bump protector1, do work, then bump protector2 */
#ifdef SHM_SAMPLING
    assert(timer->protector1 == timer->protector2);
    timer->protector1++;
    /* How about some kind of inline asm that flushes writes when the architecture
       is using some kind of relaxed multiprocessor consistency? */
#endif

    /* Note that among the data vrbles, counter is incremented last; in particular,
       after start has been written.  This avoids a nasty little race condition in
       sampling where count is 1 yet start is undefined (or using an old value) when
       read, which usually leads to a rollback.  --ari */
    if (timer->counter == 0) {
        timer->start     = DYNINSTgetCPUtime();
    }
    timer->counter++;

#ifdef SHM_SAMPLING
    timer->protector2++; /* alternatively, timer->protector2 = timer->protector1 */
    assert(timer->protector1 == timer->protector2);
#else
    timer->normalize = MILLION; /* I think this vrble is obsolete & can be removed */
#endif


#ifdef COSTTEST
    endT=DYNINSTgetCPUtime();
    DYNINSTtest[0]+=endT-startT;
    DYNINSTtestN[0]++;
#endif
}


/************************************************************************
 * void DYNINSTstopProcessTimer(tTimer* timer)
************************************************************************/
void
DYNINSTstopProcessTimer(tTimer* timer) {
    /* WARNING: write() could be instrumented to call this routine, so to avoid
       some serious infinite recursion, avoid calling anything that might directly
       or indirectly call write() in this routine; e.g. printf()!!!!! */

#ifdef COSTTEST
    time64 startT,endT;
#endif

#ifndef SHM_SAMPLING
    /* if "write" is instrumented to start/stop timers, then a timer could be
       (incorrectly) started/stopped every time a sample is written back */

    if (DYNINSTin_sample)
       return;       
#endif

#ifdef COSTTEST
    startT=DYNINSTgetCPUtime();
#endif

#ifdef SHM_SAMPLING
    assert(timer->protector1 == timer->protector2);
    timer->protector1++;

    if (timer->counter == 0)
       ; /* a strange condition; shouldn't happen.  Should we make it an assert fail? */
    else {
       if (timer->counter == 1) {
          const time64 now = DYNINSTgetCPUtime();
          timer->total += (now - timer->start);

          if (now < timer->start) {
	     fprintf(stderr, "rtinst: cpu timer rollback.\n");
	     abort();
	  }
       }
       timer->counter--;
    }

    timer->protector2++; /* alternatively, timer->protector2=timer->protector1 */
    assert(timer->protector1 == timer->protector2);
#else
    if (timer->counter == 0)
       ; /* should we make this an assert fail? */
    else if (timer->counter == 1) {
        time64 now = DYNINSTgetCPUtime();

        timer->snapShot = timer->total + (now - timer->start);

        timer->mutex    = 1;
        /*                 
         * The reason why we do the following line in that way is because
         * a small race condition: If the sampling alarm goes off
         * at this point (before timer->mutex=1), then time will go backwards 
         * the next time a sample is take (if the {wall,process} timer has not
         * been restarted).
	 *
         */

        timer->total = DYNINSTgetCPUtime() - timer->start + timer->total; 

        if (now < timer->start) {
            printf("id=%d, snapShot=%f total=%f, \n start=%f  now=%f\n",
                   timer->id.id, (double)timer->snapShot,
                   (double)timer->total, 
                   (double)timer->start, (double)now);
            printf("process timer rollback\n"); fflush(stdout);

            abort();
        }
        timer->counter = 0;
        timer->mutex = 0;
    }
    else {
      timer->counter--;
    }
#endif

#ifdef COSTTEST
    endT=DYNINSTgetCPUtime();
    DYNINSTtest[1]+=endT-startT;
    DYNINSTtestN[1]++;
#endif 
}





/************************************************************************
 * void DYNINSTstartWallTimer(tTimer* timer)
************************************************************************/
void
DYNINSTstartWallTimer(tTimer* timer) {
    /* WARNING: write() could be instrumented to call this routine, so to avoid
       some serious infinite recursion, avoid calling anything that might directly
       or indirectly call write() in this routine; e.g. printf()!!!!! */

#ifdef COSTTEST
    time64 startT, endT;
#endif

#ifndef SHM_SAMPLING
    /* if "write" is instrumented to start/stop timers, then a timer could be
       (incorrectly) started/stopped every time a sample is written back */

    if (DYNINSTin_sample) {
       return;
    }
#endif

#ifdef COSTTEST
    startT=DYNINSTgetCPUtime();
#endif

#ifdef SHM_SAMPLING
    assert(timer->protector1 == timer->protector2);
    timer->protector1++;
#endif

    /* Note that among the data vrbles, counter is incremented last; in particular,
       after start has been written.  This avoids a nasty little race condition in
       sampling where count is 1 yet start is undefined (or using an old value) when
       read, which usually leads to a rollback.  --ari */
    if (timer->counter == 0) {
        timer->start     = DYNINSTgetWalltime();
    }
    timer->counter++;

#ifdef SHM_SAMPLING
    timer->protector2++; /* or, timer->protector2 = timer->protector1 */
    assert(timer->protector1 == timer->protector2);
#else
    timer->normalize = MILLION; /* I think this vrble is obsolete & can be removed */
#endif

#ifdef COSTTEST
    endT=DYNINSTgetCPUtime();
    DYNINSTtest[2]+=endT-startT;
    DYNINSTtestN[2]++;
#endif 
}


/************************************************************************
 * void DYNINSTstopWallTimer(tTimer* timer)
************************************************************************/
void
DYNINSTstopWallTimer(tTimer* timer) {
#ifdef COSTTEST
    time64 startT, endT;
#endif

#ifndef SHM_SAMPLING
    /* if "write" is instrumented to start timers, a timer could be started */
    /* when samples are being written back */

    if (DYNINSTin_sample)
       return;
#endif

#ifdef COSTTEST
    startT=DYNINSTgetCPUtime();
#endif

#ifdef SHM_SAMPLING
    assert(timer->protector1 == timer->protector2);
    timer->protector1++;

    if (timer->counter == 0)
       ; /* a strange condition; should we make it an assert fail? */
    else if (--timer->counter == 0) {
       const time64 now = DYNINSTgetWalltime();

       timer->total += (now - timer->start);

       if (now < timer->start) {
	  fprintf(stderr, "rtinst wall timer rollback.\n");
	  abort();
       }
    }
    
    timer->protector2++; /* or, timer->protector2 = timer->protector1 */
    assert(timer->protector1 == timer->protector2);
#else
    if (timer->counter == 0)
       ; /* a strange condition; should we make it an assert fail? */
    else if (timer->counter == 1) {
        time64 now = DYNINSTgetWalltime();

        timer->snapShot = now - timer->start + timer->total;
        timer->mutex    = 1;
        /*                 
         * The reason why we do the following line in that way is because
         * a small race condition: If the sampling alarm goes off
         * at this point (before timer->mutex=1), then time will go backwards 
         * the next time a sample is take (if the {wall,process} timer has not
         * been restarted).
         */
        timer->total    = DYNINSTgetWalltime() - timer->start + timer->total;
        if (now < timer->start) {
            printf("id=%d, snapShot=%f total=%f, \n start=%f  now=%f\n",
                   timer->id.id, (double)timer->snapShot,
                   (double)timer->total, 
                   (double)timer->start, (double)now);
            printf("wall timer rollback\n"); 
            fflush(stdout);

            abort();
        }
        timer->counter  = 0;
        timer->mutex    = 0;
    }
    else {
        timer->counter--;
    }
#endif

#ifdef COSTTEST
    endT=DYNINSTgetCPUtime();
    DYNINSTtest[3]+=endT-startT;
    DYNINSTtestN[3]++;
#endif 
}



/************************************************************************
 * float DYNINSTcyclesPerSecond(void)
 *
 * need a well-defined method for finding the CPU cycle speed
 * on each CPU.
************************************************************************/
#if defined(i386_unknown_nt4_0)
#define NOPS_4  { __asm add eax, 0 __asm add eax, 0 __asm add eax, 0 __asm add eax, 0 }
#elif defined(rs6000_ibm_aix4_1)
#define NOPS_4  asm("oril 0,0,0"); asm("oril 0,0,0"); asm("oril 0,0,0"); asm("oril 0,0,0")
#else
#define NOPS_4  asm("nop"); asm("nop"); asm("nop"); asm("nop")
#endif
#define NOPS_16 NOPS_4; NOPS_4; NOPS_4; NOPS_4
/* Note: the following should probably be moved into arch-specific files,
   since different platforms could have very different ways of implementation.
   Consider, hypothetically, an OS that let you get cycles per second with a simple
   system call */
static float
DYNINSTcyclesPerSecond(void) {
    int            i;
    time64         start_cpu;
    time64         end_cpu;
    double         elapsed;
    double         speed;
    const unsigned LOOP_LIMIT = 500000;

    start_cpu = DYNINSTgetCPUtime();
    for (i = 0; i < LOOP_LIMIT; i++) {
        NOPS_16; NOPS_16; NOPS_16; NOPS_16;
        NOPS_16; NOPS_16; NOPS_16; NOPS_16;
        NOPS_16; NOPS_16; NOPS_16; NOPS_16;
        NOPS_16; NOPS_16; NOPS_16; NOPS_16;
    }
    end_cpu = DYNINSTgetCPUtime();
    elapsed = (double) (end_cpu - start_cpu);
    speed   = (double) (MILLION*256*LOOP_LIMIT)/elapsed;

#ifdef i386_unknown_solaris2_5
    /* speed for the pentium is being overestimated by a factor of 2 */
    speed /= 2;
#endif
    return speed;
}


/************************************************************************
 * void saveFPUstate(float* base)
 * void restoreFPUstate(float* base)
 *
 * save and restore state of FPU on signals.  these are null functions
 * for most well designed and implemented systems.
************************************************************************/
static void
saveFPUstate(float* base) {
#ifdef i386_unknown_solaris2_5
    /* kludge for the pentium: we need to reset the FPU here, or we get 
       strange results on fp operations.
    */
    asm("finit");
#endif
}

static void
restoreFPUstate(float* base) {
}



/* The current observed cost since the last call to 
 *      DYNINSTgetObservedCycles(false) 
 */
/************************************************************************
 * int64 DYNINSTgetObservedCycles(RT_Boolean in_signal)
 *
 * report the observed cost of instrumentation in machine cycles.
 *
 * We keep cost as a 64 bit int, but the code generated by dyninst is
 *   a 32 bit counter (for speed).  So this function also converts the
 *   cost into a 64 bit value.
 *
 * We can't reset the low part of the counter since there might be an
 *   update of the counter going on while we are checking it.  So instead,
 *   we keep track of the last value of the low counter we saw, and update
 *   the high counter by the diference.  We also need to check that the
 *   delta is not negative (which happens when the low counter roles over).
 ************************************************************************/
#ifndef SHM_SAMPLING
int64 DYNINSTgetObservedCycles(RT_Boolean in_signal) 
{
    if (in_signal) {
        return DYNINSTvalue;
    }

    /* update the 64 bit version of the counter */
    if (DYNINSTobsCostLow < DYNINSTlastLow) {
        /* counter wrap around */
        /* note the calc here assume 32 bit int, but if ints are 64bit, then
           it won't wrap in the first place */
        DYNINSTvalue += (((unsigned) 0xffffffff) - DYNINSTlastLow) + 
 	    DYNINSTobsCostLow + 1;
    } else {
        DYNINSTvalue += (DYNINSTobsCostLow - DYNINSTlastLow);
    }

    DYNINSTlastLow = DYNINSTobsCostLow;
    return DYNINSTvalue;
}
#endif


/************************************************************************
 * void DYNINSTsampleValues(void)
 *
 * dummy function for sampling timers and counters.  the actual code
 * is added by dynamic instrumentation from the paradyn daemon.
************************************************************************/
#ifndef SHM_SAMPLING
void
DYNINSTsampleValues(void) {
    DYNINSTnumReported++;
}
#endif



/************************************************************************
 * void DYNINSTgenerateTraceRecord(traceStream sid, short type,
 *   short length, void* data, int flush,time64 wall_time,time64 process_time)
************************************************************************/
void
DYNINSTgenerateTraceRecord(traceStream sid, short type, short length,
			   void *eventData, int flush,
			   time64 wall_time, time64 process_time) {
    static unsigned pipe_gone = 0;
    static unsigned inDYNINSTgenerateTraceRecord = 0;
    traceHeader     header;
    int             count;
    char            buffer[1024];

    if (inDYNINSTgenerateTraceRecord) return;
    inDYNINSTgenerateTraceRecord = 1;

    if (pipe_gone) {
        inDYNINSTgenerateTraceRecord = 0;
        return;
    }

    header.wall    = wall_time - startWall;
    header.process = process_time;
#ifdef ndef
    if(type == TR_SAMPLE){
	 traceSample *s = (traceSample*)eventData;
         printf("wall time = %f processTime = %f value = %f\n",
		(double)(header.wall/1000000.0), 
		(double)(header.process/1000000.0),
		s->value);
    }
#endif

    length = ALIGN_TO_WORDSIZE(length);

    header.type   = type;
    header.length = length;

    count = 0;
    memcpy(&buffer[count], &sid, sizeof(traceStream));
    count += sizeof(traceStream);

    memcpy(&buffer[count], &header, sizeof(header));
    count += sizeof(header);

    count = ALIGN_TO_WORDSIZE(count);
    memcpy(&buffer[count], eventData, length);
    count += length;

    
    if (!DYNINSTwriteTrace(buffer, count))
      pipe_gone = 1;

    if (flush) DYNINSTflushTrace();

    inDYNINSTgenerateTraceRecord = 0;
}


/************************************************************************
 * void DYNINSTreportBaseTramps(void)
 *
 * report the cost of base trampolines.
************************************************************************/
#ifndef SHM_SAMPLING
void
DYNINSTreportBaseTramps() {
    // NOTE: this routine has a misleading name; how about DYNINSTsampleObsCost().

    costUpdate sample;

    //
    // Adding the cost corresponding to the alarm when it goes off.
    // This value includes the time spent inside the routine (DYNINSTtotal-
    // sampleTime) plus the time spent during the context switch (121 usecs
    // for SS-10, sunos)
    //

    sample.obsCostIdeal  = ((((double) DYNINSTgetObservedCycles(1) *
                              (double)DYNINSTcyclesToUsec) + 
                             DYNINSTtotalSampleTime + 121) / 1000000.0);

#ifdef notdef
    if (DYNINSTprofile) {
	int i;
	int limit;
	int pageSize;
	int startInst;
	extern void DYNINSTfirst();


	limit = DYNINSTbufsiz;
	/* first inst code - assumes data area above code space in virtual
	 * address */
	startInst = (int) &DYNINSTfirst;
	for (i=0; i < limit; i ++) {
	    if (i * DYNINSTtoAddr > startInst) {
		instTicks += DYNINSTprofBuffer[i];
	    }
	}

	sample.obsCostLow  = ((double) instTicks ) /100.0;
	sample.obsCostHigh = sample.obsCostLow + sample.obsCostIdeal;
    }
#endif

    DYNINSTgenerateTraceRecord(0, TR_COST_UPDATE, sizeof(sample), &sample, 0,
			       DYNINSTgetWalltime(), DYNINSTgetCPUtime());
}
#endif


/************************************************************************
 * static void DYNINSTreportSamples(void)
 *
 * report samples to paradyn daemons. Called by DYNINSTinit, DYNINSTexit,
 * and DYNINSTalarmExpires.
************************************************************************/
#ifndef SHM_SAMPLING
static void 
DYNINSTreportSamples(void) {
    time64     start_cpu;
    float      fp_context[N_FP_REGS];

    ++DYNINSTin_sample;

    saveFPUstate(fp_context);
    start_cpu = DYNINSTgetCPUtime();

    /* to keep observed cost accurate due to 32-cycle rollover */
    (void) DYNINSTgetObservedCycles(0);

    DYNINSTsampleValues();
    DYNINSTreportBaseTramps();
    DYNINSTflushTrace();

    DYNINSTtotalSampleTime += (DYNINSTgetCPUtime() - start_cpu);
    restoreFPUstate(fp_context);
    --DYNINSTin_sample;
}
#endif


/************************************************************************
 * void DYNINSTalarmExpire(void)
 *
 * called periodically by signal handlers.  report sampled data back
 * to the paradyn daemons.  when the program exits, DYNINSTsampleValues
 * should be called directly.
************************************************************************/
/************************************************************************
 * DYNINSTalarmExpire is changed so that it will restart the sytem call
 * if that called is interrupted on HP
 * Duplicated for the same reason above.
 ************************************************************************/
#ifndef SHM_SAMPLING
void
#if !defined(hppa1_1_hp_hpux)
DYNINSTalarmExpire(int signo) {
#else 
DYNINSTalarmExpire(int signo, int code, struct sigcontext *scp) {
#endif

#ifdef COSTTEST
    time64 startT, endT;
#endif

    if (DYNINSTin_sample) {
        return;
    }
    DYNINSTin_sample = 1;

#ifdef COSTTEST
    startT=DYNINSTgetCPUtime();
#endif

    DYNINSTtotalAlarmExpires++;

    /* This piece of code is needed because DYNINSTalarmExpire's are always called
       at the initial pace (every .2 secs, I believe), whereas we only want to do
       stuff at the current pace (initially every .2 secs but less frequently after
       "folds").  We could just adjust the rate of SIGALRM, so this isn't
       needed. */

    if ((++DYNINSTnumSampled % DYNINSTsampleMultiple) == 0) {
      DYNINSTreportSamples();
    }

    DYNINSTin_sample = 0;

#if defined(hppa1_1_hp_hpux)
    scp->sc_syscall_action = SIG_RESTART;
#endif


#ifdef COSTTEST
    endT=DYNINSTgetCPUtime();
    DYNINSTtest[4]+=endT-startT;
    DYNINSTtestN[4]++;
#endif 
}
#endif


static void shmsampling_printf(const char *fmt, ...) {
#ifdef SHM_SAMPLING_DEBUG
   va_list args;
   va_start(args, fmt);

   vfprintf(stderr, fmt, args);

   va_end(args);

   fflush(stderr);
#endif
}

/************************************************************************
 * void DYNINSTinit()
 *
 * initialize the DYNINST library.  this function is called at the start
 * of the application program, as well as after a fork, and after an
 * attach.
 *
 ************************************************************************/
void DYNINSTinit(int theKey, int shmSegNumBytes, int paradyndPid)
{
  /* If first 2 params are -1 then we're being called by DYNINSTfork(). */
  /* If first 2 params are 0 then it just means we're not shm sampling */
  /* If 3d param is negative, then we're called from attach
     (and we use -paradyndPid as paradynd's pid).  If 3d param
     is positive, then we're not called from attach (and we use +paradyndPid
     as paradynd's pid). */
  
  int calledFromFork = (theKey == -1);
  int calledFromAttach = (paradyndPid < 0);
  
#ifndef SHM_SAMPLING
  unsigned         val;
#endif
  
  int dx;
  
#if defined(SHM_SAMPLING) && defined(MT_THREAD)
  traceThrSelf traceRec;
#endif
  
#ifdef SHM_SAMPLING_DEBUG
  char thehostname[80];
  extern int gethostname(char*,int);
  
  (void)gethostname(thehostname, 80);
  thehostname[79] = '\0';
  
  shmsampling_printf("WELCOME to DYNINSTinit (%s, pid=%d), args are %d, %d, %d\n",
		     thehostname, (int)getpid(), theKey, shmSegNumBytes,
		     paradyndPid);
#endif
  
  if (calledFromAttach)
    paradyndPid = -paradyndPid;
  
  the_paradyndPid = paradyndPid; /* important -- needed in case we fork() */
  
  TagGroupInfo.TagHierarchy = 0; /* FALSE; */
  TagGroupInfo.NumGroups = 0;
  TagGroupInfo.NumNewTags = 0;
  for(dx=0; dx < DYNINSTTagGroupsLimit; dx++) {
    TagGroupInfo.GroupTable[dx] = NULL;
  } 

#ifdef SHM_SAMPLING
  if (!calledFromFork) {
    DYNINST_shmSegKey = theKey;
    DYNINST_shmSegNumBytes = shmSegNumBytes;
    
    DYNINST_shmSegAttachedPtr = DYNINST_shm_init(theKey, shmSegNumBytes, 
						 &DYNINST_shmSegShmId);
  }
#endif
  
  /*
     In accordance with usual stdio rules, stdout is line-buffered and stderr is
     non-buffered.  Unfortunately, stdio is a little clever and when it detects
     stdout/stderr redirected to a pipe/file/whatever, it changes to fully-buffered.
     This indeed occurs with us (see paradynd/src/process.C to see how a program's
     stdout/stderr are redirected to a pipe). So we reset back to the desired
     "bufferedness" here.  See stdio.h for these calls.  When attaching, stdio
     isn't under paradynd control, so we don't do this stuff.

     Note! Since we are messing with stdio stuff here, it should go without
     saying that DYNINSTinit() (or at least this part of it) shouldn't be
     invoked until stdio has been initialized!
  */
  
  if (!calledFromAttach) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* make stdout line-buffered.  "setlinebuf(stdout)" is cleaner but HP
       doesn't seem to have it */
    setvbuf(stderr, NULL, _IONBF, 0);
    /* make stderr non-buffered */
  }
  
  DYNINSTos_init(calledFromFork, calledFromAttach);
  /* is this needed when calledFromFork?  Depends on what it does, which is in turn
     os-dependent...so, calledFromFork should probably be passed to this routine. */
  /* Initialize TagGroupInfo */
  
  startWall = 0;
  
  if (!calledFromFork)
    DYNINSTcyclesToUsec = MILLION/DYNINSTcyclesPerSecond();
  
#ifndef SHM_SAMPLING
  /* assign sampling rate to be default value in util/h/sys.h */
  val = BASESAMPLEINTERVAL;
  
  DYNINSTsamplingRate = val/MILLION;
  
#endif
  
#ifdef USE_PROF
  {
    extern int end;
    
    DYNINSTprofScale = sizeof(short);
    DYNINSTtoAddr = sizeof(short);
    DYNINSTbufsiz = (((unsigned int) &end)/DYNINSTprofScale) + 1;
    DYNINSTprofBuffer = (short *) calloc(sizeof(short), DYNINSTbufsiz);
    profil(DYNINSTprofBuffer, DYNINSTbufsiz*sizeof(short), 0, 0xffff);
    DYNINSTprofile = 1;
  }
#endif
  
  DYNINSTstartWallTimer(&DYNINSTelapsedTime);
  DYNINSTstartProcessTimer(&DYNINSTelapsedCPUTime);
  
  /* Fill in info for paradynd to receive: */
  
  DYNINST_bootstrap_info.ppid = -1; /* not needed really */
  
  if (!calledFromFork) {
#ifdef SHM_SAMPLING
    shmsampling_printf("DYNINSTinit setting appl_attachedAtPtr in bs_record to %x\n",
		       (unsigned)DYNINST_shmSegAttachedPtr);
    DYNINST_bootstrap_info.appl_attachedAtPtr = DYNINST_shmSegAttachedPtr;
#endif
  }
  
  DYNINST_bootstrap_info.pid = getpid();
#if !defined(i386_unknown_nt4_0)
  if (calledFromFork)
    DYNINST_bootstrap_info.ppid = getppid();
#else
  DYNINST_bootstrap_info.ppid = 0;
#endif

  /* We do this field last as a way to synchronize; paradynd will ignore what it
     sees in this structure until the event field is nonzero */
  if (calledFromFork)
    DYNINST_bootstrap_info.event = 2; /* 2 --> end of DYNINSTinit (forked process) */
  else if (calledFromAttach)
    DYNINST_bootstrap_info.event = 3; /* 3 --> end of DYNINSTinit (attached proc) */
  else				   
    DYNINST_bootstrap_info.event = 1; /* 1 --> end of DYNINSTinit (normal or when
					 called by exec'd proc) */
  
  
  /* If attaching, now's the time where we set up the trace stream connection fd */
  if (calledFromAttach) {
    int pid = getpid();
#if !defined(i386_unknown_nt4_0)
    int ppid = getppid();
#else
    int ppid = 0;
#endif
    unsigned attach_cookie = 0x22222222;
    
    DYNINSTinitTrace(paradyndPid);
    
    DYNINSTwriteTrace(&attach_cookie, sizeof(attach_cookie));
    DYNINSTwriteTrace(&pid, sizeof(pid));
    DYNINSTwriteTrace(&ppid, sizeof(ppid));
#ifdef SHM_SAMPLING
    DYNINSTwriteTrace(&DYNINST_shmSegKey, sizeof(DYNINST_shmSegKey));
    DYNINSTwriteTrace(&DYNINST_shmSegAttachedPtr, sizeof(DYNINST_shmSegAttachedPtr));
#endif
    DYNINSTflushTrace();
  }
  else if (!calledFromFork) {
    /* either normal startup or startup via a process having exec'd */
#if !defined(i386_unknown_nt4_0)
      /* trace stream is already open */
      DYNINSTinitTrace(-1);
#else
      /* need to get a connection to daemon */
      int cookie = 0;
      int pid = getpid();
      int ppid = paradyndPid;
      DYNINSTinitTrace(paradyndPid);
      DYNINSTwriteTrace(&cookie, sizeof(cookie));
      DYNINSTwriteTrace(&pid, sizeof(pid));
      DYNINSTwriteTrace(&ppid, sizeof(ppid));
#endif
  }
  else
    /* calledByFork -- DYNINSTfork already called DYNINSTinitTrace */
    ;
  
#if defined(SHM_SAMPLING) && defined(MT_THREAD)
  if (!DYNINST_initialize_done) {
    DYNINST_initialize_free(MAX_NUMBER_OF_THREADS);
    DYNINST_initialize_hash(MAX_NUMBER_OF_THREADS);
    traceRec.pos = DYNINST_hash_insert(DYNINSTthreadSelf());
    DYNINST_initialize_done=1;
  } else {
    traceRec.pos = DYNINST_hash_lookup(DYNINSTthreadSelf());
  }
  traceRec.tid = DYNINSTthreadSelf();
  traceRec.ppid = getpid();
  DYNINSTgenerateTraceRecord(0, TR_THRSELF, sizeof(traceRec), &traceRec, 1,
			     DYNINSTgetWalltime(),
			     DYNINSTgetCPUtime());
#endif
  
  /* Now, we stop ourselves.  When paradynd receives the forwarded signal,
     it will read from DYNINST_bootstrap_info */
  
  shmsampling_printf("DYNINSTinit (pid=%d) --> about to DYNINSTbreakPoint()\n",
		     (int)getpid());
  
  DYNINSTbreakPoint();

  /* The next instruction is necessary to avoid a race condition when we
     link the thread library. This is just a hack to get the hw counters
     to work and it should be fixed - naim 4/9/97 */
  /* usleep(1000000); */
  
  /* After the break, we clear DYNINST_bootstrap_info's event field, leaving the
     others there */
  DYNINST_bootstrap_info.event = 0; /* 0 --> nothing */
  
  shmsampling_printf("leaving DYNINSTinit (pid=%d) --> the process is running freely now\n", (int)getpid());
   
#ifndef SHM_SAMPLING
  /* what good does this do here? */
  /* We report samples here to set the starting time for metrics that were
     enabled before the user press run, so we don't loose any samples - mjrg
     */
  DYNINSTreportSamples();

  /* Install alarm only when exiting DYNINSTinit to avoid alarm going off
     before DYNINSTinit is done */
  /* Do we need to re-create the alarm signal stuff when calledFromFork is true? */
  DYNINST_install_ualarm(val);

#endif
}


/************************************************************************
 * void DYNINSTexit(void)
 *
 * handle `exit' in the application. 
 * report samples and print cost.
************************************************************************/
void
DYNINSTexit(void) {
    static int done = 0;
    if (done) return;
    done = 1;

#ifndef SHM_SAMPLING
    DYNINSTreportSamples();
#endif

    /* NOTE: For shm sampling, we should do more here.  For example, we should
       probably disconnect from the shm segments.
       Note that we don't have to inform paradynd of the exit; it will find out
       soon enough on its own, due to the waitpid() it does and/or finding an
       end-of-file on one of our piped file descriptors. */

    DYNINSTprintCost();
}



/************************************************************************
 * void DYNINSTfork(void* arg, int pid)
 *
 * track a fork() system call, and report to the paradyn daemon.
 *
 * Instrumented by paradynd to be called at the exit point of fork().  So, the address
 * space (including the instrumentation heap) has already been duplicated, though some
 * meta-data still needs to be manually duplicated by paradynd.  Note that for
 * shm_sampling, a fork() just increases the reference count for the shm segment.
 * This is not what we want, so we need to (1) detach from the old segment, (2) create
 * a new shm segment and attach to it in the *same* virtual addr spot as the old
 * one (otherwise, references to the shm segment will be out of date!), and (3) let
 * let paradynd know what segment key numbers we have chosen for the new segments, so
 * that it may attach to them (paradynd may also set some values w/in the segments such
 * as mid's).
 * 
 * Who sends the initial trace record to paradynd?  The child creates the new shm seg
 * and thus only the child can inform paradynd of the chosen keys.  One might argue that
 * the child does't have a traceRecord connection yet, but fork() should dup() all fd's
 * and take care of that limitation...
************************************************************************/
/* debug code should call forkexec_printf as a way of avoiding
   putting #ifdef FORK_EXEC_DEBUG around their fprintf(stderr, ...) statements,
   which can lead to horribly ugly code --ari */
/* similar for SHM_SAMPLING_DEBUG */
void forkexec_printf(const char *fmt, ...) {
#ifdef FORK_EXEC_DEBUG
   va_list args;
   va_start(args, fmt);

   vfprintf(stderr, fmt, args);

   va_end(args);

   fflush(stderr);
#endif
}

#if !defined(i386_unknown_nt4_0)
void
DYNINSTfork(int pid) {
    //forkexec_printf("DYNINSTfork called with pid = %d\n", pid);
    printf("DYNINSTfork -- WELCOME -- called with pid = %d\n", pid);
    fflush(stdout);

    if (pid > 0) {
       /* We used to send TR_FORK trace record here, in the parent.  But shm sampling
          requires the child to do this, so we moved the code there... */
       /* See metric.C for an explanation why it's important for the parent to
          be paused (not just the child) while propagating metric instances.
          Here's the short explanation: to initialize some of the timers and counters,
	  the child will copy some fields from the parent, and for the child to get
          values from the parent after the fork would be a bad thing.  --ari */

       forkexec_printf("DYNINSTfork parent; about to DYNINSTbreakPoint\n");
       fflush(stderr);

       DYNINSTbreakPoint();
    } else if (pid == 0) {
       /* we are the child process */
	int pid = getpid();
	int ppid = getppid();

        //char *traceEnv;

	forkexec_printf("DYNINSTfork CHILD -- welcome\n");
	fflush(stderr);

#ifdef SHM_SAMPLING
	/* Here, we need to detach from the old shm segment, create a new one
	   (in the same virtual memory location as the old one), and attach 
           to it */
	makeNewShmSegCopy();
#endif

	/* Here is where we used to send a TR_FORK trace record.  But we've found
	   that sending a trace record followed by a DYNINSTbreakPoint had unpredictable
	   results -- sometimes the breakPoint would get delivered to paradynd first.
	   So idea #2 was to fill in DYNINST_bootstrapStruct and then do a breakpoint.
	   But the breakPoint won't get forwarded to paradynd because paradynd hasn't
	   yet attached to the child process.
	   So idea #3 (the current one) is to just send all that information along the
	   new connection (whereas we used to just send the pid).

	   NOTE: soon attach will probably be implemented in a similar way -- by
	         writing to the connection.
	 */

	/* set up a connection to the daemon for the trace stream.  (The child proc
	   gets a different connection from the parent proc) */
	forkexec_printf("dyninst-fork child closing old connections...\n");
	DYNINSTcloseTrace();

	forkexec_printf("dyninst-fork child opening new connection.\n");
	assert(the_paradyndPid > 0);
	DYNINSTinitTrace(the_paradyndPid);

	forkexec_printf("dyninst-fork child pid %d opened new connection...now sending pid etc. along it\n", (int)getpid());

	{
	  unsigned fork_cookie = 0x11111111;
	  DYNINSTwriteTrace(&fork_cookie, sizeof(fork_cookie));
	}

	DYNINSTwriteTrace(&pid, sizeof(pid));
	DYNINSTwriteTrace(&ppid, sizeof(ppid));
#ifdef SHM_SAMPLING
	DYNINSTwriteTrace(&DYNINST_shmSegKey, sizeof(DYNINST_shmSegKey));
	DYNINSTwriteTrace(&DYNINST_shmSegAttachedPtr, sizeof(DYNINST_shmSegAttachedPtr));
#endif
	DYNINSTflushTrace();

	forkexec_printf("dyninst-fork child pid %d sent pid; now doing DYNINSTbreakPoint() to wait for paradynd to initialize me.\n", (int)getpid());

	DYNINSTbreakPoint();

	forkexec_printf("dyninst-fork child past DYNINSTbreakPoint()...calling DYNINSTinit(-1,-1)\n");

	DYNINSTinit(-1, -1, the_paradyndPid);
	   /* -1 params indicate called from DYNINSTfork */

	forkexec_printf("dyninst-fork child done...running freely.\n");
    }
}
#endif


void
DYNINSTexec(char *path) {
    /* paradynd instruments programs to call this routine on ENTRY to exec
       (so the exec hasn't yet taken place).  All that we do here is inform paradynd
       of the (pending) exec, and pause ourselves.  Paradynd will continue us after
       digesting the info...then, presumably, a TRAP will be generated, as the exec
       syscall completes. */

    forkexec_printf("execve called, path = %s\n", path);

    if (strlen(path) + 1 > sizeof(DYNINST_bootstrap_info.path)) {
      fprintf(stderr, "DYNINSTexec failed...path name too long\n");
      abort();
    }

    /* We used to send a TR_EXEC record here and then DYNINSTbreakPoint().
       But we've seen a race condition -- sometimes the break-point is delivered
       to paradynd before the trace record.  So instead, we fill in
       DYNINST_bootstrap_info and then do a breakpoint.  This approach is the same
       as the end of DYNINSTinit. */
    DYNINST_bootstrap_info.event = 4;
    DYNINST_bootstrap_info.pid = getpid();
    strcpy(DYNINST_bootstrap_info.path, path);

    /* The following turns OFF the alarm signal (the 0,0 parameters); when
       DYNINSTinit() runs again for the exec'd process, it'll be turned back on.  This
       stuff is necessary to avoid the process dying on an uncaught sigalarm while
       processing the exec */
#ifndef SHM_SAMPLING
    DYNINST_install_ualarm(0);
#endif

    forkexec_printf("DYNINSTexec before breakpoint\n");

    DYNINSTbreakPoint();

    /* after the breakpoint, clear DYNINST_bootstrap_info */
    DYNINST_bootstrap_info.event = 0; /* 0 --> nothing */

    forkexec_printf("DYNINSTexec after breakpoint...allowing the exec to happen now\n");
}


void
DYNINSTexecFailed() {
    time64 process_time = DYNINSTgetCPUtime();
    time64 wall_time = DYNINSTgetWalltime();
    int pid = getpid();

    forkexec_printf("DYNINSTexecFAILED\n");

    DYNINSTgenerateTraceRecord(0, TR_EXEC_FAILED, sizeof(int), &pid, 1,
			       process_time, wall_time);

#ifndef SHM_SAMPLING
    DYNINST_install_ualarm(BASESAMPLEINTERVAL);
#endif
}



/************************************************************************
 * void DYNINSTprintCost(void)
 *
 * print a detailed summary of the cost of the application's run.
************************************************************************/
void
DYNINSTprintCost(void) {
    FILE *fp;
    time64 now;
    int64 value;
    struct endStatsRec stats;
    time64 wall_time; 

    DYNINSTstopProcessTimer(&DYNINSTelapsedCPUTime);
    DYNINSTstopWallTimer(&DYNINSTelapsedTime);

#ifndef SHM_SAMPLING
    value = DYNINSTgetObservedCycles(0);
#else
    value = *(unsigned*)((char*)DYNINST_bootstrap_info.appl_attachedAtPtr + 12);
#endif
    stats.instCycles = value;

    value *= DYNINSTcyclesToUsec;

#ifndef SHM_SAMPLING
    stats.alarms      = DYNINSTtotalAlarmExpires;
#else
    stats.alarms      = 0;
#endif
    stats.numReported = DYNINSTnumReported;
    stats.instTime    = (double)value/(double)MILLION;
    stats.handlerCost = (double)DYNINSTtotalSampleTime/(double)MILLION;

    now = DYNINSTgetCPUtime();
    wall_time = DYNINSTgetWalltime();
    stats.totalCpuTime  = (double)DYNINSTelapsedCPUTime.total/(double)MILLION;
    stats.totalWallTime = (double)DYNINSTelapsedTime.total/(double)MILLION;

    stats.samplesReported = DYNINSTtotalSamples;
    stats.samplingRate    = DYNINSTsamplingRate;

    stats.userTicks = 0;
    stats.instTicks = 0;


    fp = fopen("stats.out", "w");

#ifdef USE_PROF
    if (DYNINSTprofile) {
	int i;
	int limit;
	int pageSize;
	int startInst;
	extern void DYNINSTfirst();


	limit = DYNINSTbufsiz;
	/* first inst code - assumes data area above code space in virtual
	 * address */
	startInst = (int) &DYNINSTfirst;
	fprintf(fp, "startInst = %x\n", startInst);
	fprintf(fp, "limit = %x\n", limit * DYNINSTprofScale);
	for (i=0; i < limit; i++) {
#ifdef notdef
	    if (DYNINSTprofBuffer[i]) {
		fprintf(fp, "%x %d\n", i * DYNINSTtoAddr, 
			DYNINSTprofBuffer[i]);
	    }
#endif
	    if (i * DYNINSTtoAddr > startInst) {
		stats.instTicks += DYNINSTprofBuffer[i];
	    } else {
		stats.userTicks += DYNINSTprofBuffer[i];
	    }
	}

	/* fwrite(DYNINSTprofBuffer, DYNINSTbufsiz, 1, fp); */
	fprintf(fp, "stats.instTicks %d\n", stats.instTicks);
	fprintf(fp, "stats.userTicks %d\n", stats.userTicks);
    }
#endif

#ifdef notdef
    fprintf(fp, "DYNINSTtotalAlarmExpires %d\n", stats.alarms);
    fprintf(fp, "DYNINSTnumReported %d\n", stats.numReported);
    fprintf(fp,"Raw cycle count = %f\n", (double) stats.instCycles);
    fprintf(fp,"Total instrumentation cost = %f\n", stats.instTime);
    fprintf(fp,"Total handler cost = %f\n", stats.handlerCost);
    fprintf(fp,"Total cpu time of program %f\n", stats.totalCpuTime);
    fprintf(fp,"Elapsed wall time of program %f\n",
        stats.totalWallTime/1000000.0);
    fprintf(fp,"total data samples %d\n", stats.samplesReported);
    fprintf(fp,"sampling rate %f\n", stats.samplingRate);
    fprintf(fp,"Application program ticks %d\n", stats.userTicks);
    fprintf(fp,"Instrumentation ticks %d\n", stats.instTicks);

    fclose(fp);
#endif

    /* record that the exec is done -- should be somewhere better. */
    DYNINSTgenerateTraceRecord(0, TR_EXIT, sizeof(stats), &stats, 1,
			       wall_time,now);
}



/************************************************************************
 * void DYNINSTreportNewTags(void)
 *
 * inform the paradyn daemons of new message tags.  Invoked
 * periodically by DYNINSTsampleValues (or directly from DYNINSTrecordTag,
 * if SHM_SAMPLING).
************************************************************************/
void DYNINSTreportNewTags(void)
{
  int    dx;
  time64 process_time;
  time64 wall_time;

  if(TagGroupInfo.NumNewTags > 0) {
    /* not used by consumer [createProcess() in perfStream.C], so can prob.
     * be set to a dummy value to save a little time.  */
    process_time = DYNINSTgetCPUtime();
    /* this _is_ needed; paradynd keeps the 'creation' time of each resource
     *  (resource.h) */
    wall_time = DYNINSTgetWalltime();
  }

#ifdef COSTTEST
  time64 startT,endT;
  startT=DYNINSTgetCPUtime();
#endif
  
  for(dx=0; dx < TagGroupInfo.NumNewTags; dx++) {
    struct _newresource newRes;
    
    if((TagGroupInfo.TagHierarchy) && (TagGroupInfo.NewTags[dx][2])) {
      memset(&newRes, '\0', sizeof(newRes));
      sprintf(newRes.name, "SyncObject/Message/%d",
	      TagGroupInfo.NewTags[dx][1]);
      strcpy(newRes.abstraction, "BASE");
      newRes.type = RES_TYPE_INT;

      DYNINSTgenerateTraceRecord(0, TR_NEW_RESOURCE,
				 sizeof(struct _newresource), &newRes, 1,
				 wall_time,process_time);
    }
    
    memset(&newRes, '\0', sizeof(newRes));
    if(TagGroupInfo.TagHierarchy) {
      sprintf(newRes.name, "SyncObject/Message/%d/%d", 
	      TagGroupInfo.NewTags[dx][1], TagGroupInfo.NewTags[dx][0]);
    } else {
      sprintf(newRes.name, "SyncObject/Message/%d", 
	      TagGroupInfo.NewTags[dx][0]);
    }
    strcpy(newRes.abstraction, "BASE");
    newRes.type = RES_TYPE_INT;

    DYNINSTgenerateTraceRecord(0, TR_NEW_RESOURCE, 
			       sizeof(struct _newresource), &newRes, 1,
			       wall_time,process_time);
  }
  TagGroupInfo.NumNewTags = 0;
  
#ifdef COSTTEST
  endT=DYNINSTgetCPUtime();
  DYNINSTtest[8]+=endT-startT;
  DYNINSTtestN[8]++;
#endif 
}


/************************************************************************
 * void DYNINSTrecordTag(int tagId) &&
 * void DYNINSTrecordTagAndGroup(int tagId, int groupId)
 *
 * mark a new tag in tag list.
 * Routines such as pvm_send() are instrumented to call us.
 * In the non-shm-sampling case, we just appent to DYNINSTtags, if this
 * tag hasn't been seen before.
 * In the shm-sampling case, we _also_ call DYNINSTreportNewTags().
 * One might worry about infinite recursion (DYNINSTreportNewTags() calls
 * DYNINSTgenerateTraceRecord which calls write() which can be instrumented
 * to call DYNINSTrecordTag().....) but I don't think there's a problem.
 * Let's trace it through, assuming write() is instrumented to call
 * DYNINSTrecordTag() on entry:
 * 
 * -- write() is called by the program, which calls DYNINSTrecordTag.
 * -- DYNINSTrecordTag calls DYNINSTreportNewTags, which 
 *    calls DYNINSTgenerateTraceRecord, which calls write.
 * -- write() is instrumented to call DYNINSTrecordTag, so it does.
 *    the tag equals that of the trace fd.  If it's been seen already,
 *    then DYNINSTrecordTag returns before calling DYNINSTgenerateTraceRecord.
 *    If not, then it gets reported and then (the next time write() gets
 *    implicitly called) ignored in all future calls.
 *
 * So in summary, infinite recursion is probably not a problem because
 * DYNINSTrecordTag returns before calling DYNINSTreportNewTags
 * (and hence DYNINSTgenerateTraceRecord) if the tag has been seen
 * before.
 ************************************************************************/
void DYNINSTrecordTagGroupInfo(int tagId, int tagDx,
			       int groupId, int groupDx)
{
  DynInstTagSt* tagSt;
  int           dx;
  int           newGroup;
  
  if(TagGroupInfo.NumNewTags == DYNINSTNewTagsLimit) return;

  tagSt = TagGroupInfo.GroupTable[groupDx];
  while((tagSt != NULL) && (tagSt->TagGroupId != groupId)) {
    tagSt = tagSt->Next;
  }
  if(tagSt == NULL) {
    tagSt = (DynInstTagSt *) malloc(sizeof(DynInstTagSt));
    assert(tagSt != NULL);

    tagSt->TagGroupId = groupId;
    tagSt->TGUniqueId = DYNINSTTGroup_CreateUniqueId(groupId);
    tagSt->NumTags = 0;
    for(dx=0; dx < DYNINSTTagsLimit; dx++) {
      /* -99 has nothing special about it, just a num/tag which is not used. 
       * -1 can't be used since it is used to label collective messages within
       * a group
       */         
      tagSt->TagTable[dx] = -99;
    }
    tagSt->Next = TagGroupInfo.GroupTable[groupDx];
    TagGroupInfo.GroupTable[groupDx] = tagSt;
    TagGroupInfo.NumGroups++;
    newGroup = 1;
  } else {
    assert(tagSt->TagGroupId == groupId);
    newGroup = 0;
  }

  if(tagSt->NumTags == DYNINSTTagsLimit) return;

  dx = tagDx;
  while((tagSt->TagTable[dx] != tagId) && (tagSt->TagTable[dx] != -99)) {
    dx++;
    if(dx == DYNINSTTagsLimit) dx = 0;
    assert(dx != tagId);
  }

  if(tagSt->TagTable[dx] == tagId) return;

  assert(tagSt->TagTable[dx] == -99);
  tagSt->TagTable[dx] = tagId;
  tagSt->NumTags++;
  
  TagGroupInfo.NewTags[TagGroupInfo.NumNewTags][0] = tagId;
  TagGroupInfo.NewTags[TagGroupInfo.NumNewTags][1] = tagSt->TGUniqueId;
  TagGroupInfo.NewTags[TagGroupInfo.NumNewTags][2] = newGroup;
  TagGroupInfo.NumNewTags++;

#ifdef SHM_SAMPLING
  /* Usually, DYNINSTreportNewTags() is invoked only periodically, by
   * DYNINSTalarmExpire.  But since that routine no longer exists, we need
   * another way for it to be called.  Let's just call it directly here; I
   * don't think it'll be too expensive (unless a program declares a new
   * tag every fraction of a second!)  I also don't think there will be an
   * infinite recursion problem; see comment above.
   */
  DYNINSTreportNewTags();
#endif    
}


void DYNINSTrecordTag(int tagId)
{
  assert(tagId >= 0);
  assert(TagGroupInfo.TagHierarchy == 0); /* FALSE); */
  DYNINSTrecordTagGroupInfo(tagId, tagId % DYNINSTTagsLimit,
			    -1, 0);
}

void DYNINSTrecordTagAndGroup(int tagId, int groupId)
{
  assert(tagId >= 0);
  assert(groupId >= 0);
  TagGroupInfo.TagHierarchy = 1; /* TRUE; */
  DYNINSTrecordTagGroupInfo(tagId, (tagId % DYNINSTTagsLimit),
			    groupId, (groupId % DYNINSTTagGroupsLimit));
}

void DYNINSTrecordGroup(int groupId)
{
  assert(groupId >= 0);
  TagGroupInfo.TagHierarchy = 1;  /* TRUE; */
  DYNINSTrecordTagGroupInfo(-1, 0,
			    groupId, (groupId % DYNINSTTagGroupsLimit));
}


/************************************************************************
 * void DYNINSTreportCounter(intCounter* counter)
 *
 * report value of counter to paradynd.
************************************************************************/
#ifndef SHM_SAMPLING
void
DYNINSTreportCounter(intCounter* counter) {
    traceSample sample;

#ifdef COSTTEST
    time64 endT;
    time64 startT=DYNINSTgetCPUtime();
#endif
    time64 process_time = DYNINSTgetCPUtime();
    time64 wall_time = DYNINSTgetWalltime();
    sample.value = counter->value;
       /* WARNING: changes "int" to "float", with the associated problems of
	            normalized number gaps; hence, the value has likely changed
		    a bit (more for higher integers, say in the millions). */
    sample.id    = counter->id;
    DYNINSTtotalSamples++;

    DYNINSTgenerateTraceRecord(0, TR_SAMPLE, sizeof(sample), &sample, 0,
			       wall_time, process_time);

#ifdef COSTTEST
    endT=DYNINSTgetCPUtime();
    DYNINSTtest[6]+=endT-startT;
    DYNINSTtestN[6]++;
#endif 

}
#endif


/************************************************************************
 * void DYNINSTreportTimer(tTimer* timer)
 *
 * report the timer `timer' to the paradyn daemon.
************************************************************************/
#ifndef SHM_SAMPLING
void
DYNINSTreportTimer(tTimer *timer) {
    time64 total;
    traceSample sample;

#ifdef COSTTEST
    time64 endT;
    time64 startT = DYNINSTgetCPUtime();
#endif

    time64 process_time = DYNINSTgetCPUtime();
    time64 wall_time = DYNINSTgetWalltime();
    if (timer->mutex) {
        total = timer->snapShot;
	//printf("id %d using snapshot value of %f\n", timer->id.id, (double)total);
    }
    else if (timer->counter) {
        /* timer is running */
        time64 now;
        if (timer->type == processTime) {
            now = process_time;
        } else {
            now = wall_time;
        }
        total = now - timer->start + timer->total;
	//printf("id %d using added-to-total value of %f\n", timer->id.id, (double)total);
    }
    else {
        total = timer->total;
	//printf("using %d total value of %f\n", timer->id.id, (double)total);
    }

    if (total < timer->lastValue) {
        if (timer->type == processTime) {
            printf("process ");
        }
        else {
            printf("wall ");
        }
        printf("time regressed timer %d, total = %f, last = %f\n",
            timer->id.id, (float) total, (float) timer->lastValue);
        if (timer->counter) {
            printf("timer was active\n");
        } else {
            printf("timer was inactive\n");
        }
        printf("mutex=%d, counter=%d, snapShot=%f\n",
            (int) timer->mutex, (int) timer->counter,
            (double) timer->snapShot);
        printf("start = %f, total = %f\n",
	       (double) timer->start, (double) timer->total);
        fflush(stdout);
        abort();
    }

    timer->lastValue = total;

    sample.id = timer->id;
    if (timer->normalize == 0) {
       fprintf(stderr, "DYNINSTreportTimer WARNING: timer->normalize is 0!!\n");
    }

    sample.value = ((double) total) / (double) timer->normalize;
    // check for nan now?
       // NOTE: The type of sample.value is float, not double, so some precision
       //       is lost here.  Besides, wouldn't it be better to send the raw
       //       "total", with no loss in precision; especially since timer->normalize
       //       always seems to be 1 million? (Well, it differs for cm5)
    DYNINSTtotalSamples++;

#ifdef ndef
    printf("raw sample %d = %f time = %f\n", sample.id.id, sample.value,
				(double)(now/1000000.0));  
#endif

    DYNINSTgenerateTraceRecord(0, TR_SAMPLE, sizeof(sample), &sample, 0,
				wall_time,process_time);

#ifdef COSTTEST
    endT=DYNINSTgetCPUtime();
    DYNINSTtest[5]+=endT-startT;
    DYNINSTtestN[5]++;
#endif 
}
#endif



/************************************************************************
 * DYNINST test functions.
 *
 * Since these routines aren't used regularly, shouldn't they be surrounded
 * with an ifdef? --ari
************************************************************************/
void DYNINSTsimplePrint(void) {
    printf("inside dynamic inst function\n");
}

void DYNINSTentryPrint(int arg) {
    printf("enter %d\n", arg);
}

void DYNINSTcallFrom(int arg) {
    printf("call from %d\n", arg);
}

void DYNINSTcallReturn(int arg) {
    printf("return to %d\n", arg);
}

void DYNINSTexitPrint(int arg) {
    printf("exit %d\n", arg);
}

#if defined(SHM_SAMPLING) && defined(MT_THREAD)

unsigned DYNINSTthreadTable[MAX_NUMBER_OF_THREADS][MAX_NUMBER_OF_LEVELS];

struct elemList {
  unsigned pos;
  struct elemList *next;
};

struct elemList *DYNINST_freeList=NULL;

void DYNINST_add_free(unsigned pos)
{
  struct elemList *tmp;
  tmp = (struct elemList *) malloc(sizeof(struct elemList));
  tmp->pos = pos;
  tmp->next = DYNINST_freeList;
  DYNINST_freeList = tmp;
}

unsigned DYNINST_get_free()
{
  unsigned pos;
  struct elemList *tmp;
  if (DYNINST_freeList) {
    pos = DYNINST_freeList->pos;
    tmp = DYNINST_freeList;
    DYNINST_freeList = DYNINST_freeList->next;
    free(tmp);
    return(pos);
  }
  abort();
}

void DYNINST_initialize_free(unsigned total)
{
  struct elemList *tmp;
  int i;
  for (i=0;i<total;i++) {
    tmp = (struct elemList *) malloc(sizeof(struct elemList));   
    tmp->pos = i;
    tmp->next = DYNINST_freeList;
    DYNINST_freeList = tmp;
  }
}

#define HASH(x) (x % MAX_NUMBER_OF_THREADS)

struct nlist {
  unsigned key;
  unsigned val;
  struct nlist *next;
};

struct nlist *DYNINST_ThreadTable[MAX_NUMBER_OF_THREADS];
unsigned DYNINST_currentNumberOfThreads=0;

void DYNINST_initialize_hash(unsigned total)
{
  int i;
  for (i=0;i<total;i++) DYNINST_ThreadTable[i]=NULL;
}

unsigned DYNINST_hash_lookup(unsigned k)
{
  unsigned pos;
  struct nlist *p;
  pos = HASH(k);
  p=DYNINST_ThreadTable[pos];
  while (p!=NULL) {
    if (p->key==k) break;
    else p=p->next;
  }
  assert(p!=NULL);
  return(p->val);
}

unsigned DYNINST_hash_insert(unsigned k)
{
  unsigned pos;
  struct nlist *p, *tmp;
  pos = HASH(k);
  p=DYNINST_ThreadTable[pos];
  if (p != NULL) {
    while (p!=NULL) {
      tmp=p;
      if (p->key==k) break;
      else p=p->next;
    }
    assert(p==NULL);
    assert(++DYNINST_currentNumberOfThreads<MAX_NUMBER_OF_THREADS);
    p = (struct nlist *) malloc(sizeof(struct nlist));
    p->next=NULL;
    p->key=k;
    p->val=DYNINST_get_free();
    tmp->next=p;
  } else {
    assert(++DYNINST_currentNumberOfThreads<MAX_NUMBER_OF_THREADS);
    p = (struct nlist *) malloc(sizeof(struct nlist));
    p->next=NULL;
    p->key=k;
    p->val=DYNINST_get_free();
    DYNINST_ThreadTable[pos]=p;
  }
  return(p->val);
}

void DYNINST_hash_delete(unsigned k)
{
  unsigned pos;
  struct nlist *p=NULL, *tmp=NULL;
  pos = HASH(k);
  p=DYNINST_ThreadTable[pos];
  assert(p);
  if (p->key==k) {
    DYNINST_ThreadTable[pos] = p->next;
    DYNINST_add_free(tmp->val);
    free(p);
  }
  else {
    while (p!=NULL) {
      if (p->key==k) break;
      else {
        tmp=p;
        p=p->next;
      }
    }
    assert(p!=NULL && tmp!=NULL);
    tmp->next = p->next;
    DYNINST_add_free(p->val);
    free(p);
  }
  DYNINST_currentNumberOfThreads--;
}

/************************************************************************
 * void DYNINSTthreadCreate()
 *
 * track a thr_create() system call, and report to the paradyn daemon.
 ************************************************************************/
void
DYNINSTthreadCreate(int *tid) {
    traceThread traceRec;
    time64 process_time = DYNINSTgetCPUtime();
    time64 wall_time = DYNINSTgetWalltime();
    traceRec.ppid   = getpid();
    traceRec.tid    = *tid;
    traceRec.ntids  = 1;
    traceRec.stride = 0;
    DYNINSTgenerateTraceRecord(0,TR_THREAD,sizeof(traceRec), &traceRec, 1,
			       wall_time,process_time);
    hash_insert(*tid);
}
#endif


/************************************************************************
 *
 ************************************************************************/
int DYNINSTTGroup_CreateUniqueId(int commId)
{
#ifdef PARADYN_MPI
  MPI_Group commGroup;
  MPI_Group worldGroup;
  int       ranks1[1];
  int       ranks2[1];
  int       rc;

  // assert(commId < (1<<16));
  assert(commId < 100000);
  
  rc = MPI_Comm_group(commId, &commGroup);
  assert(rc == 0);
  rc = MPI_Comm_group(MPI_COMM_WORLD, &worldGroup);
  assert(rc == 0);
  
  ranks1[0] = 0;
  rc = MPI_Group_translate_ranks(commGroup, 1, ranks1, worldGroup, ranks2);
  assert(rc == 0);

  assert(ranks2[0] != MPI_UNDEFINED);
  // assert(ranks2[0] < (1 << 16));
  // return(commId + (ranks2[0] * (1<<16)));
  assert(ranks2[0] < 20000);
  return(commId + (ranks2[0] * 100000));
#else
  return(commId);
#endif
}



/************************************************************************
 *
 ************************************************************************/
int DYNINSTTGroup_CreateLocalId(int tgUniqueId)
{
#ifdef PARADYN_MPI
  // return(tgUniqueId | ((1<<16)-1));
  return(tgUniqueId % 100000);
#else
  return(tgUniqueId);
#endif
}


/************************************************************************
 *
 ************************************************************************/
int DYNINSTTGroup_FindUniqueId(int groupId)
{
  int           groupDx = groupId % DYNINSTTagGroupsLimit;
  DynInstTagSt* tagSt;

  for(tagSt = TagGroupInfo.GroupTable[groupDx]; tagSt != NULL;
      tagSt = tagSt->Next) {
    if(tagSt->TagGroupId == groupId) return(tagSt->TGUniqueId);
  }
  return(-1);
}


int DYNINSTTwoComparesAndedExpr(int arg1, int arg2, int arg3, int arg4)
{
  return((arg1 == arg2) && (arg3 == arg4));
}

int DYNINSTCountElmsInArray(int* array, int num)
{
  int sum = 0;

  for(num--; num >= 0; num--) {
    sum += array[num];
  }
  return(sum);
}

int DYNINSTSub(int num1, int num2)
{
  return(num1-num2);
}

int DYNINSTAdd(int num1, int num2)
{
  return(num1+num2);
}

int DYNINSTMult(int num1, int num2)
{
  return(num1*num2);
}

int DYNINSTArrayField(int* array, int index)
{
  return(array[index]);
}
