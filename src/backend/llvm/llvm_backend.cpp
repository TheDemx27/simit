#include "llvm_backend.h"

#include <cstdint>
#include <iostream>
#include <stack>
#include <algorithm>

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/raw_ostream.h"

#if LLVM_MAJOR_VERSION <= 3 && LLVM_MINOR_VERSION <= 4
#include "llvm/Analysis/Verifier.h"
#else
#include "llvm/IR/Verifier.h"
#endif

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/ExecutionEngine/JIT.h"

#include "llvm/PassManager.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

#include "llvm_types.h"
#include "llvm_codegen.h"
#include "llvm_util.h"

#include "types.h"
#include "ir.h"
#include "ir_printer.h"
#include "ir_queries.h"
#include "ir_codegen.h"
#include "llvm_function.h"
#include "macros.h"
#include "runtime.h"
#include "util/collections.h"

using namespace std;
using namespace simit::ir;

namespace simit {
namespace backend {

typedef llvm::IRBuilder<true, llvm::ConstantFolder,
                        llvm::IRBuilderDefaultInserter<true>> LLVMIRBuilder;

// appease GCC
shared_ptr<llvm::EngineBuilder> createEngineBuilder(llvm::Module *module);

const std::string VAL_SUFFIX(".val");
const std::string PTR_SUFFIX(".ptr");
const std::string LEN_SUFFIX(".len");

// class LLVMBackend
bool LLVMBackend::llvmInitialized = false;

shared_ptr<llvm::EngineBuilder> createEngineBuilder(llvm::Module *module) {
  shared_ptr<llvm::EngineBuilder> engineBuilder(new llvm::EngineBuilder(module));
  return engineBuilder;
}

LLVMBackend::LLVMBackend() : builder(new LLVMIRBuilder(LLVM_CTX)) {
  if (!llvmInitialized) {
    llvm::InitializeNativeTarget();
    llvmInitialized = true;
  }
}

LLVMBackend::~LLVMBackend() {}

Function* LLVMBackend::compile(ir::Func func, vector<Var> globals) {
  this->module = new llvm::Module("simit", LLVM_CTX);

  iassert(func.getBody().defined()) << "cannot compile an undefined function";

  this->dataLayout.reset(new llvm::DataLayout(module));

  this->symtable.clear();
  this->buffers.clear();
  this->storage = Storage();
  this->globals = set<ir::Var>(globals.begin(), globals.end());

  // Add global variables to symbol table
  std::map<std::string,void**> globalPointers;
  for (auto& global : globals) {
    Type type = global.getType();
    llvm::Type* globalType = llvmType(global.getType(), globalAddrspace());
    llvm::Constant* initializer = defaultInitializer(globalType);
    llvm::GlobalVariable* globalPtr =
        new llvm::GlobalVariable(*module,
                                 globalType,
                                 false,
                                 llvm::GlobalValue::ExternalLinkage,
                                 initializer,
                                 global.getName(),
                                 nullptr,
                                 llvm::GlobalVariable::NotThreadLocal,
                                 globalAddrspace(),
                                 true);
    globalPtr->setAlignment(8);
    symtable.insert(global, globalPtr);
  }

  // Create compute functions
  vector<Func> callTree = getCallTree(func);
  std::reverse(callTree.begin(), callTree.end());

  llvm::Function *llvmFunc = nullptr;
  for (auto &f : callTree) {
    if (f.getKind() != Func::Internal) {
      continue;
    }
    iassert(f.getBody().defined());

    this->storage.add(f.getStorage());

    // Emit function
    symtable.scope();

    bool external = (f == func);
    llvmFunc = emitEmptyFunction(f.getName(), f.getArguments(),
                                 f.getResults(), external);

    // Add constants to symbol table
    for (auto &global : f.getEnvironment().globals) {
      symtable.insert(global.first, compile(global.second));
    }

    Stmt body = moveVarDeclsToFront(f.getBody());
    compile(body);
    builder->CreateRetVoid();

    symtable.unscope();
  }
  iassert(llvmFunc);


  // Declare malloc and free
  llvm::FunctionType *m =
      llvm::FunctionType::get(LLVM_INT8_PTR, {LLVM_INT}, false);
  llvm::Function *malloc =
      llvm::Function::Create(m, llvm::Function::ExternalLinkage, "malloc",
                             module);
  llvm::FunctionType *f =
      llvm::FunctionType::get(LLVM_VOID, {LLVM_INT8_PTR}, false);
  llvm::Function *free =
      llvm::Function::Create(f, llvm::Function::ExternalLinkage, "free",
                             module);


  // Create initialization function
  emitEmptyFunction(func.getName()+".init", func.getArguments(),
                    func.getResults(), true);
  for (auto &buffer : buffers) {
    Var var = buffer.first;
    llvm::Value *bufferVal = buffer.second;

    Type type = var.getType();
    llvm::Type *ltype = llvmType(type);

    iassert(type.isTensor());
    const TensorType *ttype = type.toTensor();
    llvm::Value *len = emitComputeLen(ttype, storage.get(var));
    unsigned compSize = ttype->componentType.bytes();
    llvm::Value *size = builder->CreateMul(len, llvmInt(compSize));
    llvm::Value *mem = builder->CreateCall(malloc, size);

    mem = builder->CreateCast(llvm::Instruction::CastOps::BitCast, mem, ltype);
    builder->CreateStore(mem, bufferVal);
  }
  builder->CreateRetVoid();
  symtable.clear();


  // Create de-initialization function
  emitEmptyFunction(func.getName()+".deinit", func.getArguments(),
                    func.getResults(), true);
  for (auto &buffer : buffers) {
    Var var = buffer.first;
    llvm::Value *bufferVal = buffer.second;

    llvm::Value *tmpPtr = builder->CreateLoad(bufferVal);
    tmpPtr = builder->CreateCast(llvm::Instruction::CastOps::BitCast,
                                 tmpPtr, LLVM_INT8_PTR);
    builder->CreateCall(free, tmpPtr);
  }
  builder->CreateRetVoid();
  symtable.clear();

  iassert(!llvm::verifyModule(*module))
      << "LLVM module does not pass verification";

  auto engineBuilder = createEngineBuilder(module);

#ifndef SIMIT_DEBUG
  // Run LLVM optimization passes on the function
  // We use the built-in PassManagerBuilder to build
  // the set of passes that are similar to clang's -O3
  llvm::FunctionPassManager fpm(module);
  llvm::PassManager mpm;
  llvm::PassManagerBuilder pmBuilder;
  
  pmBuilder.OptLevel = 3;

  pmBuilder.BBVectorize = 1;
  pmBuilder.LoopVectorize = 1;
//  pmBuilder.LoadCombine = 1;
  pmBuilder.SLPVectorize = 1;

  llvm::DataLayout dataLayout(module);
#if LLVM_MAJOR_VERSION >= 3 && LLVM_MINOR_VERSION >= 5
  fpm.add(new llvm::DataLayoutPass(dataLayout));
#else
  fpm.add(new llvm::DataLayout(dataLayout));
#endif

  pmBuilder.populateFunctionPassManager(fpm);
  pmBuilder.populateModulePassManager(mpm);

  fpm.doInitialization();
  fpm.run(*llvmFunc);
  fpm.doFinalization();
  
  mpm.run(*module);
#endif

  return new LLVMFunction(func, globals, llvmFunc, module, engineBuilder);
}

void LLVMBackend::compile(const ir::Literal& literal) {
  iassert(literal.type.isTensor()) << "Only tensor literals supported for now";
  const TensorType *type = literal.type.toTensor();

  if (type->order() == 0) {
    ScalarType ctype = type->componentType;
    switch (ctype.kind) {
      case ScalarType::Int: {
        iassert(ctype.bytes() == 4) << "Only 4-byte ints currently supported";
        val = llvmInt(((int*)literal.data)[0]);
        break;
      }
      case ScalarType::Float: {
        iassert(ctype.bytes() == ScalarType::floatBytes)
            << "Only " << ScalarType::floatBytes
            << "-byte float mode allowed by current float setting";
        val = llvmFP(literal.getFloatVal(0));
        break;
      }
      case ScalarType::Boolean: {
        iassert(ctype.bytes() == sizeof(bool));
        bool data = ((bool*)literal.data)[0];
        val = llvm::ConstantInt::get(LLVM_BOOL, llvm::APInt(1, data, false));
        break;
      }
      default: unreachable;
    }
  }
  else {
    // TODO: This should become a reference to a global literal
    // (unify with GPUBackend).
    val = llvmPtr(literal);
  }
  iassert(val);
}

void LLVMBackend::compile(const ir::VarExpr& varExpr) {
  if (!symtable.contains(varExpr.var)) {
    ierror << varExpr.var << " not found in symbol table";
  }

  val = symtable.get(varExpr.var);

  string ptrName = string(val->getName());
  string valName = string(val->getName()) + VAL_SUFFIX;

  // Globals are stored as pointer-pointers so we must load them
  if (util::contains(globals, varExpr.var)) {
    val = builder->CreateLoad(val, ptrName);
  }

  // Special case: check if the symbol is a scalar and the llvm value is a ptr,
  // in which case we must load the value.  This case arises because we keep
  // many scalars on the stack.  An exceptions to this are loop variables,
  // which is why we can't assume Simit scalars are always kept on the stack.
  if (isScalar(varExpr.type) && val->getType()->isPointerTy()) {
    val = builder->CreateLoad(val, valName);
  }
}

void LLVMBackend::compile(const ir::Load& load) {
  llvm::Value *buffer = compile(load.buffer);
  llvm::Value *index = compile(load.index);

  string locName = string(buffer->getName()) + PTR_SUFFIX;
  llvm::Value *bufferLoc = builder->CreateInBoundsGEP(buffer, index, locName);

  string valName = string(buffer->getName()) + VAL_SUFFIX;
  val = builder->CreateLoad(bufferLoc, valName);
}

void LLVMBackend::compile(const ir::FieldRead& fieldRead) {
  val = emitFieldRead(fieldRead.elementOrSet, fieldRead.fieldName);
}

// TODO: Get rid of Call expressions. This code is out of date, w.r.t CallStmt,
//       and is only kept around to emit loc.
void LLVMBackend::compile(const ir::Call& call) {
  std::map<Func, llvm::Intrinsic::ID> llvmIntrinsicByName =
                                  {{ir::Intrinsics::sin,llvm::Intrinsic::sin},
                                   {ir::Intrinsics::cos,llvm::Intrinsic::cos},
                                   {ir::Intrinsics::sqrt,llvm::Intrinsic::sqrt},
                                   {ir::Intrinsics::log,llvm::Intrinsic::log},
                                   {ir::Intrinsics::exp,llvm::Intrinsic::exp},
                                   {ir::Intrinsics::pow,llvm::Intrinsic::pow}};
  
  std::vector<llvm::Type*> argTypes;
  std::vector<llvm::Value*> args;
  llvm::Function *fun = nullptr;
  
  // compile arguments first
  for (auto a: call.actuals) {
    //FIX: remove once solve() is no longer needed
    //iassert(isScalar(a.type()));
    argTypes.push_back(llvmType(a.type().toTensor()->componentType));
    args.push_back(compile(a));
  }

  // these are intrinsic functions
  // first, see if this is an LLVM intrinsic
  auto foundIntrinsic = llvmIntrinsicByName.find(call.func);
  if (foundIntrinsic != llvmIntrinsicByName.end()) {
    fun = llvm::Intrinsic::getDeclaration(module, foundIntrinsic->second,
                                          argTypes);
  }
  // now check if it is an intrinsic from libm
  else if (call.func == ir::Intrinsics::atan2 ||
           call.func == ir::Intrinsics::tan   ||
           call.func == ir::Intrinsics::asin  ||
           call.func == ir::Intrinsics::acos    ) {
    auto ftype = llvm::FunctionType::get(llvmFloatType(), argTypes, false);
    std::string funcName = call.func.getName() +
        (ir::ScalarType::singleFloat() ? "_f32" : "_f64");
    fun= llvm::cast<llvm::Function>(module->getOrInsertFunction(
        funcName,ftype));
  }
  else if (call.func == ir::Intrinsics::norm) {
    iassert(args.size() == 1);
    auto type = call.actuals[0].type().toTensor();
    vector<IndexDomain> dimensions = type->getDimensions();

    // special case for vec3f
    if (dimensions[0].getSize() == 3) {
      llvm::Value *x = args[0];

      llvm::Value *x0 = loadFromArray(x, llvmInt(0));
      llvm::Value *sum = builder->CreateFMul(x0, x0);

      llvm::Value *x1 = loadFromArray(x, llvmInt(1));
      llvm::Value *x1pow = builder->CreateFMul(x1, x1);
      sum = builder->CreateFAdd(sum, x1pow);

      llvm::Value *x2 = loadFromArray(x, llvmInt(2));
      llvm::Value *x2pow = builder->CreateFMul(x2, x2);
      sum = builder->CreateFAdd(sum, x2pow);

      llvm::Function *sqrt =
          llvm::Intrinsic::getDeclaration(module, llvm::Intrinsic::sqrt,
                                          {llvmFloatType()});
      val = builder->CreateCall(sqrt, sum);
    } else {
      args.push_back(emitComputeLen(dimensions[0]));
      std::string funcName = ir::ScalarType::singleFloat() ?
          "norm_f32" : "norm_f64";
      val = emitCall(funcName, args, llvmFloatType());
    }
    
    return;
  }
  else if (call.func == ir::Intrinsics::solve) {
    // FIX: compile is making these be LLVM_DOUBLE, but I need
    // LLVM_DOUBLEPTR
    std::vector<llvm::Type*> argTypes2 =
        {llvmFloatPtrType(), llvmFloatPtrType(), llvmFloatPtrType(),
         LLVM_INT, LLVM_INT};

    auto type = call.actuals[0].type().toTensor();
    vector<IndexDomain> dimensions = type->getDimensions();
    args.push_back(emitComputeLen(dimensions[0]));
    args.push_back(emitComputeLen(dimensions[1]));

    auto ftype = llvm::FunctionType::get(llvmFloatType(), argTypes2, false);
    std::string funcName = ir::ScalarType::singleFloat() ?
        "cMatSolve_f32" : "cMatSolve_f64";
    fun = llvm::cast<llvm::Function>(
        module->getOrInsertFunction(funcName, ftype));
  }
  else if (call.func == ir::Intrinsics::loc) {
    val = emitCall("loc", args, LLVM_INT);
    return;
  }
  else if (call.func == ir::Intrinsics::dot) {
    // we need to add the vector length to the args
    auto type1 = call.actuals[0].type().toTensor();
    auto type2 = call.actuals[1].type().toTensor();
    vector<IndexDomain> type1Dimensions = type1->getDimensions();
    vector<IndexDomain> type2Dimensions = type2->getDimensions();

    uassert(type1Dimensions[0] == type2Dimensions[0]) <<
        "dimension mismatch in dot product";
    args.push_back(emitComputeLen(type1Dimensions[0]));
    std::string funcName = ir::ScalarType::singleFloat() ?
        "dot_f32" : "dot_f64";
    val = emitCall(funcName, args, llvmFloatType());
    return;
  }
  // if not an intrinsic function, try to find it in the module
  else if (module->getFunction(call.func.getName())) {
    fun = module->getFunction(call.func.getName());
  }
  else {
    not_supported_yet << "Unsupported function call";
  }
  iassert(fun);

  val = builder->CreateCall(fun, args);
}


void LLVMBackend::compile(const ir::Length& length) {
  val = emitComputeLen(length.indexSet);
}

void LLVMBackend::compile(const ir::IndexRead& indexRead) {
  // TODO: Add support for different indices (contained in the Set type).
  unsigned int indexLoc = 1 + indexRead.kind;

  iassert(indexRead.edgeSet.type().isSet());
  iassert(indexRead.edgeSet.type().toSet()->endpointSets.size() > 0);

  llvm::Value *edgesValue = compile(indexRead.edgeSet);
  val = builder->CreateExtractValue(edgesValue,{indexLoc},util::toString(indexRead));
}

void LLVMBackend::compile(const ir::TensorIndexRead& op) {
  std::cout << op << std::endl;

//  not_supported_yet;
}

void LLVMBackend::compile(const ir::Neg& negExpr) {
  iassert(isScalar(negExpr.type));
  llvm::Value *a = compile(negExpr.a);

  switch (negExpr.type.toTensor()->componentType.kind) {
    case ScalarType::Int:
      val = builder->CreateNeg(a);
      break;
    case ScalarType::Float:
      val = builder->CreateFNeg(a);
      break;
    case ScalarType::Boolean:
      iassert(false) << "Cannot negate a boolean value.";
  }
}

void LLVMBackend::compile(const ir::Add& addExpr) {
  iassert(isScalar(addExpr.type));

  llvm::Value *a = compile(addExpr.a);
  llvm::Value *b = compile(addExpr.b);

  switch (addExpr.type.toTensor()->componentType.kind) {
    case ScalarType::Int:
      val = builder->CreateAdd(a, b);
      break;
    case ScalarType::Float:
      val = builder->CreateFAdd(a, b);
      break;
    case ScalarType::Boolean:
      ierror << "Cannot add boolean values.";
      break;
  }
}

void LLVMBackend::compile(const ir::Sub& subExpr) {
  iassert(isScalar(subExpr.type));

  llvm::Value *a = compile(subExpr.a);
  llvm::Value *b = compile(subExpr.b);

  switch (subExpr.type.toTensor()->componentType.kind) {
    case ScalarType::Int:
      val = builder->CreateSub(a, b);
      break;
    case ScalarType::Float:
      val = builder->CreateFSub(a, b);
      break;
    case ScalarType::Boolean:
      iassert(false) << "Cannot subtract boolean values.";
  }
}

void LLVMBackend::compile(const ir::Mul& mulExpr) {
  iassert(isScalar(mulExpr.type));

  llvm::Value *a = compile(mulExpr.a);
  llvm::Value *b = compile(mulExpr.b);

  switch (mulExpr.type.toTensor()->componentType.kind) {
    case ScalarType::Int:
      val = builder->CreateMul(a, b);
      break;
    case ScalarType::Float:
      val = builder->CreateFMul(a, b);
      break;
    case ScalarType::Boolean:
      iassert(false) << "Cannot multiply boolean values.";
  }
}

void LLVMBackend::compile(const ir::Div& divExpr) {
  iassert(isScalar(divExpr.type));

  llvm::Value *a = compile(divExpr.a);
  llvm::Value *b = compile(divExpr.b);

  switch (divExpr.type.toTensor()->componentType.kind) {
    case ScalarType::Int:
      // TODO: Figure out what's the deal with integer div. Cast to fp, div and
      // truncate?
      not_supported_yet;
      break;
    case ScalarType::Float:
      val = builder->CreateFDiv(a, b);
      break;
    case ScalarType::Boolean:
      iassert(false) << "Cannot divide boolean values.";
  }
}

void LLVMBackend::compile(const ir::Not& notExpr) {
  iassert(isBoolean(notExpr.type));
  iassert(isBoolean(notExpr.a.type()));

  llvm::Value *a = compile(notExpr.a);

  val = builder->CreateNot(a);
}

#define LLVMBACKEND_VISIT_COMPARE_OP(Type, op, float_cmp, int_cmp)             \
void LLVMBackend::compile(Type op) {                                           \
  iassert(isBoolean(op.type));                                                 \
  iassert(isScalar(op.a.type()));                                              \
  iassert(isScalar(op.b.type()));                                              \
                                                                               \
  llvm::Value *a = compile(op.a);                                              \
  llvm::Value *b = compile(op.b);                                              \
                                                                               \
  const TensorType *ttype = op.a.type().toTensor();                            \
  if (ttype->componentType == ScalarType::Float) {                             \
    val = builder->float_cmp(a, b);                                            \
  } else {                                                                     \
    val = builder->int_cmp(a, b);                                              \
  }                                                                            \
}

LLVMBACKEND_VISIT_COMPARE_OP(const Eq&, op, CreateFCmpOEQ, CreateICmpEQ)
LLVMBACKEND_VISIT_COMPARE_OP(const Ne&, op, CreateFCmpONE, CreateICmpNE)
LLVMBACKEND_VISIT_COMPARE_OP(const Gt&, op, CreateFCmpOGT, CreateICmpSGT)
LLVMBACKEND_VISIT_COMPARE_OP(const Lt&, op, CreateFCmpOLT, CreateICmpSLT)
LLVMBACKEND_VISIT_COMPARE_OP(const Ge&, op, CreateFCmpOGE, CreateICmpSGE)
LLVMBACKEND_VISIT_COMPARE_OP(const Le&, op, CreateFCmpOLE, CreateICmpSLE)

void LLVMBackend::compile(const ir::And& andExpr) {
  iassert(isBoolean(andExpr.type));
  iassert(isBoolean(andExpr.a.type()));
  iassert(isBoolean(andExpr.b.type()));

  llvm::Value *a = compile(andExpr.a);
  llvm::Value *b = compile(andExpr.b);

  val = builder->CreateAnd(a, b);
}

void LLVMBackend::compile(const ir::Or& orExpr) {
  iassert(isBoolean(orExpr.type));
  iassert(isBoolean(orExpr.a.type()));
  iassert(isBoolean(orExpr.b.type()));

  llvm::Value *a = compile(orExpr.a);
  llvm::Value *b = compile(orExpr.b);

  val = builder->CreateOr(a, b);
}

void LLVMBackend::compile(const ir::Xor& xorExpr) {
  iassert(isBoolean(xorExpr.type));
  iassert(isBoolean(xorExpr.a.type()));
  iassert(isBoolean(xorExpr.b.type()));

  llvm::Value *a = compile(xorExpr.a);
  llvm::Value *b = compile(xorExpr.b);

  val = builder->CreateXor(a, b);
}

void LLVMBackend::compile(const ir::VarDecl& varDecl) {
  tassert(varDecl.var.getType().isTensor()) << "Only tensor decls supported";

  Var var = varDecl.var;
  llvm::Value *llvmVar = nullptr;
  if (isScalar(var.getType())) {
    ScalarType type = var.getType().toTensor()->componentType;
    llvmVar = builder->CreateAlloca(llvmType(type),nullptr,var.getName());
  }
  else {
    if (!storage.get(var).needsInitialization()) {
      return;
    }
    llvmVar = makeGlobalTensor(varDecl.var);
  }
  iassert(llvmVar);
  symtable.insert(var, llvmVar);
}

void LLVMBackend::compile(const ir::AssignStmt& assignStmt) {
  switch (assignStmt.cop) {
    case ir::CompoundOperator::None: {
      emitAssign(assignStmt.var, assignStmt.value);
      return;
    }
    case ir::CompoundOperator::Add: {
      emitAssign(assignStmt.var, Add::make(assignStmt.var, assignStmt.value));
      return;
    }
    default: ierror << "Unknown compound operator type";
  }
}

void LLVMBackend::compile(const ir::CallStmt& callStmt) {
  std::map<Func, llvm::Intrinsic::ID> llvmIntrinsicByName =
                                  {{ir::Intrinsics::sin,llvm::Intrinsic::sin},
                                   {ir::Intrinsics::cos,llvm::Intrinsic::cos},
                                   {ir::Intrinsics::sqrt,llvm::Intrinsic::sqrt},
                                   {ir::Intrinsics::log,llvm::Intrinsic::log},
                                   {ir::Intrinsics::exp,llvm::Intrinsic::exp},
                                   {ir::Intrinsics::pow,llvm::Intrinsic::pow}};
  
  std::vector<llvm::Type*> argTypes;
  std::vector<llvm::Value*> args;
  llvm::Function *fun = nullptr;
  llvm::Value *call = nullptr;

  // compile arguments first
  for (auto a: callStmt.actuals) {
    argTypes.push_back(llvmType(a.type().toTensor()->componentType));
    args.push_back(compile(a));
  }

  Func callee = callStmt.callee;
  if (callee.getKind() == Func::Intrinsic) {
    iassert(callee != ir::Intrinsics::norm && callee != ir::Intrinsics::dot)
        << "norm and dot should have been lowered";

    std::string floatTypeName = ir::ScalarType::singleFloat() ? "_f32" : "_f64";

    // first, see if this is an LLVM intrinsic
    auto foundIntrinsic = llvmIntrinsicByName.find(callStmt.callee);
    if (foundIntrinsic != llvmIntrinsicByName.end()) {
      iassert(callStmt.results.size() == 1);
      auto ctype = callStmt.results[0].getType().toTensor()->componentType;
      llvm::Type *overloadType = llvmType(ctype);
      fun = llvm::Intrinsic::getDeclaration(module, foundIntrinsic->second,
                                            {overloadType});
      call = builder->CreateCall(fun, args);
    }
    // now check if it is an intrinsic from libm
    else if (callStmt.callee == ir::Intrinsics::atan2 ||
             callStmt.callee == ir::Intrinsics::tan   ||
             callStmt.callee == ir::Intrinsics::asin  ||
             callStmt.callee == ir::Intrinsics::acos    ) {
      std::string fname = callStmt.callee.getName() + floatTypeName;
      call = emitCall(fname, args, llvmFloatType());
    }
    else if (callStmt.callee == ir::Intrinsics::mod) {
      iassert(callStmt.actuals.size() == 2) << "mod takes two inputs, got"
                                       << callStmt.actuals.size();
      call = builder->CreateSRem(compile(callStmt.actuals[0]),
                                 compile(callStmt.actuals[1]));

    }
    else if (callee == ir::Intrinsics::det) {
      iassert(args.size() == 1);
      std::string fname = callStmt.callee.getName() + "3" + floatTypeName;
      call = emitCall(fname, args, llvmFloatType());
    }
    else if (callee == ir::Intrinsics::inv) {
      iassert(args.size() == 1);

      Var result = callStmt.results[0];
      llvm::Value *llvmResult = symtable.get(result);
      args.push_back(llvmResult);

      std::string fname = callStmt.callee.getName() + "3" + floatTypeName;
      call = emitCall(fname, args);
    }
    else if (callStmt.callee == ir::Intrinsics::solve) {
    
      // we need to add additional arguments: the row_start and col_idx pointers,
      // as well as the number of rows, columns, nonzeros, and blocksize.
      auto type = callStmt.actuals[0].type().toTensor();
      vector<IndexDomain> dimensions = type->getDimensions();
      
      // FIXME: shouldn't assume this is a var expression...
      tassert(isa<VarExpr>(callStmt.actuals[0]));
      auto tensorStorage = storage.get(to<VarExpr>(callStmt.actuals[0])->var);
      llvm::Value *targetSet = compile(tensorStorage.getSystemTargetSet());
      llvm::Value *storageSet = compile(tensorStorage.getSystemStorageSet());

      // Retrieve the size of the neighbor index, which is stored in the last
      // element of neighbor start index.
      llvm::Value *setSize =
          builder->CreateExtractValue(storageSet, {0},
                                      storageSet->getName()+LEN_SUFFIX);
      
      llvm::Value *row_start =
          builder->CreateExtractValue(targetSet, {2}, "row_start");
      llvm::Value *col_idx =
          builder->CreateExtractValue(targetSet, {3}, "col_idx");
    
      
      
      llvm::Value *neighborIndexSizeLoc =
          builder->CreateInBoundsGEP(row_start, setSize,
                                     "neighbors"+LEN_SUFFIX+PTR_SUFFIX);
      llvm::Value *len = builder->CreateAlignedLoad(neighborIndexSizeLoc, 8,
                                       "neighbors"+LEN_SUFFIX);
      llvm::Value *blockSize_r;
      llvm::Value *blockSize_c;
      
      // Determine block sizes
      Type blockType = type->getBlockType();
      vector<IndexDomain> blockDimensions =
          blockType.toTensor()->getDimensions();
      if (!isScalar(blockType)) {
        // TODO: The following assumes all blocks are dense row major. The right
        //       way to assign a storage order for every block in the tensor
        //       represented by a TensorStorage.  Also assumes 2D blocks.
        blockSize_r = emitComputeLen(blockDimensions[0]);
        blockSize_c = emitComputeLen(blockDimensions[1]);
      }
      else {
        blockSize_r = llvmInt(1);
        blockSize_c = llvmInt(1);
      }
      args.push_back(row_start);
      args.push_back(col_idx);
      args.push_back(emitComputeLen(dimensions[0]));
      args.push_back(emitComputeLen(dimensions[1]));
      args.push_back(len);
      args.push_back(blockSize_r);
      args.push_back(blockSize_c);

      std::string fname = "cMatSolve" + floatTypeName;
      call = emitCall(fname, args);
    }
    else if (callStmt.callee == ir::Intrinsics::loc) {
      call = emitCall("loc", args, LLVM_INT);
    }
    else {
      ierror << "intrinsic " << callStmt.callee.getName() << " not found";
    }
    iassert(call);

    if (!call->getType()->isVoidTy()) {
      iassert(callStmt.results.size() == 1);
      Var var = callStmt.results[0];
      llvm::Value *llvmVar = symtable.get(var);
      builder->CreateStore(call, llvmVar);
    }
  }
  // If not an intrinsic function, try to find it in the module
  else {
    if (module->getFunction(callStmt.callee.getName())) {
      for (Var r : callStmt.results) {
        argTypes.push_back(llvmType(r.getType().toTensor()->componentType));

        llvm::Value *llvmResult = symtable.get(r);
        args.push_back(llvmResult);
      }
      fun = module->getFunction(callStmt.callee.getName());
      call = builder->CreateCall(fun, args);
    }
    else {
      ierror << "function " << callStmt.callee.getName() << " not found in module";
    }
  }
}

void LLVMBackend::compile(const ir::Store& store) {
  llvm::Value *buffer = compile(store.buffer);
  llvm::Value *index = compile(store.index);
  llvm::Value *value;
  switch (store.cop) {
    case CompoundOperator::None: {
      value = compile(store.value);
      break;
    }
    case CompoundOperator::Add: {
      value = compile(Add::make(Load::make(store.buffer, store.index),
                                store.value));
      break;
    }
  }
  iassert(value != nullptr);

  string locName = string(buffer->getName()) + PTR_SUFFIX;
  llvm::Value *bufferLoc = builder->CreateInBoundsGEP(buffer, index, locName);
  builder->CreateStore(value, bufferLoc);
}

void LLVMBackend::compile(const ir::FieldWrite& fieldWrite) {
  /// \todo field writes of scalars to tensors and tensors to tensors should be
  ///       handled by the lowering so that we only write scalars to scalars
  ///       in the backend
//  iassert(isScalar(op.value.type()))
//      << "assignment non-scalars should have been lowered by now";

  iassert(fieldWrite.value.type().isTensor());
  iassert(getFieldType(fieldWrite.elementOrSet,
                       fieldWrite.fieldName).isTensor());
  iassert(fieldWrite.elementOrSet.type().isSet() ||
          fieldWrite.elementOrSet.type().isElement());

  Type fieldType = getFieldType(fieldWrite.elementOrSet, fieldWrite.fieldName);
  Type valueType = fieldWrite.value.type();

  // Assigning a scalar to an n-order tensor
  if (fieldType.toTensor()->order() > 0 && valueType.toTensor()->order() == 0) {
    iassert(fieldWrite.cop == CompoundOperator::None)
        << "Compound write when assigning scalar to n-order tensor";
    if (isa<Literal>(fieldWrite.value) &&
        to<Literal>(fieldWrite.value)->getFloatVal(0) == 0.0) {
      // emit memset 0
      llvm::Value *fieldPtr = emitFieldRead(fieldWrite.elementOrSet, fieldWrite.fieldName);

      const TensorType *tensorFieldType = fieldType.toTensor();

      // For now we'll assume fields are always dense row major
      llvm::Value *fieldLen = emitComputeLen(tensorFieldType, TensorStorage::DenseRowMajor);
      unsigned compSize = tensorFieldType->componentType.bytes();
      llvm::Value *fieldSize = builder->CreateMul(fieldLen,llvmInt(compSize));

      emitMemSet(fieldPtr, llvmInt(0,8), fieldSize, compSize);
    }
    else {
      not_supported_yet;
    }
  }
  else {
    // emit memcpy
    llvm::Value *fieldPtr = emitFieldRead(fieldWrite.elementOrSet, fieldWrite.fieldName);
    llvm::Value *valuePtr;
    switch (fieldWrite.cop) {
      case ir::CompoundOperator::None: {
        valuePtr = compile(fieldWrite.value);
        break;
      }
      case ir::CompoundOperator::Add: {
        valuePtr = compile(Add::make(
            FieldRead::make(fieldWrite.elementOrSet, fieldWrite.fieldName), fieldWrite.value));
        break;
      }
    }
    iassert(valuePtr != nullptr);

    const TensorType *tensorFieldType = fieldType.toTensor();

    // For now we'll assume fields are always dense row major
    llvm::Value *fieldLen = emitComputeLen(tensorFieldType, TensorStorage::DenseRowMajor);
    unsigned elemSize = tensorFieldType->componentType.bytes();
    llvm::Value *fieldSize = builder->CreateMul(fieldLen, llvmInt(elemSize));

    emitMemCpy(fieldPtr, valuePtr, fieldSize, elemSize);
  }
}

void LLVMBackend::compile(const ir::Block& block) {
  if (block.scoped) {
    symtable.scope();
  }
  compile(block.first);
  if (block.rest.defined()) {
    compile(block.rest);
  }
  if (block.scoped) {
    symtable.unscope();
  }
}

void LLVMBackend::compile(const ir::IfThenElse& ifThenElse) {
  llvm::Function *llvmFunc = builder->GetInsertBlock()->getParent();

  llvm::Value *cond = compile(ifThenElse.condition);
  llvm::Value *condEval = builder->CreateICmpEQ(builder->getTrue(), cond);


  llvm::BasicBlock *thenBlock = llvm::BasicBlock::Create(LLVM_CTX, "then",
                                                         llvmFunc);
  llvm::BasicBlock *elseBlock = llvm::BasicBlock::Create(LLVM_CTX, "else");
  llvm::BasicBlock *exitBlock = llvm::BasicBlock::Create(LLVM_CTX, "exit");
  builder->CreateCondBr(condEval, thenBlock, elseBlock);

  builder->SetInsertPoint(thenBlock);
  compile(ifThenElse.thenBody);
  builder->CreateBr(exitBlock);
  thenBlock = builder->GetInsertBlock();

  llvmFunc->getBasicBlockList().push_back(elseBlock);

  builder->SetInsertPoint(elseBlock);
  compile(ifThenElse.elseBody);
  builder->CreateBr(exitBlock);
  elseBlock = builder->GetInsertBlock();

  llvmFunc->getBasicBlockList().push_back(exitBlock);
  builder->SetInsertPoint(exitBlock);
}

void LLVMBackend::compile(const ir::ForRange& forLoop) {
  std::string iName = forLoop.var.getName();
  
  llvm::Function *llvmFunc = builder->GetInsertBlock()->getParent();
  
  // Loop Header
  llvm::BasicBlock *entryBlock = builder->GetInsertBlock();

  llvm::Value *rangeStart = compile(forLoop.start);
  llvm::Value *rangeEnd = compile(forLoop.end);

  llvm::BasicBlock *loopBodyStart =
    llvm::BasicBlock::Create(LLVM_CTX, iName+"_loop_body", llvmFunc);
  llvm::BasicBlock *loopEnd = llvm::BasicBlock::Create(LLVM_CTX,
                                                       iName+"_loop_end",
                                                       llvmFunc);
  llvm::Value *firstCmp = builder->CreateICmpSLT(rangeStart, rangeEnd);
  builder->CreateCondBr(firstCmp, loopBodyStart, loopEnd);
  builder->SetInsertPoint(loopBodyStart);

  llvm::PHINode *i = builder->CreatePHI(LLVM_INT32, 2, iName);
  i->addIncoming(rangeStart, entryBlock);

  // Loop Body
  symtable.scope();
  symtable.insert(forLoop.var, i);
  compile(forLoop.body);
  symtable.unscope();

  // Loop Footer
  llvm::BasicBlock *loopBodyEnd = builder->GetInsertBlock();
  llvm::Value *i_nxt = builder->CreateAdd(i, builder->getInt32(1),
                                          iName+"_nxt", false, true);
  i->addIncoming(i_nxt, loopBodyEnd);

  llvm::Value *exitCond = builder->CreateICmpSLT(i_nxt, rangeEnd,
                                                 iName+"_cmp");
  builder->CreateCondBr(exitCond, loopBodyStart, loopEnd);
  builder->SetInsertPoint(loopEnd);

}

void LLVMBackend::compile(const ir::For& forLoop) {
  std::string iName = forLoop.var.getName();
  ForDomain domain = forLoop.domain;

  llvm::Value *iNum = nullptr;
  switch (domain.kind) {
    case ForDomain::IndexSet: {
      iNum = emitComputeLen(domain.indexSet);
      break;
    }
    case ForDomain::Endpoints:
      not_supported_yet;
      break;
    case ForDomain::Edges:
      not_supported_yet;
      break;
    case ForDomain::NeighborsOf:
    case ForDomain::Neighbors:
      not_supported_yet;
      break;
    case ForDomain::Diagonal:
      not_supported_yet;
      break;
  }
  iassert(iNum);

  llvm::Function *llvmFunc = builder->GetInsertBlock()->getParent();

  // Loop Header
  llvm::BasicBlock *entryBlock = builder->GetInsertBlock();

  llvm::BasicBlock *loopBodyStart =
      llvm::BasicBlock::Create(LLVM_CTX, iName+"_loop_body", llvmFunc);
  llvm::BasicBlock *loopEnd = llvm::BasicBlock::Create(LLVM_CTX,
                                                       iName+"_loop_end", llvmFunc);
  llvm::Value *firstCmp = builder->CreateICmpSLT(llvmInt(0), iNum);
  builder->CreateCondBr(firstCmp, loopBodyStart, loopEnd);
  builder->SetInsertPoint(loopBodyStart);

  llvm::PHINode *i = builder->CreatePHI(LLVM_INT32, 2, iName);
  i->addIncoming(builder->getInt32(0), entryBlock);

  // Loop Body
  symtable.scope();
  symtable.insert(forLoop.var, i);
  compile(forLoop.body);
  symtable.unscope();

  // Loop Footer
  llvm::BasicBlock *loopBodyEnd = builder->GetInsertBlock();
  llvm::Value *i_nxt = builder->CreateAdd(i, builder->getInt32(1),
                                          iName+"_nxt", false, true);
  i->addIncoming(i_nxt, loopBodyEnd);

  llvm::Value *exitCond = builder->CreateICmpSLT(i_nxt, iNum, iName+"_cmp");
  builder->CreateCondBr(exitCond, loopBodyStart, loopEnd);
  builder->SetInsertPoint(loopEnd);
}

void LLVMBackend::compile(const ir::While& whileLoop) {
  llvm::Function *llvmFunc = builder->GetInsertBlock()->getParent();

  llvm::Value *cond = compile(whileLoop.condition);
  llvm::Value *condEval = builder->CreateICmpEQ(builder->getTrue(), cond);


  llvm::BasicBlock *bodyBlock = llvm::BasicBlock::Create(LLVM_CTX, "body",
                                                         llvmFunc);
  llvm::BasicBlock *checkBlock = llvm::BasicBlock::Create(LLVM_CTX,"check");
  llvm::BasicBlock *exitBlock = llvm::BasicBlock::Create(LLVM_CTX, "exit");
  builder->CreateCondBr(condEval, bodyBlock, exitBlock);

  builder->SetInsertPoint(bodyBlock);
  compile(whileLoop.body);
  builder->CreateBr(checkBlock);
  
  // We actually need to save the original body block, because the
  // current block could be different (e.g. if another loop was added
  // as part of the while's body).
  auto priorBodyBlock = bodyBlock;
  bodyBlock = builder->GetInsertBlock();
  
  llvmFunc->getBasicBlockList().push_back(checkBlock);
  builder->SetInsertPoint(checkBlock);
  llvm::Value *cond2 = compile(whileLoop.condition);
  llvm::Value *condEval2 = builder->CreateICmpEQ(builder->getTrue(), cond2);

  builder->CreateCondBr(condEval2, priorBodyBlock, exitBlock);
  
  llvmFunc->getBasicBlockList().push_back(exitBlock);
  builder->SetInsertPoint(exitBlock);
}

void LLVMBackend::compile(const ir::Print& print) {
  llvm::Value *result = compile(print.expr);
  Type type = print.expr.type();

  switch (type.kind()) {
  case Type::Kind::Tensor: {
    const TensorType *tensor = type.toTensor();
    vector<IndexDomain> dimensions = tensor->getDimensions();

    ScalarType scalarType = tensor->componentType;
    size_t order = tensor->order();
    std::string format;
    std::vector<llvm::Value*> args;
    std::string specifier = (scalarType.kind == ScalarType::Float? "%f" : "%d");

    if (order == 0) {
      iassert(dimensions.size() == 0);
      format = specifier + "\n";
      args.push_back(result);
    } else  {
      for (const IndexDomain &id : dimensions) {
        for (const IndexSet &is : id.getIndexSets()) {
          if (is.getKind() == IndexSet::Kind::Set) {

            llvm::Function *llvmFunc = builder->GetInsertBlock()->getParent();
            llvm::BasicBlock *entryBlock = builder->GetInsertBlock();
            llvm::Value *rangeStart = llvmInt(0);
            llvm::Value *rangeEnd = builder->CreateSub(
                  emitComputeLen(tensor, TensorStorage::DenseRowMajor), llvmInt(1));

            llvm::BasicBlock *loopBodyStart =
              llvm::BasicBlock::Create(LLVM_CTX, "", llvmFunc);

            builder->CreateBr(loopBodyStart);
            builder->SetInsertPoint(loopBodyStart);

            llvm::PHINode *i = builder->CreatePHI(LLVM_INT32, 2);
            i->addIncoming(rangeStart, entryBlock);

            llvm::Value *entry = loadFromArray(result, i);
            emitPrintf(specifier + " ", {entry});

            llvm::BasicBlock *loopBodyEnd = builder->GetInsertBlock();
            llvm::Value *iNext = builder->CreateAdd(i, llvmInt(1));
            i->addIncoming(iNext, loopBodyEnd);

            llvm::Value *exitCond = builder->CreateICmpSLT(iNext, rangeEnd);
            llvm::BasicBlock *loopEnd =
                llvm::BasicBlock::Create(LLVM_CTX, "", llvmFunc);
            builder->CreateCondBr(exitCond, loopBodyStart, loopEnd);
            builder->SetInsertPoint(loopEnd);

            emitPrintf(specifier + "\n", {loadFromArray(result, iNext)});
            return;
          }
        }
      }

      if (order == 1) {
        iassert(dimensions.size() == 1);
        std::string delim = (tensor->isColumnVector ? "\n" : " ");
        size_t size = tensor->size();
        for (size_t i = 0; i < size; i++) {
          llvm::Value *index = llvmInt(i);
          llvm::Value *element = loadFromArray(result, index);
          format += specifier + delim;
          args.push_back(element);
        }
        format.back() = '\n';
      } else {
        iassert(dimensions.size() >= 2);
        size_t size = tensor->size();
        if (size % dimensions.back().getSize()) {
          not_supported_yet << "\nNot a rectangular tensor (total entries not a"
                            << "multiple of entries per row)";
        }

        for (int i = 0; i < dimensions.back().getSize(); i++) {
          format += specifier + " ";
        }
        format.back() = '\n';

        size_t numlines = size / dimensions.back().getSize();
        std::vector<std::string> formatLines(numlines, format);

        size_t stride = 1;
        for (size_t i = dimensions.size() - 2; i > 0; i--) {
          stride *= dimensions[i].getSize();
          for (size_t j = stride - 1; j < formatLines.size(); j += stride) {
            formatLines[j].push_back('\n');
          }
        }
        stride *= dimensions[0].getSize();
        for (size_t j = stride - 1; j < formatLines.size(); j += stride) {
          formatLines[j].push_back('\n');
        }
        formatLines.back().resize(formatLines.back().find_last_not_of("\n") + 2);

        size_t charCount = 1;
        for (string &str : formatLines) {
          charCount += str.length();
        }
        format.clear();
        format.reserve(charCount);
        for (string &str : formatLines) {
          format += str;
        }

        for (size_t i = 0; i < size; i++) {
          llvm::Value *index = llvmInt(i);
          llvm::Value *element = loadFromArray(result, index);
          args.push_back(element);
        }
        format.back() = '\n';
      }
    }
    emitPrintf(format, args);
  }
    return;
  case Type::Kind::Element:
  case Type::Kind::Set:
  case Type::Kind::Tuple:
    not_supported_yet;
  default:
    unreachable << "Unknown Type";
  }
}


// helper methods
llvm::Function *LLVMBackend::getBuiltIn(std::string name,
                                        llvm::Type *retTy,
                                        std::vector<llvm::Type*> argTys) {
  llvm::FunctionType *funcTy;
  if (argTys.size() > 0) {
    funcTy = llvm::FunctionType::get(retTy, argTys, false);
  }
  else {
    funcTy = llvm::FunctionType::get(retTy, false);
  }
  module->getOrInsertFunction(name, funcTy);
  return module->getFunction(name);
}

llvm::Value *LLVMBackend::emitFieldRead(const Expr &elemOrSet,
                                        std::string fieldName) {
  assert(elemOrSet.type().isElement() || elemOrSet.type().isSet());
  const ElementType *elemType = nullptr;
  int fieldsOffset = -1;
  if (elemOrSet.type().isElement()) {
    elemType = elemOrSet.type().toElement();
    fieldsOffset = 0;
  }
  else {
    const SetType *setType = elemOrSet.type().toSet();
    elemType = setType->elementType.toElement();
    fieldsOffset = 1; // jump over set size
    if (setType->endpointSets.size() > 0) {
      fieldsOffset += NUM_EDGE_INDEX_ELEMENTS; // jump over index pointers
    }
  }
  assert(fieldsOffset >= 0);

  llvm::Value *setOrElemValue = compile(elemOrSet);

  assert(elemType->hasField(fieldName));
  unsigned fieldLoc = fieldsOffset + elemType->fieldNames.at(fieldName);
  return builder->CreateExtractValue(setOrElemValue, {fieldLoc},
                                     setOrElemValue->getName()+"."+fieldName);
}

llvm::Value *LLVMBackend::emitComputeLen(const TensorType *tensorType,
                                         const TensorStorage &tensorStorage) {
  if (tensorType->order() == 0) {
    return llvmInt(1);
  }

  vector<IndexDomain> dimensions = tensorType->getDimensions();

  llvm::Value *len = nullptr;
  switch (tensorStorage.getKind()) {
    case TensorStorage::DenseRowMajor: {
      auto it = dimensions.begin();
      len = emitComputeLen(*it++);
      for (; it != dimensions.end(); ++it) {
        len = builder->CreateMul(len, emitComputeLen(*it));
      }
      break;
    }
    case TensorStorage::SystemReduced: {
      llvm::Value *targetSet = compile(tensorStorage.getSystemTargetSet());
      llvm::Value *storageSet = compile(tensorStorage.getSystemStorageSet());

      // Retrieve the size of the neighbor index, which is stored in the last
      // element of neighbor start index.
      llvm::Value *setSize =
          builder->CreateExtractValue(storageSet, {0},
                                      storageSet->getName()+LEN_SUFFIX);
      llvm::Value *neighborStartIndex =
          builder->CreateExtractValue(targetSet, {2}, "neighbors.start");
      llvm::Value *neighborIndexSizeLoc =
          builder->CreateInBoundsGEP(neighborStartIndex, setSize,
                                     "neighbors"+LEN_SUFFIX+PTR_SUFFIX);
      len = builder->CreateAlignedLoad(neighborIndexSizeLoc, 8,
                                       "neighbors"+LEN_SUFFIX);

      // Multiply by block size
      Type blockType = tensorType->getBlockType();
      if (!isScalar(blockType)) {
        // TODO: The following assumes all blocks are dense row major. The right
        //       way to assign a storage order for every block in the tensor
        //       represented by a TensorStorage
        llvm::Value *blockSize =
            emitComputeLen(blockType.toTensor(), TensorStorage::DenseRowMajor);
        len = builder->CreateMul(len, blockSize);
      }
      break;
    }
    case TensorStorage::SystemDiagonal: {
      iassert(dimensions.size() > 0);

      // Just need one outer dimensions because diagonal
      len = emitComputeLen(tensorType->getOuterDimensions()[0]);

      Type blockType = tensorType->getBlockType();
      llvm::Value *blockLen = emitComputeLen(blockType.toTensor(),
                                             TensorStorage::DenseRowMajor);
      len = builder->CreateMul(len, blockLen);
      break;
    }
    case TensorStorage::SystemNone:
      ierror << "Attempting to compute size of tensor without storage";
      break;
    case TensorStorage::Undefined:
      ierror << "Attempting to compute size of tensor with undefined storage";
      break;
  }
  iassert(len != nullptr);
  return len;
}

llvm::Value *LLVMBackend::emitComputeLen(const IndexDomain &dom) {
  assert(dom.getIndexSets().size() > 0);

  auto it = dom.getIndexSets().begin();
  llvm::Value *result = emitComputeLen(*it++);
  for (; it != dom.getIndexSets().end(); ++it) {
    result = builder->CreateMul(result, emitComputeLen(*it));
  }
  return result;
}

llvm::Value *LLVMBackend::emitComputeLen(const IndexSet &is) {
  switch (is.getKind()) {
    case IndexSet::Range:
      return llvmInt(is.getSize());
      break;
    case IndexSet::Set: {
      llvm::Value *setValue = compile(is.getSet());
      return builder->CreateExtractValue(setValue, {0},
                                         setValue->getName()+LEN_SUFFIX);
    }
    case IndexSet::Single:
      unreachable;
    case IndexSet::Dynamic:
      not_supported_yet;
      break;
  }
  unreachable;
  return nullptr;
}

llvm::Value *LLVMBackend::loadFromArray(llvm::Value *array, llvm::Value *index){
  llvm::Value *loc = builder->CreateGEP(array, index);
  return builder->CreateLoad(loc);
}

llvm::Value *LLVMBackend::emitCall(string name, vector<llvm::Value*> args) {
  return emitCall(name, args, LLVM_VOID);
}

llvm::Value *LLVMBackend::emitCall(string name, vector<llvm::Value*> args,
                                   llvm::Type *returnType) {
  std::vector<llvm::Type*> argTypes;
  for (auto &arg : args) {
    argTypes.push_back(arg->getType());
  }

  llvm::FunctionType *ftype =
      llvm::FunctionType::get(returnType, argTypes, false);
  llvm::Function *fun =
      llvm::cast<llvm::Function>(module->getOrInsertFunction(name, ftype));

  iassert(fun != nullptr)
      << "could not find" << fun << "with the given signature";

  return builder->CreateCall(fun, std::vector<llvm::Value*>(args));
}

llvm::Constant *LLVMBackend::emitGlobalString(const std::string& str) {
  auto strValue = llvm::ConstantDataArray::getString(LLVM_CTX, str);
  auto strType = llvm::ArrayType::get(LLVM_INT8, str.size()+1);

  llvm::GlobalVariable *strGlobal =
      new llvm::GlobalVariable(*module, strType, true,
                               llvm::GlobalValue::PrivateLinkage, strValue,
                               "_str");
  llvm::Constant *zero = llvm::Constant::getNullValue(LLVM_INT);

  std::vector<llvm::Constant*> idx;
  idx.push_back(zero);
  idx.push_back(zero);
  return llvm::ConstantExpr::getGetElementPtr(strGlobal, idx);
}

llvm::Function *LLVMBackend::emitEmptyFunction(const string &name,
                                               const vector<ir::Var> &arguments,
                                               const vector<ir::Var> &results,
                                               bool externalLinkage,
                                               bool doesNotThrow,
                                               bool scalarsByValue) {
  llvm::Function *llvmFunc = createPrototype(name, arguments, results, module,
                                             externalLinkage, doesNotThrow,
                                             scalarsByValue);
  auto entry = llvm::BasicBlock::Create(LLVM_CTX, "entry", llvmFunc);
  builder->SetInsertPoint(entry);

  iassert(llvmFunc->getArgumentList().size() == arguments.size()+results.size())
      << "Number of arguments to llvm func does not match simit func arguments";

  // Add arguments and results to symbol table
  auto llvmArgIt = llvmFunc->getArgumentList().begin();
  auto simitArgIt = arguments.begin();
  for (; simitArgIt < arguments.end(); ++simitArgIt, ++llvmArgIt) {
    symtable.insert(*simitArgIt, llvmArgIt);
  }

  auto simitResIt = results.begin();
  for (; simitResIt < results.end(); ++simitResIt, ++llvmArgIt) {
    symtable.insert(*simitResIt, llvmArgIt);
  }

  return llvmFunc;
}

void LLVMBackend::emitPrintf(std::string format,
                             std::vector<llvm::Value*> args) {
  llvm::Function *printfFunc = module->getFunction("printf");
  if (printfFunc == nullptr) {
    std::vector<llvm::Type*> printfArgTypes;
    printfArgTypes.push_back(llvm::Type::getInt8PtrTy(LLVM_CTX));
    llvm::FunctionType* printfType = llvm::FunctionType::get(LLVM_INT,
                                                             printfArgTypes,
                                                             true);
    printfFunc = llvm::Function::Create(printfType,
                                        llvm::Function::ExternalLinkage,
                                        llvm::Twine("printf"), module);
    printfFunc->setCallingConv(llvm::CallingConv::C);
  }

  llvm::Value *str = emitGlobalString(format);

  std::vector<llvm::Value*> printfArgs;
  for (size_t i = 0; i < args.size(); i++) {
    // printf requires float varargs be promoted to doubles!
    if (args[i]->getType()->isFloatTy()) {
      args[i] = builder->CreateFPCast(args[i], LLVM_DOUBLE);
    }
  }
  printfArgs.push_back(str);
  printfArgs.insert(printfArgs.end(), args.begin(), args.end());

  builder->CreateCall(printfFunc, printfArgs);
}

void LLVMBackend::emitAssign(Var var, const Expr& value) {
  /// \todo assignment of scalars to tensors and tensors to tensors should be
  ///       handled by the lowering so that we only assign scalars to scalars
  ///       in the backend. Probably requires copy and memset intrinsics.
//  iassert(isScalar(value.type()) &&
//         "assignment non-scalars should have been lowered by now");
  llvm::Value *valuePtr = compile(value);

  iassert(var.getType().isTensor() && value.type().isTensor());
  std::string varName = var.getName();
  iassert(symtable.contains(var)) << var << " has not been declared in:\n"
                                  << var << " = " << value << ";";

  llvm::Value *varPtr = symtable.get(var);
  iassert(varPtr->getType()->isPointerTy());

  // Globals are stored as pointer-pointers so we must load them
  if (util::contains(globals, var)) {
    varPtr = builder->CreateLoad(varPtr, var.getName());
  }

  const TensorType *varType = var.getType().toTensor();
  const TensorType *valType = value.type().toTensor();

  // Assigning a scalar to a scalar
  if (varType->order() == 0 && valType->order() == 0) {
    builder->CreateStore(valuePtr, varPtr);
    valuePtr->setName(varName + VAL_SUFFIX);
  }
  // Assign to n-order tensors
  else {
    llvm::Value *len = emitComputeLen(varType, storage.get(var));
    unsigned componentSize = varType->componentType.bytes();
    llvm::Value *size = builder->CreateMul(len, llvmInt(componentSize));

    // Assigning a scalar to an n-order tensor
    if (varType->order() > 0 && valType->order() == 0) {
      // Assigning 0 to a tensor (memset)
      if (isa<Literal>(value) && (to<Literal>(value)->getFloatVal(0) == 0.0 ||
                                  ((int*)to<Literal>(value)->data)[0] == 0  )) {
        emitMemSet(varPtr, llvmInt(0,8), size, componentSize);
      }
      // Assigning general scalar to a tensor
      else {
        not_supported_yet << "you can only currently assign a scalar to a"
                          << "tensor if the scalar is 0.";
      }
    }
    // Assign tensor to conforming tensor
    else {
      iassert(var.getType() == value.type())
          << "variable and value types don't match";
      emitMemCpy(varPtr, valuePtr, size, componentSize);
    }
  }
}

void LLVMBackend::emitMemCpy(llvm::Value *dst, llvm::Value *src,
                             llvm::Value *size, unsigned align) {
  builder->CreateMemCpy(dst, src, size, align);
}

void LLVMBackend::emitMemSet(llvm::Value *dst, llvm::Value *val,
                             llvm::Value *size, unsigned align) {
  builder->CreateMemSet(dst, val, size, align);
}

llvm::Value *LLVMBackend::makeGlobalTensor(ir::Var var) {
  // Allocate buffer for local variable in global storage.
  // TODO: We should allocate small local dense tensors on the stack
  iassert(var.getType().isTensor());
  llvm::Type *ctype = llvmType(var.getType().toTensor()->componentType);
  llvm::PointerType *globalType = llvm::PointerType::get(ctype, globalAddrspace());

  llvm::GlobalVariable* buffer =
      new llvm::GlobalVariable(*module, globalType,
                               false, llvm::GlobalValue::ExternalLinkage,
                               llvm::ConstantPointerNull::get(globalType),
                               var.getName(), nullptr,
                               llvm::GlobalVariable::NotThreadLocal,
                               globalAddrspace());
  buffer->setAlignment(8);
  buffers.insert(pair<Var, llvm::Value*>(var, buffer));

  // Add load to symtable
  return builder->CreateLoad(buffer, buffer->getName());
}

}}
