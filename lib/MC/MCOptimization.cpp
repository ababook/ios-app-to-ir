//===- lib/MC/MCOptimization.cpp - MCOptimization implement --------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// author: Xiesikefu
// function: optimize the MC code to minimize the size of constructed IR
//
//===----------------------------------------------------------------------===//

#include "llvm/MC/MCOptimization.h"
//#include "Target/AArch64/AArch64.h"

using namespace llvm;
using namespace object;


/*
 dynamic symbol table: value 
  value-> symboltable index 
  symbol table index-> symbol table string table index
  string table index -> symbol name
 */
void MCOptimization::analyze_macho_file_for_dynamic_symbol_name(llvm::object::MachOObjectFile* target_file){
    bool has_stub_section = false;
    bool has_string_table = false;
    for(section_iterator target_section = target_file->section_begin();
    target_section!=target_file->section_end(); ++target_section){
        StringRef Name;
        target_section->getName(Name);
        if(Name == "__stubs"){
            has_stub_section = true;
            stub_section_add = target_section->getAddress();
            stub_section_size = target_section->getSize();
        }
    }
    MachO::dysymtab_command tmp_dysymtab_cmd = target_file->getDysymtabLoadCommand();
    MachO::symtab_command tmp_symtab_cmd = target_file->getSymtabLoadCommand();
    StringRef file_data = target_file->getData();
    uint32_t dysymtable_off = tmp_dysymtab_cmd.indirectsymoff;
    uint32_t dysymtable_size =  tmp_dysymtab_cmd.nindirectsyms;
    uint32_t symtable_off = tmp_symtab_cmd.symoff;
    uint32_t symtable_size = tmp_symtab_cmd.nsyms;
    uint32_t str_off = tmp_symtab_cmd.stroff;

    uint8_t* file_add = (uint8_t*)file_data.data();
    uint64_t* dysymtable_name_add = (uint64_t*)malloc(sizeof(uint64_t)*dysymtable_size);
    uint32_t* dysymtable_add = (uint32_t*)(file_add + dysymtable_off);
    machO_sym_table* symtab_add = (machO_sym_table*)(file_add + symtable_off);
    uint8_t* str_add = file_add+str_off;
    
    // get the dynamic sym table name address 
    // the stub index is the dynamic symbol table index
    for(uint32_t tmp_i=0;tmp_i<dysymtable_size;tmp_i++){
        uint32_t tmp_sym_value = *(dysymtable_add+tmp_i);
        if(tmp_sym_value>symtable_size){
            continue;
        }
        machO_sym_table* tmp_sym_table = symtab_add+tmp_sym_value;
        uint32_t tmp_str_idx = tmp_sym_table->str_index;
        uint8_t* sym_name_add = str_add+tmp_str_idx;
        *(dysymtable_name_add+tmp_i) = (uint64_t)sym_name_add;
    }
    sym_name_add = dysymtable_name_add;
    sym_size = dysymtable_size;
}

char* MCOptimization::get_called_func_name(uint64_t target_address){
    
   
    if(target_address < stub_section_add){
        return nullptr;
    }
    if(target_address>(stub_section_add+stub_section_size)){
        return nullptr;
    }
    uint64_t stub_index = (target_address-stub_section_add)/stub_item_size;
    if(stub_index>sym_size){
        llvm_unreachable("stub index large than symbol table size");
    }
    uint64_t called_func_name_add  = *(sym_name_add+stub_index);
    char* tmp_name;
    tmp_name = (char*)called_func_name_add;
    return tmp_name;
}


/*
  optimize different pattern code 
 */
void MCOptimization::optimize_func_code(MCFunction* target_func){
    for(MCFunction::iterator BI = target_func->begin(),
    BE = target_func->end();BI!=BE;BI++){
        MCBasicBlock* tmp_bb;
        tmp_bb = *BI;
        std::vector<MCDecodedInst> optimized_inst = std::vector<MCDecodedInst>();
        for(auto tmp_decode_inst=tmp_bb->begin();
        tmp_decode_inst!=tmp_bb->end();tmp_decode_inst++){
            MCInst* tmp_inst; 
            tmp_inst = (MCInst*)&(tmp_decode_inst->Inst);
            unsigned tmp_opcode = tmp_inst->getOpcode();
            switch(tmp_opcode){
                default:
                break;
                case OP_BL:{
                    if(tmp_inst->getNumOperands()!=1){
                        llvm_unreachable("OP_BL has more than one operand");
                    }
                    MCOperand tmp_operand = tmp_inst->getOperand(0);
                    if(tmp_operand.isImm()==false){
                      llvm_unreachable("OP_BL the first operand is not imm");
                    }
                    uint64_t target_address = tmp_decode_inst->Address;
                    target_address = target_address + tmp_operand.getImm()*4;
                    //if(target_address>cur_file->){
                    char* target_func_name =  get_called_func_name(target_address);
                    if(target_func_name==nullptr){
                        break;
                    }
                    
                    for (std::string s : NoneSideEffectAPI) {
                        if(strcmp(target_func_name, s.c_str()) == 0){
//                            errs() << s << "\n";
                           // optimized_inst.push_back(*tmp_decode_inst);
                           uint64_t cur_inst_add = tmp_decode_inst->Address;
                            tmp_inst->setOpcode(0);
                            BL_OBJ_RELEASE_SIZE++;
//                            auto pre_decode_inst = tmp_decode_inst;
//                            pre_decode_inst--;
//                            const MCInst* tmp_pre_inst = &pre_decode_inst->Inst;
//                            StringRef tmp_name;
//                            tmp_name = target_func->getName();
//                            unsigned op_code = tmp_pre_inst->getOpcode();
//                            unsigned op_num = tmp_pre_inst->getNumOperands();
//                            for(int tmp_i=0;tmp_i<op_num;tmp_i++){
//                                MCOperand tmp_op = tmp_pre_inst->getOperand(tmp_i);
//                                if(tmp_op.isImm()){
//                                    unsigned tmp_imm = tmp_op.getImm();
//                                    //errs()<<"op "<<tmp_i<<" is imm : "<<tmp_imm<<"\n";
//                                }
//                                if(tmp_op.isReg()){
//                                    unsigned tmp_reg = tmp_op.getReg();
//                                     //errs()<<"op "<<tmp_i<<" is reg : "<<tmp_reg<<"\n";
//                                }
//                            }
                        }

                    }
                }
            }
       }
       
    }
}

void MCOptimization::try_to_optimize(){
    for(MCModule::func_iterator FI = cur_module->func_begin(),
    FE = cur_module->func_end();FI !=FE; ++FI){
        MCFunction* tmp_func;
        tmp_func = &(**FI);
        optimize_func_code(tmp_func);
    }
//    errs()<<"bl _obj_release size : "<<BL_OBJ_RELEASE_SIZE<<"\n";
}

uint64_t MCOptimization::getOptimizedInstCount() const {return BL_OBJ_RELEASE_SIZE;}

//http://infocenter.arm.com/help/index.jsp?topic=/com.arm.doc.dui0489h/Cjafcggi.html

