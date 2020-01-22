/*
 * Copyright (c) 2020 Trail of Bits, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <iostream>
#include <unordered_map>
#include <memory>
#include <queue>
#include <string>

#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Module.h>

namespace llvm
{
  class MDNode;
  class Module;
  class DICompileUnit;
  class DIFile;
  class DILocation;
  class DISubprogram;
  class Function;
} // namespace llvm

namespace mcsema
{

struct DbgMetadata
{
  void Parse(const std::string &filename);
  void AddDir(std::string filename, std::string dir_name);
  void AddEa(const std::string &filename, uint64_t ea, uint32_t line);
  void CreateEaMetadata(llvm::Function *func, uint64_t ea);

  llvm::DILocation *Fetch(llvm::Function *func, uint64_t ea);

  void Annotate(llvm::Function *func, uint64_t ea);

  llvm::DISubprogram *CreateDummyProgram(llvm::Function *func);

  void FillMissing(llvm::Module &m);
  void FillFunc(llvm::Function &f);

  ~DbgMetadata() {
    _dib->finalize();
  }

  bool Is() {
    return !ea_to_line.empty();
  }

  void OneBlockAnnotate(llvm::Function*, llvm::BasicBlock *bb, uint64_t ea);

  void Petrify() {
    for (auto [e_f, e_l] : ea_to_dbg) {
      s_ea_to_dbg[e_f->getName().str()] = e_l;
    }
  }

  void SetCtx(llvm::Function *func) {
    _ctx = { func };
    _ctx.Clear();
  }

  struct Ctx {
    llvm::Function *func;
    llvm::Instruction *current;
    llvm::BasicBlock *block;
    llvm::DILocation *dil;

    void Clear() {
      for (auto &bb: *func) {
        for (auto &inst : bb)
          inst.setDebugLoc({});
      }
      current = &*func->begin()->rbegin();
      block = current->getParent();
    }

    void PropagateForward(llvm::DILocation *loc) {
      if (!current) {
        current = &*block->begin();
      }

      dil = loc;
      auto it = llvm::BasicBlock::iterator(current);
      if (it != block->end()) {
        ++it;
      } else {
        return;
      }
      for(;it != block->end(); ++it) {
        it->setDebugLoc(loc);
      }
      current = &*std::prev(it);
    }

    void SetBlock(llvm::BasicBlock *new_b) {
      block = new_b;
      current = nullptr;
    }
  };

  llvm::Module &_m;

  Ctx _ctx = { nullptr };
  std::unique_ptr< llvm::DIBuilder > _dib = std::make_unique< llvm::DIBuilder >(_m);

  llvm::DICompileUnit *_cu = nullptr;

  // We need upper_bound
  using Lines = std::map< uint64_t, llvm::DILocation * >;

  std::unordered_map< uint64_t, uint64_t > ea_to_line;
  std::unordered_map< llvm::Function *, Lines > ea_to_dbg;

  // Remember just names since some refinement passes may remove functions
  std::unordered_map< std::string, Lines > s_ea_to_dbg;
  std::unordered_map< std::string, llvm::DIFile * > _dirs;
  std::unordered_map< llvm::Function *, llvm::DISubprogram * > _func_to_dbg;

  DbgMetadata(llvm::Module &module) : _m(module) {}
};

struct Propagate {

  using Lines = DbgMetadata::Lines;

  llvm::Function &func;
  const Lines &lines;

  Propagate(llvm::Function &f, const Lines &l) : func(f), lines(l) {};

  std::optional<uint64_t> Ea(llvm::BasicBlock &bb) {
    if (!bb.hasName())
      return {};
    auto name = bb.getName();
    auto suffix = name.split('_').second;
    uint64_t ea = std::strtoul(suffix.str().c_str(), nullptr, 16);
    if (ea == 0)
      return {};
    return { ea };
  }

  llvm::DILocation *BlockLine(llvm::BasicBlock &bb) {
    auto ea = Ea(bb);
    if (!ea)
      return nullptr;

    auto dil = lines.lower_bound(*ea);
    if (dil != lines.begin())
      dil = std::prev(dil);

    return dil->second;
  }

  llvm::DILocation *ExactLine(llvm::BasicBlock &bb) {
    auto ea = Ea(bb);
    if (!ea)
      return nullptr;
    auto dil = lines.find(*ea);
    if (dil == lines.end())
      return nullptr;
    return dil->second;
  }

  llvm::DILocation *Init(llvm::BasicBlock &bb) {
    if (auto exact = ExactLine(bb))
      return exact;
    auto from_name = BlockLine(bb);
    return from_name;
  }

  void Run() {
    if (!func.begin()->begin()->getDebugLoc())
      return;
    for (auto &bb : func) {
      Work(bb);
    }
  }

  void Work(llvm::BasicBlock &bb) {
    llvm::DILocation *dil = Init(bb);

    for (auto &inst : bb) {
      if (auto loc = inst.getDebugLoc()) {
        if (loc && dil && loc->getLine() <= dil->getLine()) {
          inst.setDebugLoc(dil);
        } else {
          dil = loc;
        }
      } else {
        if (dil)
          inst.setDebugLoc(dil);
      }
    }
  }

};

} // namespace mcsema
