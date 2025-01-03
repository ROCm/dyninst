/*
 * See the dyninst/COPYRIGHT file for copyright information.
 *
 * We provide the Paradyn Tools (below described as "Paradyn")
 * on an AS IS basis, and do not warrant its validity or performance.
 * We reserve the right to update, modify, or discontinue this
 * software at any time.  We shall have no obligation to supply such
 * updates or modifications or any other form of support to you.
 *
 * By your use of Paradyn, you understand and agree that we (or any
 * other person or entity with proprietary rights in Paradyn) are
 * under no obligation to provide either maintenance services,
 * update services, notices of latent defects, or correction of
 * defects for Paradyn.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#if !defined(PROC_SERVICE_WRAPPER_H_)
#    define PROC_SERVICE_WRAPPER_H_

#    if defined(os_freebsd)
#        include <proc_service.h>
#    else
/**
 * Note.  The following contents of this file come from glibc proc_service.h.  They
 *probably should have distributed this file, but didn't.  Since we're all LGPL this is
 *just a little friendly sharing.
 **/

/* The definitions in this file must correspond to those in the debugger.  */
#        include <sys/procfs.h>

/* Functions in this interface return one of these status codes.  */
typedef enum
{
    PS_OK,      /* Generic "call succeeded".  */
    PS_ERR,     /* Generic error. */
    PS_BADPID,  /* Bad process handle.  */
    PS_BADLID,  /* Bad LWP identifier.  */
    PS_BADADDR, /* Bad address.  */
    PS_NOSYM,   /* Could not find given symbol.  */
    PS_NOFREGS  /* FPU register set not available for given LWP.  */
} ps_err_e;

/* This type is opaque in this interface.
   It's defined by the user of libthread_db.  */
struct ps_prochandle;

/* Read or write process memory at the given address.  */
extern PC_EXPORT ps_err_e
ps_pdread(struct ps_prochandle*, psaddr_t, void*, size_t);
extern PC_EXPORT ps_err_e
ps_pdwrite(struct ps_prochandle*, psaddr_t, const void*, size_t);
extern PC_EXPORT ps_err_e
ps_ptread(struct ps_prochandle*, psaddr_t, void*, size_t);
extern PC_EXPORT ps_err_e
ps_ptwrite(struct ps_prochandle*, psaddr_t, const void*, size_t);

/* Get and set the given LWP's general or FPU register set.  */
extern PC_EXPORT ps_err_e
ps_lgetregs(struct ps_prochandle*, lwpid_t, prgregset_t);
extern PC_EXPORT ps_err_e
ps_lsetregs(struct ps_prochandle*, lwpid_t, const prgregset_t);
extern PC_EXPORT ps_err_e
ps_lgetfpregs(struct ps_prochandle*, lwpid_t, prfpregset_t*);
extern PC_EXPORT ps_err_e
ps_lsetfpregs(struct ps_prochandle*, lwpid_t, const prfpregset_t*);

/* Return the PID of the process.  */
extern PC_EXPORT pid_t
ps_getpid(struct ps_prochandle*);

/* Fetch the special per-thread address associated with the given LWP.
   This call is only used on a few platforms (most use a normal register).
   The meaning of the `int' parameter is machine-dependent.  */
extern PC_EXPORT ps_err_e
ps_get_thread_area(const struct ps_prochandle*, lwpid_t, int, psaddr_t*);

/* Look up the named symbol in the named DSO in the symbol tables
   associated with the process being debugged, filling in *SYM_ADDR
   with the corresponding run-time address.  */
extern PC_EXPORT ps_err_e
ps_pglobal_lookup(struct ps_prochandle*, const char* object_name, const char* sym_name,
                  psaddr_t* sym_addr);

/* Stop or continue the entire process.  */
extern PC_EXPORT ps_err_e
ps_pstop(const struct ps_prochandle*);
extern PC_EXPORT ps_err_e
ps_pcontinue(const struct ps_prochandle*);

/* Stop or continue the given LWP alone.  */
extern PC_EXPORT ps_err_e
ps_lstop(const struct ps_prochandle*, lwpid_t);
extern PC_EXPORT ps_err_e
ps_lcontinue(const struct ps_prochandle*, lwpid_t);

#    endif
#endif
