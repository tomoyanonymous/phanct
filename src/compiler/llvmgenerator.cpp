#include "compiler/llvmgenerator.hpp"

namespace mimium {
// LLVMGenerator::LLVMGenerator(std::string filename, bool i_isjit)
//     : isjit(i_isjit),
//       taskfn_typeid(0),
//       tasktype_list(),
//       mainentry(nullptr),
//       currentblock(nullptr) {
//   init(filename);
// }
LLVMGenerator::LLVMGenerator(llvm::LLVMContext& ctx)
    : isjit(true),
      taskfn_typeid(0),
      tasktype_list(),
      ctx(ctx),
      mainentry(nullptr),
      currentblock(nullptr) {
  builder = std::make_unique<llvm::IRBuilder<>>(ctx);
}
void LLVMGenerator::init(std::string filename) {
  module = std::make_unique<llvm::Module>(filename, ctx);
  // module->setDataLayout(LLVMGetDefaultTargetTriple());
}
void LLVMGenerator::setDataLayout() {
  module->setDataLayout(llvm::sys::getProcessTriple());
}
void LLVMGenerator::setDataLayout(const llvm::DataLayout& dl) {
  module->setDataLayout(dl);
}

void LLVMGenerator::reset(std::string filename) {
  dropAllReferences();
  init(filename);
}

void LLVMGenerator::initJit() {}

// LLVMGenerator::LLVMGenerator(llvm::LLVMContext& _ctx,std::string _filename){
//     // ctx.reset();
//     // ctx = std::move(&_ctx);
// }

LLVMGenerator::~LLVMGenerator() { dropAllReferences(); }
void LLVMGenerator::dropAllReferences() {
  namemap.clear();
  if(module!=nullptr){
  module->dropAllReferences();
  }
}
// auto LLVMGenerator::getRawStructType(types::Struct& type) -> llvm::Type* {
//   auto ttype = static_cast<types::Tuple>(type);
//   return getRawTupleType(ttype);
// }
// auto LLVMGenerator::getRawTupleType(types::Tuple& type) -> llvm::Type* {
//   llvm::Type* res;
//   // prevent from type duplication(more efficient impl?)
//   auto search = std::find_if(structtype_map.begin(), structtype_map.end(),
//                              [&](auto& s2) { return s2.second == type; });
//   if (search == structtype_map.end()) {
//     std::vector<llvm::Type*> field;
//     for (auto& a : type.arg_types) {
//       // TODO(Tomoya): need to identify case of free variable(pointer) and user
//       // defined structs
//       field.push_back(getType(a));
//     }
//     auto newname = "fvtype." + std::to_string(struct_index++);
//     res = llvm::StructType::create(ctx, field, newname);
//     structtype_map.emplace(newname, type);
//   } else {
//     res = module->getTypeByName(search->first);
//   }
//   if (res == nullptr) {
//     throw std::runtime_error("could not find struct type");
//   }
//   return res;
// }
auto LLVMGenerator::getType(types::Value& type) -> llvm::Type* {
  return std::visit(TypeConverter(*builder, *module), type);
}

llvm::Function* LLVMGenerator::getForeignFunction(std::string name) {
  auto& [type, targetname] = LLVMBuiltin::ftable.find(name)->second;
  auto fnc = module->getOrInsertFunction(
      name, llvm::cast<llvm::FunctionType>(getType(type)));
  auto* fn = llvm::cast<llvm::Function>(fnc.getCallee());
  fn->setCallingConv(llvm::CallingConv::C);
  return fn;
}

void LLVMGenerator::setBB(llvm::BasicBlock* newblock) {
  builder->SetInsertPoint(newblock);
}
void LLVMGenerator::createMiscDeclarations() {
  // create malloc
  auto* malloctype = llvm::FunctionType::get(builder->getInt8PtrTy(),
                                             {builder->getInt64Ty()}, false);
  auto res = module->getOrInsertFunction("malloc", malloctype).getCallee();
  namemap.emplace("malloc", res);
}

// Create mimium_main() function it returns address of closure object for dsp()
// function if it exists.

void LLVMGenerator::createMainFun() {
  auto* fntype = llvm::FunctionType::get(builder->getInt8PtrTy(), false);
  auto* mainfun = llvm::Function::Create(
      fntype, llvm::Function::ExternalLinkage, "mimium_main", module.get());
  mainfun->setCallingConv(llvm::CallingConv::C);
  using Akind = llvm::Attribute;
  std::vector<llvm::Attribute::AttrKind> attrs = {
      Akind::NoUnwind, Akind::NoInline, Akind::OptimizeNone};
  llvm::AttributeSet aset;
  for (auto& a : attrs) {
    aset = aset.addAttribute(ctx, a);
  }

  mainfun->addAttributes(llvm::AttributeList::FunctionIndex, aset);
  mainentry = llvm::BasicBlock::Create(ctx, "entry", mainfun);
}
void LLVMGenerator::createTaskRegister(bool isclosure = false) {
  std::vector<llvm::Type*> argtypes = {
      builder->getDoubleTy(),   // time
      builder->getInt8PtrTy(),  // address to function
      builder->getDoubleTy()    // argument(single)
  };
  std::string name = "addTask";
  if (isclosure) {
    argtypes.push_back(builder->getInt8PtrTy());
    name = "addTask_cls";
  }  // address to closure args(instead of void* type)
  auto* fntype = llvm::FunctionType::get(builder->getVoidTy(), argtypes, false);
  auto addtask = module->getOrInsertFunction(name, fntype);
  auto addtaskfun = llvm::cast<llvm::Function>(addtask.getCallee());

  addtaskfun->setCallingConv(llvm::CallingConv::C);
  using Akind = llvm::Attribute;
  std::vector<llvm::Attribute::AttrKind> attrs = {
      Akind::NoUnwind, Akind::NoInline, Akind::OptimizeNone};
  llvm::AttributeSet aset;
  for (auto& a : attrs) {
    aset = aset.addAttribute(ctx, a);
  }
  addtaskfun->addAttributes(llvm::AttributeList::FunctionIndex, aset);
  typemap.emplace(name, fntype);
  namemap.emplace(name, addtask.getCallee());
}

llvm::Type* LLVMGenerator::getOrCreateTimeStruct(types::Time& t) {
  llvm::StringRef name = types::toString(t);
  llvm::Type* res = module->getTypeByName(name);
  if (res == nullptr) {
    llvm::Type* containtype = std::visit(
        overloaded{[&](types::Float& /*f*/) { return builder->getDoubleTy(); },
                   [&](auto& /*v*/) { return builder->getVoidTy(); }},
        t.val);

    res = llvm::StructType::create(ctx, {builder->getDoubleTy(), containtype},
                                   name);
  }
  return res;
}
void LLVMGenerator::preprocess() {
  createMiscDeclarations();
  createTaskRegister(true);   // for non-closure function
  createTaskRegister(false);  // for closure
  createMainFun();
  setBB(mainentry);
}

void LLVMGenerator::generateCode(std::shared_ptr<MIRblock> mir) {
  preprocess();
  for (auto& inst : mir->instructions) {
    visitInstructions(inst, true);
  }
  if (mainentry->getTerminator() == nullptr) {  // insert return
    auto dspaddress = namemap.find("dsp_cap");
    if (dspaddress != namemap.end()) {
      builder->CreateRet(dspaddress->second);
    } else {
      builder->CreateRet(
          llvm::ConstantPointerNull::get(builder->getInt8PtrTy()));
    }
  }
}

// Creates Allocation instruction or call malloc function depends on context
llvm::Value* LLVMGenerator::createAllocation(bool isglobal, llvm::Type* type,
                                             llvm::Value* ArraySize = nullptr,
                                             const llvm::Twine& name = "") {
  llvm::Value* res = nullptr;
  if (isglobal) {
    auto rawname ="ptr_" + name.str() + "_raw";
    auto size = module->getDataLayout().getTypeAllocSize(type);
    auto sizeinst = llvm::ConstantInt::get(ctx, llvm::APInt(64, size, false));
    auto rawres = builder->CreateCall(module->getFunction("malloc"), {sizeinst},
                                      rawname);
    res = builder->CreatePointerCast(rawres, llvm::PointerType::get(type, 0),
                                     "ptr_" + name);
    namemap.try_emplace(rawname,rawres);
  } else {
    res = builder->CreateAlloca(type, ArraySize, "ptr_" + name);
  }
  return res;
};
// Create StoreInst if storing to already allocated value
bool LLVMGenerator::createStoreOw(std::string varname,
                                  llvm::Value* val_to_store) {
  bool res = false;
  auto ptrname = "ptr_" + varname;
  auto it = namemap.find(varname);
  auto it2 = namemap.find(ptrname);
  if (it != namemap.cend() && it2 != namemap.cend()) {
    builder->CreateStore(val_to_store, namemap[ptrname]);
    res = true;
  }
  return res;
}
void LLVMGenerator::createAddTaskFn(const std::shared_ptr<FcallInst> i,
                                    const bool isclosure, const bool isglobal) {
  auto tv = namemap[i->args[0]];
  auto timeval = builder->CreateExtractValue(tv, 0);
  auto val = builder->CreateExtractValue(tv, 1);
  auto targetfn = llvm::cast<llvm::Function>(namemap[i->fname]);
  auto ptrtofn =
      llvm::ConstantExpr::getBitCast(targetfn, builder->getInt8PtrTy());
  // auto taskid = taskfn_typeid++;
  tasktype_list.emplace_back(i->type);

  std::vector<llvm::Value*> args = {timeval, ptrtofn, val};
  llvm::Value* addtask_fn;
  if (isclosure) {
    llvm::Value* clsptr =
        (isglobal) ? namemap[i->fname + "_cap"] : namemap["clsarg_" + i->fname];

    auto* upcasted = builder->CreateBitCast(clsptr, builder->getInt8PtrTy());
    namemap.emplace(i->fname + "_cls_i8", upcasted);
    args.push_back(upcasted);
    addtask_fn = namemap["addTask_cls"];
  } else {
    addtask_fn = namemap["addTask"];
  }
  auto* res = builder->CreateCall(addtask_fn, args);
  namemap.emplace(i->lv_name, res);
}
void LLVMGenerator::createFcall(std::shared_ptr<FcallInst> i,
                                std::vector<llvm::Value*>& args) {
  llvm::Function* fun;
  if (LLVMBuiltin::isBuiltin(i->fname)) {
    auto it = LLVMBuiltin::ftable.find(i->fname);
    BuiltinFnInfo& fninfo = it->second;
    auto* fntype = llvm::cast<llvm::FunctionType>(getType(fninfo.mmmtype));
    auto fn = module->getOrInsertFunction(fninfo.target_fnname, fntype);
    fun = llvm::cast<llvm::Function>(fn.getCallee());
    fun->setCallingConv(llvm::CallingConv::C);

  } else {
    fun = module->getFunction(i->fname);
    if (fun == nullptr) {
      throw std::logic_error("function could not be referenced");
    }
  }

  if (std::holds_alternative<types::Void>(i->type)) {
    builder->CreateCall(fun, args);
  } else {
    auto res = builder->CreateCall(fun, args, i->lv_name);
    namemap.emplace(i->lv_name, res);
  }
}
void LLVMGenerator::visitInstructions(const Instructions& inst, bool isglobal) {
  std::visit(
      overloaded{
          [](auto i) {},
          [&, this](const std::shared_ptr<AllocaInst>& i) {
            auto ptrname = "ptr_" + i->lv_name;
            auto* type = getType(i->type);
            auto* ptr = createAllocation(isglobal, type, nullptr, i->lv_name);
            typemap.emplace(ptrname, type);
            namemap.emplace(ptrname, ptr);
          },
          [&, this](const std::shared_ptr<NumberInst>& i) {
            auto ptr = namemap.find("ptr_" + i->lv_name);
            auto* finst =
                llvm::ConstantFP::get(this->ctx, llvm::APFloat(i->val));
            if (ptr != namemap.end()) {  // case of temporary value
              builder->CreateStore(finst, ptr->second);
              auto res = builder->CreateLoad(ptr->second, i->lv_name);
              namemap.emplace(i->lv_name, res);
            } else {
              namemap.emplace(i->lv_name, finst);
            }
          },
          [&, this](const std::shared_ptr<TimeInst>& i) {
            auto* type = typemap[i->lv_name];
            if (type == nullptr) {
              type = getType(i->type);
              typemap.try_emplace(i->lv_name, type);
            }
            auto* time =
                builder->CreateFPToUI(namemap[i->time], builder->getDoubleTy());
            auto* val = namemap[i->val];
            auto* fpzero = llvm::ConstantFP::get(builder->getDoubleTy(), 0.);
            auto* struct_p = llvm::ConstantStruct::get(
                llvm::cast<llvm::StructType>(type), {fpzero, fpzero});
            auto tmp1 = builder->CreateInsertValue(struct_p, time, 0);
            auto tmp2 = builder->CreateInsertValue(tmp1, val, 1);
            namemap.emplace(i->lv_name, tmp2);
          },
          [&, this](const std::shared_ptr<RefInst>& i) {  // TODO
            auto ptrname = "ptr_" + i->lv_name;
            auto ptrptrname = "ptr_" + ptrname;
            auto* ptrtoptr = createAllocation(
                isglobal, llvm::PointerType::get(typemap[i->val], 0), nullptr,
                ptrname);
            builder->CreateStore(namemap[ptrname], ptrtoptr);
            auto* ptr = builder->CreateLoad(ptrtoptr, ptrptrname);
            namemap.emplace(ptrname, ptr);
            namemap.emplace(ptrptrname, ptrtoptr);
          },
          [&, this](const std::shared_ptr<AssignInst>& i) {
            if (std::holds_alternative<types::Float>(i->type)) {
              // copy assignment
              builder->CreateStore(namemap[i->val],
                                   namemap["ptr_" + i->lv_name]);
              // rename old register name
              auto oldval = namemap.extract(i->lv_name);
              oldval.key() += "_o";

              auto* newval =
                  builder->CreateLoad(namemap["ptr_" + i->lv_name], i->lv_name);
              namemap.emplace(i->lv_name, newval);
            }
          },
          [&, this](const std::shared_ptr<OpInst>& i) {
            llvm::Value* retvalue;
            auto* lhs = namemap[i->lhs];
            auto* rhs = namemap[i->rhs];
            switch (i->getOPid()) {
              case OP_ID::ADD:
                retvalue = builder->CreateFAdd(lhs, rhs, i->lv_name);
                break;
              case OP_ID::SUB:
                retvalue = builder->CreateFSub(lhs, rhs, i->lv_name);
                break;
              case OP_ID::MUL:
                retvalue = builder->CreateFMul(lhs, rhs, i->lv_name);
                break;
              case OP_ID::DIV:
                retvalue = builder->CreateFDiv(lhs, rhs, i->lv_name);
                break;
              case OP_ID::MOD:
                retvalue = builder->CreateCall(getForeignFunction("fmod"),
                                               {lhs, rhs}, i->lv_name);
                break;
              default:
                retvalue = builder->CreateUnreachable();
                break;
            }
            namemap.emplace(i->lv_name, retvalue);
          },
          [&, this](const std::shared_ptr<FunInst>& i) {
            bool hasfv = !i->freevariables.empty();
            auto* ft =
                llvm::cast<llvm::FunctionType>(getType(i->type));  // NOLINT
            llvm::Function* f = llvm::Function::Create(
                ft, llvm::Function::ExternalLinkage, i->lv_name, module.get());
            auto f_it = f->args().begin();

            std::for_each(i->args.begin(), i->args.end(),
                          [&](std::string s) { (f_it++)->setName(s); });
            // if function is closure,append closure argument, dsp function is
            // forced to be closure function
            if (hasfv || i->lv_name == "dsp") {
              auto it = f->args().end();
              auto lastelem = (--it);
              auto name = "clsarg_" + i->lv_name;
              lastelem->setName(name);
              namemap.emplace(name, (llvm::Value*)lastelem);
            }
            namemap.emplace(i->lv_name, f);
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", f);
            builder->SetInsertPoint(bb);
            currentblock = bb;
            f_it = f->args().begin();
            std::for_each(i->args.begin(), i->args.end(),
                          [&](std::string s) { namemap.emplace(s, f_it++); });

            auto arg_end = f->arg_end();
            llvm::Value* lastarg = --arg_end;
            for (int id = 0; id < i->freevariables.size(); id++) {
              std::string newname = "fv_" + i->freevariables[id];
              llvm::Value* gep = builder->CreateStructGEP(lastarg, id, "fv");
              auto ptrname = "ptr_" + newname;
              llvm::Value* ptrload = builder->CreateLoad(gep, ptrname);
              llvm::Value* valload = builder->CreateLoad(ptrload, newname);
              namemap.try_emplace(ptrname, ptrload);
              namemap.try_emplace(newname, valload);
            }
            for (auto& cinsts : i->body->instructions) {
              visitInstructions(cinsts, false);
            }
            if (currentblock->getTerminator() == nullptr &&
                ft->getReturnType()->isVoidTy()) {
              builder->CreateRetVoid();
            }
            setBB(mainentry);
            currentblock = mainentry;
          },
          [&, this](const std::shared_ptr<MakeClosureInst>& i) {
            //store the capture address to memory
            auto* structtype = (llvm::PointerType*)getType(i->capturetype);
            llvm::Value* capture_ptr =
                createAllocation(isglobal, structtype->getElementType(), nullptr, i->fname+"_cap");
            module->dump();
            int idx = 0;
            for (auto& cap : i->captures) {
              llvm::Value* gep =
                  builder->CreateStructGEP(structtype->getElementType(), capture_ptr, idx, "");
              builder->CreateStore(namemap["ptr_" + cap], gep);
              idx += 1;
            }
                        namemap.emplace(i->fname+"_cap", capture_ptr);
              auto* atype =llvm::ArrayType::get(getType(i->type),1);
            auto* gptr = (llvm::GlobalVariable*)module->getOrInsertGlobal("ptr_to_"+i->fname,atype);
            gptr->setInitializer(llvm::ConstantArray::get(atype,{module->getFunction(i->fname)}));
            gptr->setConstant(true);
            gptr->setLinkage(llvm::GlobalValue::LinkageTypes::PrivateLinkage);
            auto ptrtoptr = builder->CreateInBoundsGEP(atype,gptr,llvm::ConstantInt::get(ctx,llvm::APInt(32,0)),"ptrptr_"+i->fname);
            auto ptr = builder->CreateLoad(ptrtoptr,"ptr_"+i->fname);
            //store the pointer to function(1 size array type)
            // auto* ptrtofntype = getType(i->type);
            // llvm::Value* fn_ptr = createAllocation(isglobal,ptrtofntype,nullptr,i->fname+"_cls");
            // llvm::Value* f = module->getFunction(i->fname);
            // auto res = builder->CreateInsertValue(fn_ptr, f, 0);
            namemap.emplace("ptr_"+i->fname,ptr);


          },
          [&, this](const std::shared_ptr<FcallInst>& i) {
            bool isclosure = i->ftype == CLOSURE;
            std::vector<llvm::Value*> args;
            std::for_each(i->args.begin(), i->args.end(),
                          [&](auto& a) { args.emplace_back(namemap[a]); });
            if (isclosure) {
              args.emplace_back(namemap[i->fname + "_cap"]);
            }
            if (i->istimed) {  // if arguments is timed value, call addTask
              createAddTaskFn(i, isclosure, isglobal);
            } else {
              createFcall(i, args);
            }
          },
          [&, this](const std::shared_ptr<ReturnInst>& i) {
            builder->CreateRet(namemap[i->val]);
          }},
      inst);
}

llvm::Error LLVMGenerator::doJit(const size_t opt_level) {
  return jitengine->addModule(
      std::move(module));  // do JIT compilation for module
}
void* LLVMGenerator::execute() {
  llvm::Error err = doJit();
  Logger::debug_log(err, Logger::ERROR);
  auto mainfun = jitengine->lookup("mimium_main");
  Logger::debug_log(mainfun, Logger::ERROR);
  auto fnptr =
      llvm::jitTargetAddressToPointer<void* (*)()>(mainfun->getAddress());
  //
  void* res = fnptr();
  return res;
}
void LLVMGenerator::outputToStream(llvm::raw_ostream& stream) {
  module->print(stream, nullptr, false, true);
}

}  // namespace mimium