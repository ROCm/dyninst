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

#if !defined(INSTRUCTION_CATEGORIES_H)
#    define INSTRUCTION_CATEGORIES_H

#    include "util.h"
#    include "entryIDs.h"

namespace Dyninst
{
namespace InstructionAPI
{
enum InsnCategory
{
    c_CallInsn,
    c_ReturnInsn,
    c_BranchInsn,
    c_CompareInsn,
    c_PrefetchInsn,
    c_SysEnterInsn,
    c_SyscallInsn,
    c_VectorInsn,
    c_GPUKernelExitInsn,
    c_NoCategory
};
INSTRUCTION_EXPORT InsnCategory
entryToCategory(entryID e);

}  // namespace InstructionAPI
}  // namespace Dyninst

#endif  //! defined(INSTRUCTION_CATEGORIES_H)
