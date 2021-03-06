/*
 * Copyright (c) 2017 Trail of Bits, Inc.
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
#ifndef REMILL_BC_COMPAT_RUNTIMEDYLD_H_
#define REMILL_BC_COMPAT_RUNTIMEDYLD_H_

#include "remill/BC/Version.h"

#include <llvm/ExecutionEngine/RuntimeDyld.h>

#if LLVM_VERSION_NUMBER < LLVM_VERSION(4, 0)
namespace llvm {
using JITSymbol = llvm::RuntimeDyld::SymbolInfo;
using JITSymbolResolver = llvm::RuntimeDyld::SymbolResolver;
using RTDyldMemoryManager = llvm::RuntimeDyld::MemoryManager;
}  // namespace llvm
#else
# include <llvm/ExecutionEngine/JITSymbol.h>
# include <llvm/ExecutionEngine/RTDyldMemoryManager.h>
#endif
#endif  // REMILL_BC_COMPAT_RUNTIMEDYLD_H_
