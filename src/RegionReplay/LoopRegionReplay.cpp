/*****************************************************************************
 * This file is part of CERE.                                                *
 *                                                                           *
 * Copyright (c) 2013-2015, Universite de Versailles St-Quentin-en-Yvelines  *
 *                                                                           *
 * CERE is free software: you can redistribute it and/or modify it under     *
 * the terms of the GNU Lesser General Public License as published by        *
 * the Free Software Foundation, either version 3 of the License,            *
 * or (at your option) any later version.                                    *
 *                                                                           *
 * CERE is distributed in the hope that it will be useful,                   *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of            *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the             *
 * GNU General Public License for more details.                              *
 *                                                                           *
 * You should have received a copy of the GNU General Public License         *
 * along with CERE.  If not, see <http://www.gnu.org/licenses/>.             *
 *****************************************************************************/

//===----------------------------------------------------------------------===//
//
// This file insert functions needed to replay a region. It inserts a load
// function to load dumped memory. It also creates a wrapper to call the region
// to replay. These load functions are defined in the memory_dump module.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "region-replay"
#include "llvm/Support/Debug.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/LoopPass.h"
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/DataLayout.h>
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/CommandLine.h"
#include <string>
#include <sstream>
#include <fstream>
#include <set>
#include "RegionReplay.h"

#include <iostream>

#if LLVM_VERSION_MINOR == 5 || LLVM_VERSION_MAJOR > 3
#include "llvm/IR/InstIterator.h"
#else
#include "llvm/Support/InstIterator.h"
#endif

using namespace llvm;

STATISTIC(LoopCounter, "Counts number of replayed regions");

extern cl::opt<std::string> RegionName;
extern cl::opt<int> Invocation;

namespace {
struct LoopRegionReplay : public ModulePass {
  static char ID;
  unsigned NumLoops;
  int InvocationToReplay;
  std::string RegionToReplay;

  explicit LoopRegionReplay(unsigned numLoops = ~0)
      : ModulePass(ID), NumLoops(numLoops), RegionToReplay(RegionName),
        InvocationToReplay(Invocation) {}

  virtual bool runOnModule(Module &M) override;
  bool runOnFunction(Function &F);
  bool maybeCreateWrapper(Function &F, Module *mod);
};
}

char LoopRegionReplay::ID = 0;
static RegisterPass<LoopRegionReplay> X("region-replay", "Replay regions");

bool LoopRegionReplay::runOnModule(Module &M) {
  // No main, no instrumentation!
  Function *Main = M.getFunction("main");

  // Using fortran? ... this kind of works
  if (!Main)
    Main = M.getFunction("MAIN__");

  if (Main) { // We are in the module with the main function
    std::string funcName = "real_main";
    //~ConstantInt* const_int1_11 = ConstantInt::get(M.getContext(), APInt(1,
    //StringRef("0"), 10)); //false

    Function *mainFunction = M.getFunction(funcName);
    if (!mainFunction) {
      std::vector<Type *> FuncTy_8_args;
      FunctionType *FuncTy_8 = FunctionType::get(
          /*Result=*/Type::getVoidTy(M.getContext()),
          /*Params=*/FuncTy_8_args,
          /*isVarArg=*/false);
      Function *mainFunction = Function::Create(
          FuncTy_8, GlobalValue::ExternalLinkage, funcName, &M);
      std::vector<Value *> void_16_params;
      CallInst::Create(mainFunction, "", &*Main->begin()->begin());
    }
  }

  std::vector<Function *> preexistingFunctions;
  for (auto &F : M.functions()) {
    preexistingFunctions.push_back(&F);
  }
  for (auto *F : preexistingFunctions) {
    runOnFunction(*F);
  }
  return true;
}

/**Called on each function int the current Module**/
bool LoopRegionReplay::runOnFunction(Function &F) {
  Module *mod = F.getParent();
  // Get all loops int the current function and visit them
  maybeCreateWrapper(F, mod);
  return true;
}

bool LoopRegionReplay::maybeCreateWrapper(Function &F, Module *mod) {
  // If we want to replay a particular region, look if it's this one
  if (RegionToReplay != F.getName()) {
    return false;
  }

  // Create the wrapper function and add a basicBlock in it
  Function *wrapperFunction = createWrapperFunction(&F, mod);
  BasicBlock *label_entry =
      BasicBlock::Create(mod->getContext(), "entry", wrapperFunction, 0);

  // Now we create the load function and add a call in the wrapper function

  // Create the signature of the load function
  FunctionType *LoadFuncTy = createLoadFunctionType(mod);
  // Create the function load
  Function *func_load = mod->getFunction("load");
  if (!func_load) {
    func_load = Function::Create(
        /*Type=*/LoadFuncTy,
        /*Linkage=*/GlobalValue::ExternalLinkage,
        /*Name=*/"load", mod);
  }

  // Params for the load function
  std::vector<Value *> loadParams = createLoadFunctionParameters(
      mod, &F, label_entry, InvocationToReplay);

  // Add call to load
  CallInst::Create(func_load, loadParams, "", label_entry);

  // Ok now we have adresses of variables used by the loop in ptr_vla array,
  // so we have to read it, and create parameters for the loop
  std::vector<Value *> loopParams = createLoopParameters(
      &F, mod, cast<AllocaInst>(loadParams.back()), label_entry);

  // Call the function with the extracted loop
  CallInst::Create(&F, loopParams, "", label_entry);

  // We must add an anti dead code function
  std::vector<Type *> FuncADCE_args;
  // Signature: void anti_dead_code_elimination(int, ...);
  FuncADCE_args.push_back(IntegerType::get(mod->getContext(), 32));
  FunctionType *FuncADCE_type = FunctionType::get(
      /*Result=*/Type::getVoidTy(mod->getContext()),
      /*Params=*/FuncADCE_args,
      /*isVarArg=*/true);
  // create antiDCE function
  Function *antiDeadCodeFunction =
      Function::Create(FuncADCE_type, GlobalValue::ExternalLinkage,
                       "anti_dead_code_elimination", mod);
  antiDeadCodeFunction->addFnAttr(Attribute::NoInline);
  // AntiDCE function takes exactly the same parameters then the loop
  // + the number of parameters as we pass them as variable argument
  ConstantInt *nbParam =
      ConstantInt::get(mod->getContext(), APInt(32, loopParams.size(), 10));
  loopParams.insert(loopParams.begin(), nbParam);
  // Add the call
  CallInst::Create(antiDeadCodeFunction, loopParams, "", label_entry);
  // Add a return instruction for wrapper function
  ReturnInst::Create(mod->getContext(), label_entry);

  return true;
}
