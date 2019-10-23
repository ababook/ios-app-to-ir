#define DEBUG_TYPE "llvm-dec"
#include "llvm/ADT/StringExtras.h"
#include <unistd.h>
#include "llvm/ADT/Triple.h"
#include "llvm/DC/DCInstrSema.h"
#include "llvm/DC/DCRegisterSema.h"
#include "llvm/DC/DCTranslator.h"
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
#include "llvm/MC/MCOptimization.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "FunctionNamePass.h"
#include "TailCallPass.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include <thread>

using namespace llvm;
using namespace object;

static cl::opt<std::string>
InputFilename(cl::Positional, cl::desc("Input object file"), cl::Required);

static cl::opt<std::string>
TripleName("triple", cl::desc("Target triple to disassemble for, "
                              "see -version for available targets"));

static cl::opt<uint64_t>
TranslationEntrypoint("entrypoint",
                      cl::desc("Address to start translating from "
                               "(default = object entrypoint)"));

static cl::opt<bool>
AnnotateIROutput("annot", cl::desc("Enable IR output anotations"),
                 cl::init(false));

static cl::opt<bool>
        NoPrint("no-print", cl::desc("Do not print the produced source"),
                         cl::init(false));

static cl::opt<bool>
     PrintBitcode("bc", cl::desc("Bitcode output"),
                      cl::init(false));

static cl::opt<unsigned>
TransOptLevel("O",
              cl::desc("Optimization level. [-O0, -O1, -O2, or -O3] "
                       "(default = '-O0')"),
              cl::Prefix,
              cl::init(0u));
static cl::opt<bool>
EnableDisassemblyCache("enable-mcod-disass-cache",
    cl::desc("Enable the MC Object disassembly instruction cache"),
    cl::init(false), cl::Hidden);

static cl::opt<bool>
OptimizeOption("MC_opt",cl::desc("try to optimize MC instruction"),cl::init(false));

static cl::opt<bool>
RecordAdd("REC_add",cl::desc("start to record the address of instruction"),cl::init(false));

static cl::opt<std::string>
        OutputFilename("o", cl::desc("Output filename"), cl::value_desc("filename"));



static StringRef ToolName;

static const Target *getTarget(const ObjectFile *Obj) {
  // Figure out the target triple.
  Triple TheTriple("unknown-unknown-unknown");
  if (TripleName.empty()) {
  if (Obj) {
    TheTriple.setArch(Triple::ArchType(Obj->getArch()));
    // TheTriple defaults to ELF, and COFF doesn't have an environment:
    // the best we can do here is indicate that it is mach-o.
    if (Obj->isMachO())
      TheTriple.setObjectFormat(Triple::MachO);
  }
  } else {
    TheTriple.setTriple(TripleName);
  }

  // Get the Target.
  std::string Error;
  const Target *TheTarget = TargetRegistry::lookupTarget("", TheTriple, Error);
  if (!TheTarget) {
    errs() << ToolName << ": " << Error;
    return 0;
  }

  // Update the triple name and return the found target.
  TripleName = TheTriple.getTriple();
  return TheTarget;
}

uint32_t get_all_code_size(MCModule* target_module){
  uint32_t code_size=0;
  for(MCModule::func_iterator func = target_module->func_begin();func!=target_module->func_end();func++){
      MCFunction* tmp_func = &(**func);
      for(MCFunction::iterator bb = tmp_func->begin();bb!=tmp_func->end();bb++){
        MCBasicBlock* tmp_bb  = &(**bb);
        code_size += tmp_bb->size();
      }
  }
  return code_size;
}

/*
add by -death
*/

std::mutex result_mod_lock;

void  multi_thread_DC_translate(int cur_thread_num, int total_thread_num,
                                   std::vector<Module*>* result_mod_vec){

  
  auto Binary = createBinary(InputFilename);
  if (std::error_code ec = Binary.getError()) {
    errs() << ToolName << ": '" << InputFilename << "': "
           << ec.message() << ".\n";
    return ;
  }

  ObjectFile *Obj;
  if (!(Obj = dyn_cast<ObjectFile>((*Binary).getBinary())))
    errs() << ToolName << ": '" << InputFilename << "': "
           << "Unrecognized file type.\n";

  const Target *TheTarget = getTarget(Obj);

  std::unique_ptr<const MCRegisterInfo> MRI(
    TheTarget->createMCRegInfo(TripleName));
  if (!MRI) {
    errs() << "error: no register info for target " << TripleName << "\n";
    return ;
  }

  // Set up disassembler.
  std::unique_ptr<const MCAsmInfo> MAI(
    TheTarget->createMCAsmInfo(*MRI, TripleName));
  if (!MAI) {
    errs() << "error: no assembly info for target " << TripleName << "\n";
    return ;
  }

  std::unique_ptr<const MCSubtargetInfo> STI(
      TheTarget->createMCSubtargetInfo(TripleName, "", ""));
  if (!STI) {
    errs() << "error: no subtarget info for target " << TripleName << "\n";
    return ;
  }

  std::unique_ptr<const MCInstrInfo> MII(TheTarget->createMCInstrInfo());
  if (!MII) {
    errs() << "error: no instruction info for target " << TripleName << "\n";
    return ;
  }

  std::unique_ptr<const MCObjectFileInfo> MOFI(new MCObjectFileInfo);
  MCContext Ctx(MAI.get(), MRI.get(), MOFI.get());

  std::unique_ptr<MCDisassembler> DisAsm(
      TheTarget->createMCDisassembler(*STI, Ctx));
  if (!DisAsm) {
    errs() << "error: no disassembler for target " << TripleName << "\n";
    return ;
  }

  std::unique_ptr<MCDisassembler> DisAsmImpl;
  if (EnableDisassemblyCache) {
    DisAsmImpl = std::move(DisAsm);
    DisAsm.reset(new MCCachingDisassembler(*DisAsmImpl, *STI));
  }

  std::unique_ptr<MCInstPrinter> MIP(
      TheTarget->createMCInstPrinter(Triple(TripleName), 0, *MAI, *MII, *MRI));
  if (!MIP) {
    errs() << "error: no instprinter for target " << TripleName << "\n";
    return ;
  }

  std::unique_ptr<MCRelocationInfo> RelInfo(
      TheTarget->createMCRelocationInfo(TripleName, Ctx));
  if (!RelInfo) {
    errs() << "error: no relocation info for target " << TripleName << "\n";
    return ;
  }
  std::unique_ptr<MCObjectSymbolizer> MOS(
      TheTarget->createMCObjectSymbolizer(Ctx, *Obj, std::move(RelInfo)));
  if (!MOS) {
    errs() << "error: no object symbolizer for target " << TripleName << "\n";
    return ;
  }
  // FIXME: should we set the symbolizer on OD? maybe under a CLI option.

  std::unique_ptr<const MCInstrAnalysis> MIA(
      TheTarget->createMCInstrAnalysis(MII.get()));

  std::unique_ptr<MCObjectDisassembler> OD(
      new MCObjectDisassembler(*Obj, *DisAsm, *MIA));
  std::unique_ptr<MCModule> MCM(OD->buildModule());
  

  /*
    add by -death
   */
  if(OptimizeOption){
    uint32_t code_size;
    code_size = get_all_code_size(&(*MCM));
    if(MachOObjectFile *MachO = dyn_cast<MachOObjectFile>(Obj)){
      std::unique_ptr<MCOptimization> MCOpt(new MCOptimization(&(*MCM),MachO));
      MCOpt->try_to_optimize();
    }
    errs()<<"all code size : "<<code_size<<"\n";
  }
  

  TransOpt::Level TOLvl;
  switch (TransOptLevel) {
  default:
    errs() << ToolName << ": invalid optimization level.\n";
    return ;
  case 0: TOLvl = TransOpt::None; break;
  case 1: TOLvl = TransOpt::Less; break;
  case 2: TOLvl = TransOpt::Default; break;
  case 3: TOLvl = TransOpt::Aggressive; break;
  }    
  // FIXME: should we have a non-default datalayout?
  DataLayout DL("");
  
  
  std::unique_ptr<DCRegisterSema> DRS(
      TheTarget->createDCRegisterSema(TripleName, *MRI, *MII, DL));
  if (!DRS) {
    errs() << "error: no dc register sema for target " << TripleName << "\n";
    return ;
  }
  std::unique_ptr<DCInstrSema> DIS(
      TheTarget->createDCInstrSema(TripleName, *DRS, *MRI, *MII));
  if (!DIS) {
    errs() << "error: no dc instruction sema for target " << TripleName << "\n";
    return ;
  }
  
  /*
  add by -death 
   */
  DIS->set_record_add(RecordAdd);
  /*
  add by -death end 
  */
  LLVMContext* tmp_llvm_context = new LLVMContext();

  std::unique_ptr<DCTranslator> DT(
    new DCTranslator(*tmp_llvm_context, DL,
                     TOLvl, *DIS, *DRS, *MIP, *STI, *MCM,
                     OD.get(), AnnotateIROutput));

  if (!TranslationEntrypoint)
    TranslationEntrypoint = MOS->getEntrypoint();

  //DT->tranlsateTargetNumFunction(cur_thread_num,total_thread_num);
  DT->translateTargetNumFunction(cur_thread_num,total_thread_num);
  
  Module* result_mod;
  result_mod = DT->getCurrentTranslationModule();
  if (MachOObjectFile *MachO = dyn_cast<MachOObjectFile>(Obj)) {
        legacy::PassManager *pm = new legacy::PassManager();
        MCObjectDisassembler::AddressSetTy functionStarts = OD->findFunctionStarts();
//        pm->add(new TailCallPass(functionStarts));
        pm->add(new FunctionNamePass(MachO, DisAsm));
        pm->run(*DT->getCurrentTranslationModule());
    }
  std::lock_guard<std::mutex> lock(result_mod_lock);
  errs()<<"cur thread function size : "<<result_mod->getFunctionList().size()<<"\n";
  result_mod_vec->push_back(result_mod);
  DT->releaseAllModule();
  
}
/*
add by -death end 
*/

/*
add by -death 
*/

void repair_reference(Value* ori_method, Value* real_method){
  errs()<<"ori method "<<ori_method->getName()<<" use size : "<<ori_method->getNumUses()<<"\n";
  for(auto user_start = ori_method->user_begin(), user_end = ori_method->user_end();
            user_start!=user_end;){
              User* tmp_user;
              tmp_user = *user_start;
              user_start++;
              for(int tmp_i=0;tmp_i<tmp_user->getNumOperands();tmp_i++){
                if(tmp_user->getOperand(tmp_i)==ori_method){
                  //errs()<<"this is triggered\n";
                  //errs()<<"replace the call in method"<<tmp_user->getFun
                  tmp_user->setOperand(tmp_i,real_method);
                  CallInst* tmp_inst = dyn_cast<CallInst>(tmp_user);
                  
                  if(tmp_inst ==nullptr){
                    continue;
                  }
                 // errs()<<"replace method : "<<tmp_inst->getParent()->getParent()->getName()
                  //<<" instruction  calls the function "<<ori_method->getName()<<"\n";
                  ValueName* tmp_val_name = tmp_inst->getValueName();
                  tmp_inst->erasethisValueName();
                  tmp_inst->mutateFunctionType(((Function*)real_method)->getFunctionType());
                  tmp_inst->setValueName(tmp_val_name);
                }
              }
              
            }
}

void repair_type_reference(Module* target_module){
  int conflict_size = 0;

  for(auto Func_S = target_module->begin(),Func_E = target_module->end();Func_S!=Func_E;Func_S++){
    Function* target_func = dyn_cast<Function>(Func_S);
    for(auto BB_S = target_func->begin(),BB_E = target_func->end();BB_S!=BB_E;BB_S++){
      BasicBlock* target_bb = dyn_cast<BasicBlock>(BB_S);
      for(auto Inst_S = target_bb->begin(),Inst_E = target_bb->end();Inst_S!=Inst_E;Inst_S++){
        CallInst* target_inst = dyn_cast<CallInst>(Inst_S);
        if(target_inst==nullptr){
          continue;
        }
        Function* callee = target_inst->getCalledFunction();
        if(callee == nullptr){
          continue;
        }
        FunctionType* called_fty = target_inst->getFunctionType();
        for(int tmp_i=0;tmp_i<target_inst->getNumArgOperands();tmp_i++){

        
          if(target_inst->getArgOperand(tmp_i)->getType() != called_fty->getFunctionParamType(tmp_i)){
            
            int call_user_num = 0;
            for(auto user_S = target_inst->getArgOperand(tmp_i)->user_begin(), 
              user_E = target_inst->getArgOperand(tmp_i)->user_end();user_S != user_E; user_S++){
                CallInst* target_inst = dyn_cast<CallInst>(*user_S);
                if(target_inst == nullptr){
                  call_user_num++;
                }
              }
            if(call_user_num > 1 ){
              conflict_size++;
            }
          }
        }
        
      }
    }
  }
  errs()<<"conflict size "<<conflict_size<<"\n";
}

Module* combine_all_the_module(std::vector<Module*>* all_the_module){
  LLVMContext* tmp_llvm_context = new LLVMContext();
  Module* new_module = new Module("final module",*tmp_llvm_context);
  errs()<<new_module->getModuleIdentifier()<<"\n";
  std::map<std::string,Value*> method_declaration;
  std::set<std::string> method_already_insert; 
  for (int tmp_i=0;tmp_i<all_the_module->size();tmp_i++){
    Module* tmp_mod = all_the_module->at(tmp_i);
    for(auto S = tmp_mod->begin(),E = tmp_mod->end();S!=E;S++){
      std::string tmp_name = S->getName();
      if(method_declaration.count(tmp_name)==0){
        method_declaration[tmp_name] = &*S;
      }
      else{
        if(S->isDeclaration()==false){
          method_declaration[tmp_name] = &*S;
        }
      }
    }
  
    
  }
  errs()<<"method size : "<<method_declaration.size()<<"\n";
  for(auto S = method_declaration.begin(),E = method_declaration.end();S!=E;S++){
   // errs()<<"method name : "<<S->first<<"\n";
  //  errs()<<"is declaration : "<<S->second<<"\n";
  }

  for(int tmp_i=0;tmp_i<all_the_module->size();tmp_i++){
    Module* tmp_mod = all_the_module->at(tmp_i);
    //tmp_mod->ValSymTab->dump();
    //errs()<<tmp_mod->GlobalScopeAsm<<"\n";
    DataLayout tmp_dl = tmp_mod->DL;
    int function_size = tmp_mod->FunctionList.size();
    auto S = tmp_mod->begin();
    auto E = tmp_mod->end();
    while(S!=E){
      Function* tmp_func;
      tmp_func = S;
      S++;
      std::string tmp_func_name = tmp_func->getName();
      if(tmp_func != method_declaration[tmp_func_name]){
   //     repair_reference(tmp_func,method_declaration[tmp_func_name]);
      //  continue;
      }
      tmp_func->setParent(nullptr);
      //method_already_insert.insert(tmp_func->getName());
      new_module->FunctionList.push_back(tmp_func);
    }
    
    
  }
  return new_module;
}
/*
add by -death end 
*/

int main(int argc, char **argv) {
    //git
  //result_mod_vec = new std::vector<Module*>();
  
  sys::PrintStackTraceOnErrorSignal();
  PrettyStackTraceProgram X(argc, argv);
  llvm_shutdown_obj Y;

  InitializeAllTargetInfos();
  InitializeAllTargetDCs();
  InitializeAllTargetMCs();
  InitializeAllAsmParsers();
  InitializeAllDisassemblers();

  cl::ParseCommandLineOptions(argc, argv, "Function disassembler\n");

  ToolName = argv[0];

  /*
  add by -death
  */
 std::vector<Module*>* result_mod_vec = new std::vector<Module*>();
  if(result_mod_vec->empty()==false){
    errs()<<"result_mod_vec != empty\n";
  }
  //multi_thread_DC_translate(TheTarget,&*MRI,&*MII,TOLvl,
    //        &*MIP,&*STI,&*MCM,&*OD,&*MOS,0,1);

  int thread_size = 16;
  std::vector<std::thread> tmp_thread_vector;
  for(int tmp_i=0;tmp_i<thread_size; tmp_i++){
    std::thread tmp_t(multi_thread_DC_translate,tmp_i,thread_size,result_mod_vec);
    tmp_thread_vector.push_back(std::move(tmp_t));
  }

  for(int tmp_i=0;tmp_i<thread_size; tmp_i++){
    tmp_thread_vector.at(tmp_i).join();
  }


  Module* final_result_mod;
 // final_result_mod = combine_all_the_module(result_mod_vec);
  //final_result_mod = result_mod_vec->at(0);
 // repair_type_reference(final_result_mod);
//  int q = final_result_mod->FunctionList.size();
  

 // errs()<<"result : "<<q<<"\n";
   if (!NoPrint) {
      for(int tmp_i=0;tmp_i<thread_size;tmp_i++){
        std::error_code EC;
        sys::fs::OpenFlags OpenFlags = sys::fs::F_None;
        std::string output_name(OutputFilename.c_str());
        output_name.append(std::to_string(tmp_i));
        std::unique_ptr<tool_output_file> FDOut = llvm::make_unique<tool_output_file>(output_name, EC,
                                                         OpenFlags);
        if (EC) {
            errs() << EC.message() << '\n';
            return -1;
        }

        if (PrintBitcode) {
            //WriteBitcodeToFile(result_mod_vec->at(0),FDOut->os(),true);
            WriteBitcodeToFile(result_mod_vec->at(tmp_i), FDOut->os(), true);
        } else {
           // FDOut->os() << *result_mod_vec->at(0);
            FDOut->os() << *result_mod_vec->at(tmp_i);
        }

        FDOut->keep();
      }
        
        //DT->printCurrentModule(FDOut->os());


    }
  /*
  add by -death end 
  */
  /*
  auto Binary = createBinary(InputFilename);
  if (std::error_code ec = Binary.getError()) {
    errs() << ToolName << ": '" << InputFilename << "': "
           << ec.message() << ".\n";
    return 1;
  }

  ObjectFile *Obj;
  if (!(Obj = dyn_cast<ObjectFile>((*Binary).getBinary())))
    errs() << ToolName << ": '" << InputFilename << "': "
           << "Unrecognized file type.\n";

  const Target *TheTarget = getTarget(Obj);

  std::unique_ptr<const MCRegisterInfo> MRI(
    TheTarget->createMCRegInfo(TripleName));
  if (!MRI) {
    errs() << "error: no register info for target " << TripleName << "\n";
    return 1;
  }

  // Set up disassembler.
  std::unique_ptr<const MCAsmInfo> MAI(
    TheTarget->createMCAsmInfo(*MRI, TripleName));
  if (!MAI) {
    errs() << "error: no assembly info for target " << TripleName << "\n";
    return 1;
  }

  std::unique_ptr<const MCSubtargetInfo> STI(
      TheTarget->createMCSubtargetInfo(TripleName, "", ""));
  if (!STI) {
    errs() << "error: no subtarget info for target " << TripleName << "\n";
    return 1;
  }

  std::unique_ptr<const MCInstrInfo> MII(TheTarget->createMCInstrInfo());
  if (!MII) {
    errs() << "error: no instruction info for target " << TripleName << "\n";
    return 1;
  }

  std::unique_ptr<const MCObjectFileInfo> MOFI(new MCObjectFileInfo);
  MCContext Ctx(MAI.get(), MRI.get(), MOFI.get());

  std::unique_ptr<MCDisassembler> DisAsm(
      TheTarget->createMCDisassembler(*STI, Ctx));
  if (!DisAsm) {
    errs() << "error: no disassembler for target " << TripleName << "\n";
    return 1;
  }

  std::unique_ptr<MCDisassembler> DisAsmImpl;
  if (EnableDisassemblyCache) {
    DisAsmImpl = std::move(DisAsm);
    DisAsm.reset(new MCCachingDisassembler(*DisAsmImpl, *STI));
  }

  std::unique_ptr<MCInstPrinter> MIP(
      TheTarget->createMCInstPrinter(Triple(TripleName), 0, *MAI, *MII, *MRI));
  if (!MIP) {
    errs() << "error: no instprinter for target " << TripleName << "\n";
    return 1;
  }

  std::unique_ptr<MCRelocationInfo> RelInfo(
      TheTarget->createMCRelocationInfo(TripleName, Ctx));
  if (!RelInfo) {
    errs() << "error: no relocation info for target " << TripleName << "\n";
    return 1;
  }
  std::unique_ptr<MCObjectSymbolizer> MOS(
      TheTarget->createMCObjectSymbolizer(Ctx, *Obj, std::move(RelInfo)));
  if (!MOS) {
    errs() << "error: no object symbolizer for target " << TripleName << "\n";
    return 1;
  }
  // FIXME: should we set the symbolizer on OD? maybe under a CLI option.

  std::unique_ptr<const MCInstrAnalysis> MIA(
      TheTarget->createMCInstrAnalysis(MII.get()));

  std::unique_ptr<MCObjectDisassembler> OD(
      new MCObjectDisassembler(*Obj, *DisAsm, *MIA));
  std::unique_ptr<MCModule> MCM(OD->buildModule());
  */

  /*
    add by -death
   */
  /*
  if(OptimizeOption){
    uint32_t code_size;
    code_size = get_all_code_size(&(*MCM));
    if(MachOObjectFile *MachO = dyn_cast<MachOObjectFile>(Obj)){
      std::unique_ptr<MCOptimization> MCOpt(new MCOptimization(&(*MCM),MachO));
      MCOpt->try_to_optimize();
    }
    errs()<<"all code size : "<<code_size<<"\n";
  }
  */
  
  /*
    add by -death end 
   */
  /*
  if (!MCM)
    return 1;

  TransOpt::Level TOLvl;
  switch (TransOptLevel) {
  default:
    errs() << ToolName << ": invalid optimization level.\n";
    return 1;
  case 0: TOLvl = TransOpt::None; break;
  case 1: TOLvl = TransOpt::Less; break;
  case 2: TOLvl = TransOpt::Default; break;
  case 3: TOLvl = TransOpt::Aggressive; break;
  }
  

  // FIXME: should we have a non-default datalayout?

  DataLayout DL("");
  

  std::unique_ptr<DCRegisterSema> DRS(
      TheTarget->createDCRegisterSema(TripleName, *MRI, *MII, DL));
  if (!DRS) {
    errs() << "error: no dc register sema for target " << TripleName << "\n";
    return 1;
  }
  std::unique_ptr<DCInstrSema> DIS(
      TheTarget->createDCInstrSema(TripleName, *DRS, *MRI, *MII));
  if (!DIS) {
    errs() << "error: no dc instruction sema for target " << TripleName << "\n";
    return 1;
  }
  /*
  add by -death 
   */
  //DIS->set_record_add(RecordAdd);
  /*
  add by -death end 
  */
  /*
  std::unique_ptr<DCTranslator> DT(
    new DCTranslator(getGlobalContext(), DL,
                     TOLvl, *DIS, *DRS, *MIP, *STI, *MCM,
                     OD.get(), AnnotateIROutput));

  if (!TranslationEntrypoint)
    TranslationEntrypoint = MOS->getEntrypoint();

//  DT->createMainFunctionWrapper(
//      DT->translateRecursivelyAt(TranslationEntrypoint));
    //DT->translateAllKnownFunctions();
    */
    /*
    *
    * add by -death
    */
    /*
    DT->translateOneFunction();
    Module* result_mod  = DT->getCurrentTranslationModule();
    int func_list_size = result_mod->getFunctionList().size();
    for (auto S = result_mod->begin(), E = result_mod->end();S!=E;S++){
      StringRef tmp_name = S->getName();
      bool is_declaration = S->isDeclaration();

    }
    int global_list_size = result_mod->getGlobalList().size();
    int alias_list_size = result_mod->getAliasList().size();
    int NamedMD_list_size = result_mod->getNamedMDList().size();
    //int ValueSymbole_Table_size = result_mod->getValueSymbolTable().
    /*
    add by -death end 
    */
   /*
    Function *main_fn = DT->getCurrentTranslationModule()->getFunction("fn_" + utohexstr(TranslationEntrypoint));

    */
//    assert(main_fn);
/*
    if (main_fn)
        DT->createMainFunctionWrapper(main_fn);

    if (MachOObjectFile *MachO = dyn_cast<MachOObjectFile>(Obj)) {
        legacy::PassManager *pm = new legacy::PassManager();
        MCObjectDisassembler::AddressSetTy functionStarts = OD->findFunctionStarts();
//        pm->add(new TailCallPass(functionStarts));
        pm->add(new FunctionNamePass(MachO, DisAsm));
        pm->run(*DT->getCurrentTranslationModule());
    }

    if (!NoPrint) {
        std::error_code EC;
        sys::fs::OpenFlags OpenFlags = sys::fs::F_None;
        if (!Binary)
            OpenFlags |= sys::fs::F_Text;
        std::unique_ptr<tool_output_file> FDOut = llvm::make_unique<tool_output_file>(OutputFilename, EC,
                                                         OpenFlags);
        if (EC) {
            errs() << EC.message() << '\n';
            return -1;
        }

        if (PrintBitcode) {
            WriteBitcodeToFile(DT->getCurrentTranslationModule(), FDOut->os(), true);
        } else {
            FDOut->os() << *DT->getCurrentTranslationModule();
        }

        FDOut->keep();
        //DT->printCurrentModule(FDOut->os());


    }
    */
  return 0;
}
