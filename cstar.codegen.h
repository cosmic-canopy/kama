#ifndef __CSTAR_CODEGEN_H__
#define __CSTAR_CODEGEN_H__

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/ExecutionEngine/GenericValue.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "cstar.forward.h"
#include <cctype>
#include <cstdio>
#include <string>
#include <memory>
#include <vector>
#include <map>

typedef std::shared_ptr<llvm::LLVMContext>      SharedLLVMContext;
typedef std::unique_ptr<llvm::Module>           UniqueModule;
typedef std::unique_ptr<llvm::DIBuilder>        UniqueDIBuilder;
typedef std::unique_ptr<llvm::IRBuilder<> >     UniqueIRBuilder;

enum OptimizationLevel {
  Debug, O1, O2, O3
};

class CstarTypeInfo
{
public:

};

class SymbolEntry
{
public:
    SymbolEntry(llvm::Value* val, SharedAST node)
    : llvmVal(val)
    , node(node)
    {
    }
    llvm::Value*    llvmVal;
    SharedAST       node;
};

class CodeGenBlock 
{
public:
    CodeGenBlock(llvm::Function* function, llvm::DIScope* debugScope)
    : function(function)
    , locals()
    , debugScope(debugScope)
    , breakTarget(NULL)
    , continueTarget(NULL)
    {
    }
    llvm::Function*      function;
    NameValueMap         locals;
    llvm::DIScope*       debugScope;
    llvm::BasicBlock*    breakTarget;
    llvm::BasicBlock*    continueTarget;
};

class CodeGenContext 
{
    int                  _mMaxErrorCount;
    int                  _mCurrentErrorCount;
    CodeGenBlockList     _mScopeContextList;
    llvm::Function*      _mMainFunction;
    SharedString         _mNamespace;
    NameValueMap         _mModuleSymbols;

    // Debug Info
    llvm::DICompileUnit* _mDCompileUnit;
    llvm::DIFile*        _mDGlobalScope;

public:
    int                  line;
    int                  col;
    OptimizationLevel    opt;

    SharedString         _mName;
    SharedLLVMContext    _mContext;
    UniqueModule         _mModule;
    UniqueIRBuilder      _mBuilder;
    UniqueDIBuilder      _mDebugBuilder;

    CodeGenContext(SharedString moduleName, OptimizationLevel opt, int maxErrorCount = 10);
    ~CodeGenContext();
    
    bool isErrorLimitReached(){ return _mCurrentErrorCount >= _mMaxErrorCount; }
    bool handleCodeGenError(ASTNode& node, const llvm::Twine &error);
    bool handleError(int line, int column, llvm::StringRef section, const llvm::Twine &error);

    SharedString getModuleName() { return _mName; }
    llvm::Module* getModule() { return _mModule.get(); }
    llvm::LLVMContext& getLLVMContext(){ return *(_mContext.get()); }
    llvm::IRBuilder<>* getIRBuilder(){ return _mBuilder.get();}
    llvm::DIBuilder* getDIBuilder(){ return _mDebugBuilder.get(); }

    llvm::DICompileUnit* getDebugCompileUnit(){ return _mDCompileUnit; }
    llvm::DIFile* getDebugFile(){ return _mDGlobalScope; }

    bool hasMainFunction(){ return _mMainFunction != NULL; };
    llvm::Function* addMainFunction(llvm::Function* cstarMain);

    void runOptimizationPasses();
    void emitDebugLocation(ASTNode* node);
    void finalizeDebugInfo();
    void verifyContextModule();
    void verifyContextFunction(llvm::Function* func, llvm::StringRef name);
    void writeBitcodeToFile(llvm::raw_ostream *llvmOut){ llvm::WriteBitcodeToFile(_mModule.get(), *llvmOut); }
    void dumpModule(){ _mModule->dump(); }

    void generateCode(SharedCompilationUnit root);
    void printDebugAST(SharedCompilationUnit root);

    // TODO: add this when supporting JIT
    llvm::GenericValue runCode();

    void pushScopeContext(llvm::Function* functionCtx, llvm::DIScope* debugScope) { SharedCodeGenBlock codegenBlock = std::make_shared<CodeGenBlock>(functionCtx, debugScope); _mScopeContextList.push_back(codegenBlock); }
    void popScopeContext() { _mScopeContextList.pop_back(); }

    llvm::BasicBlock *getBreakTarget();
    void setBreakTarget(llvm::BasicBlock* target){ _mScopeContextList.back()->breakTarget = target; }
    llvm::BasicBlock *getContinueTarget();
    void setContinueTarget(llvm::BasicBlock* target){ _mScopeContextList.back()->continueTarget = target; }
    SharedSymbolEntry getSymbolInScopeByName( llvm::StringRef name );
    bool addSymbolToScope( std::string name, SharedSymbolEntry symbol );
    llvm::Function* currentFunction() { return _mScopeContextList.back()->function; }
    llvm::DIScope* currentDebugScope() { return (_mScopeContextList.size() > 0) ? _mScopeContextList.back()->debugScope : _mDGlobalScope; }
};

#endif //__CSTAR_CODEGEN_H__