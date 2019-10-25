//===-- llvm/MC/MCObjectDisassembler.h --------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the declaration of the MCObjectDisassembler class, which
// can be used to construct an MCModule and an MC CFG from an ObjectFile.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_MC_MCOPTIMIZATION_H
#define LLVM_MC_MCOPTIMIZATION_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/DataTypes.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCAnalysis/MCCachingDisassembler.h"
#include "llvm/MC/MCAnalysis/MCFunction.h"
#include "llvm/MC/MCAnalysis/MCModule.h"
#include "llvm/MC/MCAnalysis/MCObjectSymbolizer.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstBuilder.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrAnalysis.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectDisassembler.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include <vector>
#include "llvm/Object/ObjectiveCFile.h"

extern "C" {
  unsigned AArch64GetOpcodeType(unsigned); 
}

namespace llvm {
  
/// \brief Disassemble an ObjectFile to an MCModule and MCFunctions.
/// This class builds on MCDisassembler to create a control flow graph
/// consisting of MCFunctions and MCBasicBlocks.
class MCOptimization {
    #define OP_BL 125
    #define OP_BR 127
  
private: 
  /*
    current important value
   */
  MCModule* cur_module;
  llvm::object::MachOObjectFile* cur_file;
  //llvm::object::SectionRef* stub_section; 
  uint64_t stub_section_add;
  uint64_t stub_section_size;
  struct machO_sym_table{
    uint32_t str_index;
    uint32_t pad;
    uint64_t value;
  };
  uint64_t* sym_name_add;
  uint32_t sym_size;
  
  uint32_t BL_OBJ_RELEASE_SIZE;

  const char* objc_release_str = "_objc_release";
  
  std::list<std::string> NoneSideEffectAPI =
    {
/*
 https://clang.llvm.org/docs/AutomaticReferenceCounting.html
 
 id objc_autorelease(id value);
 void objc_autoreleasePoolPop(void *pool);
 void *objc_autoreleasePoolPush(void);
 id objc_autoreleaseReturnValue(id value);
 void objc_copyWeak(id *dest, id *src); x
 void objc_destroyWeak(id *object);
 id objc_initWeak(id *object, id value);
 id objc_loadWeak(id *object);
 id objc_loadWeakRetained(id *object);
 void objc_moveWeak(id *dest, id *src);
 void objc_release(id value);
 id objc_retain(id value);
 id objc_retainAutorelease(id value);
 id objc_retainAutoreleaseReturnValue(id value);
 id objc_retainAutoreleasedReturnValue(id value);
 id objc_retainBlock(id value);
 void objc_storeStrong(id *object, id value);
 id objc_storeWeak(id *object, id value);
 
 */
// for these explicitly state as `Always returns value'
        "_objc_autorelease",
        "_objc_autoreleaseReturnValue",
        "_objc_retain",
        "_objc_retainAutorelease",
        "_objc_retainAutoreleaseReturnValue",
        "_objc_retainAutoreleasedReturnValue",

// for those remainings that take 1 arguments.
        "_objc_release",
        "_objc_autoreleasePoolPop",
        "_objc_autoreleasePoolPush",
        "_objc_destroyWeak",
        "_objc_loadWeak",
        "_objc_loadWeakRetained",
            
    };
  
    
  const uint64_t stub_item_size = 0xC;
  /*
  
   */
  void analyze_macho_file_for_dynamic_symbol_name(llvm::object::MachOObjectFile*);
  void optimize_func_code(MCFunction*);
  char* get_called_func_name(uint64_t);
public:
  MCOptimization(MCModule* target_module, llvm::object::MachOObjectFile* target_obj_file){
    cur_module = target_module;
    cur_file = target_obj_file;
    BL_OBJ_RELEASE_SIZE = 0;
    analyze_macho_file_for_dynamic_symbol_name(cur_file);
  }
  void try_to_optimize();
  uint64_t getOptimizedInstCount() const;

};
}
#endif
