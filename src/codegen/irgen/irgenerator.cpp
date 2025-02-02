// Copyright (c) 2014-2015 Dropbox, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "codegen/irgen/irgenerator.h"

#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include "analysis/function_analysis.h"
#include "analysis/scoping_analysis.h"
#include "analysis/type_analysis.h"
#include "codegen/codegen.h"
#include "codegen/compvars.h"
#include "codegen/irgen.h"
#include "codegen/irgen/util.h"
#include "codegen/osrentry.h"
#include "codegen/patchpoints.h"
#include "codegen/type_recording.h"
#include "core/ast.h"
#include "core/cfg.h"
#include "core/types.h"
#include "core/util.h"
#include "runtime/generator.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

extern "C" void dumpLLVM(llvm::Value* v) {
    v->getType()->dump();
    v->dump();
}

llvm::Value* IRGenState::getScratchSpace(int min_bytes) {
    llvm::BasicBlock& entry_block = getLLVMFunction()->getEntryBlock();

    if (scratch_space) {
        assert(scratch_space->getParent() == &entry_block);
        assert(scratch_space->isStaticAlloca());
        if (scratch_size >= min_bytes)
            return scratch_space;
    }

    llvm::AllocaInst* new_scratch_space;
    // If the entry block is currently empty, we have to be more careful:
    if (entry_block.begin() == entry_block.end()) {
        new_scratch_space = new llvm::AllocaInst(g.i8, getConstantInt(min_bytes, g.i64), "scratch", &entry_block);
    } else {
        new_scratch_space = new llvm::AllocaInst(g.i8, getConstantInt(min_bytes, g.i64), "scratch",
                                                 entry_block.getFirstInsertionPt());
    }

    assert(new_scratch_space->isStaticAlloca());

    if (scratch_space)
        scratch_space->replaceAllUsesWith(new_scratch_space);

    scratch_size = min_bytes;
    scratch_space = new_scratch_space;

    return scratch_space;
}

llvm::Value* IRGenState::getFrameInfoVar() {
    if (!frame_info) {
        llvm::BasicBlock& entry_block = getLLVMFunction()->getEntryBlock();

        llvm::IRBuilder<true> builder(&entry_block);

        if (entry_block.begin() != entry_block.end())
            builder.SetInsertPoint(&entry_block, entry_block.getFirstInsertionPt());


        llvm::AllocaInst* al = builder.CreateAlloca(g.frame_info_type, NULL, "frame_info");
        assert(al->isStaticAlloca());

        static_assert(offsetof(FrameInfo, exc) == 0, "");
        static_assert(offsetof(ExcInfo, type) == 0, "");
        llvm::Value* exctype_gep
            = builder.CreateConstInBoundsGEP2_32(builder.CreateConstInBoundsGEP2_32(al, 0, 0), 0, 0);
        builder.CreateStore(embedConstantPtr(NULL, g.llvm_value_type_ptr), exctype_gep);

        frame_info = al;
    }
    return frame_info;
}

ScopeInfo* IRGenState::getScopeInfo() {
    return getSourceInfo()->getScopeInfo();
}

ScopeInfo* IRGenState::getScopeInfoForNode(AST* node) {
    auto source = getSourceInfo();
    return source->scoping->getScopeInfoForNode(node);
}

class IREmitterImpl : public IREmitter {
private:
    IRGenState* irstate;
    IRBuilder* builder;
    llvm::BasicBlock*& curblock;
    IRGenerator* irgenerator;

    llvm::CallSite emitCall(UnwindInfo unw_info, llvm::Value* callee, const std::vector<llvm::Value*>& args) {
        if (unw_info.needsInvoke()) {
            llvm::BasicBlock* normal_dest
                = llvm::BasicBlock::Create(g.context, curblock->getName(), irstate->getLLVMFunction());
            normal_dest->moveAfter(curblock);

            llvm::InvokeInst* rtn = getBuilder()->CreateInvoke(callee, normal_dest, unw_info.exc_dest, args);
            getBuilder()->SetInsertPoint(normal_dest);
            curblock = normal_dest;
            return rtn;
        } else {
            return getBuilder()->CreateCall(callee, args);
        }
    }


    llvm::CallSite emitPatchpoint(llvm::Type* return_type, const ICSetupInfo* pp, llvm::Value* func,
                                  const std::vector<llvm::Value*>& args,
                                  const std::vector<llvm::Value*>& ic_stackmap_args, UnwindInfo unw_info) {
        if (pp == NULL)
            assert(ic_stackmap_args.size() == 0);

        PatchpointInfo* info = PatchpointInfo::create(currentFunction(), pp, ic_stackmap_args.size());
        int64_t pp_id = reinterpret_cast<int64_t>(info);
        int pp_size = pp ? pp->totalSize() : CALL_ONLY_SIZE;

        std::vector<llvm::Value*> pp_args;
        pp_args.push_back(getConstantInt(pp_id, g.i64)); // pp_id: will fill this in later
        pp_args.push_back(getConstantInt(pp_size, g.i32));
        pp_args.push_back(func);
        pp_args.push_back(getConstantInt(args.size(), g.i32));

        pp_args.insert(pp_args.end(), args.begin(), args.end());

        int num_scratch_bytes = info->scratchSize();
        llvm::Value* scratch_space = irstate->getScratchSpace(num_scratch_bytes);
        pp_args.push_back(scratch_space);

        pp_args.insert(pp_args.end(), ic_stackmap_args.begin(), ic_stackmap_args.end());

        irgenerator->addFrameStackmapArgs(info, unw_info.current_stmt, pp_args);

        llvm::Intrinsic::ID intrinsic_id;
        if (return_type->isIntegerTy() || return_type->isPointerTy()) {
            intrinsic_id = llvm::Intrinsic::experimental_patchpoint_i64;
        } else if (return_type->isVoidTy()) {
            intrinsic_id = llvm::Intrinsic::experimental_patchpoint_void;
        } else if (return_type->isDoubleTy()) {
            intrinsic_id = llvm::Intrinsic::experimental_patchpoint_double;
        } else {
            return_type->dump();
            abort();
        }
        llvm::Function* patchpoint = this->getIntrinsic(intrinsic_id);

        llvm::CallSite rtn = this->emitCall(unw_info, patchpoint, pp_args);
        return rtn;
    }

public:
    explicit IREmitterImpl(IRGenState* irstate, llvm::BasicBlock*& curblock, IRGenerator* irgenerator)
        : irstate(irstate), builder(new IRBuilder(g.context)), curblock(curblock), irgenerator(irgenerator) {
        builder->setEmitter(this);
        builder->SetInsertPoint(curblock);
    }

    IRBuilder* getBuilder() override { return builder; }

    GCBuilder* getGC() override { return irstate->getGC(); }

    llvm::Function* getIntrinsic(llvm::Intrinsic::ID intrinsic_id) override {
        return llvm::Intrinsic::getDeclaration(g.cur_module, intrinsic_id);
    }

    llvm::Value* getScratch(int num_bytes) override { return irstate->getScratchSpace(num_bytes); }

    void releaseScratch(llvm::Value* scratch) override { assert(0); }

    CompiledFunction* currentFunction() override { return irstate->getCurFunction(); }
    llvm::BasicBlock* currentBasicBlock() override { return curblock; }

    void setCurrentBasicBlock(llvm::BasicBlock* bb) override {
        curblock = bb;
        getBuilder()->SetInsertPoint(curblock);
    }

    llvm::BasicBlock* createBasicBlock(const char* name) override {
        return llvm::BasicBlock::Create(g.context, name, irstate->getLLVMFunction());
    }

    llvm::Value* createCall(UnwindInfo unw_info, llvm::Value* callee, const std::vector<llvm::Value*>& args) override {
        if (ENABLE_FRAME_INTROSPECTION) {
            llvm::Type* rtn_type = llvm::cast<llvm::FunctionType>(llvm::cast<llvm::PointerType>(callee->getType())
                                                                      ->getElementType())->getReturnType();

            llvm::Value* bitcasted = getBuilder()->CreateBitCast(callee, g.i8->getPointerTo());
            llvm::CallSite cs = emitPatchpoint(rtn_type, NULL, bitcasted, args, {}, unw_info);

            if (rtn_type == cs->getType()) {
                return cs.getInstruction();
            } else if (rtn_type == g.i1) {
                return getBuilder()->CreateTrunc(cs.getInstruction(), rtn_type);
            } else if (llvm::isa<llvm::PointerType>(rtn_type)) {
                return getBuilder()->CreateIntToPtr(cs.getInstruction(), rtn_type);
            } else {
                cs.getInstruction()->getType()->dump();
                rtn_type->dump();
                RELEASE_ASSERT(0, "don't know how to convert those");
            }
        } else {
            return emitCall(unw_info, callee, args).getInstruction();
        }
    }

    llvm::Value* createCall(UnwindInfo unw_info, llvm::Value* callee) override {
        return createCall(unw_info, callee, std::vector<llvm::Value*>());
    }

    llvm::Value* createCall(UnwindInfo unw_info, llvm::Value* callee, llvm::Value* arg1) override {
        return createCall(unw_info, callee, std::vector<llvm::Value*>({ arg1 }));
    }

    llvm::Value* createCall2(UnwindInfo unw_info, llvm::Value* callee, llvm::Value* arg1, llvm::Value* arg2) override {
        return createCall(unw_info, callee, { arg1, arg2 });
    }

    llvm::Value* createCall3(UnwindInfo unw_info, llvm::Value* callee, llvm::Value* arg1, llvm::Value* arg2,
                             llvm::Value* arg3) override {
        return createCall(unw_info, callee, { arg1, arg2, arg3 });
    }

    llvm::Value* createIC(const ICSetupInfo* pp, void* func_addr, const std::vector<llvm::Value*>& args,
                          UnwindInfo unw_info) override {
        assert(irstate->getEffortLevel() != EffortLevel::INTERPRETED);

        std::vector<llvm::Value*> stackmap_args;

        llvm::CallSite rtn
            = emitPatchpoint(pp->hasReturnValue() ? g.i64 : g.void_, pp,
                             embedConstantPtr(func_addr, g.i8->getPointerTo()), args, stackmap_args, unw_info);

        rtn.setCallingConv(pp->getCallingConvention());
        return rtn.getInstruction();
    }
};
IREmitter* createIREmitter(IRGenState* irstate, llvm::BasicBlock*& curblock, IRGenerator* irgenerator) {
    return new IREmitterImpl(irstate, curblock, irgenerator);
}

static std::unordered_map<AST_expr*, std::vector<const std::string*>*> made_keyword_storage;
static std::vector<const std::string*>* getKeywordNameStorage(AST_Call* node) {
    auto it = made_keyword_storage.find(node);
    if (it != made_keyword_storage.end())
        return it->second;

    auto rtn = new std::vector<const std::string*>();
    made_keyword_storage.insert(it, std::make_pair(node, rtn));
    return rtn;
}

const std::string CREATED_CLOSURE_NAME = "#created_closure";
const std::string PASSED_CLOSURE_NAME = "#passed_closure";
const std::string PASSED_GENERATOR_NAME = "#passed_generator";

bool isIsDefinedName(const std::string& name) {
    return startswith(name, "!is_defined_");
}

InternedString getIsDefinedName(InternedString name, InternedStringPool& interned_strings) {
    // TODO could cache this
    return interned_strings.get("!is_defined_" + name.str());
}

class IRGeneratorImpl : public IRGenerator {
private:
    IRGenState* irstate;

    llvm::BasicBlock* curblock;
    IREmitterImpl emitter;
    SymbolTable symbol_table;
    std::unordered_map<CFGBlock*, llvm::BasicBlock*>& entry_blocks;
    CFGBlock* myblock;
    TypeAnalysis* types;

    enum State {
        RUNNING,  // normal
        DEAD,     // passed a Return statement; still syntatically valid but the code should not be compiled
        FINISHED, // passed a pseudo-node such as Branch or Jump; internal error if there are any more statements
    } state;

public:
    IRGeneratorImpl(IRGenState* irstate, std::unordered_map<CFGBlock*, llvm::BasicBlock*>& entry_blocks,
                    CFGBlock* myblock, TypeAnalysis* types)
        : irstate(irstate), curblock(entry_blocks[myblock]), emitter(irstate, curblock, this),
          entry_blocks(entry_blocks), myblock(myblock), types(types), state(RUNNING) {}

    ~IRGeneratorImpl() { delete emitter.getBuilder(); }

private:
    OpInfo getOpInfoForNode(AST* ast, UnwindInfo unw_info) {
        assert(ast);

        EffortLevel effort = irstate->getEffortLevel();
        bool record_types = (effort != EffortLevel::INTERPRETED && effort != EffortLevel::MAXIMAL);

        TypeRecorder* type_recorder;
        if (record_types) {
            type_recorder = getTypeRecorderForNode(ast);
        } else {
            type_recorder = NULL;
        }

        return OpInfo(irstate->getEffortLevel(), type_recorder, unw_info);
    }

    OpInfo getEmptyOpInfo(UnwindInfo unw_info) { return OpInfo(irstate->getEffortLevel(), NULL, unw_info); }

    void createExprTypeGuard(llvm::Value* check_val, AST_expr* node, llvm::Value* node_value,
                             AST_stmt* current_statement) {
        assert(check_val->getType() == g.i1);

        llvm::Metadata* md_vals[]
            = { llvm::MDString::get(g.context, "branch_weights"), llvm::ConstantAsMetadata::get(getConstantInt(1000)),
                llvm::ConstantAsMetadata::get(getConstantInt(1)) };
        llvm::MDNode* branch_weights = llvm::MDNode::get(g.context, llvm::ArrayRef<llvm::Metadata*>(md_vals));

        // For some reason there doesn't seem to be the ability to place the new BB
        // right after the current bb (can only place it *before* something else),
        // but we can put it somewhere arbitrary and then move it.
        llvm::BasicBlock* success_bb
            = llvm::BasicBlock::Create(g.context, "check_succeeded", irstate->getLLVMFunction());
        success_bb->moveAfter(curblock);

        llvm::BasicBlock* deopt_bb = llvm::BasicBlock::Create(g.context, "check_failed", irstate->getLLVMFunction());

        llvm::BranchInst* guard = emitter.getBuilder()->CreateCondBr(check_val, success_bb, deopt_bb, branch_weights);

        curblock = deopt_bb;
        emitter.getBuilder()->SetInsertPoint(curblock);
        llvm::Value* v = emitter.createCall2(UnwindInfo(current_statement, NULL), g.funcs.deopt,
                                             embedConstantPtr(node, g.i8->getPointerTo()), node_value);
        emitter.getBuilder()->CreateRet(v);

        curblock = success_bb;
        emitter.getBuilder()->SetInsertPoint(curblock);
    }

    template <typename T> InternedString internString(T&& s) {
        return irstate->getSourceInfo()->getInternedStrings().get(std::forward<T>(s));
    }

    InternedString getIsDefinedName(InternedString name) {
        return pyston::getIsDefinedName(name, irstate->getSourceInfo()->getInternedStrings());
    }

    CompilerVariable* evalAttribute(AST_Attribute* node, UnwindInfo unw_info) {
        CompilerVariable* value = evalExpr(node->value, unw_info);

        CompilerVariable* rtn = value->getattr(emitter, getOpInfoForNode(node, unw_info), &node->attr.str(), false);
        value->decvref(emitter);
        return rtn;
    }

    CompilerVariable* evalClsAttribute(AST_ClsAttribute* node, UnwindInfo unw_info) {
        CompilerVariable* value = evalExpr(node->value, unw_info);
        CompilerVariable* rtn = value->getattr(emitter, getOpInfoForNode(node, unw_info), &node->attr.str(), true);
        value->decvref(emitter);
        return rtn;
    }

    CompilerVariable* evalLangPrimitive(AST_LangPrimitive* node, UnwindInfo unw_info) {
        switch (node->opcode) {
            case AST_LangPrimitive::ISINSTANCE: {
                assert(node->args.size() == 3);
                CompilerVariable* obj = evalExpr(node->args[0], unw_info);
                CompilerVariable* cls = evalExpr(node->args[1], unw_info);
                CompilerVariable* flags = evalExpr(node->args[2], unw_info);

                ConcreteCompilerVariable* converted_obj = obj->makeConverted(emitter, obj->getBoxType());
                ConcreteCompilerVariable* converted_cls = cls->makeConverted(emitter, cls->getBoxType());
                ConcreteCompilerVariable* converted_flags = flags->makeConverted(emitter, INT);
                obj->decvref(emitter);
                cls->decvref(emitter);
                flags->decvref(emitter);

                llvm::Value* v = emitter.createCall(
                    unw_info, g.funcs.isinstance,
                    { converted_obj->getValue(), converted_cls->getValue(), converted_flags->getValue() });
                assert(v->getType() == g.i1);

                return boolFromI1(emitter, v);
            }
            case AST_LangPrimitive::LANDINGPAD: {
                // llvm::Function* _personality_func = g.stdlib_module->getFunction("__py_personality_v0");
                llvm::Function* _personality_func = g.stdlib_module->getFunction("__gxx_personality_v0");
                assert(_personality_func);
                llvm::Value* personality_func = g.cur_module->getOrInsertFunction(_personality_func->getName(),
                                                                                  _personality_func->getFunctionType());
                assert(personality_func);
                llvm::LandingPadInst* landing_pad = emitter.getBuilder()->CreateLandingPad(
                    llvm::StructType::create(std::vector<llvm::Type*>{ g.i8_ptr, g.i64 }), personality_func, 1);
                landing_pad->addClause(embedConstantPtr(NULL, g.i8_ptr));

                llvm::Value* cxaexc_pointer = emitter.getBuilder()->CreateExtractValue(landing_pad, { 0 });

                if (irstate->getEffortLevel() != EffortLevel::INTERPRETED) {
                    llvm::Value* excinfo_pointer
                        = emitter.getBuilder()->CreateCall(g.funcs.__cxa_begin_catch, cxaexc_pointer);
                    llvm::Value* excinfo_pointer_casted
                        = emitter.getBuilder()->CreateBitCast(excinfo_pointer, g.llvm_excinfo_type->getPointerTo());

                    auto* builder = emitter.getBuilder();
                    llvm::Value* exc_type
                        = builder->CreateLoad(builder->CreateConstInBoundsGEP2_32(excinfo_pointer_casted, 0, 0));
                    llvm::Value* exc_value
                        = builder->CreateLoad(builder->CreateConstInBoundsGEP2_32(excinfo_pointer_casted, 0, 1));
                    llvm::Value* exc_traceback
                        = builder->CreateLoad(builder->CreateConstInBoundsGEP2_32(excinfo_pointer_casted, 0, 2));
                    assert(exc_type->getType() == g.llvm_value_type_ptr);
                    assert(exc_value->getType() == g.llvm_value_type_ptr);
                    assert(exc_traceback->getType() == g.llvm_value_type_ptr);

                    return makeTuple({ new ConcreteCompilerVariable(UNKNOWN, exc_type, true),
                                       new ConcreteCompilerVariable(UNKNOWN, exc_value, true),
                                       new ConcreteCompilerVariable(UNKNOWN, exc_traceback, true) });
                } else {
                    // TODO This doesn't get hit, right?
                    abort();

                    // The interpreter can't really support the full C++ exception handling model since it's
                    // itself written in C++.  Let's make it easier for the interpreter and use a simpler interface:
                    llvm::Value* exc_obj = emitter.getBuilder()->CreateBitCast(cxaexc_pointer, g.llvm_value_type_ptr);
                }
            }
            case AST_LangPrimitive::LOCALS: {
                assert(node->args.size() == 0);

                llvm::Value* v = emitter.getBuilder()->CreateCall(g.funcs.createDict);
                ConcreteCompilerVariable* rtn = new ConcreteCompilerVariable(DICT, v, true);

                for (auto& p : symbol_table) {
                    if (p.first.str()[0] == '!' || p.first.str()[0] == '#')
                        continue;

                    ConcreteCompilerVariable* is_defined_var
                        = static_cast<ConcreteCompilerVariable*>(_getFake(getIsDefinedName(p.first), true));

                    static const std::string setitem_str("__setitem__");
                    if (!is_defined_var) {
                        ConcreteCompilerVariable* converted = p.second->makeConverted(emitter, p.second->getBoxType());

                        // TODO super dumb that it reallocates the name again
                        CallattrFlags flags = {.cls_only = true, .null_on_nonexistent = false };
                        CompilerVariable* _r
                            = rtn->callattr(emitter, getEmptyOpInfo(unw_info), &setitem_str, flags, ArgPassSpec(2),
                                            { makeStr(new std::string(p.first.str())), converted }, NULL);
                        converted->decvref(emitter);
                        _r->decvref(emitter);
                    } else {
                        assert(is_defined_var->getType() == BOOL);

                        llvm::BasicBlock* was_defined
                            = llvm::BasicBlock::Create(g.context, "was_defined", irstate->getLLVMFunction());
                        llvm::BasicBlock* join
                            = llvm::BasicBlock::Create(g.context, "join", irstate->getLLVMFunction());
                        emitter.getBuilder()->CreateCondBr(i1FromBool(emitter, is_defined_var), was_defined, join);

                        emitter.getBuilder()->SetInsertPoint(was_defined);
                        ConcreteCompilerVariable* converted = p.second->makeConverted(emitter, p.second->getBoxType());
                        // TODO super dumb that it reallocates the name again
                        CallattrFlags flags = {.cls_only = true, .null_on_nonexistent = false };
                        CompilerVariable* _r
                            = rtn->callattr(emitter, getEmptyOpInfo(unw_info), &setitem_str, flags, ArgPassSpec(2),
                                            { makeStr(new std::string(p.first.str())), converted }, NULL);
                        converted->decvref(emitter);
                        _r->decvref(emitter);
                        emitter.getBuilder()->CreateBr(join);
                        emitter.getBuilder()->SetInsertPoint(join);
                    }
                }

                return rtn;
            }
            case AST_LangPrimitive::GET_ITER: {
                assert(node->args.size() == 1);
                CompilerVariable* obj = evalExpr(node->args[0], unw_info);
                auto rtn = obj->getPystonIter(emitter, getOpInfoForNode(node, unw_info));
                obj->decvref(emitter);
                return rtn;
            }
            case AST_LangPrimitive::IMPORT_FROM: {
                assert(node->args.size() == 2);
                assert(node->args[0]->type == AST_TYPE::Name);
                assert(node->args[1]->type == AST_TYPE::Str);

                CompilerVariable* module = evalExpr(node->args[0], unw_info);
                ConcreteCompilerVariable* converted_module = module->makeConverted(emitter, module->getBoxType());
                module->decvref(emitter);

                auto ast_str = ast_cast<AST_Str>(node->args[1]);
                assert(ast_str->str_type == AST_Str::STR);
                const std::string& name = ast_str->str_data;
                assert(name.size());

                llvm::Value* r = emitter.createCall2(unw_info, g.funcs.importFrom, converted_module->getValue(),
                                                     embedConstantPtr(&name, g.llvm_str_type_ptr));

                CompilerVariable* v = new ConcreteCompilerVariable(UNKNOWN, r, true);

                converted_module->decvref(emitter);
                return v;
            }
            case AST_LangPrimitive::IMPORT_STAR: {
                assert(node->args.size() == 1);
                assert(node->args[0]->type == AST_TYPE::Name);

                RELEASE_ASSERT(irstate->getSourceInfo()->ast->type == AST_TYPE::Module,
                               "import * not supported in functions (yet)");

                CompilerVariable* module = evalExpr(node->args[0], unw_info);
                ConcreteCompilerVariable* converted_module = module->makeConverted(emitter, module->getBoxType());
                module->decvref(emitter);

                llvm::Value* r = emitter.createCall2(
                    unw_info, g.funcs.importStar, converted_module->getValue(),
                    embedConstantPtr(irstate->getSourceInfo()->parent_module, g.llvm_module_type_ptr));
                CompilerVariable* v = new ConcreteCompilerVariable(UNKNOWN, r, true);

                converted_module->decvref(emitter);
                return v;
            }
            case AST_LangPrimitive::IMPORT_NAME: {
                assert(node->args.size() == 3);
                assert(node->args[0]->type == AST_TYPE::Num);
                assert(static_cast<AST_Num*>(node->args[0])->num_type == AST_Num::INT);
                assert(node->args[2]->type == AST_TYPE::Str);

                int level = static_cast<AST_Num*>(node->args[0])->n_int;

                // TODO this could be a constant Box* too
                CompilerVariable* froms = evalExpr(node->args[1], unw_info);
                ConcreteCompilerVariable* converted_froms = froms->makeConverted(emitter, froms->getBoxType());
                froms->decvref(emitter);

                auto ast_str = ast_cast<AST_Str>(node->args[2]);
                assert(ast_str->str_type == AST_Str::STR);
                const std::string& module_name = ast_str->str_data;

                llvm::Value* imported = emitter.createCall3(unw_info, g.funcs.import, getConstantInt(level, g.i32),
                                                            converted_froms->getValue(),
                                                            embedConstantPtr(&module_name, g.llvm_str_type_ptr));
                ConcreteCompilerVariable* v = new ConcreteCompilerVariable(UNKNOWN, imported, true);

                converted_froms->decvref(emitter);
                return v;
            }
            case AST_LangPrimitive::NONE: {
                return getNone();
            }
            case AST_LangPrimitive::NONZERO: {
                assert(node->args.size() == 1);
                CompilerVariable* obj = evalExpr(node->args[0], unw_info);

                ConcreteCompilerVariable* rtn = obj->nonzero(emitter, getOpInfoForNode(node, unw_info));
                assert(rtn->getType() == BOOL);
                llvm::Value* v = i1FromBool(emitter, rtn);
                assert(v->getType() == g.i1);

                obj->decvref(emitter);

                return boolFromI1(emitter, v);
            }
            case AST_LangPrimitive::SET_EXC_INFO: {
                assert(node->args.size() == 3);
                CompilerVariable* type = evalExpr(node->args[0], unw_info);
                CompilerVariable* value = evalExpr(node->args[1], unw_info);
                CompilerVariable* traceback = evalExpr(node->args[2], unw_info);

                auto* builder = emitter.getBuilder();

                llvm::Value* frame_info = irstate->getFrameInfoVar();
                llvm::Value* exc_info = builder->CreateConstInBoundsGEP2_32(frame_info, 0, 0);
                assert(exc_info->getType() == g.llvm_excinfo_type->getPointerTo());

                ConcreteCompilerVariable* converted_type = type->makeConverted(emitter, UNKNOWN);
                builder->CreateStore(converted_type->getValue(), builder->CreateConstInBoundsGEP2_32(exc_info, 0, 0));
                converted_type->decvref(emitter);
                ConcreteCompilerVariable* converted_value = value->makeConverted(emitter, UNKNOWN);
                builder->CreateStore(converted_value->getValue(), builder->CreateConstInBoundsGEP2_32(exc_info, 0, 1));
                converted_value->decvref(emitter);
                ConcreteCompilerVariable* converted_traceback = traceback->makeConverted(emitter, UNKNOWN);
                builder->CreateStore(converted_traceback->getValue(),
                                     builder->CreateConstInBoundsGEP2_32(exc_info, 0, 2));
                converted_traceback->decvref(emitter);

                return getNone();
            }
            case AST_LangPrimitive::UNCACHE_EXC_INFO: {
                assert(node->args.empty());

                auto* builder = emitter.getBuilder();

                llvm::Value* frame_info = irstate->getFrameInfoVar();
                llvm::Value* exc_info = builder->CreateConstInBoundsGEP2_32(frame_info, 0, 0);
                assert(exc_info->getType() == g.llvm_excinfo_type->getPointerTo());

                llvm::Constant* v = embedConstantPtr(0, g.llvm_value_type_ptr);
                builder->CreateStore(v, builder->CreateConstInBoundsGEP2_32(exc_info, 0, 0));
                builder->CreateStore(v, builder->CreateConstInBoundsGEP2_32(exc_info, 0, 1));
                builder->CreateStore(v, builder->CreateConstInBoundsGEP2_32(exc_info, 0, 2));

                return getNone();
            }
            default:
                RELEASE_ASSERT(0, "%d", node->opcode);
        }
    }

    CompilerVariable* _evalBinExp(AST* node, CompilerVariable* left, CompilerVariable* right, AST_TYPE::AST_TYPE type,
                                  BinExpType exp_type, UnwindInfo unw_info) {
        assert(left);
        assert(right);

        if (type == AST_TYPE::Div && (irstate->getSourceInfo()->parent_module->future_flags & FF_DIVISION)) {
            type = AST_TYPE::TrueDiv;
        }

        return left->binexp(emitter, getOpInfoForNode(node, unw_info), right, type, exp_type);
    }

    CompilerVariable* evalBinOp(AST_BinOp* node, UnwindInfo unw_info) {
        CompilerVariable* left = evalExpr(node->left, unw_info);
        CompilerVariable* right = evalExpr(node->right, unw_info);

        assert(node->op_type != AST_TYPE::Is && node->op_type != AST_TYPE::IsNot && "not tested yet");

        CompilerVariable* rtn = this->_evalBinExp(node, left, right, node->op_type, BinOp, unw_info);
        left->decvref(emitter);
        right->decvref(emitter);
        return rtn;
    }

    CompilerVariable* evalAugBinOp(AST_AugBinOp* node, UnwindInfo unw_info) {
        CompilerVariable* left = evalExpr(node->left, unw_info);
        CompilerVariable* right = evalExpr(node->right, unw_info);

        assert(node->op_type != AST_TYPE::Is && node->op_type != AST_TYPE::IsNot && "not tested yet");

        CompilerVariable* rtn = this->_evalBinExp(node, left, right, node->op_type, AugBinOp, unw_info);
        left->decvref(emitter);
        right->decvref(emitter);
        return rtn;
    }

    CompilerVariable* evalCompare(AST_Compare* node, UnwindInfo unw_info) {
        RELEASE_ASSERT(node->ops.size() == 1, "");

        CompilerVariable* left = evalExpr(node->left, unw_info);
        CompilerVariable* right = evalExpr(node->comparators[0], unw_info);

        assert(left);
        assert(right);

        CompilerVariable* rtn = _evalBinExp(node, left, right, node->ops[0], Compare, unw_info);
        left->decvref(emitter);
        right->decvref(emitter);
        return rtn;
    }

    CompilerVariable* evalCall(AST_Call* node, UnwindInfo unw_info) {
        bool is_callattr;
        bool callattr_clsonly = false;
        const std::string* attr = NULL;
        CompilerVariable* func;
        if (node->func->type == AST_TYPE::Attribute) {
            is_callattr = true;
            callattr_clsonly = false;
            AST_Attribute* attr_ast = ast_cast<AST_Attribute>(node->func);
            func = evalExpr(attr_ast->value, unw_info);
            attr = &attr_ast->attr.str();
        } else if (node->func->type == AST_TYPE::ClsAttribute) {
            is_callattr = true;
            callattr_clsonly = true;
            AST_ClsAttribute* attr_ast = ast_cast<AST_ClsAttribute>(node->func);
            func = evalExpr(attr_ast->value, unw_info);
            attr = &attr_ast->attr.str();
        } else {
            is_callattr = false;
            func = evalExpr(node->func, unw_info);
        }

        std::vector<CompilerVariable*> args;
        std::vector<const std::string*>* keyword_names;
        if (node->keywords.size()) {
            keyword_names = getKeywordNameStorage(node);

            // Only add the keywords to the array the first time, since
            // the later times we will hit the cache which will have the
            // keyword names already populated:
            if (!keyword_names->size()) {
                for (auto kw : node->keywords) {
                    keyword_names->push_back(&kw->arg.str());
                }
            }
        } else {
            keyword_names = NULL;
        }

        for (int i = 0; i < node->args.size(); i++) {
            CompilerVariable* a = evalExpr(node->args[i], unw_info);
            args.push_back(a);
        }

        for (int i = 0; i < node->keywords.size(); i++) {
            CompilerVariable* a = evalExpr(node->keywords[i]->value, unw_info);
            args.push_back(a);
        }

        if (node->starargs)
            args.push_back(evalExpr(node->starargs, unw_info));
        if (node->kwargs)
            args.push_back(evalExpr(node->kwargs, unw_info));

        struct ArgPassSpec argspec(node->args.size(), node->keywords.size(), node->starargs != NULL,
                                   node->kwargs != NULL);


        // if (VERBOSITY("irgen") >= 1)
        //_addAnnotation("before_call");

        CompilerVariable* rtn;
        if (is_callattr) {
            CallattrFlags flags = {.cls_only = callattr_clsonly, .null_on_nonexistent = false };
            rtn = func->callattr(emitter, getOpInfoForNode(node, unw_info), attr, flags, argspec, args, keyword_names);
        } else {
            rtn = func->call(emitter, getOpInfoForNode(node, unw_info), argspec, args, keyword_names);
        }

        func->decvref(emitter);
        for (int i = 0; i < args.size(); i++) {
            args[i]->decvref(emitter);
        }

        return rtn;
    }

    CompilerVariable* evalDict(AST_Dict* node, UnwindInfo unw_info) {
        llvm::Value* v = emitter.getBuilder()->CreateCall(g.funcs.createDict);
        ConcreteCompilerVariable* rtn = new ConcreteCompilerVariable(DICT, v, true);
        if (node->keys.size()) {
            static const std::string setitem_str("__setitem__");
            CompilerVariable* setitem = rtn->getattr(emitter, getEmptyOpInfo(unw_info), &setitem_str, true);
            for (int i = 0; i < node->keys.size(); i++) {
                CompilerVariable* key = evalExpr(node->keys[i], unw_info);
                CompilerVariable* value = evalExpr(node->values[i], unw_info);
                assert(key);
                assert(value);

                std::vector<CompilerVariable*> args;
                args.push_back(key);
                args.push_back(value);
                // TODO should use callattr
                CompilerVariable* rtn = setitem->call(emitter, getEmptyOpInfo(unw_info), ArgPassSpec(2), args, NULL);
                rtn->decvref(emitter);

                key->decvref(emitter);
                value->decvref(emitter);
            }
            setitem->decvref(emitter);
        }
        return rtn;
    }

    void _addAnnotation(const char* message) {
        llvm::Instruction* inst = emitter.getBuilder()->CreateCall(
            llvm::Intrinsic::getDeclaration(g.cur_module, llvm::Intrinsic::donothing));
        llvm::Metadata* md_vals[] = { llvm::ConstantAsMetadata::get(getConstantInt(0)) };
        llvm::MDNode* mdnode = llvm::MDNode::get(g.context, md_vals);
        inst->setMetadata(message, mdnode);
    }

    CompilerVariable* evalIndex(AST_Index* node, UnwindInfo unw_info) { return evalExpr(node->value, unw_info); }

    CompilerVariable* evalLambda(AST_Lambda* node, UnwindInfo unw_info) {
        AST_Return* expr = new AST_Return();
        expr->value = node->body;

        std::vector<AST_stmt*> body = { expr };
        CompilerVariable* func = _createFunction(node, unw_info, node->args, body);
        ConcreteCompilerVariable* converted = func->makeConverted(emitter, func->getBoxType());
        func->decvref(emitter);

        return converted;
    }


    CompilerVariable* evalList(AST_List* node, UnwindInfo unw_info) {
        std::vector<CompilerVariable*> elts;
        for (int i = 0; i < node->elts.size(); i++) {
            CompilerVariable* value = evalExpr(node->elts[i], unw_info);
            elts.push_back(value);
        }

        llvm::Value* v = emitter.getBuilder()->CreateCall(g.funcs.createList);
        ConcreteCompilerVariable* rtn = new ConcreteCompilerVariable(LIST, v, true);

        llvm::Value* f = g.funcs.listAppendInternal;
        llvm::Value* bitcast = emitter.getBuilder()->CreateBitCast(
            v, *llvm::cast<llvm::FunctionType>(llvm::cast<llvm::PointerType>(f->getType())->getElementType())
                    ->param_begin());

        for (int i = 0; i < node->elts.size(); i++) {
            CompilerVariable* elt = elts[i];
            ConcreteCompilerVariable* converted = elt->makeConverted(emitter, elt->getBoxType());
            elt->decvref(emitter);

            emitter.createCall2(unw_info, f, bitcast, converted->getValue());
            converted->decvref(emitter);
        }
        return rtn;
    }

    ConcreteCompilerVariable* getNone() {
        ConcreteCompilerVariable* v = new ConcreteCompilerVariable(
            typeFromClass(none_cls), embedConstantPtr(None, g.llvm_value_type_ptr), false);
        return v;
    }

    ConcreteCompilerVariable* _getGlobal(AST_Name* node, UnwindInfo unw_info) {
        if (node->id.str() == "None")
            return getNone();

        bool do_patchpoint = ENABLE_ICGETGLOBALS && (irstate->getEffortLevel() != EffortLevel::INTERPRETED);
        if (do_patchpoint) {
            ICSetupInfo* pp = createGetGlobalIC(getOpInfoForNode(node, unw_info).getTypeRecorder());

            std::vector<llvm::Value*> llvm_args;
            llvm_args.push_back(embedConstantPtr(irstate->getSourceInfo()->parent_module, g.llvm_module_type_ptr));
            llvm_args.push_back(embedConstantPtr(&node->id.str(), g.llvm_str_type_ptr));

            llvm::Value* uncasted = emitter.createIC(pp, (void*)pyston::getGlobal, llvm_args, unw_info);
            llvm::Value* r = emitter.getBuilder()->CreateIntToPtr(uncasted, g.llvm_value_type_ptr);
            return new ConcreteCompilerVariable(UNKNOWN, r, true);
        } else {
            llvm::Value* r
                = emitter.createCall2(unw_info, g.funcs.getGlobal,
                                      embedConstantPtr(irstate->getSourceInfo()->parent_module, g.llvm_module_type_ptr),
                                      embedConstantPtr(&node->id.str(), g.llvm_str_type_ptr));
            return new ConcreteCompilerVariable(UNKNOWN, r, true);
        }
    }

    CompilerVariable* evalName(AST_Name* node, UnwindInfo unw_info) {
        auto scope_info = irstate->getScopeInfo();

        bool is_kill = irstate->getSourceInfo()->liveness->isKill(node, myblock);
        assert(!is_kill || node->id.str()[0] == '#');

        ScopeInfo::VarScopeType vst = scope_info->getScopeTypeOfName(node->id);
        if (vst == ScopeInfo::VarScopeType::GLOBAL) {
            assert(!is_kill);
            return _getGlobal(node, unw_info);
        } else if (vst == ScopeInfo::VarScopeType::DEREF) {
            assert(!is_kill);
            assert(scope_info->takesClosure());

            CompilerVariable* closure = symbol_table[internString(PASSED_CLOSURE_NAME)];
            assert(closure);

            return closure->getattr(emitter, getEmptyOpInfo(unw_info), &node->id.str(), false);
        } else {
            if (symbol_table.find(node->id) == symbol_table.end()) {
                // classdefs have different scoping rules than functions:
                if (vst == ScopeInfo::VarScopeType::NAME) {
                    return _getGlobal(node, unw_info);
                }

                // TODO should mark as DEAD here, though we won't end up setting all the names appropriately
                // state = DEAD;
                llvm::CallSite call = emitter.createCall(
                    unw_info, g.funcs.assertNameDefined,
                    { getConstantInt(0, g.i1), getStringConstantPtr(node->id.str() + '\0'),
                      embedConstantPtr(UnboundLocalError, g.llvm_class_type_ptr), getConstantInt(true, g.i1) });
                call.setDoesNotReturn();
                return undefVariable();
            }

            InternedString defined_name = getIsDefinedName(node->id);
            ConcreteCompilerVariable* is_defined_var
                = static_cast<ConcreteCompilerVariable*>(_getFake(defined_name, true));

            if (is_defined_var) {
                if (vst == ScopeInfo::VarScopeType::NAME) {
                    llvm::Value* v = handlePotentiallyUndefined(
                        is_defined_var, g.llvm_value_type_ptr, curblock, emitter, false,
                        [=](IREmitter& emitter) {
                            CompilerVariable* local = symbol_table[node->id];
                            return local->makeConverted(emitter, local->getBoxType())->getValue();
                        },
                        [=](IREmitter& emitter) { return _getGlobal(node, unw_info)->getValue(); });

                    return new ConcreteCompilerVariable(UNKNOWN, v, true);
                }

                emitter.createCall(unw_info, g.funcs.assertNameDefined,
                                   { i1FromBool(emitter, is_defined_var), getStringConstantPtr(node->id.str() + '\0'),
                                     embedConstantPtr(UnboundLocalError, g.llvm_class_type_ptr),
                                     getConstantInt(true, g.i1) });

                // At this point we know the name must be defined (otherwise the assert would have fired):
                _popFake(defined_name);
            }

            CompilerVariable* rtn = symbol_table[node->id];
            if (is_kill)
                symbol_table.erase(node->id);
            else
                rtn->incvref();
            return rtn;
        }
    }

    CompilerVariable* evalNum(AST_Num* node, UnwindInfo unw_info) {
        if (node->num_type == AST_Num::INT)
            return makeInt(node->n_int);
        else if (node->num_type == AST_Num::FLOAT)
            return makeFloat(node->n_float);
        else if (node->num_type == AST_Num::COMPLEX)
            return makePureImaginary(emitter, node->n_float);
        else
            return makeLong(emitter, node->n_long);
    }

    CompilerVariable* evalRepr(AST_Repr* node, UnwindInfo unw_info) {
        CompilerVariable* var = evalExpr(node->value, unw_info);
        ConcreteCompilerVariable* cvar = var->makeConverted(emitter, var->getBoxType());
        var->decvref(emitter);

        std::vector<llvm::Value*> args{ cvar->getValue() };
        llvm::Value* rtn = emitter.createCall(unw_info, g.funcs.repr, args);
        cvar->decvref(emitter);
        rtn = emitter.getBuilder()->CreateBitCast(rtn, g.llvm_value_type_ptr);

        return new ConcreteCompilerVariable(STR, rtn, true);
    }

    CompilerVariable* evalSet(AST_Set* node, UnwindInfo unw_info) {
        std::vector<CompilerVariable*> elts;
        for (int i = 0; i < node->elts.size(); i++) {
            CompilerVariable* value = evalExpr(node->elts[i], unw_info);
            elts.push_back(value);
        }

        llvm::Value* v = emitter.getBuilder()->CreateCall(g.funcs.createSet);
        ConcreteCompilerVariable* rtn = new ConcreteCompilerVariable(SET, v, true);

        static std::string add_str("add");

        for (int i = 0; i < node->elts.size(); i++) {
            CompilerVariable* elt = elts[i];
            CallattrFlags flags = {.cls_only = true, .null_on_nonexistent = false };
            CompilerVariable* r = rtn->callattr(emitter, getOpInfoForNode(node, unw_info), &add_str, flags,
                                                ArgPassSpec(1), { elt }, NULL);
            r->decvref(emitter);
            elt->decvref(emitter);
        }

        return rtn;
    }

    CompilerVariable* evalSlice(AST_Slice* node, UnwindInfo unw_info) {
        CompilerVariable* start, *stop, *step;
        start = node->lower ? evalExpr(node->lower, unw_info) : getNone();
        stop = node->upper ? evalExpr(node->upper, unw_info) : getNone();
        step = node->step ? evalExpr(node->step, unw_info) : getNone();

        ConcreteCompilerVariable* cstart, *cstop, *cstep;
        cstart = start->makeConverted(emitter, start->getBoxType());
        cstop = stop->makeConverted(emitter, stop->getBoxType());
        cstep = step->makeConverted(emitter, step->getBoxType());
        start->decvref(emitter);
        stop->decvref(emitter);
        step->decvref(emitter);

        std::vector<llvm::Value*> args;
        args.push_back(cstart->getValue());
        args.push_back(cstop->getValue());
        args.push_back(cstep->getValue());
        llvm::Value* rtn = emitter.getBuilder()->CreateCall(g.funcs.createSlice, args);

        cstart->decvref(emitter);
        cstop->decvref(emitter);
        cstep->decvref(emitter);
        return new ConcreteCompilerVariable(SLICE, rtn, true);
    }

    CompilerVariable* evalStr(AST_Str* node, UnwindInfo unw_info) {
        if (node->str_type == AST_Str::STR) {
            return makeStr(&node->str_data);
        } else if (node->str_type == AST_Str::UNICODE) {
            return makeUnicode(emitter, &node->str_data);
        } else {
            RELEASE_ASSERT(0, "%d", node->str_type);
        }
    }

    CompilerVariable* evalSubscript(AST_Subscript* node, UnwindInfo unw_info) {
        CompilerVariable* value = evalExpr(node->value, unw_info);
        CompilerVariable* slice = evalExpr(node->slice, unw_info);

        CompilerVariable* rtn = value->getitem(emitter, getOpInfoForNode(node, unw_info), slice);
        value->decvref(emitter);
        slice->decvref(emitter);
        return rtn;
    }

    CompilerVariable* evalTuple(AST_Tuple* node, UnwindInfo unw_info) {
        std::vector<CompilerVariable*> elts;
        for (int i = 0; i < node->elts.size(); i++) {
            CompilerVariable* value = evalExpr(node->elts[i], unw_info);
            elts.push_back(value);
        }

        // TODO makeTuple should probably just transfer the vref, but I want to keep things consistent
        CompilerVariable* rtn = makeTuple(elts);
        for (int i = 0; i < node->elts.size(); i++) {
            elts[i]->decvref(emitter);
        }
        return rtn;
    }

    CompilerVariable* evalUnaryOp(AST_UnaryOp* node, UnwindInfo unw_info) {
        CompilerVariable* operand = evalExpr(node->operand, unw_info);

        if (node->op_type == AST_TYPE::Not) {
            ConcreteCompilerVariable* rtn = operand->nonzero(emitter, getOpInfoForNode(node, unw_info));
            operand->decvref(emitter);

            assert(rtn->getType() == BOOL);
            llvm::Value* v = i1FromBool(emitter, rtn);
            assert(v->getType() == g.i1);

            llvm::Value* negated = emitter.getBuilder()->CreateNot(v);
            rtn->decvref(emitter);
            return boolFromI1(emitter, negated);
        } else {
            // TODO These are pretty inefficient, but luckily I don't think they're used that often:
            ConcreteCompilerVariable* converted = operand->makeConverted(emitter, operand->getBoxType());
            operand->decvref(emitter);

            llvm::Value* rtn = emitter.createCall2(unw_info, g.funcs.unaryop, converted->getValue(),
                                                   getConstantInt(node->op_type, g.i32));
            converted->decvref(emitter);

            return new ConcreteCompilerVariable(UNKNOWN, rtn, true);
        }
    }

    CompilerVariable* evalYield(AST_Yield* node, UnwindInfo unw_info) {
        CompilerVariable* generator = symbol_table[internString(PASSED_GENERATOR_NAME)];
        assert(generator);
        ConcreteCompilerVariable* convertedGenerator = generator->makeConverted(emitter, generator->getBoxType());


        CompilerVariable* value = node->value ? evalExpr(node->value, unw_info) : getNone();
        ConcreteCompilerVariable* convertedValue = value->makeConverted(emitter, value->getBoxType());
        value->decvref(emitter);

        llvm::Value* rtn
            = emitter.createCall2(unw_info, g.funcs.yield, convertedGenerator->getValue(), convertedValue->getValue());
        convertedGenerator->decvref(emitter);
        convertedValue->decvref(emitter);

        return new ConcreteCompilerVariable(UNKNOWN, rtn, true);
    }

    ConcreteCompilerVariable* unboxVar(ConcreteCompilerType* t, llvm::Value* v, bool grabbed) {
        if (t == BOXED_INT) {
            llvm::Value* unboxed = emitter.getBuilder()->CreateCall(g.funcs.unboxInt, v);
            ConcreteCompilerVariable* rtn = new ConcreteCompilerVariable(INT, unboxed, true);
            return rtn;
        }
        if (t == BOXED_FLOAT) {
            llvm::Value* unboxed = emitter.getBuilder()->CreateCall(g.funcs.unboxFloat, v);
            ConcreteCompilerVariable* rtn = new ConcreteCompilerVariable(FLOAT, unboxed, true);
            return rtn;
        }
        if (t == BOXED_BOOL) {
            llvm::Value* unboxed = emitter.getBuilder()->CreateCall(g.funcs.unboxBool, v);
            return boolFromI1(emitter, unboxed);
        }
        return new ConcreteCompilerVariable(t, v, grabbed);
    }

    CompilerVariable* evalExpr(AST_expr* node, UnwindInfo unw_info) {
        // printf("%d expr: %d\n", node->type, node->lineno);
        if (node->lineno) {
            emitter.getBuilder()->SetCurrentDebugLocation(
                llvm::DebugLoc::get(node->lineno, 0, irstate->getFuncDbgInfo()));
        }

        CompilerVariable* rtn = NULL;
        switch (node->type) {
            case AST_TYPE::Attribute:
                rtn = evalAttribute(ast_cast<AST_Attribute>(node), unw_info);
                break;
            case AST_TYPE::AugBinOp:
                rtn = evalAugBinOp(ast_cast<AST_AugBinOp>(node), unw_info);
                break;
            case AST_TYPE::BinOp:
                rtn = evalBinOp(ast_cast<AST_BinOp>(node), unw_info);
                break;
            case AST_TYPE::Call:
                rtn = evalCall(ast_cast<AST_Call>(node), unw_info);
                break;
            case AST_TYPE::Compare:
                rtn = evalCompare(ast_cast<AST_Compare>(node), unw_info);
                break;
            case AST_TYPE::Dict:
                rtn = evalDict(ast_cast<AST_Dict>(node), unw_info);
                break;
            case AST_TYPE::Index:
                rtn = evalIndex(ast_cast<AST_Index>(node), unw_info);
                break;
            case AST_TYPE::Lambda:
                rtn = evalLambda(ast_cast<AST_Lambda>(node), unw_info);
                break;
            case AST_TYPE::List:
                rtn = evalList(ast_cast<AST_List>(node), unw_info);
                break;
            case AST_TYPE::Name:
                rtn = evalName(ast_cast<AST_Name>(node), unw_info);
                break;
            case AST_TYPE::Num:
                rtn = evalNum(ast_cast<AST_Num>(node), unw_info);
                break;
            case AST_TYPE::Repr:
                rtn = evalRepr(ast_cast<AST_Repr>(node), unw_info);
                break;
            case AST_TYPE::Set:
                rtn = evalSet(ast_cast<AST_Set>(node), unw_info);
                break;
            case AST_TYPE::Slice:
                rtn = evalSlice(ast_cast<AST_Slice>(node), unw_info);
                break;
            case AST_TYPE::Str:
                rtn = evalStr(ast_cast<AST_Str>(node), unw_info);
                break;
            case AST_TYPE::Subscript:
                rtn = evalSubscript(ast_cast<AST_Subscript>(node), unw_info);
                break;
            case AST_TYPE::Tuple:
                rtn = evalTuple(ast_cast<AST_Tuple>(node), unw_info);
                break;
            case AST_TYPE::UnaryOp:
                rtn = evalUnaryOp(ast_cast<AST_UnaryOp>(node), unw_info);
                break;
            case AST_TYPE::Yield:
                rtn = evalYield(ast_cast<AST_Yield>(node), unw_info);
                break;

            case AST_TYPE::ClsAttribute:
                rtn = evalClsAttribute(ast_cast<AST_ClsAttribute>(node), unw_info);
                break;
            case AST_TYPE::LangPrimitive:
                rtn = evalLangPrimitive(ast_cast<AST_LangPrimitive>(node), unw_info);
                break;
            default:
                printf("Unhandled expr type: %d (irgenerator.cpp:" STRINGIFY(__LINE__) ")\n", node->type);
                exit(1);
        }

        assert(rtn);

        // Out-guarding:
        BoxedClass* speculated_class = types->speculatedExprClass(node);
        if (speculated_class != NULL) {
            assert(rtn);

            ConcreteCompilerType* speculated_type = typeFromClass(speculated_class);
            if (VERBOSITY("irgen") >= 1) {
                printf("Speculating that %s is actually %s, at ", rtn->getConcreteType()->debugName().c_str(),
                       speculated_type->debugName().c_str());
                PrintVisitor printer;
                node->accept(&printer);
                printf("\n");
            }

            // That's not really a speculation.... could potentially handle this here, but
            // I think it's better to just not generate bad speculations:
            assert(!rtn->canConvertTo(speculated_type));

            ConcreteCompilerVariable* old_rtn = rtn->makeConverted(emitter, UNKNOWN);
            rtn->decvref(emitter);

            llvm::Value* guard_check = old_rtn->makeClassCheck(emitter, speculated_class);
            assert(guard_check->getType() == g.i1);
            createExprTypeGuard(guard_check, node, old_rtn->getValue(), unw_info.current_stmt);

            rtn = unboxVar(speculated_type, old_rtn->getValue(), true);
        }

        assert(rtn);

        return rtn;
    }

    void _setFake(InternedString name, CompilerVariable* val) {
        assert(name.str()[0] == '!');
        CompilerVariable*& cur = symbol_table[name];
        assert(cur == NULL);
        cur = val;
    }

    CompilerVariable* _getFake(InternedString name, bool allow_missing = false) {
        assert(name.str()[0] == '!');
        auto it = symbol_table.find(name);
        if (it == symbol_table.end()) {
            assert(allow_missing);
            return NULL;
        }
        return it->second;
    }

    CompilerVariable* _popFake(InternedString name, bool allow_missing = false) {
        CompilerVariable* rtn = _getFake(name, allow_missing);
        symbol_table.erase(name);
        if (!allow_missing)
            assert(rtn != NULL);
        return rtn;
    }

    void _doSet(InternedString name, CompilerVariable* val, UnwindInfo unw_info) {
        assert(name.str() != "None");

        auto scope_info = irstate->getScopeInfo();
        assert(!scope_info->refersToClosure(name));

        if (scope_info->refersToGlobal(name)) {
            assert(!scope_info->saveInClosure(name));

            // TODO do something special here so that it knows to only emit a monomorphic inline cache?
            ConcreteCompilerVariable* module = new ConcreteCompilerVariable(
                MODULE, embedConstantPtr(irstate->getSourceInfo()->parent_module, g.llvm_value_type_ptr), false);
            module->setattr(emitter, getEmptyOpInfo(unw_info), &name.str(), val);
            module->decvref(emitter);
        } else {
            CompilerVariable*& prev = symbol_table[name];
            if (prev != NULL) {
                prev->decvref(emitter);
            }
            prev = val;
            val->incvref();

            // Clear out the is_defined name since it is now definitely defined:
            assert(!isIsDefinedName(name.str()));
            InternedString defined_name = getIsDefinedName(name);
            _popFake(defined_name, true);

            if (scope_info->saveInClosure(name)) {
                CompilerVariable* closure = symbol_table[internString(CREATED_CLOSURE_NAME)];
                assert(closure);

                closure->setattr(emitter, getEmptyOpInfo(unw_info), &name.str(), val);
            }
        }
    }

    void _doSetattr(AST_Attribute* target, CompilerVariable* val, UnwindInfo unw_info) {
        CompilerVariable* t = evalExpr(target->value, unw_info);
        t->setattr(emitter, getEmptyOpInfo(unw_info), &target->attr.str(), val);
        t->decvref(emitter);
    }

    void _doSetitem(AST_Subscript* target, CompilerVariable* val, UnwindInfo unw_info) {
        CompilerVariable* tget = evalExpr(target->value, unw_info);
        CompilerVariable* slice = evalExpr(target->slice, unw_info);

        ConcreteCompilerVariable* converted_target = tget->makeConverted(emitter, tget->getBoxType());
        ConcreteCompilerVariable* converted_slice = slice->makeConverted(emitter, slice->getBoxType());
        tget->decvref(emitter);
        slice->decvref(emitter);

        ConcreteCompilerVariable* converted_val = val->makeConverted(emitter, val->getBoxType());

        // TODO add a CompilerVariable::setattr, which can (similar to getitem)
        // statically-resolve the function if possible, and only fall back to
        // patchpoints if it couldn't.
        bool do_patchpoint = ENABLE_ICSETITEMS && (irstate->getEffortLevel() != EffortLevel::INTERPRETED);
        if (do_patchpoint) {
            ICSetupInfo* pp = createSetitemIC(getEmptyOpInfo(unw_info).getTypeRecorder());

            std::vector<llvm::Value*> llvm_args;
            llvm_args.push_back(converted_target->getValue());
            llvm_args.push_back(converted_slice->getValue());
            llvm_args.push_back(converted_val->getValue());

            emitter.createIC(pp, (void*)pyston::setitem, llvm_args, unw_info);
        } else {
            emitter.createCall3(unw_info, g.funcs.setitem, converted_target->getValue(), converted_slice->getValue(),
                                converted_val->getValue());
        }

        converted_target->decvref(emitter);
        converted_slice->decvref(emitter);
        converted_val->decvref(emitter);
    }

    void _doUnpackTuple(AST_Tuple* target, CompilerVariable* val, UnwindInfo unw_info) {
        int ntargets = target->elts.size();

        std::vector<CompilerVariable*> unpacked = val->unpack(emitter, getOpInfoForNode(target, unw_info), ntargets);

#ifndef NDEBUG
        for (auto e : target->elts) {
            ASSERT(e->type == AST_TYPE::Name && ast_cast<AST_Name>(e)->id.str()[0] == '#',
                   "should only be unpacking tuples into cfg-generated names!");
        }
#endif

        for (int i = 0; i < ntargets; i++) {
            CompilerVariable* thisval = unpacked[i];
            _doSet(target->elts[i], thisval, unw_info);
            thisval->decvref(emitter);
        }
    }

    void _doSet(AST* target, CompilerVariable* val, UnwindInfo unw_info) {
        switch (target->type) {
            case AST_TYPE::Attribute:
                _doSetattr(ast_cast<AST_Attribute>(target), val, unw_info);
                break;
            case AST_TYPE::Name:
                _doSet(ast_cast<AST_Name>(target)->id, val, unw_info);
                break;
            case AST_TYPE::Subscript:
                _doSetitem(ast_cast<AST_Subscript>(target), val, unw_info);
                break;
            case AST_TYPE::Tuple:
                _doUnpackTuple(ast_cast<AST_Tuple>(target), val, unw_info);
                break;
            default:
                ASSERT(0, "Unknown type for IRGenerator: %d", target->type);
                abort();
        }
    }

    void doAssert(AST_Assert* node, UnwindInfo unw_info) {
        AST_expr* test = node->test;
        assert(test->type == AST_TYPE::Num);
        AST_Num* num = ast_cast<AST_Num>(test);
        assert(num->num_type == AST_Num::INT);
        assert(num->n_int == 0);

        std::vector<llvm::Value*> llvm_args;
        llvm_args.push_back(embedConstantPtr(irstate->getSourceInfo()->parent_module, g.llvm_module_type_ptr));

        ConcreteCompilerVariable* converted_msg = NULL;
        if (node->msg) {
            CompilerVariable* msg = evalExpr(node->msg, unw_info);
            converted_msg = msg->makeConverted(emitter, msg->getBoxType());
            msg->decvref(emitter);
            llvm_args.push_back(converted_msg->getValue());
        } else {
            llvm_args.push_back(embedConstantPtr(NULL, g.llvm_value_type_ptr));
        }
        llvm::CallSite call = emitter.createCall(unw_info, g.funcs.assertFail, llvm_args);
        call.setDoesNotReturn();
    }

    void doAssign(AST_Assign* node, UnwindInfo unw_info) {
        CompilerVariable* val = evalExpr(node->value, unw_info);

        for (int i = 0; i < node->targets.size(); i++) {
            _doSet(node->targets[i], val, unw_info);
        }
        val->decvref(emitter);
    }

    void doClassDef(AST_ClassDef* node, UnwindInfo unw_info) {
        assert(node->type == AST_TYPE::ClassDef);
        ScopeInfo* scope_info = irstate->getScopeInfoForNode(node);
        assert(scope_info);

        std::vector<CompilerVariable*> bases;
        for (auto b : node->bases) {
            CompilerVariable* base = evalExpr(b, unw_info);
            bases.push_back(base);
        }

        CompilerVariable* _bases_tuple = makeTuple(bases);
        for (auto b : bases) {
            b->decvref(emitter);
        }

        ConcreteCompilerVariable* bases_tuple = _bases_tuple->makeConverted(emitter, _bases_tuple->getBoxType());
        _bases_tuple->decvref(emitter);

        std::vector<CompilerVariable*> decorators;
        for (auto d : node->decorator_list) {
            decorators.push_back(evalExpr(d, unw_info));
        }

        CLFunction* cl = wrapFunction(node, nullptr, node->body, irstate->getSourceInfo());

        // TODO duplication with _createFunction:
        CompilerVariable* created_closure = NULL;
        if (scope_info->takesClosure()) {
            created_closure = symbol_table[internString(CREATED_CLOSURE_NAME)];
            assert(created_closure);
        }

        // TODO kind of silly to create the function just to usually-delete it afterwards;
        // one reason to do this is to pass the closure through if necessary,
        // but since the classdef can't create its own closure, shouldn't need to explicitly
        // create that scope to pass the closure through.
        CompilerVariable* func = makeFunction(emitter, cl, created_closure, false, {});

        CompilerVariable* attr_dict = func->call(emitter, getEmptyOpInfo(unw_info), ArgPassSpec(0), {}, NULL);

        func->decvref(emitter);

        ConcreteCompilerVariable* converted_attr_dict = attr_dict->makeConverted(emitter, attr_dict->getBoxType());
        attr_dict->decvref(emitter);


        llvm::Value* classobj = emitter.createCall3(unw_info, g.funcs.createUserClass,
                                                    embedConstantPtr(&node->name.str(), g.llvm_str_type_ptr),
                                                    bases_tuple->getValue(), converted_attr_dict->getValue());

        // Note: createuserClass is free to manufacture non-class objects
        CompilerVariable* cls = new ConcreteCompilerVariable(UNKNOWN, classobj, true);

        for (int i = decorators.size() - 1; i >= 0; i--) {
            cls = decorators[i]->call(emitter, getOpInfoForNode(node, unw_info), ArgPassSpec(1), { cls }, NULL);
            decorators[i]->decvref(emitter);
        }

        _doSet(irstate->getSourceInfo()->mangleName(node->name), cls, unw_info);
        cls->decvref(emitter);
    }

    void doDelete(AST_Delete* node, UnwindInfo unw_info) {
        for (AST_expr* target : node->targets) {
            switch (target->type) {
                case AST_TYPE::Subscript:
                    _doDelitem(static_cast<AST_Subscript*>(target), unw_info);
                    break;
                case AST_TYPE::Attribute:
                    _doDelAttr(static_cast<AST_Attribute*>(target), unw_info);
                    break;
                case AST_TYPE::Name:
                    _doDelName(static_cast<AST_Name*>(target), unw_info);
                    break;
                default:
                    ASSERT(0, "Unsupported del target: %d", target->type);
                    abort();
            }
        }
    }

    // invoke delitem in objmodel.cpp, which will invoke the listDelitem of list
    void _doDelitem(AST_Subscript* target, UnwindInfo unw_info) {
        CompilerVariable* tget = evalExpr(target->value, unw_info);
        CompilerVariable* slice = evalExpr(target->slice, unw_info);

        ConcreteCompilerVariable* converted_target = tget->makeConverted(emitter, tget->getBoxType());
        ConcreteCompilerVariable* converted_slice = slice->makeConverted(emitter, slice->getBoxType());
        tget->decvref(emitter);
        slice->decvref(emitter);

        bool do_patchpoint = ENABLE_ICDELITEMS && (irstate->getEffortLevel() != EffortLevel::INTERPRETED);
        if (do_patchpoint) {
            ICSetupInfo* pp = createDelitemIC(getEmptyOpInfo(unw_info).getTypeRecorder());

            std::vector<llvm::Value*> llvm_args;
            llvm_args.push_back(converted_target->getValue());
            llvm_args.push_back(converted_slice->getValue());

            emitter.createIC(pp, (void*)pyston::delitem, llvm_args, unw_info);
        } else {
            emitter.createCall2(unw_info, g.funcs.delitem, converted_target->getValue(), converted_slice->getValue());
        }

        converted_target->decvref(emitter);
        converted_slice->decvref(emitter);
    }

    void _doDelAttr(AST_Attribute* node, UnwindInfo unw_info) {
        CompilerVariable* value = evalExpr(node->value, unw_info);
        value->delattr(emitter, getEmptyOpInfo(unw_info), &node->attr.str());
    }

    void _doDelName(AST_Name* target, UnwindInfo unw_info) {
        auto scope_info = irstate->getScopeInfo();
        if (scope_info->refersToGlobal(target->id)) {
            // Can't use delattr since the errors are different:
            emitter.createCall2(unw_info, g.funcs.delGlobal,
                                embedConstantPtr(irstate->getSourceInfo()->parent_module, g.llvm_module_type_ptr),
                                embedConstantPtr(&target->id.str(), g.llvm_str_type_ptr));
            return;
        }

        assert(!scope_info->refersToClosure(target->id));
        assert(!scope_info->saveInClosure(
            target->id)); // SyntaxError: can not delete variable 'x' referenced in nested scope

        // A del of a missing name generates different error messages in a function scope vs a classdef scope
        bool local_error_msg = (irstate->getSourceInfo()->ast->type != AST_TYPE::ClassDef);

        if (symbol_table.count(target->id) == 0) {
            llvm::CallSite call = emitter.createCall(
                unw_info, g.funcs.assertNameDefined,
                { getConstantInt(0, g.i1), getStringConstantPtr(target->id.str() + '\0'),
                  embedConstantPtr(NameError, g.llvm_class_type_ptr), getConstantInt(local_error_msg, g.i1) });
            call.setDoesNotReturn();
            return;
        }

        InternedString defined_name = getIsDefinedName(target->id);
        ConcreteCompilerVariable* is_defined_var = static_cast<ConcreteCompilerVariable*>(_getFake(defined_name, true));

        if (is_defined_var) {
            emitter.createCall(unw_info, g.funcs.assertNameDefined,
                               { i1FromBool(emitter, is_defined_var), getStringConstantPtr(target->id.str() + '\0'),
                                 embedConstantPtr(NameError, g.llvm_class_type_ptr),
                                 getConstantInt(local_error_msg, g.i1) });
            _popFake(defined_name);
        }

        symbol_table.erase(target->id);
    }

    CompilerVariable* _createFunction(AST* node, UnwindInfo unw_info, AST_arguments* args,
                                      const std::vector<AST_stmt*>& body) {
        CLFunction* cl = wrapFunction(node, args, body, irstate->getSourceInfo());

        std::vector<ConcreteCompilerVariable*> defaults;
        for (auto d : args->defaults) {
            CompilerVariable* e = evalExpr(d, unw_info);
            ConcreteCompilerVariable* converted = e->makeConverted(emitter, e->getBoxType());
            e->decvref(emitter);
            defaults.push_back(converted);
        }

        CompilerVariable* created_closure = NULL;

        bool takes_closure;
        // Optimization: when compiling a module, it's nice to not have to run analyses into the
        // entire module's source code.
        // If we call getScopeInfoForNode, that will trigger an analysis of that function tree,
        // but we're only using it here to figure out if that function takes a closure.
        // Top level functions never take a closure, so we can skip the analysis.
        if (irstate->getSourceInfo()->ast->type == AST_TYPE::Module)
            takes_closure = false;
        else {
            takes_closure = irstate->getScopeInfoForNode(node)->takesClosure();
        }

        bool is_generator = cl->source->is_generator;

        if (takes_closure) {
            if (irstate->getScopeInfo()->createsClosure()) {
                created_closure = symbol_table[internString(CREATED_CLOSURE_NAME)];
            } else {
                assert(irstate->getScopeInfo()->passesThroughClosure());
                created_closure = symbol_table[internString(PASSED_CLOSURE_NAME)];
            }
            assert(created_closure);
        }

        CompilerVariable* func = makeFunction(emitter, cl, created_closure, is_generator, defaults);

        for (auto d : defaults) {
            d->decvref(emitter);
        }

        return func;
    }

    void doFunctionDef(AST_FunctionDef* node, UnwindInfo unw_info) {
        std::vector<CompilerVariable*> decorators;
        for (auto d : node->decorator_list) {
            decorators.push_back(evalExpr(d, unw_info));
        }

        CompilerVariable* func = _createFunction(node, unw_info, node->args, node->body);

        for (int i = decorators.size() - 1; i >= 0; i--) {
            func = decorators[i]->call(emitter, getOpInfoForNode(node, unw_info), ArgPassSpec(1), { func }, NULL);
            decorators[i]->decvref(emitter);
        }

        _doSet(irstate->getSourceInfo()->mangleName(node->name), func, unw_info);
        func->decvref(emitter);
    }

    void doPrint(AST_Print* node, UnwindInfo unw_info) {
        ConcreteCompilerVariable* dest = NULL;
        if (node->dest) {
            auto d = evalExpr(node->dest, unw_info);
            dest = d->makeConverted(emitter, d->getConcreteType());
            d->decvref(emitter);
        } else {
            llvm::Value* sys_stdout_val = emitter.createCall(unw_info, g.funcs.getSysStdout);
            dest = new ConcreteCompilerVariable(UNKNOWN, sys_stdout_val, true);
            // TODO: speculate that sys.stdout is a file?
        }
        assert(dest);

        static const std::string write_str("write");
        static const std::string newline_str("\n");
        static const std::string space_str(" ");

        int nvals = node->values.size();
        for (int i = 0; i < nvals; i++) {
            CompilerVariable* var = evalExpr(node->values[i], unw_info);
            ConcreteCompilerVariable* converted = var->makeConverted(emitter, var->getBoxType());
            var->decvref(emitter);

            // begin code for handling of softspace
            bool new_softspace = (i < nvals - 1) || (!node->nl);
            llvm::Value* dospace = emitter.createCall(unw_info, g.funcs.softspace,
                                                      { dest->getValue(), getConstantInt(new_softspace, g.i1) });
            assert(dospace->getType() == g.i1);

            llvm::BasicBlock* ss_block = llvm::BasicBlock::Create(g.context, "softspace", irstate->getLLVMFunction());
            llvm::BasicBlock* join_block = llvm::BasicBlock::Create(g.context, "print", irstate->getLLVMFunction());

            emitter.getBuilder()->CreateCondBr(dospace, ss_block, join_block);

            curblock = ss_block;
            emitter.getBuilder()->SetInsertPoint(ss_block);
            CallattrFlags flags = {.cls_only = false, .null_on_nonexistent = false };
            auto r = dest->callattr(emitter, getOpInfoForNode(node, unw_info), &write_str, flags, ArgPassSpec(1),
                                    { makeStr(&space_str) }, NULL);
            r->decvref(emitter);

            emitter.getBuilder()->CreateBr(join_block);
            curblock = join_block;
            emitter.getBuilder()->SetInsertPoint(join_block);
            // end code for handling of softspace


            llvm::Value* v = emitter.createCall(unw_info, g.funcs.str, converted->getValue());
            v = emitter.getBuilder()->CreateBitCast(v, g.llvm_value_type_ptr);
            auto s = new ConcreteCompilerVariable(STR, v, true);
            r = dest->callattr(emitter, getOpInfoForNode(node, unw_info), &write_str, flags, ArgPassSpec(1), { s },
                               NULL);
            s->decvref(emitter);
            r->decvref(emitter);
            converted->decvref(emitter);
        }

        if (node->nl) {
            CallattrFlags flags = {.cls_only = false, .null_on_nonexistent = false };
            auto r = dest->callattr(emitter, getOpInfoForNode(node, unw_info), &write_str, flags, ArgPassSpec(1),
                                    { makeStr(&newline_str) }, NULL);
            r->decvref(emitter);

            if (nvals == 0) {
                emitter.createCall(unw_info, g.funcs.softspace, { dest->getValue(), getConstantInt(0, g.i1) });
            }
        }

        dest->decvref(emitter);
    }

    void doReturn(AST_Return* node, UnwindInfo unw_info) {
        assert(!unw_info.needsInvoke());

        CompilerVariable* val;
        if (node->value == NULL) {
            if (irstate->getReturnType() == VOID) {
                endBlock(DEAD);
                emitter.getBuilder()->CreateRetVoid();
                return;
            }

            val = new ConcreteCompilerVariable(NONE, embedConstantPtr(None, g.llvm_value_type_ptr), false);
        } else {
            val = evalExpr(node->value, unw_info);
        }
        assert(val);

        // If we ask the return variable to become UNKNOWN (the typical return type),
        // it will be forced to split a copy of itself and incref.
        // But often the return variable will already be in the right shape, so in
        // that case asking it to convert to itself ends up just being an incvref
        // and doesn't end up emitting an incref+decref pair.
        // This could also be handled by casting from the CompilerVariable to
        // ConcreteCOmpilerVariable, but this way feels a little more robust to me.
        ConcreteCompilerType* opt_rtn_type = irstate->getReturnType();
        if (irstate->getReturnType()->llvmType() == val->getConcreteType()->llvmType())
            opt_rtn_type = val->getConcreteType();

        ConcreteCompilerVariable* rtn = val->makeConverted(emitter, opt_rtn_type);
        rtn->ensureGrabbed(emitter);
        val->decvref(emitter);

        for (auto& p : symbol_table) {
            p.second->decvref(emitter);
        }
        symbol_table.clear();

        endBlock(DEAD);

        ASSERT(rtn->getVrefs() == 1, "%d", rtn->getVrefs());
        assert(rtn->getValue());
        emitter.getBuilder()->CreateRet(rtn->getValue());
    }

    void doBranch(AST_Branch* node, UnwindInfo unw_info) {
        assert(!unw_info.needsInvoke());

        assert(node->iftrue->idx > myblock->idx);
        assert(node->iffalse->idx > myblock->idx);

        CompilerVariable* val = evalExpr(node->test, unw_info);
        assert(val);

        // We could call nonzero here if there is no try-catch block?
        ASSERT(val->getType() == BOOL, "should have called NONZERO before this; is %s",
               val->getType()->debugName().c_str());
        llvm::Value* v = i1FromBool(emitter, static_cast<ConcreteCompilerVariable*>(val));
        assert(v->getType() == g.i1);

        llvm::BasicBlock* iftrue = entry_blocks[node->iftrue];
        llvm::BasicBlock* iffalse = entry_blocks[node->iffalse];

        endBlock(FINISHED);

        emitter.getBuilder()->CreateCondBr(v, iftrue, iffalse);
    }

    void doExpr(AST_Expr* node, UnwindInfo unw_info) {
        CompilerVariable* var = evalExpr(node->value, unw_info);

        var->decvref(emitter);
    }

    void doOSRExit(llvm::BasicBlock* normal_target, AST_Jump* osr_key) {
        llvm::BasicBlock* starting_block = curblock;
        llvm::BasicBlock* onramp = llvm::BasicBlock::Create(g.context, "onramp", irstate->getLLVMFunction());

        // Code to check if we want to do the OSR:
        llvm::GlobalVariable* edgecount_ptr = new llvm::GlobalVariable(
            *g.cur_module, g.i64, false, llvm::GlobalValue::InternalLinkage, getConstantInt(0, g.i64), "edgecount");
        llvm::Value* curcount = emitter.getBuilder()->CreateLoad(edgecount_ptr);
        llvm::Value* newcount = emitter.getBuilder()->CreateAdd(curcount, getConstantInt(1, g.i64));
        emitter.getBuilder()->CreateStore(newcount, edgecount_ptr);

        auto effort = irstate->getEffortLevel();
        int osr_threshold;
        if (effort == EffortLevel::MINIMAL)
            osr_threshold = OSR_THRESHOLD_BASELINE;
        else if (effort == EffortLevel::MODERATE)
            osr_threshold = OSR_THRESHOLD_T2;
        else
            RELEASE_ASSERT(0, "Unknown effort: %d", (int)effort);
        llvm::Value* osr_test = emitter.getBuilder()->CreateICmpSGT(newcount, getConstantInt(osr_threshold));

        llvm::Metadata* md_vals[]
            = { llvm::MDString::get(g.context, "branch_weights"), llvm::ConstantAsMetadata::get(getConstantInt(1)),
                llvm::ConstantAsMetadata::get(getConstantInt(1000)) };
        llvm::MDNode* branch_weights = llvm::MDNode::get(g.context, llvm::ArrayRef<llvm::Metadata*>(md_vals));
        emitter.getBuilder()->CreateCondBr(osr_test, onramp, normal_target, branch_weights);

        // Emitting the actual OSR:
        emitter.getBuilder()->SetInsertPoint(onramp);
        OSREntryDescriptor* entry = OSREntryDescriptor::create(irstate->getCurFunction(), osr_key);
        OSRExit* exit = new OSRExit(irstate->getCurFunction(), entry);
        llvm::Value* partial_func = emitter.getBuilder()->CreateCall(g.funcs.compilePartialFunc,
                                                                     embedConstantPtr(exit, g.i8->getPointerTo()));

        std::vector<llvm::Value*> llvm_args;
        std::vector<llvm::Type*> llvm_arg_types;
        std::vector<ConcreteCompilerVariable*> converted_args;

        SortedSymbolTable sorted_symbol_table(symbol_table.begin(), symbol_table.end());

        // For OSR calls, we use the same calling convention as in some other places; namely,
        // arg1, arg2, arg3, argarray [nargs is ommitted]
        // It would be nice to directly pass all variables as arguments, instead of packing them into
        // an array, for a couple reasons (eliminate copies, and allow for a tail call).
        // But this doesn't work if the IR is being interpreted, because the interpreter can't
        // do arbitrary-arity function calls (yet?).  One possibility is to pass them as an
        // array for the interpreter and as all arguments for compilation, but I'd rather avoid
        // having two different calling conventions for the same thing.  Plus, this would
        // prevent us from having two OSR exits point to the same OSR entry; not something that
        // we're doing right now but something that would be nice in the future.

        llvm::Value* arg_array = NULL, * malloc_save = NULL;
        if (sorted_symbol_table.size() > 3) {
            // Leave in the ability to use malloc but I guess don't use it.
            // Maybe if there are a ton of live variables it'd be nice to have them be
            // heap-allocated, or if we don't immediately return the result of the OSR?
            bool use_malloc = false;
            if (false) {
                llvm::Value* n_bytes = getConstantInt((sorted_symbol_table.size() - 3) * sizeof(Box*), g.i64);
                llvm::Value* l_malloc = embedConstantPtr(
                    (void*)malloc, llvm::FunctionType::get(g.i8->getPointerTo(), g.i64, false)->getPointerTo());
                malloc_save = emitter.getBuilder()->CreateCall(l_malloc, n_bytes);
                arg_array = emitter.getBuilder()->CreateBitCast(malloc_save, g.llvm_value_type_ptr->getPointerTo());
            } else {
                llvm::Value* n_varargs = llvm::ConstantInt::get(g.i64, sorted_symbol_table.size() - 3, false);
                arg_array = emitter.getBuilder()->CreateAlloca(g.llvm_value_type_ptr, n_varargs);
            }
        }

        int arg_num = -1;
        for (const auto& p : sorted_symbol_table) {
            arg_num++;

            // This line can never get hit right now since we unnecessarily force every variable to be concrete
            // for a loop, since we generate all potential phis:
            ASSERT(p.second->getType() == p.second->getConcreteType(), "trying to pass through %s\n",
                   p.second->getType()->debugName().c_str());

            ConcreteCompilerVariable* var = p.second->makeConverted(emitter, p.second->getConcreteType());
            converted_args.push_back(var);

            assert(var->getType() != BOXED_INT && "should probably unbox it, but why is it boxed in the first place?");
            assert(var->getType() != BOXED_FLOAT
                   && "should probably unbox it, but why is it boxed in the first place?");

            // This line can never get hit right now for the same reason that the variables must already be concrete,
            // because we're over-generating phis.
            ASSERT(var->isGrabbed(), "%s", p.first.c_str());
            // var->ensureGrabbed(emitter);

            llvm::Value* val = var->getValue();

            if (arg_num < 3) {
                llvm_args.push_back(val);
                llvm_arg_types.push_back(val->getType());
            } else {
                llvm::Value* ptr = emitter.getBuilder()->CreateConstGEP1_32(arg_array, arg_num - 3);

                if (var->getType() == INT || var->getType() == BOOL) {
                    val = emitter.getBuilder()->CreateIntToPtr(val, g.llvm_value_type_ptr);
                } else if (var->getType() == FLOAT) {
                    // val = emitter.getBuilder()->CreateBitCast(val, g.llvm_value_type_ptr);
                    ptr = emitter.getBuilder()->CreateBitCast(ptr, g.double_->getPointerTo());
                } else if (var->getType() == GENERATOR) {
                    ptr = emitter.getBuilder()->CreateBitCast(ptr, g.llvm_generator_type_ptr->getPointerTo());
                } else if (var->getType() == UNDEF) {
                    // TODO if there are any undef variables, we're in 'unreachable' territory.
                    // Do we even need to generate any of this code?

                    // Currently we represent 'undef's as 'i16 undef'
                    val = emitter.getBuilder()->CreateIntToPtr(val, g.llvm_value_type_ptr);
                } else if (var->getType() == CLOSURE) {
                    ptr = emitter.getBuilder()->CreateBitCast(ptr, g.llvm_closure_type_ptr->getPointerTo());
                } else {
                    assert(val->getType() == g.llvm_value_type_ptr);
                }

                emitter.getBuilder()->CreateStore(val, ptr);
            }

            ConcreteCompilerType*& t = entry->args[p.first];
            if (t == NULL)
                t = var->getType();
            else
                ASSERT(t == var->getType(), "%s %s\n", t->debugName().c_str(), var->getType()->debugName().c_str());
        }

        if (sorted_symbol_table.size() > 3) {
            llvm_args.push_back(arg_array);
            llvm_arg_types.push_back(arg_array->getType());
        }

        llvm::FunctionType* ft
            = llvm::FunctionType::get(irstate->getReturnType()->llvmType(), llvm_arg_types, false /*vararg*/);
        partial_func = emitter.getBuilder()->CreateBitCast(partial_func, ft->getPointerTo());

        llvm::CallInst* rtn = emitter.getBuilder()->CreateCall(partial_func, llvm_args);

        // If we alloca'd the arg array, we can't make this into a tail call:
        if (arg_array == NULL && malloc_save != NULL) {
            rtn->setTailCall(true);
        }

        if (malloc_save != NULL) {
            llvm::Value* l_free = embedConstantPtr(
                (void*)free, llvm::FunctionType::get(g.void_, g.i8->getPointerTo(), false)->getPointerTo());
            emitter.getBuilder()->CreateCall(l_free, malloc_save);
        }

        for (int i = 0; i < converted_args.size(); i++) {
            converted_args[i]->decvref(emitter);
        }

        if (irstate->getReturnType() == VOID)
            emitter.getBuilder()->CreateRetVoid();
        else
            emitter.getBuilder()->CreateRet(rtn);

        emitter.getBuilder()->SetInsertPoint(starting_block);
    }

    void doJump(AST_Jump* node, UnwindInfo unw_info) {
        endBlock(FINISHED);

        llvm::BasicBlock* target = entry_blocks[node->target];

        if (ENABLE_OSR && node->target->idx < myblock->idx && irstate->getEffortLevel() < EffortLevel::MAXIMAL) {
            assert(node->target->predecessors.size() > 1);
            doOSRExit(target, node);
        } else {
            emitter.getBuilder()->CreateBr(target);
        }
    }

    void doRaise(AST_Raise* node, UnwindInfo unw_info) {
        // It looks like ommitting the second and third arguments are equivalent to passing None,
        // but ommitting the first argument is *not* the same as passing None.

        if (node->arg0 == NULL) {
            assert(!node->arg1);
            assert(!node->arg2);

            emitter.createCall(unw_info, g.funcs.raise0, std::vector<llvm::Value*>());
            emitter.getBuilder()->CreateUnreachable();

            endBlock(DEAD);
            return;
        }

        std::vector<llvm::Value*> args;
        for (auto a : { node->arg0, node->arg1, node->arg2 }) {
            if (a) {
                CompilerVariable* v = evalExpr(a, unw_info);
                ConcreteCompilerVariable* converted = v->makeConverted(emitter, v->getBoxType());
                v->decvref(emitter);
                args.push_back(converted->getValue());
            } else {
                args.push_back(embedConstantPtr(None, g.llvm_value_type_ptr));
            }
        }

        emitter.createCall(unw_info, g.funcs.raise3, args);
        emitter.getBuilder()->CreateUnreachable();

        endBlock(DEAD);
    }

    void doStmt(AST_stmt* node, UnwindInfo unw_info) {
        // printf("%d stmt: %d\n", node->type, node->lineno);
        if (node->lineno) {
            emitter.getBuilder()->SetCurrentDebugLocation(
                llvm::DebugLoc::get(node->lineno, 0, irstate->getFuncDbgInfo()));
        }

        switch (node->type) {
            case AST_TYPE::Assert:
                doAssert(ast_cast<AST_Assert>(node), unw_info);
                break;
            case AST_TYPE::Assign:
                doAssign(ast_cast<AST_Assign>(node), unw_info);
                break;
            case AST_TYPE::ClassDef:
                doClassDef(ast_cast<AST_ClassDef>(node), unw_info);
                break;
            case AST_TYPE::Delete:
                doDelete(ast_cast<AST_Delete>(node), unw_info);
                break;
            case AST_TYPE::Expr:
                doExpr(ast_cast<AST_Expr>(node), unw_info);
                break;
            case AST_TYPE::FunctionDef:
                doFunctionDef(ast_cast<AST_FunctionDef>(node), unw_info);
                break;
            // case AST_TYPE::If:
            // doIf(ast_cast<AST_If>(node));
            // break;
            // case AST_TYPE::Import:
            //     doImport(ast_cast<AST_Import>(node), unw_info);
            //     break;
            // case AST_TYPE::ImportFrom:
            //     doImportFrom(ast_cast<AST_ImportFrom>(node), unw_info);
            //     break;
            case AST_TYPE::Global:
                // Should have been handled already
                break;
            case AST_TYPE::Pass:
                break;
            case AST_TYPE::Print:
                doPrint(ast_cast<AST_Print>(node), unw_info);
                break;
            case AST_TYPE::Return:
                assert(!unw_info.needsInvoke());
                doReturn(ast_cast<AST_Return>(node), unw_info);
                break;
            case AST_TYPE::Branch:
                assert(!unw_info.needsInvoke());
                doBranch(ast_cast<AST_Branch>(node), unw_info);
                break;
            case AST_TYPE::Jump:
                assert(!unw_info.needsInvoke());
                doJump(ast_cast<AST_Jump>(node), unw_info);
                break;
            case AST_TYPE::Invoke: {
                assert(!unw_info.needsInvoke());
                AST_Invoke* invoke = ast_cast<AST_Invoke>(node);
                doStmt(invoke->stmt, UnwindInfo(node, entry_blocks[invoke->exc_dest]));

                assert(state == RUNNING || state == DEAD);
                if (state == RUNNING) {
                    emitter.getBuilder()->CreateBr(entry_blocks[invoke->normal_dest]);
                    endBlock(FINISHED);
                }

                break;
            }
            case AST_TYPE::Raise:
                doRaise(ast_cast<AST_Raise>(node), unw_info);
                break;
            default:
                printf("Unhandled stmt type at " __FILE__ ":" STRINGIFY(__LINE__) ": %d\n", node->type);
                exit(1);
        }
    }

    void loadArgument(InternedString name, ConcreteCompilerType* t, llvm::Value* v, UnwindInfo unw_info) {
        ConcreteCompilerVariable* var = unboxVar(t, v, false);
        _doSet(name, var, unw_info);
        var->decvref(emitter);
    }

    void loadArgument(AST_expr* name, ConcreteCompilerType* t, llvm::Value* v, UnwindInfo unw_info) {
        ConcreteCompilerVariable* var = unboxVar(t, v, false);
        _doSet(name, var, unw_info);
        var->decvref(emitter);
    }

    bool allowableFakeEndingSymbol(InternedString name) {
        // TODO this would be a great place to be able to use interned versions of the static names...
        return isIsDefinedName(name.str()) || name.str() == PASSED_CLOSURE_NAME || name.str() == CREATED_CLOSURE_NAME
               || name.str() == PASSED_GENERATOR_NAME;
    }

    void endBlock(State new_state) {
        assert(state == RUNNING);

        // cf->func->dump();

        SourceInfo* source = irstate->getSourceInfo();
        ScopeInfo* scope_info = irstate->getScopeInfo();

        // Additional names to remove; remove them after iteration is done to new mess up the iterators
        std::vector<InternedString> also_remove;
        for (SymbolTable::iterator it = symbol_table.begin(); it != symbol_table.end();) {
            if (allowableFakeEndingSymbol(it->first)) {
                ++it;
                continue;
            }

            // ASSERT(it->first[0] != '!' || isIsDefinedName(it->first), "left a fake variable in the real
            // symbol table? '%s'", it->first.c_str());

            if (!source->liveness->isLiveAtEnd(it->first, myblock)) {
                // printf("%s dead at end of %d; grabbed = %d, %d vrefs\n", it->first.c_str(), myblock->idx,
                // it->second->isGrabbed(), it->second->getVrefs());
                also_remove.push_back(getIsDefinedName(it->first));

                it->second->decvref(emitter);
                it = symbol_table.erase(it);
            } else if (source->phis->isRequiredAfter(it->first, myblock)) {
                assert(!scope_info->refersToGlobal(it->first));
                ConcreteCompilerType* phi_type = types->getTypeAtBlockEnd(it->first, myblock);
                // printf("Converting %s from %s to %s\n", it->first.c_str(),
                // it->second->getType()->debugName().c_str(), phi_type->debugName().c_str());
                // printf("have to convert %s from %s to %s\n", it->first.c_str(),
                // it->second->getType()->debugName().c_str(), phi_type->debugName().c_str());
                ConcreteCompilerVariable* v = it->second->makeConverted(emitter, phi_type);
                it->second->decvref(emitter);
                symbol_table[it->first] = v->split(emitter);
                ++it;
            } else {
#ifndef NDEBUG
                if (myblock->successors.size()) {
                    // TODO getTypeAtBlockEnd will automatically convert up to the concrete type, which we don't want
                    // here, but this is just for debugging so I guess let it happen for now:
                    ConcreteCompilerType* ending_type = types->getTypeAtBlockEnd(it->first, myblock);
                    ASSERT(it->second->canConvertTo(ending_type), "%s is supposed to be %s, but somehow is %s",
                           it->first.c_str(), ending_type->debugName().c_str(),
                           it->second->getType()->debugName().c_str());
                }
#endif

                ++it;
            }
        }

        for (const auto& s : also_remove) {
            symbol_table.erase(s);
        }

        const PhiAnalysis::RequiredSet& all_phis = source->phis->getAllRequiredAfter(myblock);
        for (PhiAnalysis::RequiredSet::const_iterator it = all_phis.begin(), end = all_phis.end(); it != end; ++it) {
            // printf("phi will be required for %s\n", it->c_str());
            assert(!scope_info->refersToGlobal(*it));
            CompilerVariable*& cur = symbol_table[*it];

            InternedString defined_name = getIsDefinedName(*it);

            if (cur != NULL) {
                // printf("defined on this path; ");

                ConcreteCompilerVariable* is_defined
                    = static_cast<ConcreteCompilerVariable*>(_popFake(defined_name, true));

                if (source->phis->isPotentiallyUndefinedAfter(*it, myblock)) {
                    // printf("is potentially undefined later, so marking it defined\n");
                    if (is_defined) {
                        _setFake(defined_name, is_defined);
                    } else {
                        _setFake(defined_name, makeBool(1));
                    }
                } else {
                    // printf("is defined in all later paths, so not marking\n");
                    assert(!is_defined);
                }
            } else {
                // printf("no st entry, setting undefined\n");
                ConcreteCompilerType* phi_type = types->getTypeAtBlockEnd(*it, myblock);
                cur = new ConcreteCompilerVariable(phi_type, llvm::UndefValue::get(phi_type->llvmType()), true);
                _setFake(defined_name, makeBool(0));
            }
        }

        state = new_state;
    }

public:
    void addFrameStackmapArgs(PatchpointInfo* pp, AST_stmt* current_stmt,
                              std::vector<llvm::Value*>& stackmap_args) override {
        int initial_args = stackmap_args.size();

        stackmap_args.push_back(irstate->getFrameInfoVar());

        assert(INT->llvmType() == g.i64);
        stackmap_args.push_back(getConstantInt((uint64_t)current_stmt, g.i64));
        pp->addFrameVar("!current_stmt", INT);

        if (ENABLE_FRAME_INTROSPECTION) {
            // TODO: don't need to use a sorted symbol table if we're explicitly recording the names!
            // nice for debugging though.
            SortedSymbolTable sorted_symbol_table(symbol_table.begin(), symbol_table.end());

            for (const auto& p : sorted_symbol_table) {
                CompilerVariable* v = p.second;

                v->serializeToFrame(stackmap_args);
                pp->addFrameVar(p.first.str(), v->getType());
            }
        }

        int num_frame_args = stackmap_args.size() - initial_args;
        pp->setNumFrameArgs(num_frame_args);
    }

    EndingState getEndingSymbolTable() override {
        assert(state == FINISHED || state == DEAD);

        // for (SymbolTable::iterator it = symbol_table.begin(); it != symbol_table.end(); ++it) {
        // printf("%s %p %d\n", it->first.c_str(), it->second, it->second->getVrefs());
        //}

        SourceInfo* source = irstate->getSourceInfo();

        SymbolTable* st = new SymbolTable(symbol_table);
        ConcreteSymbolTable* phi_st = new ConcreteSymbolTable();

        if (myblock->successors.size() == 0) {
            for (auto& p : *st) {
                p.second->decvref(emitter);
            }
            st->clear();
            symbol_table.clear();
            return EndingState(st, phi_st, curblock);
        } else if (myblock->successors.size() > 1) {
            // Since there are no critical edges, all successors come directly from this node,
            // so there won't be any required phis.
            return EndingState(st, phi_st, curblock);
        }

        assert(myblock->successors.size() == 1); // other cases should have been handled

        // In theory this case shouldn't be necessary:
        if (myblock->successors[0]->predecessors.size() == 1) {
            // If the next block has a single predecessor, don't have to
            // emit any phis.
            // Should probably not emit no-op jumps like this though.
            return EndingState(st, phi_st, curblock);
        }

        for (SymbolTable::iterator it = st->begin(); it != st->end();) {
            if (allowableFakeEndingSymbol(it->first) || source->phis->isRequiredAfter(it->first, myblock)) {
                ASSERT(it->second->isGrabbed(), "%s", it->first.c_str());
                assert(it->second->getVrefs() == 1);
                // this conversion should have already happened... should refactor this.
                ConcreteCompilerType* ending_type;
                if (isIsDefinedName(it->first.str())) {
                    assert(it->second->getType() == BOOL);
                    ending_type = BOOL;
                } else if (it->first.str() == PASSED_CLOSURE_NAME) {
                    ending_type = getPassedClosureType();
                } else if (it->first.str() == CREATED_CLOSURE_NAME) {
                    ending_type = getCreatedClosureType();
                } else if (it->first.str() == PASSED_GENERATOR_NAME) {
                    ending_type = GENERATOR;
                } else {
                    ending_type = types->getTypeAtBlockEnd(it->first, myblock);
                }
                //(*phi_st)[it->first] = it->second->makeConverted(emitter, it->second->getConcreteType());
                // printf("%s %p %d\n", it->first.c_str(), it->second, it->second->getVrefs());
                (*phi_st)[it->first] = it->second->split(emitter)->makeConverted(emitter, ending_type);
                it = st->erase(it);
            } else {
                ++it;
            }
        }
        return EndingState(st, phi_st, curblock);
    }

    void giveLocalSymbol(InternedString name, CompilerVariable* var) override {
        assert(name.str() != "None");
        ASSERT(!irstate->getScopeInfo()->refersToGlobal(name), "%s", name.c_str());
        assert(var->getType() != BOXED_INT);
        assert(var->getType() != BOXED_FLOAT);
        CompilerVariable*& cur = symbol_table[name];
        assert(cur == NULL);
        cur = var;
    }

    void copySymbolsFrom(SymbolTable* st) override {
        assert(st);
        DupCache cache;
        for (SymbolTable::iterator it = st->begin(); it != st->end(); ++it) {
            // printf("Copying in %s: %p, %d\n", it->first.c_str(), it->second, it->second->getVrefs());
            symbol_table[it->first] = it->second->dup(cache);
            // printf("got: %p, %d\n", symbol_table[it->first], symbol_table[it->first]->getVrefs());
        }
    }

    ConcreteCompilerType* getPassedClosureType() {
        // TODO could know the exact closure shape
        return CLOSURE;
    }

    ConcreteCompilerType* getCreatedClosureType() {
        // TODO could know the exact closure shape
        return CLOSURE;
    }

    void doFunctionEntry(const ParamNames& param_names, const std::vector<ConcreteCompilerType*>& arg_types) override {
        assert(param_names.totalParameters() == arg_types.size());

        auto scope_info = irstate->getScopeInfo();

        llvm::Value* passed_closure = NULL;
        llvm::Function::arg_iterator AI = irstate->getLLVMFunction()->arg_begin();

        if (scope_info->takesClosure()) {
            passed_closure = AI;
            symbol_table[internString(PASSED_CLOSURE_NAME)]
                = new ConcreteCompilerVariable(getPassedClosureType(), AI, true);
            ++AI;
        }

        if (scope_info->createsClosure()) {
            if (!passed_closure)
                passed_closure = embedConstantPtr(nullptr, g.llvm_closure_type_ptr);

            llvm::Value* new_closure = emitter.getBuilder()->CreateCall(g.funcs.createClosure, passed_closure);
            symbol_table[internString(CREATED_CLOSURE_NAME)]
                = new ConcreteCompilerVariable(getCreatedClosureType(), new_closure, true);
        }

        if (irstate->getSourceInfo()->is_generator) {
            symbol_table[internString(PASSED_GENERATOR_NAME)] = new ConcreteCompilerVariable(GENERATOR, AI, true);
            ++AI;
        }


        std::vector<llvm::Value*> python_parameters;
        for (int i = 0; i < arg_types.size(); i++) {
            assert(AI != irstate->getLLVMFunction()->arg_end());

            if (i == 3) {
                for (int i = 3; i < arg_types.size(); i++) {
                    llvm::Value* ptr = emitter.getBuilder()->CreateConstGEP1_32(AI, i - 3);
                    llvm::Value* loaded = emitter.getBuilder()->CreateLoad(ptr);

                    if (arg_types[i]->llvmType() == g.i64)
                        loaded = emitter.getBuilder()->CreatePtrToInt(loaded, arg_types[i]->llvmType());
                    else
                        assert(arg_types[i]->llvmType() == g.llvm_value_type_ptr);

                    python_parameters.push_back(loaded);
                }
                ++AI;
                break;
            }

            python_parameters.push_back(AI);
            ++AI;
        }

        assert(AI == irstate->getLLVMFunction()->arg_end());
        assert(python_parameters.size() == param_names.totalParameters());

        int i = 0;
        for (; i < param_names.args.size(); i++) {
            loadArgument(internString(param_names.args[i]), arg_types[i], python_parameters[i],
                         UnwindInfo::cantUnwind());
        }

        if (param_names.vararg.size()) {
            loadArgument(internString(param_names.vararg), arg_types[i], python_parameters[i],
                         UnwindInfo::cantUnwind());
            i++;
        }

        if (param_names.kwarg.size()) {
            loadArgument(internString(param_names.kwarg), arg_types[i], python_parameters[i], UnwindInfo::cantUnwind());
            i++;
        }

        assert(i == arg_types.size());
    }

    void run(const CFGBlock* block) override {
        for (int i = 0; i < block->body.size(); i++) {
            if (state == DEAD)
                break;
            assert(state != FINISHED);
            doStmt(block->body[i], UnwindInfo(block->body[i], NULL));
        }
    }

    void doSafePoint() override { emitter.getBuilder()->CreateCall(g.funcs.allowGLReadPreemption); }
};

IRGenerator* createIRGenerator(IRGenState* irstate, std::unordered_map<CFGBlock*, llvm::BasicBlock*>& entry_blocks,
                               CFGBlock* myblock, TypeAnalysis* types) {
    return new IRGeneratorImpl(irstate, entry_blocks, myblock, types);
}

CLFunction* wrapFunction(AST* node, AST_arguments* args, const std::vector<AST_stmt*>& body, SourceInfo* source) {
    // Different compilations of the parent scope of a functiondef should lead
    // to the same CLFunction* being used:
    static std::unordered_map<AST*, CLFunction*> made;

    CLFunction*& cl = made[node];
    if (cl == NULL) {
        SourceInfo* si = new SourceInfo(source->parent_module, source->scoping, node, body);
        if (args)
            cl = new CLFunction(args->args.size(), args->defaults.size(), args->vararg.str().size(),
                                args->kwarg.str().size(), si);
        else
            cl = new CLFunction(0, 0, 0, 0, si);
    }
    return cl;
}
}
