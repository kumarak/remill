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

#include <glog/logging.h>

#include <functional>
#include <ios>
#include <set>
#include <string>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#include <llvm/ADT/SmallVector.h>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Type.h>

#include <llvm/Support/raw_ostream.h>

#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include "remill/Arch/Arch.h"
#include "remill/Arch/Instruction.h"

#include "remill/BC/ABI.h"
#include "remill/BC/IntrinsicTable.h"
#include "remill/BC/Lifter.h"
#include "remill/BC/Util.h"

#include "remill/OS/OS.h"

namespace remill {
namespace {

// Try to find the function that implements this semantics.
llvm::Function *GetInstructionFunction(llvm::Module *module,
                                       const std::string &function) {
  std::stringstream ss;
  ss << "ISEL_" << function;
  auto isel_name = ss.str();

  auto isel = FindGlobaVariable(module, isel_name);
  if (!isel) {
    return nullptr;  // Falls back on `UNIMPLEMENTED_INSTRUCTION`.
  }

  if (!isel->isConstant() || !isel->hasInitializer()) {
    LOG(FATAL)
        << "Expected a `constexpr` variable as the function pointer for "
        << "instruction semantic function " << function
        << ": " << LLVMThingToString(isel);
  }

  auto sem = isel->getInitializer()->stripPointerCasts();
  return llvm::dyn_cast_or_null<llvm::Function>(sem);
}

}  // namespace

InstructionLifter::~InstructionLifter(void) {}

InstructionLifter::InstructionLifter(llvm::IntegerType *word_type_,
                                     const IntrinsicTable *intrinsics_)
    : word_type(word_type_),
      intrinsics(intrinsics_) {}

// Lift a single instruction into a basic block.
bool InstructionLifter::LiftIntoBlock(
    Instruction &arch_inst, llvm::BasicBlock *block) {

  llvm::Function *func = block->getParent();
  llvm::Module *module = func->getParent();
  llvm::Function *isel_func = nullptr;

  if (arch_inst.IsValid()) {
    isel_func = GetInstructionFunction(module, arch_inst.function);
  } else {
    DLOG(ERROR)
        << "Cannot decode instruction bytes at "
        << std::hex << arch_inst.pc;

    isel_func = GetInstructionFunction(module, "INVALID_INSTRUCTION");
    arch_inst.operands.clear();
    if (!isel_func) {
      LOG(ERROR)
          << "INVALID_INSTRUCTION doesn't exist.";
      return false;
    }
  }

  if (!isel_func) {
    DLOG(ERROR)
        << "Cannot lift instruction at " << std::hex << arch_inst.pc << ", "
        << arch_inst.function << " doesn't exist: " << arch_inst.Serialize();

    isel_func = GetInstructionFunction(module, "UNSUPPORTED_INSTRUCTION");
    if (!isel_func) {
      LOG(ERROR)
          << "UNSUPPORTED_INSTRUCTION doesn't exist; not using it in place of "
          << arch_inst.function;
      return false;
    }
    arch_inst.operands.clear();
  }

  llvm::IRBuilder<> ir(block);
  auto mem_ptr = LoadMemoryPointerRef(block);
  auto state_ptr = LoadStatePointer(block);
  auto pc_ptr = LoadProgramCounterRef(block);

  // Begin an atomic block.
  if (arch_inst.is_atomic_read_modify_write) {
    std::vector<llvm::Value *> args = {ir.CreateLoad(mem_ptr)};
    ir.CreateStore(
        ir.CreateCall(intrinsics->atomic_begin, args),
        mem_ptr);
  }

  std::vector<llvm::Value *> args;
  args.reserve(arch_inst.operands.size() + 2);

  // First two arguments to an instruction semantics function are the
  // state pointer, and a pointer to the memory pointer.
  args.push_back(nullptr);
  args.push_back(state_ptr);

  auto isel_func_type = isel_func->getFunctionType();
  auto arg_num = 2U;

  for (auto &op : arch_inst.operands) {
    CHECK(arg_num < isel_func_type->getNumParams())
        << "Function " << arch_inst.function << ", implemented by "
        << isel_func->getName().str() << ", should have at least "
        << arg_num << " arguments for instruction "
        << arch_inst.Serialize();

    auto arg = remill::NthArgument(isel_func, arg_num);
    auto arg_type = arg->getType();
    auto operand = LiftOperand(arch_inst, block, arg, op);
    arg_num += 1;
    auto op_type = operand->getType();
    CHECK(op_type == arg_type)
        << "Lifted operand " << op.Serialize() << " to "
        << arch_inst.function << " does not have the correct type. Expected "
        << LLVMThingToString(arg_type) << " but got "
        << LLVMThingToString(op_type) << ".";

    args.push_back(operand);
  }

  // Update the current program counter. Control-flow instructions may update
  // the program counter in the semantics code.
  ir.CreateStore(
      ir.CreateAdd(
          ir.CreateLoad(pc_ptr),
          llvm::ConstantInt::get(word_type, arch_inst.NumBytes())),
      pc_ptr);

  // Pass in current value of the memory pointer.
  args[0] = ir.CreateLoad(mem_ptr);

  // Call the function that implements the instruction semantics.
  ir.CreateStore(ir.CreateCall(isel_func, args), mem_ptr);

  // End an atomic block.
  if (arch_inst.is_atomic_read_modify_write) {
    std::vector<llvm::Value *> args = {ir.CreateLoad(mem_ptr)};
    ir.CreateStore(
        ir.CreateCall(intrinsics->atomic_end, args),
        mem_ptr);
  }

  return true;
}

namespace {

// Load the address of a register.
static llvm::Value *LoadRegAddress(llvm::BasicBlock *block,
                                   std::string reg_name) {
  return new llvm::LoadInst(
      FindVarInFunction(block->getParent(), reg_name), "", block);
}

// Load the value of a register.
static llvm::Value *LoadRegValue(llvm::BasicBlock *block,
                                 std::string reg_name) {
  return new llvm::LoadInst(LoadRegAddress(block, reg_name), "", block);
}

// Return a register value, or zero.
static llvm::Value *LoadWordRegValOrZero(llvm::BasicBlock *block,
                                         const std::string &reg_name,
                                         llvm::ConstantInt *zero) {
  if (reg_name.empty()) {
    return zero;
  }

  auto val = LoadRegValue(block, reg_name);
  auto val_type = llvm::dyn_cast_or_null<llvm::IntegerType>(val->getType());
  auto word_type = zero->getType();

  CHECK(val_type)
      << "Register " << reg_name << " expected to be an integer.";

  auto val_size = val_type->getBitWidth();
  auto word_size = word_type->getBitWidth();
  CHECK(val_size <= word_size)
      << "Register " << reg_name << " expected to be no larger than the "
      << "machine word size (" << word_type->getBitWidth() << " bits).";

  if (val_size < word_size) {
    val = new llvm::ZExtInst(val, word_type, "", block);
  }

  return val;
}

}  // namespace

llvm::Value *InstructionLifter::LiftShiftRegisterOperand(
    Instruction &inst, llvm::BasicBlock *block,
    llvm::Argument *arg, Operand &op) {

  llvm::Function *func = block->getParent();
  llvm::Module *module = func->getParent();
  auto &context = module->getContext();
  auto &arch_reg = op.shift_reg.reg;

  auto arg_type = arg->getType();
  CHECK(arg_type->isIntegerTy())
    << "Expected " << arch_reg.name << " to be an integral type "
    << "for instruction at " << std::hex << inst.pc;

  const llvm::DataLayout data_layout(module);
  auto reg = LoadRegValue(block, arch_reg.name);
  auto reg_type = reg->getType();
  auto reg_size = data_layout.getTypeAllocSizeInBits(reg_type);
  auto word_size = data_layout.getTypeAllocSizeInBits(word_type);
  auto op_type = llvm::Type::getIntNTy(context, op.size);

  const uint64_t zero = 0;
  const uint64_t one = 1;
  const uint64_t shift_size = op.shift_reg.shift_size;

  const auto shift_val = llvm::ConstantInt::get(op_type, shift_size);

  llvm::IRBuilder<> ir(block);

  auto curr_size = reg_size;
  if (Operand::ShiftRegister::kExtendInvalid != op.shift_reg.extend_op) {

    auto extract_type = llvm::Type::getIntNTy(
        context, op.shift_reg.extract_size);

    if (reg_size > op.shift_reg.extract_size) {
      curr_size = op.shift_reg.extract_size;
      reg = ir.CreateTrunc(reg, extract_type);

    } else {
      CHECK(reg_size == op.shift_reg.extract_size)
          << "Invalid extraction size. Can't extract "
          << op.shift_reg.extract_size << " bits from a " << reg_size
          << "-bit value in operand " << op.Serialize() << " of instruction at "
          << std::hex << inst.pc;
    }

    if (op.size > op.shift_reg.extract_size) {
      switch (op.shift_reg.extend_op) {
        case Operand::ShiftRegister::kExtendSigned:
          reg = ir.CreateSExt(reg, op_type);
          curr_size = op.size;
          break;
        case Operand::ShiftRegister::kExtendUnsigned:
          reg = ir.CreateZExt(reg, op_type);
          curr_size = op.size;
          break;
        default:
          LOG(FATAL)
              << "Invalid extend operation type for instruction at "
              << std::hex << inst.pc;
          break;
      }
    }
  }

  CHECK(curr_size <= op.size);

  if (curr_size < op.size) {
    reg = ir.CreateZExt(reg, op_type);
    curr_size = op.size;
  }

  if (Operand::ShiftRegister::kShiftInvalid != op.shift_reg.shift_op) {

    CHECK(shift_size < op.size)
        << "Shift of size " << shift_size
        << " is wider than the base register size in shift register in "
        << inst.Serialize();

    switch (op.shift_reg.shift_op) {
      // Left shift.
      case Operand::ShiftRegister::kShiftLeftWithZeroes:
        reg = ir.CreateShl(reg, shift_val);
        break;

      // Masking shift left.
      case Operand::ShiftRegister::kShiftLeftWithOnes: {
        const auto mask_val = llvm::ConstantInt::get(
            reg_type, ~((~zero) << shift_size));
        reg = ir.CreateOr(ir.CreateShl(reg, shift_val), mask_val);
        break;
      }

      // Logical right shift.
      case Operand::ShiftRegister::kShiftUnsignedRight:
        reg = ir.CreateLShr(reg, shift_val);
        break;

      // Arithmetic right shift.
      case Operand::ShiftRegister::kShiftSignedRight:
        reg = ir.CreateAShr(reg, shift_val);
        break;

      // Rotate left.
      case Operand::ShiftRegister::kShiftLeftAround: {
        const uint64_t shr_amount = (~shift_size + one) & (op.size - one);
        const auto shr_val = llvm::ConstantInt::get(op_type, shr_amount);
        const auto val1 = ir.CreateLShr(reg, shr_val);
        const auto val2 = ir.CreateShl(reg, shift_val);
        reg = ir.CreateOr(val1, val2);
        break;
      }

      // Rotate right.
      case Operand::ShiftRegister::kShiftRightAround: {
        const uint64_t shl_amount = (~shift_size + one) & (op.size - one);
        const auto shl_val = llvm::ConstantInt::get(op_type, shl_amount);
        const auto val1 = ir.CreateLShr(reg, shift_val);
        const auto val2 = ir.CreateShl(reg, shl_val);
        reg = ir.CreateOr(val1, val2);
        break;
      }

      case Operand::ShiftRegister::kShiftInvalid:
        break;
    }
  }

  if (word_size > op.size) {
    reg = ir.CreateZExt(reg, word_type);
  } else {
    CHECK(word_size == op.size)
        << "Final size of operand " << op.Serialize() << " is " << op.size
        << " bits, but address size is " << word_size;
  }

  return reg;
}

namespace {

static llvm::Type *IntendedArgumentType(llvm::Argument *arg) {
  for (auto user : arg->users()) {
    if (auto cast_inst = llvm::dyn_cast<llvm::IntToPtrInst>(user)) {
      return cast_inst->getType();
    }
  }
  return arg->getType();
}

static llvm::Value *ConvertToIntendedType(Instruction &inst, Operand &op,
                                          llvm::BasicBlock *block,
                                          llvm::Value *val,
                                          llvm::Type *intended_type) {
  auto val_type = val->getType();
  if (val->getType() == intended_type) {
    return val;
  } else if (val_type->isPointerTy()) {
    if (intended_type->isPointerTy()) {
      return new llvm::BitCastInst(val, intended_type, "", block);
    } else if (intended_type->isIntegerTy()) {
      return new llvm::PtrToIntInst(val, intended_type, "", block);
    }
  }

  LOG(FATAL)
      << "Unable to convert value " << LLVMThingToString(val)
      << " to intended argument type " << LLVMThingToString(intended_type)
      << " for operand " << op.Serialize() << " of instruction "
      << inst.Serialize();

  return nullptr;
}

}  // namespace

// Load a register operand. This deals uniformly with write- and read-operands
// for registers. In the case of write operands, the argument type is always
// a pointer. In the case of read operands, the argument type is sometimes
// a pointer (e.g. when passing a vector to an instruction semantics function).
llvm::Value *InstructionLifter::LiftRegisterOperand(
    Instruction &inst, llvm::BasicBlock *block,
    llvm::Argument *arg, Operand &op) {

  llvm::Function *func = block->getParent();
  llvm::Module *module = func->getParent();
  auto &arch_reg = op.reg;

  const auto real_arg_type = arg->getType();
  auto arg_type = real_arg_type;

  // LLVM on AArch64 converts things like `RnW<uint64_t>`, which is a struct
  // containing a `uint64_t *`, into a `uintptr_t` when they are being passed
  // as arguments.
  if (arg_type->isIntegerTy() && GetHostArch()->IsAArch64()) {
    arg_type = IntendedArgumentType(arg);
  }

  if (llvm::isa<llvm::PointerType>(arg_type)) {
    auto val = LoadRegAddress(block, arch_reg.name);
    return ConvertToIntendedType(inst, op, block, val, real_arg_type);

  } else {
    CHECK(arg_type->isIntegerTy() || arg_type->isFloatingPointTy())
        << "Expected " << arch_reg.name << " to be an integral or float type "
        << "for instruction at " << std::hex << inst.pc;

    auto val = LoadRegValue(block, arch_reg.name);

    const llvm::DataLayout data_layout(module);
    auto val_type = val->getType();
    auto val_size = data_layout.getTypeAllocSizeInBits(val_type);
    auto arg_size = data_layout.getTypeAllocSizeInBits(arg_type);
    auto word_size = data_layout.getTypeAllocSizeInBits(word_type);

    if (val_size < arg_size) {
      if (arg_type->isIntegerTy()) {
        CHECK(val_type->isIntegerTy())
            << "Expected " << arch_reg.name << " to be an integral type "
            << "for instruction at " << std::hex << inst.pc;

        CHECK(word_size == arg_size)
            << "Expected integer argument to be machine word size ("
            << word_size << " bits) but is is " << arg_size << " instead "
            << "in instruction at " << std::hex << inst.pc;

        val = new llvm::ZExtInst(val, word_type, "", block);

      } else if (arg_type->isFloatingPointTy()) {
        CHECK(val_type->isFloatingPointTy())
            << "Expected " << arch_reg.name << " to be a floating point type "
            << "for instruction at " << std::hex << inst.pc;

        val = new llvm::FPExtInst(val, arg_type, "", block);
      }

    } else if (val_size > arg_size) {
      if (arg_type->isIntegerTy()) {
        CHECK(val_type->isIntegerTy())
            << "Expected " << arch_reg.name << " to be an integral type "
            << "for instruction at " << std::hex << inst.pc;

        CHECK(word_size == arg_size)
            << "Expected integer argument to be machine word size ("
            << word_size << " bits) but is is " << arg_size << " instead "
            << "in instruction at " << std::hex << inst.pc;

        val = new llvm::TruncInst(val, arg_type, "", block);

      } else if (arg_type->isFloatingPointTy()) {
        CHECK(val_type->isFloatingPointTy())
            << "Expected " << arch_reg.name << " to be a floating point type "
            << "for instruction at " << std::hex << inst.pc;

        val = new llvm::FPTruncInst(val, arg_type, "", block);
      }
    }

    return ConvertToIntendedType(inst, op, block, val, real_arg_type);
  }
}

// Lift an immediate operand.
llvm::Value *InstructionLifter::LiftImmediateOperand(Instruction &inst,
                                                     llvm::BasicBlock *,
                                                     llvm::Argument *arg,
                                                     Operand &arch_op) {
  auto arg_type = arg->getType();
  if (arch_op.size > word_type->getBitWidth()) {
    CHECK(arg_type->isIntegerTy(static_cast<uint32_t>(arch_op.size)))
        << "Argument to semantics function for instruction at " << std::hex
        << inst.pc << " is not an integer. This may not be surprising because "
        << "the immediate operand is " << arch_op.size << " bits, but the "
        << "machine word size is " << word_type->getBitWidth() << " bits.";

    CHECK(arch_op.size <= 64)
        << "Decode error! Immediate operands can be at most 64 bits! "
        << "Operand structure encodes a truncated " << arch_op.size << " bit "
        << "value for instruction at " << std::hex << inst.pc;

    return llvm::ConstantInt::get(
        arg_type, arch_op.imm.val, arch_op.imm.is_signed);

  } else {
    CHECK(arg_type->isIntegerTy(word_type->getBitWidth()))
        << "Bad semantics function implementation for instruction at "
        << std::hex << inst.pc << ". Integer constants that are "
        << "smaller than the machine word size should be represented as "
        << "machine word sized arguments to semantics functions.";

    return llvm::ConstantInt::get(
        word_type, arch_op.imm.val, arch_op.imm.is_signed);
  }
}

// Zero-extend a value to be the machine word size.
llvm::Value *InstructionLifter::LiftAddressOperand(
    Instruction &inst, llvm::BasicBlock *block, llvm::Argument *, Operand &op) {
  auto &arch_addr = op.addr;
  auto zero = llvm::ConstantInt::get(word_type, 0, false);
  auto word_size = word_type->getBitWidth();

  CHECK(word_size >= arch_addr.base_reg.size)
      << "Memory base register " << arch_addr.base_reg.name
      << "for instruction at " << std::hex << inst.pc
      << " is wider than the machine word size.";

  CHECK(word_size >= arch_addr.index_reg.size)
      << "Memory index register " << arch_addr.base_reg.name
      << "for instruction at " << std::hex << inst.pc
      << " is wider than the machine word size.";

  auto addr = LoadWordRegValOrZero(block, arch_addr.base_reg.name, zero);
  auto index = LoadWordRegValOrZero(block, arch_addr.index_reg.name, zero);
  auto scale = llvm::ConstantInt::get(
      word_type, static_cast<uint64_t>(arch_addr.scale), true);
  auto segment = LoadWordRegValOrZero(
      block, arch_addr.segment_base_reg.name, zero);

  llvm::IRBuilder<> ir(block);

  if (zero != index) {
    addr = ir.CreateAdd(addr, ir.CreateMul(index, scale));
  }

  if (arch_addr.displacement) {
    if (0 < arch_addr.displacement) {
      addr = ir.CreateAdd(addr, llvm::ConstantInt::get(
          word_type, static_cast<uint64_t>(arch_addr.displacement)));
    } else {
      addr = ir.CreateSub(addr, llvm::ConstantInt::get(
          word_type, static_cast<uint64_t>(-arch_addr.displacement)));
    }
  }

  // Compute the segmented address.
  if (zero != segment) {
    addr = ir.CreateAdd(addr, segment);
  }

  // Memory address is smaller than the machine word size (e.g. 32-bit address
  // used in 64-bit).
  if (arch_addr.address_size < word_size) {
    auto addr_type = llvm::Type::getIntNTy(
        block->getContext(), static_cast<unsigned>(arch_addr.address_size));

    addr = ir.CreateZExt(
        ir.CreateTrunc(addr, addr_type),
        word_type);
  }

  return addr;
}

// Lift an operand for use by the instruction.
llvm::Value *InstructionLifter::LiftOperand(Instruction &inst,
                                            llvm::BasicBlock *block,
                                            llvm::Argument *arg,
                                            Operand &arch_op) {
  auto arg_type = arg->getType();
  switch (arch_op.type) {
    case Operand::kTypeInvalid:
      LOG(FATAL)
          << "Decode error! Cannot lift invalid operand.";
      return nullptr;

    case Operand::kTypeShiftRegister:
      CHECK(Operand::kActionRead == arch_op.action)
          << "Can't write to a shift register operand "
          << "for instruction at " << std::hex << inst.pc;

      return LiftShiftRegisterOperand(inst, block, arg, arch_op);

    case Operand::kTypeRegister:
      if (arch_op.size != arch_op.reg.size) {
        LOG(FATAL)
            << "Operand size and register size must match for register "
            << arch_op.reg.name << " in instruction " << inst.Serialize();
      }
      return LiftRegisterOperand(inst, block, arg, arch_op);

    case Operand::kTypeImmediate:
      return LiftImmediateOperand(inst, block, arg, arch_op);

    case Operand::kTypeAddress:
      if (arg_type != word_type) {
        LOG(FATAL)
            << "Expected that a memory operand should be represented by "
            << "machine word type. Argument type is "
            << LLVMThingToString(arg_type) << " and word type is "
            << LLVMThingToString(word_type) << " in instruction at "
            << std::hex << inst.pc;
      }

      return LiftAddressOperand(inst, block, arg, arch_op);
  }

  LOG(FATAL)
      << "Got a unknown operand type of " << static_cast<int>(arch_op.type)
      << " in instruction at " << std::hex << inst.pc;

  return nullptr;
}

}  // namespace remill
