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

#if !defined(INSTRUCTIONAST_H)
#    define INSTRUCTIONAST_H


#include "util.h"
#include <string>
#include <vector>
#include <set>
#include <iostream>
#include "Result.h"
#include "ArchSpecificFormatters.h"
#include "boost/enable_shared_from_this.hpp"

namespace Dyninst
{
namespace InstructionAPI
{
class InstructionAST;

      InstructionAST();
      InstructionAST(const InstructionAST&) = default;
      virtual ~InstructionAST();

    InstructionAST();
    virtual ~InstructionAST();

    /// Compare two AST nodes for equality.
    ///
    /// Non-leaf nodes are equal
    /// if they are of the same type and their children are equal.  %RegisterASTs
    /// are equal if they represent the same register.  %Immediates are equal if they
    /// represent the same value.
    bool operator==(const InstructionAST& rhs) const;

    /// Children of this node are appended to the vector \c children
    virtual void getChildren(vector<InstructionAST::Ptr>& children) const = 0;

    /// \param uses The use set of this node is appended to the vector \c uses
    ///
    /// The use set of an %InstructionAST is defined as follows:
    ///   - A %RegisterAST uses itself
    ///   - A %BinaryFunction uses the use sets of its children
    ///   - An %Immediate uses nothing
    ///   - A %Dereference uses the use set of its child
    virtual void getUses(set<InstructionAST::Ptr>& uses) = 0;

    /// \return True if \c findMe is used by this AST node.
    /// \param findMe AST node to find in the use set of this node
    ///
    /// Unlike \c getUses, \c isUsed looks for \c findMe as a subtree
    /// of the current tree.  \c getUses is designed to return a minimal
    /// set of registers used in this tree, whereas \c isUsed is designed
    /// to allow searches for arbitrary subexpressions
    virtual bool isUsed(InstructionAST::Ptr findMe) const = 0;

      /// The \c format interface returns the contents of an %InstructionAST
      /// object as a string.  By default, \c format() produces assembly language.
      virtual std::string format(formatStyle how = defaultStyle) const = 0;
  
    protected:
      friend class RegisterAST;
      friend class Immediate;
      virtual bool isStrictEqual(const InstructionAST& rhs) const= 0;
      virtual bool checkRegID(MachRegister, unsigned int = 0, unsigned int = 0) const;
      virtual const Result& eval() const = 0;
    };
  }
}

    /// The \c format interface returns the contents of an %InstructionAST
    /// object as a string.  By default, \c format() produces assembly language.
    virtual std::string format(formatStyle how = defaultStyle) const = 0;

protected:
    friend class RegisterAST;
    friend class Immediate;
    virtual bool isStrictEqual(const InstructionAST& rhs) const = 0;
    virtual bool checkRegID(MachRegister, unsigned int = 0, unsigned int = 0) const;
    virtual const Result& eval() const = 0;
};
}  // namespace InstructionAPI
}  // namespace Dyninst
#    if defined(_MSC_VER)
#        pragma warning(pop)
#    endif

#endif  //! defined(INSTRUCTIONAST_H)
