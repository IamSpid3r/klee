//===-- ModuleUtil.cpp ----------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/Internal/Support/ModuleUtil.h"
#include "klee/Config/Version.h"
// FIXME: This does not belong here.
#include "../Core/Common.h"

#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/IR/Module.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/DataStream.h"
#else
#include "llvm/Function.h"
#include "llvm/Instructions.h"
#include "llvm/IntrinsicInst.h"
#include "llvm/Module.h"
#endif

#include "llvm/Linker.h"
#include "llvm/Assembly/AssemblyAnnotationWriter.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Support/Path.h"

#include <map>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>

using namespace llvm;
using namespace klee;

Module *klee::linkWithLibrary(Module *module, 
                              const std::string &libraryName) {
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
  if (!sys::fs::exists(libraryName)) {
    klee_error("Link with library %s failed. No such file.",
        libraryName.c_str());
  }

  OwningPtr<MemoryBuffer> Buffer;
  if (error_code ec = MemoryBuffer::getFile(libraryName,Buffer)) {
    klee_error("Link with library %s failed: %s", libraryName.c_str(),
        ec.message().c_str());
  }

  sys::fs::file_magic magic = sys::fs::identify_magic(Buffer->getBuffer());

  LLVMContext &Context = getGlobalContext();
  std::string ErrorMessage;

  if (magic == sys::fs::file_magic::bitcode) {
    Module *Result = 0;
    Result = ParseBitcodeFile(Buffer.get(), Context, &ErrorMessage);


    if (!Result || Linker::LinkModules(module, Result, Linker::DestroySource,
        &ErrorMessage))
      klee_error("Link with library %s failed: %s", libraryName.c_str(),
          ErrorMessage.c_str());

    delete Result;

  } else if (magic == sys::fs::file_magic::archive) {
    OwningPtr<object::Binary> arch;
    if (error_code ec = object::createBinary(Buffer.take(), arch))
      klee_error("Link with library %s failed: %s", libraryName.c_str(),
          ec.message().c_str());

    if (object::Archive *a = dyn_cast<object::Archive>(arch.get())) {
      for (object::Archive::child_iterator i = a->begin_children(),
          e = a->end_children(); i != e; ++i) {
        OwningPtr<object::Binary> child;
        if (i->getAsBinary(child)) {
          // Try opening it as a bitcode file.
          OwningPtr<MemoryBuffer> buff;
          if (error_code ec = i->getMemoryBuffer(buff))
            klee_error("Link with library %s failed: %s", libraryName.c_str(),
                ec.message().c_str());
          Module *Result = 0;
          if (buff)
            Result = ParseBitcodeFile(buff.get(), Context, &ErrorMessage);

          if (!Result || Linker::LinkModules(module, Result,
              Linker::DestroySource, &ErrorMessage))
            klee_error("Link with library %s failed: %s", libraryName.c_str(),
                ErrorMessage.c_str());
          delete Result;

          continue;
        }
        if (object::ObjectFile *o = dyn_cast<object::ObjectFile>(child.get())) {
          klee_warning("Link with library: Object file %s in archive %s found. "
              "Currently not supported.",
              o->getFileName().data(), libraryName.c_str());
        }
      }
    }
  } else if (magic.is_object()) {
    OwningPtr<object::Binary> obj;
    if (object::ObjectFile *o = dyn_cast<object::ObjectFile>(obj.get())) {
      klee_warning("Link with library: Object file %s in archive %s found. "
          "Currently not supported.",
          o->getFileName().data(), libraryName.c_str());
    }
  } else {
    klee_error("Link with library %s failed: Unrecognized file type.",
        libraryName.c_str());
  }

  return module;
#else
  Linker linker("klee", module, false);

  llvm::sys::Path libraryPath(libraryName);
  bool native = false;
    
  if (linker.LinkInFile(libraryPath, native)) {
    klee_error("Linking library %s failed", libraryName.c_str());
  }
    
  return linker.releaseModule();
#endif
}

Function *klee::getDirectCallTarget(CallSite cs) {
  Value *v = cs.getCalledValue();
  if (Function *f = dyn_cast<Function>(v)) {
    return f;
  } else if (llvm::ConstantExpr *ce = dyn_cast<llvm::ConstantExpr>(v)) {
    if (ce->getOpcode()==Instruction::BitCast)
      if (Function *f = dyn_cast<Function>(ce->getOperand(0)))
        return f;

    // NOTE: This assert may fire, it isn't necessarily a problem and
    // can be disabled, I just wanted to know when and if it happened.
    assert(0 && "FIXME: Unresolved direct target for a constant expression.");
  }
  
  return 0;
}

static bool valueIsOnlyCalled(const Value *v) {
  for (Value::const_use_iterator it = v->use_begin(), ie = v->use_end();
       it != ie; ++it) {
    if (const Instruction *instr = dyn_cast<Instruction>(*it)) {
      if (instr->getOpcode()==0) continue; // XXX function numbering inst
      if (!isa<CallInst>(instr) && !isa<InvokeInst>(instr)) return false;
      
      // Make sure that the value is only the target of this call and
      // not an argument.
      for (unsigned i=1,e=instr->getNumOperands(); i!=e; ++i)
        if (instr->getOperand(i)==v)
          return false;
    } else if (const llvm::ConstantExpr *ce = 
               dyn_cast<llvm::ConstantExpr>(*it)) {
      if (ce->getOpcode()==Instruction::BitCast)
        if (valueIsOnlyCalled(ce))
          continue;
      return false;
    } else if (const GlobalAlias *ga = dyn_cast<GlobalAlias>(*it)) {
      // XXX what about v is bitcast of aliasee?
      if (v==ga->getAliasee() && !valueIsOnlyCalled(ga))
        return false;
    } else {
      return false;
    }
  }

  return true;
}

bool klee::functionEscapes(const Function *f) {
  return !valueIsOnlyCalled(f);
}
