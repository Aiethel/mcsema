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

#include <mcsema/BC/Dbg.h>
#include <remill/BC/Annotate.h>

#include <llvm/IR/CFG.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/DIBuilder.h>

#include <llvm/Support/raw_ostream.h>

#include <cstdlib>
#include <iostream>
#include <fstream>
#include <queue>
#include <tuple>
#include <vector>
#include <sstream>

std::vector< std::string > split(std::string str)
{
  std::vector< std::string > out;
  std::istringstream ss{ std::move(str) };
  for(std::string token; std::getline(ss, token, ' ');)
    out.push_back(std::move(token));
  return out;
}

namespace mcsema
{

void DbgMetadata::AddDir(std::string filename, std::string dir_name) {

  auto node = _dib->createFile(filename, std::move(dir_name));
  _dirs.insert({ std::move(filename), node });

  if (!_cu) {
    _cu = _dib->createCompileUnit(llvm::dwarf::DW_LANG_C,
                                  node, "McSema", 0, "", 0);
  }

}

void DbgMetadata::AddEa(const std::string &filename, uint64_t ea, uint32_t line) {
  ea_to_line.insert({ ea, line });
}

void DbgMetadata::CreateEaMetadata(llvm::Function *func, uint64_t ea) {
  auto line = ea_to_line.find(ea);
  if (line == ea_to_line.end()) {
      return;
  }
  auto d_func = _func_to_dbg.find(func);
  if (d_func == _func_to_dbg.end()) {
      CreateDummyProgram(func);
  }
  auto scope = *_dirs.begin();
  auto node = llvm::DILocation::get(_m.getContext(), line->second, 0, _func_to_dbg[ func ]);
  ea_to_dbg.find(func)->second.insert({ ea, node });
}

llvm::DILocation *DbgMetadata::Fetch(llvm::Function *func, uint64_t ea) {
  if (!_func_to_dbg.count(func))
    return nullptr;

  auto ea_to_loc = ea_to_dbg.find(func)->second;
  if (!ea_to_loc.count(ea))
      CreateEaMetadata(func, ea);

  ea_to_loc = ea_to_dbg.find(func)->second;
  auto d_loc = ea_to_loc.find(ea);
  return (d_loc == ea_to_loc.end()) ? nullptr : d_loc->second;
}


llvm::DISubprogram *DbgMetadata::CreateDummyProgram(llvm::Function *func) {
  if (!remill::HasOriginType<remill::LiftedFunction>(func) ||
      func->getName().contains("_init"))
    return nullptr;

  static uint64_t counter = 4000;

  auto param_ref_arr = _dib->getOrCreateTypeArray({});
  auto s_type = _dib->createSubroutineType(param_ref_arr);

  auto file = _dirs.begin()->second;
  auto name = func->getName();
  auto d_program = _dib->createFunction(
      file,
      name,
      name,
      file,
      0,
      s_type,
      false,
      true,
      0
     );
  ea_to_dbg.insert({ func, {} });
  _func_to_dbg.insert({ func, d_program });
  return d_program;
}

void DbgMetadata::Parse(const std::string & filename) {
  if (filename.empty())
    return;


  std::ifstream in{ filename };
  for (std::string line; std::getline(in, line);)
  {
    auto words = split(std::move(line));
    if (words[ 0 ] == "Directory")
    {
      AddDir(words[ 1 ], std::move(words[ 2 ]));
      continue;
    }
    AddEa(
        words[ 0 ],
        std::strtoul(words[ 2 ].c_str(), nullptr, 16),
        std::strtoul(words[ 1 ].c_str(), nullptr, 10));
  }
}

void DbgMetadata::OneBlockAnnotate(llvm::Function *func,
                                   llvm::BasicBlock *bb,
                                   uint64_t ea) {
  auto dil_ea = Fetch(func, ea);
  for (auto &inst : *bb) {
    if (!inst.getDebugLoc())
      inst.setDebugLoc(dil_ea);
  }
}


void DbgMetadata::FillMissing(llvm::Module &m) {
  for (auto &func : m) {
    for (auto [s, e_lines] : s_ea_to_dbg) {
      if (func.getName().endswith(s)) {
        Propagate(func, e_lines).Run();
        break;
      }
    }
  }
}

llvm::DILocation *get_first_dil(llvm::BasicBlock &bb) {
  for (auto &inst : bb) {
    if (auto loc = inst.getDebugLoc()) {
      return loc;
    }
  }
  return nullptr;
}

void DbgMetadata::FillFunc(llvm::Function &func) {
  for (auto &bb : func) {
    llvm::DILocation *dil = nullptr;
    for (auto &inst : bb) {
      if (auto loc = inst.getDebugLoc()) {
        dil = loc;
      } else {
        if (dil)
          inst.setDebugLoc(dil);
      }
    }
  }
}


void DbgMetadata::Annotate(llvm::Function *func, uint64_t ea)
{
  auto dil_ea = Fetch(func, ea);
  _ctx.PropagateForward(dil_ea);
}

} // namespace mcsema
