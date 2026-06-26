#include "cstar.codegen.h"
#include "cstar.parser.hpp"
#include "cstar.ast.h"

using namespace std;
using namespace llvm;

//------------------------------------------------------------------------------ 
//                              ASTNode
//------------------------------------------------------------------------------

ASTNode::ASTNode(CodeGenContext& context) 
: line(context.line)
, column(context.col)
{
}

CodeGenRtn ASTNode::codeGen(CodeGenContext& context)
{
    context.emitDebugLocation(this);
    return codeGenInternal(context);
}

void ASTNode::debugPrintPart(CodeGenContext& context, SharedAST node, const llvm::Twine& prefix)
{ 
    node->debugPrint(context, "    " + prefix);
}

//------------------------------------------------------------------------------ 
//                              CodeGenContext
//------------------------------------------------------------------------------

CodeGenContext::CodeGenContext(SharedString moduleName, OptimizationLevel level, int maxErrorCount) 
: _mMaxErrorCount(maxErrorCount)
, _mCurrentErrorCount(0)
, _mScopeContextList()
, _mMainFunction(NULL)
, _mNamespace(NULL)
, _mModuleSymbols()
, _mDCompileUnit(NULL)
, _mDGlobalScope(NULL)
, line(CSTAR_LEXERINSTANCE_DEFAULT_LINE_ONE)
, col(CSTAR_LEXERINSTANCE_DEFAULT_COLUMN_ONE)
, opt(level)
, _mName(moduleName)
, _mContext(std::make_shared<LLVMContext>())
, _mModule(llvm::make_unique<Module>(moduleName->c_str(), *_mContext))
, _mBuilder(llvm::make_unique<IRBuilder<> >(_mModule->getContext()))
, _mDebugBuilder((level == Debug) ? llvm::make_unique<DIBuilder>(*_mModule) : NULL)
{
    if(_mDebugBuilder)
    {
        std::string triple = sys::getProcessTriple();
        _mModule->setTargetTriple(triple);
        Triple targetTriple = Triple(triple);
        //TargetInfo tInfo(targetTriple);
        //_mModule->setDataLayout(tInfo->getDataLayoutString());

        _mModule->addModuleFlag(Module::Warning, "Debug Info Version", DEBUG_METADATA_VERSION);

        // Darwin only supports dwarf2.
        if (targetTriple.isOSDarwin())
        {
            _mModule->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 2);
        }

        _mDCompileUnit = _mDebugBuilder->createCompileUnit(dwarf::DW_LANG_C, moduleName->c_str(), ".", "CStar Compiler", 0, "", 0);
        _mDGlobalScope = _mDebugBuilder->createFile(_mDCompileUnit->getFilename(), _mDCompileUnit->getDirectory());
    }
}

CodeGenContext::~CodeGenContext()
{
    // Context needs to outlive the module
    _mModule = NULL;
    _mContext = NULL;
}

bool CodeGenContext::handleError(int line, int column, llvm::StringRef section, const llvm::Twine &error)
{
    std::cout << line << ":" << column << ":" << section.data() << " Error:" << error.str() << endl;
    ++_mCurrentErrorCount;
    return isErrorLimitReached();
}

bool CodeGenContext::handleCodeGenError(ASTNode& node, const llvm::Twine &error)
{
    // TODO: may want to provide a way to allow callers to override line and column
    return handleError(node.line, node.column, "CodeGen", error);
}

void CodeGenContext::generateCode(SharedCompilationUnit root)
{
    std::cout << "Generating code for module: " << root->name->c_str() << endl;
    root->codeGen(*this);

//    verifyContextModule();
}

void CodeGenContext::printDebugAST(SharedCompilationUnit root)
{
    std::cout << "Debug Printing AST for module: " << root->name->c_str() << endl;
    root->debugPrint(*this, "- ");
    std::cout << endl;
}

void CodeGenContext::runOptimizationPasses()
{
    // TODO: use legacy for now? or continue with new ?
    //PassManager<Module> PM;
    //PM.addPass(createPromoteMemoryToRegisterPass());
    //PM.run(*_mModule);
    legacy::PassManager pm;
    
    pm.add(createPromoteMemoryToRegisterPass());
    // Do simple "peephole" optimizations and bit-twiddling optzns.
    pm.add(createInstructionCombiningPass());
    // Reassociate expressions.
    pm.add(createReassociatePass());
    // Eliminate Common SubExpressions.
    pm.add(createGVNPass());
    // Simplify the control flow graph (deleting unreachable blocks, etc).
    pm.add(createCFGSimplificationPass());

    pm.run(*_mModule);
}

llvm::Function* CodeGenContext::addMainFunction(llvm::Function* cstarMain) 
{
    Function* rtn = NULL;
    if(!hasMainFunction())
    {
        Module* mod = getModule();
        //define i32 @main(i32 %argc, i8 **%argv)
        _mMainFunction = cast<Function>(mod->getOrInsertFunction("main", 
                            IntegerType::getInt32Ty(mod->getContext()),
                            IntegerType::getInt32Ty(mod->getContext()),
                            PointerType::getUnqual(PointerType::getUnqual(IntegerType::getInt8Ty(mod->getContext()))), NULL));
        {
            Function::arg_iterator args = _mMainFunction->arg_begin();
            Value *arg_0 = &*args++;
            arg_0->setName("argc");
            Value *arg_1 = &*args++;
            arg_1->setName("argv");
        }

        //main.0:
        BasicBlock *bb = BasicBlock::Create(mod->getContext(), "main.0", _mMainFunction);
        _mBuilder->SetInsertPoint(bb);

        //call void @cstar_main()
        CallInst *main_call = _mBuilder->CreateCall(cstarMain, None, "call_cstar_main");
        main_call->setTailCall(false);

        //ret i32 0
        //ReturnInst::Create(mod->getContext(), ConstantInt::get(mod->getContext(), APInt(32, 0)), bb);
        _mBuilder->CreateRet(main_call);
        rtn = _mMainFunction;
    }
    return rtn;
}

void CodeGenContext::emitDebugLocation(ASTNode* node)
{
    if(_mDebugBuilder)
    {
        if(node)
        {
            _mBuilder->SetCurrentDebugLocation(DebugLoc::get(node->line, node->column, currentDebugScope()));
        }
        else
        {
            _mBuilder->SetCurrentDebugLocation(DebugLoc());
        }
    }
}

void CodeGenContext::finalizeDebugInfo()
{
    if(opt == Debug)_mDebugBuilder->finalize(); 

    verifyContextModule();
}

void CodeGenContext::verifyContextModule()
{
    std::string errorString;
    llvm::raw_string_ostream rso(errorString);
    std::cout<<rso.str();

    if(verifyModule(*_mModule, &rso))
    {
        std::cout << "Failed To Verify Module: " << " error: " << rso.str() << std::endl;
        dumpModule();
    }
    else
    {
        std::cout << "Module Verified" << std::endl;
    }
}

void CodeGenContext::verifyContextFunction(Function* func, StringRef name)
{
    std::string errorString;
    llvm::raw_string_ostream rso(errorString);
    std::cout<<rso.str();

    if(verifyFunction(*func, &rso))
    {
        std::cout << "Failed To Verify Function: " << name.data() << " error: " << rso.str() << endl;
        func->dump();
    }
    else
    {
        std::cout << "Function Verified" << std::endl;
    }
}

llvm::BasicBlock *CodeGenContext::getBreakTarget()
{
    BasicBlock *rtn = NULL;

    for (auto scopeIt = _mScopeContextList.rbegin(); scopeIt != _mScopeContextList.rend(); ++scopeIt )
    {
        rtn = (*scopeIt)->breakTarget;
        if(rtn)
        {
            break;
        }
    }

    return rtn;
}

llvm::BasicBlock *CodeGenContext::getContinueTarget()
{
    BasicBlock *rtn = NULL;

    for (auto scopeIt = _mScopeContextList.rbegin(); scopeIt != _mScopeContextList.rend(); ++scopeIt )
    {
        rtn = (*scopeIt)->continueTarget;
        if(rtn)
        {
            break;
        }
    }

    return rtn;
}

SharedSymbolEntry CodeGenContext::getSymbolInScopeByName( llvm::StringRef name )
{
    SharedSymbolEntry rtn;
    std::string n(name.data());
    
    for (auto scopeIt = _mScopeContextList.rbegin(); scopeIt != _mScopeContextList.rend(); ++scopeIt )
    {
        NameValueMap nameValMap = (*scopeIt)->locals;
        auto entryIt = nameValMap.find(n);
        if(entryIt != nameValMap.end())
        {
            rtn = entryIt->second;
            break;
        }
    }

    if(!rtn)
    {
        auto entryIt = _mModuleSymbols.find(n);
        if(entryIt != _mModuleSymbols.end())
        {
            rtn = entryIt->second;
        }
    }

    return rtn;
}

bool CodeGenContext::addSymbolToScope( std::string name, SharedSymbolEntry symbol )
{
    bool rtn = false;
    SharedSymbolEntry entry = getSymbolInScopeByName(name);
    if(!entry)
    {
        rtn = true;
        if(_mScopeContextList.size() > 0)
        {
            _mScopeContextList.back()->locals[name] = symbol;
        }
        else
        {
            _mModuleSymbols[name] = symbol;
        }
    }
    return rtn;
}

//------------------------------------------------------------------------------ 
//                              Helpers
//------------------------------------------------------------------------------

Value* getConstantInt(CodeGenContext& context, unsigned bits, uint64_t val, bool isSigned)
{
    return ConstantInt::get(context.getLLVMContext(), APInt(bits, val, isSigned));
}

Value* createDefault(CodeGenContext& context, SharedIdentifier type)
{
    Value* rtn = NULL;
    switch(type->builtInVal)
    {
        case IDENTIFIER_INT8_VAL:
            rtn = getConstantInt(context, 8, 0, true);
        break;
        case IDENTIFIER_UINT8_VAL:
            rtn = getConstantInt(context, 8, 0, false);
        break;
        case IDENTIFIER_INT16_VAL:
            rtn = getConstantInt(context, 16, 0, true);
        break;
        case IDENTIFIER_UINT16_VAL:
            rtn = getConstantInt(context, 16, 0, false);
        break;
        case IDENTIFIER_INT32_VAL:
            rtn = getConstantInt(context, 32, 0, true);
        break;
        case IDENTIFIER_UINT32_VAL:
            rtn = getConstantInt(context, 32, 0, false);
        break;
        case IDENTIFIER_INT64_VAL:
            rtn = getConstantInt(context, 64, 0, true);
        break;
        case IDENTIFIER_UINT64_VAL:
            rtn = getConstantInt(context, 64, 0, false);
        break;
        case IDENTIFIER_BOOL_VAL:
            rtn = ConstantInt::getFalse(context.getLLVMContext());
        break;
        case IDENTIFIER_FLOAT32_VAL:
            rtn = ConstantFP::get(context.getLLVMContext(), APFloat(0.0f));
        break;
        case IDENTIFIER_FLOAT64_VAL:
            rtn = ConstantFP::get(context.getLLVMContext(), APFloat(0.0));
        break;
        case IDENTIFIER_STRING_VAL:
        // TODO: hook up string
        break;
        case IDENTIFIER_VOID_VAL:
            rtn = NULL;
        break;
        case IDENTIFIER_NONE_VAL:
        default:
        // No Op
        break;
    }

    if(rtn == NULL)
    {
        //TODO: Handle complex types
        SharedString value = type->value;
        SharedStringList qualifier = type->qualifier;
        SharedString generic = type->generic;
        if(!generic && qualifier->empty())
        {
        }
    }

    return rtn;
}

Type* getTypeFromIdentifier(CodeGenContext& context, SharedIdentifier identifier)
{
    Type* rtn = NULL;
    switch(identifier->builtInVal)
    {
        case IDENTIFIER_INT8_VAL:
        case IDENTIFIER_UINT8_VAL:
            rtn = Type::getInt8Ty(context.getLLVMContext());
        break;
        case IDENTIFIER_INT16_VAL:
        case IDENTIFIER_UINT16_VAL:
            rtn = Type::getInt16Ty(context.getLLVMContext());
        break;
        case IDENTIFIER_INT32_VAL:
        case IDENTIFIER_UINT32_VAL:
            rtn = Type::getInt32Ty(context.getLLVMContext());
        break;
        case IDENTIFIER_INT64_VAL:
        case IDENTIFIER_UINT64_VAL:
            rtn = Type::getInt64Ty(context.getLLVMContext());
        break;
        case IDENTIFIER_BOOL_VAL:
            rtn = Type::getInt1Ty(context.getLLVMContext());
        break;
        case IDENTIFIER_FLOAT32_VAL:
            rtn = Type::getFloatTy(context.getLLVMContext());
        break;
        case IDENTIFIER_FLOAT64_VAL:
            rtn = Type::getDoubleTy(context.getLLVMContext());
        break;
        case IDENTIFIER_STRING_VAL:
        // TODO: hook up string
        break;
        case IDENTIFIER_VOID_VAL:
            rtn = Type::getVoidTy(context.getLLVMContext());
        break;
        case IDENTIFIER_NONE_VAL:
        default:
        // No Op
        break;
    }

    if(rtn == NULL)
    {
        //TODO: Handle complex types
        SharedString value = identifier->value;
        SharedStringList qualifier = identifier->qualifier;
        SharedString generic = identifier->generic;
        if(!generic && qualifier->empty())
        {
        }
    }

    return rtn;
}

AllocaInst *createEntryBlockAlloca(Type* type, Function *function, StringRef name)
{
    IRBuilder<> tempBuilder(&function->getEntryBlock(), function->getEntryBlock().begin());
    return tempBuilder.CreateAlloca(type, nullptr, name);
}

DIType* getDebugTypeFromIdentifier(CodeGenContext& context, SharedIdentifier identifier)
{
    // TODO: cache these probably using a map
    // TODO: look into qualified types like "const" with createQualifiedType
    DIType* rtn = NULL;
    DIBuilder* dbuilder = context.getDIBuilder();
    Type* t = getTypeFromIdentifier(context, identifier);
    if(t && !t->isVoidTy())
    {
        int alignment = context.getModule()->getDataLayout().getABITypeAlignment(t);
        switch(identifier->builtInVal)
        {
            case IDENTIFIER_INT8_VAL:
                rtn = dbuilder->createBasicType("int8", 8, alignment, dwarf::DW_ATE_signed);
            break;
            case IDENTIFIER_UINT8_VAL:
                rtn = dbuilder->createBasicType("uint8", 8, alignment, dwarf::DW_ATE_unsigned);
            break;
            case IDENTIFIER_INT16_VAL:
                rtn = dbuilder->createBasicType("int16", 16, alignment, dwarf::DW_ATE_signed);
            break;
            case IDENTIFIER_UINT16_VAL:
                rtn = dbuilder->createBasicType("uint16", 16, alignment, dwarf::DW_ATE_unsigned);
            break;
            case IDENTIFIER_INT32_VAL:
                rtn = dbuilder->createBasicType("int32", 32, alignment, dwarf::DW_ATE_signed);
            break;
            case IDENTIFIER_UINT32_VAL:
                rtn = dbuilder->createBasicType("uint32", 32, alignment, dwarf::DW_ATE_unsigned);
            break;
            case IDENTIFIER_INT64_VAL:
                rtn = dbuilder->createBasicType("int64", 64, alignment, dwarf::DW_ATE_signed);
            break;
            case IDENTIFIER_UINT64_VAL:
                rtn = dbuilder->createBasicType("uint64", 64, alignment, dwarf::DW_ATE_unsigned);
            break;
            case IDENTIFIER_BOOL_VAL:
                rtn = dbuilder->createBasicType("bool", 1, alignment, dwarf::DW_ATE_boolean);
            break;
            case IDENTIFIER_FLOAT32_VAL:
                rtn = dbuilder->createBasicType("float", 32, alignment, dwarf::DW_ATE_float);
            break;
            case IDENTIFIER_FLOAT64_VAL:
                rtn = dbuilder->createBasicType("double", 64, alignment, dwarf::DW_ATE_float);
            break;
            case IDENTIFIER_STRING_VAL:
            // TODO: hook up string
            break;
            case IDENTIFIER_VOID_VAL:
                // No Op - Handled at if above
            break;
            case IDENTIFIER_NONE_VAL:
            default:
            // No Op
            break;
        }

        if(rtn == NULL)
        {
            //TODO: Handle complex types
            SharedString value = identifier->value;
            SharedStringList qualifier = identifier->qualifier;
            SharedString generic = identifier->generic;
            if(!generic && qualifier->empty())
            {
            }
        }
    }

    return rtn;
}

DISubroutineType* getDebugFunctionType(CodeGenContext& context, SharedIdentifier returnType, SharedParameterList params)
{
    SmallVector<Metadata *, 8> returnPlusParams;

    DISubroutineType* rtn = NULL;
    DIBuilder* dbuilder = context.getDIBuilder();
    DIType* debugRtnType = getDebugTypeFromIdentifier(context, returnType);

    returnPlusParams.push_back(debugRtnType);

    for (unsigned i = 0, e = params->size(); i != e; ++i)
    {
        returnPlusParams.push_back(getDebugTypeFromIdentifier(context, (*params)[i]->type));
    }

    rtn = dbuilder->createSubroutineType(dbuilder->getOrCreateTypeArray(returnPlusParams));
    return rtn;
}

//------------------------------------------------------------------------------ 
//                              Code Generation
//------------------------------------------------------------------------------

CodeGenRtn CompilationUnit::codeGenInternal(CodeGenContext& context)
{
    bool failed = false;
    if( nameSpace )
    {
        failed = (nameSpace->codeGen(context)) ? failed : context.isErrorLimitReached();
    }

    if(!failed)
    {
        for (auto usingDecl : *usingDeclarationList) 
        {
            failed = (usingDecl->codeGen(context)) ? failed : context.isErrorLimitReached();
            if(failed)
            {
                break;
            }
        }

        if(!failed)
        {
            for (auto codeDecl : *codeDeclarationList) 
            {
                failed = (codeDecl->codeGen(context)) ? failed : context.isErrorLimitReached();
                if(failed)
                {
                    break;
                }
            }
        }
    }

	return NULL;
}

CodeGenRtn Int8Node::codeGenInternal(CodeGenContext& context)
{
    return getConstantInt(context, 8, value, true);
}

CodeGenRtn Int16Node::codeGenInternal(CodeGenContext& context)
{
    return getConstantInt(context, 16, value, true);
}

CodeGenRtn Int32Node::codeGenInternal(CodeGenContext& context)
{
    return getConstantInt(context, 32, value, true);
}

CodeGenRtn Int64Node::codeGenInternal(CodeGenContext& context)
{
    return getConstantInt(context, 64, value, true);
}

CodeGenRtn UInt8Node::codeGenInternal(CodeGenContext& context)
{
    return getConstantInt(context, 8, value, false);
}

CodeGenRtn UInt16Node::codeGenInternal(CodeGenContext& context)
{
    return getConstantInt(context, 16, value, false);
}

CodeGenRtn UInt32Node::codeGenInternal(CodeGenContext& context)
{
    return getConstantInt(context, 32, value, false);
}

CodeGenRtn UInt64Node::codeGenInternal(CodeGenContext& context)
{
    return getConstantInt(context, 64, value, false);
}

CodeGenRtn Float32Node::codeGenInternal(CodeGenContext& context)
{
    return ConstantFP::get(context.getLLVMContext(), APFloat(value));
}

CodeGenRtn Float64Node::codeGenInternal(CodeGenContext& context)
{
    return ConstantFP::get(context.getLLVMContext(), APFloat(value));
}

CodeGenRtn StringNode::codeGenInternal(CodeGenContext& context)
{
    //TODO: implement me
    return NULL;
}

CodeGenRtn BooleanNode::codeGenInternal(CodeGenContext& context)
{
    return (value) ? ConstantInt::getTrue(context.getLLVMContext()) : ConstantInt::getFalse(context.getLLVMContext());
}

CodeGenRtn NullNode::codeGenInternal(CodeGenContext& context)
{
    //TODO: implement me
    return NULL;
}

CodeGenRtn IdentifierNode::codeGenInternal(CodeGenContext& context)
{
    Value* rtn = NULL;
    SharedSymbolEntry entry = context.getSymbolInScopeByName(*value);
    // TODO: handle functions?
    Value* val = entry ? entry->llvmVal : NULL;
    if(val)
    {
        IRBuilder<>* builder = context.getIRBuilder();
        rtn = builder->CreateLoad(val, value->c_str());
    }
    else
    {
        context.handleCodeGenError(*this, "Unknown variable name: " + Twine(value->c_str()));

    }
    return rtn;
}

CodeGenRtn FunctionDeclarationNode::codeGenInternal(CodeGenContext& context)
{
    bool isMainFunc = (name->value->compare("main") == 0);
    bool isValid = !isMainFunc || !context.hasMainFunction(); 
    CodeGenRtn rtn = NULL;
    if(isValid)
    {
        if(modifier)
        {
            // TODO: handle modifier for functions
            // TODO: main should not have a modifier applied
            // modifier;
        }

        SmallVector<Type*, 8> argumentList;
        for (auto param : *parameters) 
        {
            Type* argType = getTypeFromIdentifier(context, param->type);
            argumentList.push_back(argType);
        }
        FunctionType *functionType = FunctionType::get(getTypeFromIdentifier(context, returnType), makeArrayRef(argumentList), false);

        std::string funcName = isMainFunc ? "cstar_main" : *(name->value);
        Function *func = Function::Create(functionType, Function::ExternalLinkage, funcName, context.getModule());
        rtn = func;

        // Set names for all arguments.
        unsigned index = 0;
        for (auto &p : func->args())
        {
            SharedString n = (*parameters)[index++]->identifier->value;
            p.setName(*n);
        }

        llvm::BasicBlock* bb = NULL;
        if(block)
        {
            // Create a new basic block to start insertion into.
            IRBuilder<>* builder = context.getIRBuilder();
            BasicBlock *entryBlock = BasicBlock::Create(context.getLLVMContext(), "entry", func);
            builder->SetInsertPoint(entryBlock);
            context.emitDebugLocation(this);

            DISubprogram *debugScope = NULL;
            DIFile *funcDFile = NULL;
            DIBuilder* dbuilder = context.getDIBuilder();
            if(dbuilder)
            {
            funcDFile = dbuilder->createFile(context.getDebugCompileUnit()->getFilename(), 
                                                         context.getDebugCompileUnit()->getDirectory());

                DISubroutineType* debugSubRType = getDebugFunctionType(context, returnType, parameters);

                //TODO: modify flags as needed.
                debugScope = dbuilder->createFunction(
    /*context.currentDebugScope()*/funcDFile, funcName, StringRef(), funcDFile, line,
                debugSubRType,
                false /* internal linkage */, true /* definition */, line,
                0/*DINode::FlagPrototyped*/, false);
                func->setSubprogram(debugScope);
            }
//std::cout << "test 5" << endl;
//context.verifyContextModule();
            context.pushScopeContext(func, debugScope);

            context.emitDebugLocation(NULL);

            unsigned argIndex = 0;
            for(auto &arg : func->args())
            {
                AllocaInst *alloca = createEntryBlockAlloca( getTypeFromIdentifier(context, (*parameters)[argIndex]->type), func, arg.getName());

                if(dbuilder)
                {
                    DILocalVariable *dV = dbuilder->createParameterVariable(
                        debugScope, arg.getName(), argIndex+1, funcDFile, line, getDebugTypeFromIdentifier(context, (*parameters)[argIndex]->type),
                        true/*AlwaysPreserve*/);

                    dbuilder->insertDeclare(alloca, dV, dbuilder->createExpression(),
                                            DebugLoc::get(line, column, debugScope),
                                            builder->GetInsertBlock());
                }

                // Store the initial value into the alloca.
                builder->CreateStore(&arg, alloca);

                if(context.addSymbolToScope( arg.getName(), std::make_shared<SymbolEntry>(alloca, (*parameters)[argIndex]) ))
                {

                }
                else
                {
                    context.handleCodeGenError(*this, "name already in scope");
                }
                ++argIndex;
            }

            if(block->statements->size() == 0)
            {
                builder->CreateRetVoid();
            }
            else
            {
                bb = cast<llvm::BasicBlock>(block->codeGen(context));
                if(bb)
                {
                    if(!(bb->getTerminator()))
                    {
                        builder->CreateRetVoid();
                    }
                }
                else
                {

                }
            }
            context.popScopeContext();
        }
        else if(!modifier || modifier->value->compare(""))
        {
            context.handleCodeGenError(*this, "missing body on non-extern function: " + Twine(name->value->c_str()));
            rtn = NULL;
        }

        if(isMainFunc && (bb != NULL))
        {
            // We have specific rules for main.
            // 1. It can only be declared once
            // 2. It cannot be called by cstar logic in any way
            //TODO: if(returnType->builtInVal == IDENTIFIER_VOID_VAL)
            context.addMainFunction(func);
        }

        //context.verifyContextFunction(func, funcName);
    }
    else
    {
        context.handleCodeGenError(*this, "main can only be defined once");
        rtn = NULL;
    }

    return rtn;
}

CodeGenRtn BlockNode::codeGenInternal(CodeGenContext& context)
{
    BasicBlock *bb = BasicBlock::Create(context.getLLVMContext(), "block", context.currentFunction());
    Value* rtn = bb;
    IRBuilder<>* builder = context.getIRBuilder();
    builder->CreateBr(bb);
    builder->SetInsertPoint(bb);

    bool failed = false;
    for (auto statement : *statements) 
    {
        failed = (statement->codeGen(context) != NULL) ? failed : context.isErrorLimitReached();
        if(failed)
        {
            rtn = NULL;
            break;
        }
    }

    return bb;
}

CodeGenRtn LocalVariableDeclaration::codeGenInternal(CodeGenContext& context)
{
    IRBuilder<>* builder = context.getIRBuilder();
    Type* t = getTypeFromIdentifier(context, type);
    Value* rtn = NULL;

    //TODO: test to see if we want to allow initializers along the way or just the last one
    for (auto variable : *variables) 
    {
        // TODO: make sure initializer is of the correct type
        Value* initer = variable->initializer->codeGen(context);
        initer = (initer == NULL) ? createDefault(context, variable->name) : initer;

        if(initer == NULL)
        {
            context.handleCodeGenError(*this, "invalid default initializer");
        }
        else 
        {
            //TODO: decide how I want to handle unsigned vs signed types
            // as LLVM removed any distinguishing features from the IR
            // I will need to track this separately to make sure it works in all 
            // use cases.  Here it is easy enough to test against my "type"
            // http://nondot.org/~sabre/LLVMNotes/TypeSystemChanges.txt
            //type.

            if(initer->getType() != t)
            {
                context.handleCodeGenError(*this, "invalid type for initializer");
            }
            else
            {
                AllocaInst * alloca = createEntryBlockAlloca( t, context.currentFunction(), variable->name->value->c_str());
                builder->CreateStore(initer, alloca);

                if(context.addSymbolToScope( *(variable->name->value), std::make_shared<SymbolEntry>(alloca, variable) ))
                {
                    rtn = alloca; // only need to return something valid to signal no error
                }
                else
                {
                    context.handleCodeGenError(*this, "name already in scope");
                }
            }
        }

        if(context.isErrorLimitReached())
        {
            rtn = NULL;
            break;
        }
    }

    return rtn;
}

CodeGenRtn ConstLocalVariableDeclaration::codeGenInternal(CodeGenContext& context)
{
    // type->codeGen(context);
    // for (auto variable : *variables) 
    // {
    //     variable->codeGen(context);
    // }

    return NULL;
}

CodeGenRtn IfNode::codeGenInternal(CodeGenContext& context)
{
    Value* rtn = NULL;
    Value* cond = booleanExpression->codeGen(context);
    if(cond)
    {
        if(cond->getType() != IntegerType::getInt1Ty(context.getLLVMContext()))
        {
            context.handleCodeGenError(*this, "if conditional must evaluate to type bool");
        }
        else
        {
            BasicBlock *ifBB = BasicBlock::Create(context.getLLVMContext(), "if.then", context.currentFunction());
            BasicBlock *elseBB = BasicBlock::Create(context.getLLVMContext(), "if.else");
            BasicBlock *mergeBB = BasicBlock::Create(context.getLLVMContext(), "if.end");

            IRBuilder<>* builder = context.getIRBuilder();
            builder->CreateCondBr(cond, ifBB, elseBB);
            builder->SetInsertPoint(ifBB);

            Value* ifV = ifStatement->codeGen(context);
            if(ifV)
            {
                builder->CreateBr(mergeBB);
                // codeGen of 'if' may change the current block, update ifBB for the PHI.
                ifBB = builder->GetInsertBlock();

                context.currentFunction()->getBasicBlockList().push_back(elseBB);
                builder->SetInsertPoint(elseBB);

                bool error = false;
                if(elseStatement)
                {
                    Value* elseV = elseStatement->codeGen(context);
                    if(!elseV)
                    {
                        error = true;
                    }
                }

                if(!error)
                {
                    builder->CreateBr(mergeBB);
                    // codeGen of 'else' can change the current block, update elseBB for the PHI.
                    elseBB = builder->GetInsertBlock();

                    // Emit merge block
                    context.currentFunction()->getBasicBlockList().push_back(mergeBB);
                    builder->SetInsertPoint(mergeBB);
                    rtn = cond; // simply want a non null here so no errors
                }
            }
        }
    }

    return rtn;
}

CodeGenRtn SwitchNode::codeGenInternal(CodeGenContext& context)
{
//    std::cout << "Creating SwitchNode: " << endl;
//
//    expression->codeGen(context);
//    for (auto section : *switchsections) 
//    {
//        section->codeGen(context);
//    }

    return NULL;
}

CodeGenRtn SwitchSectionNode::codeGenInternal(CodeGenContext& context)
{
    // std::cout << "Creating SwitchSectionNode: " << endl;

    // for (auto label : *labels) 
    // {
    //     label->codeGen(context);
    // }
    // for (auto statement : *statementList) 
    // {
    //     statement->codeGen(context);
    // }

    return NULL;
}

CodeGenRtn SwitchLabelNode::codeGenInternal(CodeGenContext& context)
{
    // std::cout << "Creating SwitchLabelNode: " << endl;

    // constantExpression->codeGen(context);

    return NULL;
}

CodeGenRtn WhileNode::codeGenInternal(CodeGenContext& context)
{
    Value* rtn = NULL;

    BasicBlock *condBB = BasicBlock::Create(context.getLLVMContext(), "while.cond", context.currentFunction());
    BasicBlock *bodyBB = BasicBlock::Create(context.getLLVMContext(), "while.body");
    BasicBlock *endBB = BasicBlock::Create(context.getLLVMContext(), "while.end");

    IRBuilder<>* builder = context.getIRBuilder();

    builder->CreateBr(condBB);
    builder->SetInsertPoint(condBB);
    context.pushScopeContext(context.currentFunction(), context.currentDebugScope());
    context.setBreakTarget(endBB);
    context.setContinueTarget(condBB);

    Value* cond = booleanExpression->codeGen(context);
    if(cond && !context.isErrorLimitReached())
    {
        builder->CreateCondBr(cond, bodyBB, endBB);
        context.currentFunction()->getBasicBlockList().push_back(bodyBB);
        builder->SetInsertPoint(bodyBB);

        Value* stmnt = whileStatement->codeGen(context);

        if(stmnt && !context.isErrorLimitReached())
        {
            bool hasTerminator = false;
            if (BasicBlock *bb = dyn_cast<BasicBlock>(stmnt))
            {
                if(bb->getTerminator())
                {
                    hasTerminator = true;
                }
            }

            if(!hasTerminator)
            {
                builder->CreateBr(condBB);
            }
            bodyBB = builder->GetInsertBlock();

            context.currentFunction()->getBasicBlockList().push_back(endBB);
            builder->SetInsertPoint(endBB);
            rtn = cond; // simply want a valid return here... may do away with this soon
        }
    }

    return rtn;
}

CodeGenRtn DoWhileNode::codeGenInternal(CodeGenContext& context)
{
    Value* rtn = NULL;

    BasicBlock *bodyBB = BasicBlock::Create(context.getLLVMContext(), "do.body", context.currentFunction());
    BasicBlock *condBB = BasicBlock::Create(context.getLLVMContext(), "do.cond");
    BasicBlock *endBB = BasicBlock::Create(context.getLLVMContext(), "do.end");

    IRBuilder<>* builder = context.getIRBuilder();

    builder->CreateBr(bodyBB);
    builder->SetInsertPoint(bodyBB);
    context.pushScopeContext(context.currentFunction(), context.currentDebugScope());
    context.setBreakTarget(endBB);
    context.setContinueTarget(condBB);

    Value* stmnt = doWhileStatement->codeGen(context);
    if(!context.isErrorLimitReached())
    {
        bool hasTerminator = false;
        if (BasicBlock *bb = dyn_cast<BasicBlock>(stmnt))
        {
            if(bb->getTerminator())
            {
                hasTerminator = true;
            }
        }

        if(!hasTerminator)
        {
            builder->CreateBr(condBB);
        }
        
        bodyBB = builder->GetInsertBlock();

        context.currentFunction()->getBasicBlockList().push_back(condBB);
        builder->SetInsertPoint(condBB);
        Value* cond = booleanExpression->codeGen(context);

        if(cond && !context.isErrorLimitReached())
        {
            builder->CreateCondBr(cond, bodyBB, endBB);

            context.currentFunction()->getBasicBlockList().push_back(endBB);
            builder->SetInsertPoint(endBB);
            rtn = cond; // simply want a valid return here... may do away with this soon
        }
    }

    return rtn;
}

CodeGenRtn ForNode::codeGenInternal(CodeGenContext& context)
{
    // for (auto initializerStatement : *initializerStatements) 
    // {
    //     initializerStatement->codeGen(context);
    // }

    // booleanExpression->codeGen(context);

    // for (auto iteratorStatement : *iteratorStatements) 
    // {
    //     iteratorStatement->codeGen(context);
    // }

    // body->codeGen(context);

    return NULL;
}

CodeGenRtn ForEachNode::codeGenInternal(CodeGenContext& context)
{
    // type->codeGen(context);
    // name->codeGen(context);
    // expression->codeGen(context);
    // body->codeGen(context);

    return NULL;
}

CodeGenRtn BreakNode::codeGenInternal(CodeGenContext& context)
{
    Value* rtn = NULL;
    BasicBlock* block = context.getBreakTarget();
    if(block)
    {
        IRBuilder<>* builder = context.getIRBuilder();
        rtn = builder->CreateBr(block);
    }
    else
    {
        context.handleCodeGenError(*this, "Invalid Break");
    }
    return rtn;
}

CodeGenRtn ContinueNode::codeGenInternal(CodeGenContext& context)
{
    Value* rtn = NULL;
    BasicBlock* block = context.getContinueTarget();
    if(block)
    {
        IRBuilder<>* builder = context.getIRBuilder();
        rtn = builder->CreateBr(block);
    }
    else
    {
        context.handleCodeGenError(*this, "Invalid Continue");
    }
    return rtn;
}

CodeGenRtn ReturnNode::codeGenInternal(CodeGenContext& context)
{
    Value* rtn = NULL;
    IRBuilder<>* builder = context.getIRBuilder();
    Value* returnVal = expression->codeGen(context);
    if(returnVal)
    {
        if(returnVal->getType() == context.currentFunction()->getReturnType())
        {
            rtn = builder->CreateRet(returnVal);
        }
        else
        {
            context.handleCodeGenError(*this, "return type mismatch");
        }
    }
    return rtn;
}

CodeGenRtn MemberAccessNode::codeGenInternal(CodeGenContext& context)
{
    // std::cout << "Creating MemberAccessNode: " << endl;

    // identifier->codeGen(context);
    // if(expression)
    // {
    //     expression->codeGen(context);
    // }
    // else if(classType)
    // {
    //     classType->codeGen(context);
    // }

    return NULL;
}

CodeGenRtn ArgumentNode::codeGenInternal(CodeGenContext& context)
{
    // name->codeGen(context);

    // if(modifier)
    // {
    //     modifier->codeGen(context);
    // }
    // expression->codeGen(context);

    return NULL;
}

CodeGenRtn ElementAccessNode::codeGenInternal(CodeGenContext& context)
{
    // std::cout << "Creating ElementAccessNode: " << endl;

    // if(identifier)
    // {
    //     identifier->codeGen(context);
    // } else if( expression )
    // {
    //     expression->codeGen(context);
    // }

    // for (auto express : *expressionlist) 
    // {
    //     express->codeGen(context);
    // }

    return NULL;
}

CodeGenRtn ThisAccessNode::codeGenInternal(CodeGenContext& context)
{
    std::cout << "Creating ThisAccessNode: " << endl;
    return NULL;
}

CodeGenRtn BaseAccessNode::codeGenInternal(CodeGenContext& context)
{
    // std::cout << "Creating BaseAccessNode: " << endl;

    // if(identifier)
    // {
    //     identifier->codeGen(context);
    // }
    // else if(expressionlist)
    // {
    //     for (auto expression : *expressionlist) 
    //     {
    //         expression->codeGen(context);
    //     }
    // }

    return NULL;
}

CodeGenRtn SimpleUnaryExpressionNode::codeGenInternal(CodeGenContext& context)
{
    switch(token)
    {
        case EXCLAMATION:
        break;
        case TILDE:
        break;
        case PLUS:
        break;
        case MINUS:
        break;
    }
    // expression->codeGen(context);

    return NULL;
}

CodeGenRtn CastNode::codeGenInternal(CodeGenContext& context)
{
    // type->codeGen(context);
    // unaryExpression->codeGen(context);

    return NULL;
}

CodeGenRtn BinaryExpressionNode::codeGenInternal(CodeGenContext& context)
{
    Value* rtn = NULL;
    Value *lhs = LHS->codeGen(context);
    Value *rhs = RHS->codeGen(context);
    if (lhs && rhs)
    {
        Type* lhsType = lhs->getType();
        Type* rhsType = rhs->getType();
        if(lhsType != rhsType)
        {
            context.handleCodeGenError(*this, "type mismatch in binary expression");
        }
        else
        {
            //TODO: if this is complex type find the operator function and call the class operator
            //class operators are handled in ClassOperatorDeclarationNode

            IRBuilder<>* builder = context.getIRBuilder();
            switch(token)
            {
                case STAR:
                    //TODO: support object
                    if(lhsType->isFloatingPointTy())
                    {
                        rtn = builder->CreateFMul(lhs, rhs, "multmp");
                    }
                    else if(lhsType->isIntegerTy())
                    {
                        rtn = builder->CreateMul(lhs, rhs, "multmp");
                    }
                break;
                case SLASH:
                    //TODO: support object
                    if(lhsType->isFloatingPointTy())
                    {
                        rtn = builder->CreateFDiv(lhs, rhs, "divtmp");
                    }
                    else if(lhsType->isIntegerTy())
                    {
                        //TODO: consider supporting UDiv
                        rtn = builder->CreateSDiv(lhs, rhs, "divtmp");
                    }
                break;
                case PERCENT:
                    //TODO: support object
                    if(lhsType->isFloatingPointTy())
                    {
                        rtn = builder->CreateFRem(lhs, rhs, "modtmp");
                    }
                    else if(lhsType->isIntegerTy())
                    {
                        //TODO: consider supporting URem
                        rtn = builder->CreateSRem(lhs, rhs, "modtmp");
                    }
                break;
                case PLUS:
                    //TODO: support object
                    if(lhsType->isFloatingPointTy())
                    {
                        rtn = builder->CreateFAdd(lhs, rhs, "addtmp");
                    }
                    else if(lhsType->isIntegerTy())
                    {
                        rtn = builder->CreateAdd(lhs, rhs, "addtmp");
                    }
                break;
                case MINUS:
                    //TODO: support object
                    if(lhsType->isFloatingPointTy())
                    {
                        rtn = builder->CreateFSub(lhs, rhs, "subtmp");
                    }
                    else if(lhsType->isIntegerTy())
                    {
                        rtn = builder->CreateSub(lhs, rhs, "subtmp");
                    }
                break;
                case LTLT:
                    //TODO: support object
                    if(lhsType->isFloatingPointTy())
                    {
                        context.handleCodeGenError(*this, "cannot left shift a floating point number");
                    }
                    else if(lhsType->isIntegerTy())
                    {
                        //TODO: type check that rhsType is not signed? Or atleast not negative?
                        rtn = builder->CreateShl(lhs, rhs, "shltmp");
                    }
                break;
                case GTGT:
                    //TODO: support object
                    if(lhsType->isFloatingPointTy())
                    {
                        context.handleCodeGenError(*this, "cannot right shift a floating point number");
                    }
                    else if(lhsType->isIntegerTy())
                    {
                        //TODO: type check that rhsType is not signed? Or atleast not negative?
                        //TODO: consider adding CreateAShr for the signed varient
                        rtn = builder->CreateLShr(lhs, rhs, "shrtmp");
                    }
                break;
                case LT:
                    //TODO: support object
                    if(lhsType->isFloatingPointTy())
                    {
                        rtn = builder->CreateFCmpULT(lhs, rhs, "fcmplttmp");
                    }
                    else if(lhsType->isIntegerTy())
                    {
                        // TODO: signed vs unsigned
                        rtn = builder->CreateICmpSLT(lhs, rhs, "sicmplttmp");
                    }
                break;
                case GT:
                    //TODO: support object
                    if(lhsType->isFloatingPointTy())
                    {
                        rtn = builder->CreateFCmpUGT(lhs, rhs, "fcmpgttmp");
                    }
                    else if(lhsType->isIntegerTy())
                    {
                        // TODO: signed vs unsigned
                        rtn = builder->CreateICmpSGT(lhs, rhs, "sicmpgttmp");
                    }
                break;
                case LEQ:
                    //TODO: support object
                    if(lhsType->isFloatingPointTy())
                    {
                        rtn = builder->CreateFCmpULE(lhs, rhs, "fcmpletmp");
                    }
                    else if(lhsType->isIntegerTy())
                    {
                        // TODO: signed vs unsigned
                        rtn = builder->CreateICmpSLE(lhs, rhs, "sicmpletmp");
                    }
                break;
                case GEQ:
                    //TODO: support object
                    if(lhsType->isFloatingPointTy())
                    {
                        rtn = builder->CreateFCmpUGE(lhs, rhs, "fcmpgetmp");
                    }
                    else if(lhsType->isIntegerTy())
                    {
                        // TODO: signed vs unsigned
                        rtn = builder->CreateICmpSGE(lhs, rhs, "sicmpgetmp");
                    }
                break;
                case EQEQ:
                    //TODO: support object
                    if(lhsType->isFloatingPointTy())
                    {
                        rtn = builder->CreateFCmpUEQ(lhs, rhs, "fcmpeqtmp");
                    }
                    else if(lhsType->isIntegerTy())
                    {
                        rtn = builder->CreateICmpEQ(lhs, rhs, "icmpeqtmp");
                    }
                break;
                case NOTEQ:
                    //TODO: support object
                    if(lhsType->isFloatingPointTy())
                    {
                        rtn = builder->CreateFCmpUNE(lhs, rhs, "fcmpnetmp");
                    }
                    else if(lhsType->isIntegerTy())
                    {
                        rtn = builder->CreateICmpNE(lhs, rhs, "icmpnetmp");
                    }
                break;
                case AMP:
                    //TODO: support object
                    if(lhsType->isFloatingPointTy())
                    {
                        context.handleCodeGenError(*this, "wrong type for bitwise AND");
                    }
                    else if(lhsType->isIntegerTy())
                    {
                        rtn = builder->CreateAnd(lhs, rhs, "andtmp");
                    }
                break;
                case CARET:
                    //TODO: support object
                    if(lhsType->isFloatingPointTy())
                    {
                        context.handleCodeGenError(*this, "wrong type for bitwise XOR");
                    }
                    else if(lhsType->isIntegerTy())
                    {
                        rtn = builder->CreateXor(lhs, rhs, "xortmp");
                    }
                break;
                case BAR:
                    //TODO: support object
                    if(lhsType->isFloatingPointTy())
                    {
                        context.handleCodeGenError(*this, "wrong type for bitwise OR");
                    }
                    else if(lhsType->isIntegerTy())
                    {
                        rtn = builder->CreateOr(lhs, rhs, "ortmp");
                    }
                break;
                default:
                    context.handleCodeGenError(*this, "Unknown operator for binary expression");
                break;
            }
        }
    }

    return rtn;
}

CodeGenRtn LogicalAndOrNode::codeGenInternal(CodeGenContext& context)
{
    Value* rtn = NULL;
    IRBuilder<>* builder = context.getIRBuilder();
    Value *lhs = LHS->codeGen(context);
    BasicBlock *entryBB = builder->GetInsertBlock();
    if(lhs)
    {
        if(lhs->getType()->isIntegerTy(1))
        {
            switch(token)
            {
                case ANDAND:
                {
                    BasicBlock *rhsBB = BasicBlock::Create(context.getLLVMContext(), "land.rhs", context.currentFunction());
                    BasicBlock *mergeBB = BasicBlock::Create(context.getLLVMContext(), "land.end");

                    builder->CreateCondBr(lhs, rhsBB, mergeBB);
                    builder->SetInsertPoint(rhsBB);

                    Value *rhs = RHS->codeGen(context);
                    if(rhs)
                    {
                        if(rhs->getType()->isIntegerTy(1))
                        {
                            builder->CreateBr(mergeBB);
                            // codeGen of 'rhs' may change the current block, update rhs for the PHI.
                            rhsBB = builder->GetInsertBlock();

                            context.currentFunction()->getBasicBlockList().push_back(mergeBB);
                            builder->SetInsertPoint(mergeBB);

                            PHINode* p = builder->CreatePHI(rhs->getType(), 2, "landtmp");

                            p->addIncoming(lhs, entryBB);
                            p->addIncoming(rhs, rhsBB);
                            rtn = p;
                        }
                        else
                        {
                            context.handleCodeGenError(*this, "Right Hand Side of Logical AND must evaluate to boolean type.");
                        }
                    }
                }
                break;
                case OROR:
                {
                    BasicBlock *rhsBB = BasicBlock::Create(context.getLLVMContext(), "lor.rhs", context.currentFunction());
                    BasicBlock *mergeBB = BasicBlock::Create(context.getLLVMContext(), "lor.end");

                    builder->CreateCondBr(lhs, mergeBB, rhsBB);
                    builder->SetInsertPoint(rhsBB);

                    Value *rhs = RHS->codeGen(context);
                    if(rhs)
                    {
                        if(rhs->getType()->isIntegerTy(1))
                        {
                            builder->CreateBr(mergeBB);
                            // codeGen of 'rhs' may change the current block, update rhs for the PHI.
                            rhsBB = builder->GetInsertBlock();

                            context.currentFunction()->getBasicBlockList().push_back(mergeBB);
                            builder->SetInsertPoint(mergeBB);

                            PHINode* p = builder->CreatePHI(rhs->getType(), 2, "lortmp");

                            p->addIncoming(lhs, entryBB);
                            p->addIncoming(rhs, rhsBB);
                            rtn = p;
                        }
                        else
                        {
                            context.handleCodeGenError(*this, "Right Hand Side of Logical OR must evaluate to boolean type.");
                        }
                    }
                }
                break;
                default:
                    context.handleCodeGenError(*this, "Unknown operator for logical AND|OR expression");
                break;
            }
        }
        else
        {
            context.handleCodeGenError(*this, "Left Hand Side of Logical OR must evaluate to boolean type.");
        }
    }

    return rtn;
}

CodeGenRtn TernaryExpressionNode::codeGenInternal(CodeGenContext& context)
{
    Value* rtn = NULL;
    Value* cond = condition->codeGen(context);
    if(cond)
    {
        if(cond->getType() != IntegerType::getInt1Ty(context.getLLVMContext()))
        {
            context.handleCodeGenError(*this, "Ternary conditional must evaluate to type bool");
        }
        else
        {
            BasicBlock *ifBB = BasicBlock::Create(context.getLLVMContext(), "if", context.currentFunction());
            BasicBlock *elseBB = BasicBlock::Create(context.getLLVMContext(), "else");
            BasicBlock *mergeBB = BasicBlock::Create(context.getLLVMContext(), "ifcont");

            IRBuilder<>* builder = context.getIRBuilder();
            builder->CreateCondBr(cond, ifBB, elseBB);
            builder->SetInsertPoint(ifBB);

            Value* ifV = LHS->codeGen(context);
            if(ifV)
            {
                builder->CreateBr(mergeBB);
                // codeGen of 'if' may change the current block, update ifBB for the PHI.
                ifBB = builder->GetInsertBlock();

                context.currentFunction()->getBasicBlockList().push_back(elseBB);
                builder->SetInsertPoint(elseBB);

                Value* elseV = RHS->codeGen(context);
                if(elseV)
                {
                    builder->CreateBr(mergeBB);
                    // codeGen of 'else' can change the current block, update elseBB for the PHI.
                    elseBB = builder->GetInsertBlock();

                    // Emit merge block
                    context.currentFunction()->getBasicBlockList().push_back(mergeBB);
                    builder->SetInsertPoint(mergeBB);

                    Type* rtnType1 = ifV->getType();
                    Type* rtnType2 = elseV->getType();
                    // TODO: decide what other tests to run here to be sure we are returning a proper value
                    if(rtnType1 == rtnType2)
                    {
                        PHINode* p = builder->CreatePHI(rtnType1, 2, "iftmp");

                        p->addIncoming(ifV, ifBB);
                        p->addIncoming(elseV, elseBB);
                        rtn = p;
                    }
                }
            }
        }
    }

    return rtn;
}

CodeGenRtn AssignmentNode::codeGenInternal(CodeGenContext& context)
{
    Value* rtn = NULL;
    // TODO: must make sure lhs is an lvalue type (variable, a property access, an indexer access) reference: https://msdn.microsoft.com/en-us/library/aa691314(v=vs.71).aspx
    // IdentifierNode, MemberAccessNode, ElementAccessNode

// TODO: use actual reflection system here
    IdentifierNode *identifierNode = static_cast<IdentifierNode *>(unaryExpression.get());
    if (!identifierNode)
    {
        context.handleCodeGenError(*this, "destination of assigment must be a variable");
    }
    else
    {
        SharedSymbolEntry entry = context.getSymbolInScopeByName(*(identifierNode->value));
        Value* target = entry ? entry->llvmVal : NULL;

        if(target)
        {
            Value* exp = expression->codeGen(context);
            if(exp)
            {
                // TODO: handle all types including signed/unsigned
                if(target->getType()->getContainedType(0) == exp->getType())
                {
                    IRBuilder<>* builder = context.getIRBuilder();
                    if(token != EQ)
                    {
                        Type* t = target->getType()->getContainedType(0);
                        Value* lhs = unaryExpression->codeGen(context);
                        switch(token)
                        {
                            case PLUSEQ:
                            {
                                //TODO: support object
                                if(t->isFloatingPointTy())
                                {
                                    // TODO: support float vs double
                                    exp = builder->CreateFAdd(lhs, exp, "pluseq");
                                }
                                else if(t->isIntegerTy())
                                {
                                    // TODO: support signed vs unsigned
                                    exp = builder->CreateAdd(lhs, exp, "pluseq");
                                }
                            }
                            break;
                            case MINUSEQ:
                            {
                                //TODO: support object
                                if(t->isFloatingPointTy())
                                {
                                    exp = builder->CreateFSub(lhs, exp, "minuseq");
                                }
                                else if(t->isIntegerTy())
                                {
                                    exp = builder->CreateSub(lhs, exp, "minuseq");
                                }
                            }
                            break;
                            case STAREQ:
                            {
                                //TODO: support object
                                if(t->isFloatingPointTy())
                                {
                                    exp = builder->CreateFMul(lhs, exp, "muleq");
                                }
                                else if(t->isIntegerTy())
                                {
                                    exp = builder->CreateMul(lhs, exp, "muleq");
                                }
                            }
                            break;
                            case DIVEQ:
                            {
                                //TODO: support object
                                if(t->isFloatingPointTy())
                                {
                                    exp = builder->CreateFDiv(lhs, exp, "diveq");
                                }
                                else if(t->isIntegerTy())
                                {
                                    //TODO: consider supporting UDiv
                                    exp = builder->CreateSDiv(lhs, exp, "diveq");
                                }
                            }
                            break;
                            case MODEQ:
                            {
                                //TODO: support object
                                if(t->isFloatingPointTy())
                                {
                                    exp = builder->CreateFRem(lhs, exp, "modeq");
                                }
                                else if(t->isIntegerTy())
                                {
                                    //TODO: consider supporting URem
                                    exp = builder->CreateSRem(lhs, exp, "modeq");
                                }
                            }
                            break;
                            case XOREQ:
                            {
                                //TODO: support object
                                if(t->isFloatingPointTy())
                                {
                                    context.handleCodeGenError(*this, "wrong type for bitwise XOR assigment");
                                }
                                else if(t->isIntegerTy())
                                {
                                    exp = builder->CreateXor(lhs, exp, "xoreq");
                                }
                            }
                            break;
                            case ANDEQ:
                            {
                                //TODO: support object
                                if(t->isFloatingPointTy())
                                {
                                    context.handleCodeGenError(*this, "wrong type for bitwise AND assigment");
                                }
                                else if(t->isIntegerTy())
                                {
                                    exp = builder->CreateAnd(lhs, exp, "andeq");
                                }
                            }
                            break;
                            case OREQ:
                            {
                                //TODO: support object
                                if(t->isFloatingPointTy())
                                {
                                    context.handleCodeGenError(*this, "wrong type for bitwise OR assigment");
                                }
                                else if(t->isIntegerTy())
                                {
                                    exp = builder->CreateOr(lhs, exp, "oreq");
                                }
                            }
                            break;
                            case GTGTEQ:
                            {
                                //TODO: support object
                                if(t->isFloatingPointTy())
                                {
                                    context.handleCodeGenError(*this, "cannot right shift a floating point number");
                                }
                                else if(t->isIntegerTy())
                                {
                                    //TODO: type check that rhsType is not signed? Or atleast not negative?
                                    //TODO: consider adding CreateAShr for the signed varient
                                    exp = builder->CreateLShr(lhs, exp, "shreq");
                                }
                            }
                            break;
                            case LTLTEQ:
                            {
                                //TODO: support object
                                if(t->isFloatingPointTy())
                                {
                                    context.handleCodeGenError(*this, "cannot left shift a floating point number");
                                }
                                else if(t->isIntegerTy())
                                {
                                    //TODO: type check that rhsType is not signed? Or atleast not negative?
                                    exp = builder->CreateShl(lhs, exp, "shleq");
                                }
                            }
                            break;
                        }
                    }

                    if(exp && !context.isErrorLimitReached())
                    {
                        rtn = builder->CreateStore(exp, target);
                    }
                }
                else
                {
                    context.handleCodeGenError(*this, "type mismatch for assigment");
                }
            }
        }
        else
        {
            context.handleCodeGenError(*this, "unknown lvalue for assigment");
        }
    }

    return rtn;
}

CodeGenRtn ObjectCreationNode::codeGenInternal(CodeGenContext& context)
{
    // std::cout << "Creating ObjectCreationNode: " << endl;

    // type->codeGen(context);
    // for (auto arg : *args) 
    // {
    //     arg->codeGen(context);
    // }

    return NULL;
}

CodeGenRtn InvocationNode::codeGenInternal(CodeGenContext& context)
{
    if(expression)
    {
        //expression->codeGen(context);
        context.handleCodeGenError(*this, "TODO: invocations on expressions are not yet implemented!");
    }
    else if(identifier)
    {
        //identifier->codeGen(context);

        // TODO: change to getMethodInScopeByName passing params as well ?
        SharedSymbolEntry entry = context.getSymbolInScopeByName(*(identifier->value));
        Value* target = entry ? entry->llvmVal : NULL;
        if(target)
        {

        }
        else
        {
            context.handleCodeGenError(*this, "Unknown target of invocation: " + Twine(identifier->value->c_str()));
        }
    }

    // TODO: need to get the mapping of parameter names to index
    //  also need to take default values into account

    // for (auto arg : *args) 
    // {
    //     arg->codeGen(context);
    // }

    return NULL;
}

CodeGenRtn PreIncrDecrNode::codeGenInternal(CodeGenContext& context)
{
    Value* rtn = NULL;
    Value* v = expression->codeGen(context);
    if(v)
    {
        IRBuilder<>* builder = context.getIRBuilder();
        Type* t = v->getType();
        switch(token)
        {
            case PLUSPLUS:
                //TODO: support object
                if(t->isFloatingPointTy())
                {
                    // TODO: support float vs double
                    Value *stepVal = ConstantFP::get(context.getLLVMContext(), APFloat(1.0));
                    rtn = builder->CreateFAdd(v, stepVal, "inc");
                }
                else if(t->isIntegerTy())
                {
                    // TODO: support signed vs unsigned
                    Value *stepVal = getConstantInt(context, t->getPrimitiveSizeInBits(), 1, false);
                    rtn = builder->CreateAdd(v, stepVal, "inc");
                }
            break;
            case MINUSMINUS:
                //TODO: support object
                if(t->isFloatingPointTy())
                {
                    // TODO: support float vs double
                    Value *stepVal = ConstantFP::get(context.getLLVMContext(), APFloat(1.0));
                    rtn = builder->CreateFSub(v, stepVal, "dec");
                }
                else if(t->isIntegerTy())
                {
                    // TODO: support signed vs unsigned
                    Value *stepVal = getConstantInt(context, t->getPrimitiveSizeInBits(), 1, false);
                    rtn = builder->CreateSub(v, stepVal, "dec");
                }
            break;
        }
    }

    return rtn;
}

CodeGenRtn PostIncrDecrNode::codeGenInternal(CodeGenContext& context)
{
    Value* rtn = NULL;

    // TODO: Decide if we really want a post increment
    // if so we need to return alloca from expression
    context.handleCodeGenError(*this, "Post Inc/dec not currently supported");

    return rtn;
}

CodeGenRtn ClassDeclarationNode::codeGenInternal(CodeGenContext& context)
{
    // std::cout << "Creating ClassDeclarationNode: " << endl;

    // for (auto mod : *modifiers) 
    // {
    //     mod->codeGen(context);
    // }

    // name->codeGen(context);

    // if(baseTypes)
    // {
    //     baseTypes->codeGen(context);
    // }

    // for (auto member : *members) 
    // {
    //     member->codeGen(context);
    // }

    return NULL;
}

CodeGenRtn ClassBaseDeclarationNode::codeGenInternal(CodeGenContext& context)
{
    // std::cout << "Creating ClassBaseDeclarationNode: " << endl;

    // if(base)
    // {
    //     base->codeGen(context);
    // }

    // for (auto interface : *interfaces) 
    // {
    //     interface->codeGen(context);
    // }

    return NULL;
}

CodeGenRtn ClassMemberDeclarationNode::codeGenInternal(CodeGenContext& context)
{
    std::cout << "Creating ClassMemberDeclarationNode: " << endl;
    return NULL;
}

CodeGenRtn ClassConstDeclarationNode::codeGenInternal(CodeGenContext& context)
{
    // std::cout << "Creating ClassConstDeclarationNode: " << endl;

    // for (auto mod : *modifiers) 
    // {
    //     mod->codeGen(context);
    // }

    // type->codeGen(context);

    // for (auto declarator : *declarators) 
    // {
    //     declarator->codeGen(context);
    // }

    return NULL;
}

CodeGenRtn ClassFieldDeclarationNode::codeGenInternal(CodeGenContext& context)
{
    // std::cout << "Creating ClassFieldDeclarationNode: " << endl;

    // for (auto mod : *modifiers) 
    // {
    //     mod->codeGen(context);
    // }

    // type->codeGen(context);

    // for (auto declarator : *declarators) 
    // {
    //     declarator->codeGen(context);
    // }

    return NULL;
}

CodeGenRtn ClassMethodDeclarationNode::codeGenInternal(CodeGenContext& context)
{
    // std::cout << "Creating ClassMethodDeclarationNode: " << endl;

    // for (auto mod : *modifiers) 
    // {
    //     mod->codeGen(context);
    // }

    // returnType->codeGen(context);
    // name->codeGen(context);

    // for (auto param : *params) 
    // {
    //     param->codeGen(context);
    // }

    // if(body)
    // {
    //     body->codeGen(context);
    // }

    return NULL;
}

CodeGenRtn ClassOperatorDeclarationNode::codeGenInternal(CodeGenContext& context)
{
    // std::cout << "Creating ClassOperatorDeclarationNode: " << endl;

    // for (auto mod : *modifiers) 
    // {
    //     mod->codeGen(context);
    // }

    // operatorDeclarator->codeGen(context);

    // if(body)
    // {
    //     body->codeGen(context);
    // }

    return NULL;
}

CodeGenRtn ClassOperatorDeclaratorNode::codeGenInternal(CodeGenContext& context)
{
    // std::cout << "Creating ClassOperatorDeclaratorNode: " << opToken << endl;

    // returnType->codeGen(context);

    // param1Type->codeGen(context);
    // param1Name->codeGen(context);

    // if(param2Type && param2Name)
    // {
    //     param2Type->codeGen(context);
    //     param2Name->codeGen(context);
    // }

    return NULL;
}

CodeGenRtn ClassConstructorDeclarationNode::codeGenInternal(CodeGenContext& context)
{
    // std::cout << "Creating ClassConstructorDeclarationNode: " << endl;

    // for (auto mod : *modifiers) 
    // {
    //     mod->codeGen(context);
    // }

    // declarator->codeGen(context);

    // if(body)
    // {
    //     body->codeGen(context);
    // }

    return NULL;
}

CodeGenRtn ClassConstructorDeclaratorNode::codeGenInternal(CodeGenContext& context)
{
    // std::cout << "Creating ClassConstructorDeclaratorNode: " << endl;

    // constructorName->codeGen(context);

    // for (auto param : *params) 
    // {
    //     param->codeGen(context);
    // }

    // if(initializer)
    // {
    //     initializer->codeGen(context);
    // }

    return NULL;
}

CodeGenRtn ClassConstructorInitializerNode::codeGenInternal(CodeGenContext& context)
{
    // std::cout << "Creating ClassConstructorInitializerNode: " << endl;

    // for (auto arg : *args) 
    // {
    //     arg->codeGen(context);
    // }

    return NULL;
}

CodeGenRtn ClassDestructorDeclarationNode::codeGenInternal(CodeGenContext& context)
{
    // std::cout << "Creating ClassDestructorDeclarationNode: " << endl;

    // for (auto mod : *modifiers) 
    // {
    //     mod->codeGen(context);
    // }

    // destructorName->codeGen(context);
    // body->codeGen(context);

    return NULL;
}

CodeGenRtn EnumDeclarationNode::codeGenInternal(CodeGenContext& context)
{
    // for (auto modifier : *modifiers) 
    // {
    //     modifier->codeGen(context);
    // }

    // identifier->codeGen(context);

    // for (auto decl : *body) 
    // {
    //     decl->codeGen(context);
    // }

    return NULL;
}

CodeGenRtn EnumMemberDeclarationNode::codeGenInternal(CodeGenContext& context)
{
    // identifier->codeGen(context);
    // if(constantExpression)
    // {
    //     constantExpression->codeGen(context);
    // }

    return NULL;
}

CodeGenRtn InterfaceDeclarationNode::codeGenInternal(CodeGenContext& context)
{
    // std::cout << "Creating InterfaceDeclarationNode: " << endl;

    // for (auto mod : *modifiers) 
    // {
    //     mod->codeGen(context);
    // }
    // identifier->codeGen(context);
    // for (auto baseType : *baseTypes) 
    // {
    //     baseType->codeGen(context);
    // }
    // for (auto funcDecl : *body) 
    // {
    //     funcDecl->codeGen(context);
    // }

    return NULL;
}

//------------------------------------------------------------------------------ 
//                              Debug AST Printing
//------------------------------------------------------------------------------

void CompilationUnit::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "CompilationUnit:" << name->c_str() << endl;

    if( nameSpace )
    {
        debugPrintPart(context, nameSpace, prefix);
    }

    for (auto usingDecl : *usingDeclarationList) 
    {
        debugPrintPart(context, usingDecl, prefix);
    }

    for (auto codeDecl : *codeDeclarationList) 
    {
        debugPrintPart(context, codeDecl, prefix);
    }
}

void NamespaceDeclarationNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "NamespaceDeclarationNode" << endl;
    debugPrintPart(context, name, prefix);
}

void UsingDeclarationNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "UsingDeclarationNode" << endl;
    debugPrintPart(context, identifier, prefix);

    if(alias)
    {
        debugPrintPart(context, alias, prefix);
    }
}

void Int8Node::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "Int8Node:" << value << endl;
}

void Int16Node::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "Int16Node:" << value << endl;
}

void Int32Node::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "Int32Node:" << value << endl;
}

void Int64Node::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "Int64Node:" << value << endl;
}

void UInt8Node::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "UInt8Node:" << value << endl;
}

void UInt16Node::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "UInt16Node " << value << endl;
}

void UInt32Node::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "UInt32Node:" << value << endl;
}

void UInt64Node::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "UInt64Node:" << value << endl;
}

void Float32Node::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "Float32Node:" << value << endl;
}

void Float64Node::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "Float64Node:" << value << endl;
}

void StringNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "StringNode" << endl;
}

void BooleanNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "BooleanNode:" << value << endl;
}

void NullNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "NullNode" << endl;
}

void IdentifierNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "IdentifierNode:";

    for (auto qualif : *qualifier) 
    {
        stream << qualif->c_str() << ".";
    }

    stream << value->c_str();

    if(generic)
    {
        stream << "<" << generic->c_str() << ">";
    }

    stream << endl;
}

void ModifierNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "ModifierNode:";
    stream << value->c_str();

    int count = targets->size();
    if(count > 0)
    {
        int i = 0;
        stream << " Targets: [";
        for (auto target : *targets) 
        {
            ++i;
            debugPrintPart(context, target, prefix);
            if(i != count)
            {
                stream << ", ";
            }
        }
        stream << "]";
    }

    stream << endl;
}

void FunctionDeclarationNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "FunctionDeclarationNode" << endl;
    
    if(modifier)
    {
        debugPrintPart(context, modifier, prefix);
    }

    debugPrintPart(context, returnType, prefix);
    debugPrintPart(context, name, prefix);

    for (auto param : *parameters) 
    {
        debugPrintPart(context, param, prefix);
    }

    if(block)
    {
        debugPrintPart(context, block, prefix);
    }
}

void FunctionParameterNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "FunctionParameterNode" << endl;
    if(modifier)
    {
        debugPrintPart(context, modifier, prefix);
    }

    debugPrintPart(context, type, prefix);
    debugPrintPart(context, identifier, prefix);
}

void BlockNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "BlockNode" << endl;
    for (auto statement : *statements) 
    {
        debugPrintPart(context, statement, prefix);
    }
}

void VariableDeclarator::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "VariableDeclarator" << endl;
    debugPrintPart(context, name, prefix);

    if(initializer)
    {
        debugPrintPart(context, initializer, prefix);
    }
}

void ConstVariableDeclarator::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "ConstVariableDeclarator" << endl;
    debugPrintPart(context, name, prefix);
    if(initializer)
    {
        debugPrintPart(context, initializer, prefix);
    }
}

void LocalVariableDeclaration::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "LocalVariableDeclaration" << endl;
    debugPrintPart(context, type, prefix);
    for (auto variable : *variables) 
    {
        debugPrintPart(context, variable, prefix);
    }
}

void ConstLocalVariableDeclaration::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "ConstLocalVariableDeclaration" << endl;
    debugPrintPart(context, type, prefix);

    for (auto variable : *variables) 
    {
        debugPrintPart(context, variable, prefix);
    }
}

void IfNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "IfNode" << endl;
    debugPrintPart(context, booleanExpression, prefix);
    debugPrintPart(context, ifStatement, prefix);

    if(elseStatement)
    {
        debugPrintPart(context, elseStatement, prefix);
    }
}

void SwitchNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "SwitchNode" << endl;
    debugPrintPart(context, expression, prefix);
    for (auto section : *switchsections) 
    {
        debugPrintPart(context, section, prefix);
    }
}

void SwitchSectionNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "SwitchSectionNode" << endl;
    for (auto label : *labels) 
    {
        debugPrintPart(context, label, prefix);
    }

    for (auto statement : *statementList) 
    {
        debugPrintPart(context, statement, prefix);
    }
}

void SwitchLabelNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "SwitchLabelNode" << endl;
    debugPrintPart(context, constantExpression, prefix);
}

void WhileNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "WhileNode" << endl;
    debugPrintPart(context, booleanExpression, prefix);
    debugPrintPart(context, whileStatement, prefix);
}

void DoWhileNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "DoWhileNode" << endl;
    debugPrintPart(context, booleanExpression, prefix);
    debugPrintPart(context, doWhileStatement, prefix);
}

void ForNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "ForNode" << endl;

    for (auto initializerStatement : *initializerStatements) 
    {
        debugPrintPart(context, initializerStatement, prefix);
    }
    debugPrintPart(context, booleanExpression, prefix);

    for (auto iteratorStatement : *iteratorStatements) 
    {
        debugPrintPart(context, iteratorStatement, prefix);
    }

    debugPrintPart(context, body, prefix);
}

void ForEachNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "ForEachNode" << endl;
    debugPrintPart(context, type, prefix);
    debugPrintPart(context, name, prefix);
    debugPrintPart(context, expression, prefix);
    debugPrintPart(context, body, prefix);
}

void BreakNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "BreakNode" << endl;
}

void ContinueNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "ContinueNode" << endl;
}

void ReturnNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "ReturnNode" << endl;
    debugPrintPart(context, expression, prefix);
}

void MemberAccessNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "MemberAccessNode" << endl;
    debugPrintPart(context, identifier, prefix);
    if(expression)
    {
        debugPrintPart(context, expression, prefix);
    }
    else if(classType)
    {
        debugPrintPart(context, classType, prefix);
    }
}

void ArgumentNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "ArgumentNode" << endl;
    debugPrintPart(context, name, prefix);

    if(modifier)
    {
        debugPrintPart(context, modifier, prefix);
    }
    debugPrintPart(context, expression, prefix);
}

void ElementAccessNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "ElementAccessNode" << endl;
    if(identifier)
    {
        debugPrintPart(context, identifier, prefix);
    } 
    else if( expression )
    {
        debugPrintPart(context, expression, prefix);
    }

    for (auto express : *expressionlist) 
    {
        debugPrintPart(context, express, prefix);
    }
}

void ThisAccessNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "ThisAccessNode" << endl;
}

void BaseAccessNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "BaseAccessNode" << endl;
    if(identifier)
    {
        debugPrintPart(context, identifier, prefix);
    }
    else if(expressionlist)
    {
        for (auto expression : *expressionlist) 
        {
            debugPrintPart(context, expression, prefix);
        }
    }
}

void SimpleUnaryExpressionNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "SimpleUnaryExpressionNode" << endl;
    debugPrintPart(context, expression, prefix);
}

void CastNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "CastNode" << endl;
    debugPrintPart(context, type, prefix);
    debugPrintPart(context, unaryExpression, prefix);
}

void BinaryExpressionNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "BinaryExpressionNode" << endl;
    debugPrintPart(context, LHS, prefix);
    debugPrintPart(context, RHS, prefix);
}

void LogicalAndOrNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "LogicalAndOrNode" << endl;
    debugPrintPart(context, LHS, prefix);
    debugPrintPart(context, RHS, prefix);
}

void TernaryExpressionNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "TernaryExpressionNode" << endl;
    debugPrintPart(context, condition, prefix);
    debugPrintPart(context, LHS, prefix);
    debugPrintPart(context, RHS, prefix);
}

void AssignmentNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "AssignmentNode" << endl;
    debugPrintPart(context, unaryExpression, prefix);
    debugPrintPart(context, expression, prefix);
}

void ObjectCreationNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "ObjectCreationNode" << endl;
    debugPrintPart(context, type, prefix);

    for (auto arg : *args) 
    {
        debugPrintPart(context, arg, prefix);
    }
}

void InvocationNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "InvocationNode" << endl;
    if(expression)
    {
        debugPrintPart(context, expression, prefix);
    }
    else if(identifier)
    {
        debugPrintPart(context, identifier, prefix);
    }

    for (auto arg : *args) 
    {
        debugPrintPart(context, arg, prefix);
    }
}

void PreIncrDecrNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "PreIncrDecrNode" << endl;
    debugPrintPart(context, expression, prefix);
}

void PostIncrDecrNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "PostIncrDecrNode" << endl;
    debugPrintPart(context, expression, prefix);
}

void ClassDeclarationNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "ClassDeclarationNode" << endl;
    for (auto mod : *modifiers) 
    {
        debugPrintPart(context, mod, prefix);
    }

    debugPrintPart(context, name, prefix);

    if(baseTypes)
    {
        debugPrintPart(context, baseTypes, prefix);
    }

    for (auto member : *members) 
    {
        debugPrintPart(context, member, prefix);
    }
}

void ClassBaseDeclarationNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "ClassBaseDeclarationNode" << endl;
    if(base)
    {
        debugPrintPart(context, base, prefix);
    }

    for (auto interface : *interfaces) 
    {
        debugPrintPart(context, interface, prefix);
    }
}

void ClassMemberDeclarationNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "ClassMemberDeclarationNode" << endl;
}

void ClassConstDeclarationNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "ClassConstDeclarationNode" << endl;
    for (auto mod : *modifiers) 
    {
        debugPrintPart(context, mod, prefix);
    }

    debugPrintPart(context, type, prefix);

    for (auto declarator : *declarators) 
    {
        debugPrintPart(context, declarator, prefix);
    }
}

void ClassFieldDeclarationNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "ClassFieldDeclarationNode" << endl;
    for (auto mod : *modifiers) 
    {
        debugPrintPart(context, mod, prefix);
    }

    debugPrintPart(context, type, prefix);

    for (auto declarator : *declarators) 
    {
        debugPrintPart(context, declarator, prefix);
    }
}

void ClassMethodDeclarationNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "ClassMethodDeclarationNode" << endl;
    for (auto mod : *modifiers) 
    {
        debugPrintPart(context, mod, prefix);
    }

    debugPrintPart(context, returnType, prefix);
    debugPrintPart(context, name, prefix);

    for (auto param : *params) 
    {
        debugPrintPart(context, param, prefix);
    }

    if(body)
    {
        debugPrintPart(context, body, prefix);
    }
}

void ClassOperatorDeclarationNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "ClassOperatorDeclarationNode" << endl;
    for (auto mod : *modifiers) 
    {
        debugPrintPart(context, mod, prefix);
    }

    debugPrintPart(context, operatorDeclarator, prefix);

    if(body)
    {
        debugPrintPart(context, body, prefix);
    }
}

void ClassOperatorDeclaratorNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "ClassOperatorDeclaratorNode" << endl;
    debugPrintPart(context, returnType, prefix);

    debugPrintPart(context, param1Type, prefix);
    debugPrintPart(context, param1Name, prefix);

    if(param2Type && param2Name)
    {
        debugPrintPart(context, param2Type, prefix);
        debugPrintPart(context, param2Name, prefix);
    }
}

void ClassConstructorDeclarationNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "ClassConstructorDeclarationNode" << endl;
    for (auto mod : *modifiers) 
    {
        debugPrintPart(context, mod, prefix);
    }

    debugPrintPart(context, declarator, prefix);

    if(body)
    {
        debugPrintPart(context, body, prefix);
    }
}

void ClassConstructorDeclaratorNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "ClassConstructorDeclaratorNode" << endl;
    debugPrintPart(context, constructorName, prefix);

    for (auto param : *params) 
    {
        debugPrintPart(context, param, prefix);
    }

    if(initializer)
    {
        debugPrintPart(context, initializer, prefix);
    }
}

void ClassConstructorInitializerNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "ClassConstructorInitializerNode" << endl;
    for (auto arg : *args) 
    {
        debugPrintPart(context, arg, prefix);
    }
}

void ClassDestructorDeclarationNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "ClassDestructorDeclarationNode" << endl;
    for (auto mod : *modifiers) 
    {
        debugPrintPart(context, mod, prefix);
    }

    debugPrintPart(context, destructorName, prefix);
    debugPrintPart(context, body, prefix);
}

void EnumDeclarationNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "EnumDeclarationNode" << endl;
    for (auto modifier : *modifiers) 
    {
        debugPrintPart(context, modifier, prefix);
    }

    debugPrintPart(context, identifier, prefix);

    for (auto decl : *body) 
    {
        debugPrintPart(context, decl, prefix);
    }
}

void EnumMemberDeclarationNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "EnumMemberDeclarationNode" << endl;
    debugPrintPart(context, identifier, prefix);
    if(constantExpression)
    {
        debugPrintPart(context, constantExpression, prefix);
    }
}

void InterfaceDeclarationNode::debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix)
{
    stream << prefix.str() << "InterfaceDeclarationNode" << endl;
    for (auto mod : *modifiers) 
    {
        debugPrintPart(context, mod, prefix);
    }
    debugPrintPart(context, identifier, prefix);
    for (auto baseType : *baseTypes) 
    {
        debugPrintPart(context, baseType, prefix);
    }
    for (auto funcDecl : *body) 
    {
        debugPrintPart(context, funcDecl, prefix);
    }
}