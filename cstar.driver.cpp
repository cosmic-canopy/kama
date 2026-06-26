#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include <execinfo.h>
#include <signal.h>

#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/ManagedStatic.h"
#include "cstar.parser.hpp"
#include "cstar.lexer.hpp"
#include "cstar.codegen.h"

using namespace llvm;

//------------------------------------------------------------------------------ 
//                              Commandline Options
//------------------------------------------------------------------------------

cl::OptionCategory CompilerCategory("Compiler Options", "Options for controlling the compilation process.");

cl::opt<std::string> OutputFilename("o", cl::desc("Output filename"), cl::value_desc("filename"), cl::cat(CompilerCategory));
cl::list<std::string>  InputFilenames("i", cl::desc("Input files"), cl::value_desc("filenames"), cl::OneOrMore, cl::cat(CompilerCategory));
cl::opt<bool> PrintSupportedTargets("t", cl::desc("Print Supported Targets"), cl::cat(CompilerCategory));
cl::opt<bool> PrintAST("ast", cl::desc("Print AST"), cl::cat(CompilerCategory));

cl::opt<OptimizationLevel> OptimizationLevel(cl::desc("Choose optimization level:"),
  cl::values(
   clEnumValN(Debug, "g", "No optimizations, enable debugging"),
    clEnumVal(O1        , "Enable trivial optimizations"),
    clEnumVal(O2        , "Enable default optimizations"),
    clEnumVal(O3        , "Enable expensive optimizations"),
   clEnumValEnd));

// TODO: Add linker options

//------------------------------------------------------------------------------ 
//                              Main Driver Entry
//------------------------------------------------------------------------------
void handler(int sig) 
{
    const int bufferSize = 20;
    void *buffer[bufferSize];
    size_t size;

    // get void*'s for all entries on the stack
    size = backtrace(buffer, bufferSize);

    // print out all the frames to stderr
    fprintf(stderr, "Error: signal %d:\n", sig);
    backtrace_symbols_fd(buffer, size, STDERR_FILENO);
    exit(1);
}

int main(int argc, char *argv[])
{
    cl::HideUnrelatedOptions( CompilerCategory );
    cl::ParseCommandLineOptions(argc, argv, " CStar compiler\n");

    signal(SIGSEGV, handler);   // install our handler

    int rtn = 0;
    //yydebug=1;

    raw_ostream *llvmOut = &outs();

    if(OutputFilename != "")
    {
        std::error_code EC;
        llvmOut = new raw_fd_ostream(OutputFilename, EC, sys::fs::OpenFlags::F_None);
    }

    //TODO: Decide what initializers we need and what cli options control them
    LLVMInitializeAllTargetInfos();
    LLVMInitializeAllTargets();
    LLVMInitializeAllAsmPrinters();
    LLVMInitializeAllAsmParsers();

    if(PrintSupportedTargets)
    {
        TargetRegistry::printRegisteredTargetsForVersion();
    }

    for (auto inputfile : InputFilenames) 
    {
        yyscan_t myscanner;
        struct LexerInstanceData extra = {
            CSTAR_LEXERINSTANCE_DEFAULT_LINE_ONE,
            CSTAR_LEXERINSTANCE_DEFAULT_COLUMN_ONE,
            NULL,
            CreateCodegenContext(std::make_shared<std::string>(inputfile.c_str()), OptimizationLevel),
            NULL};
        yylex_init_extra( &extra, &myscanner);
        
        FILE *input = yyget_in(myscanner);
        FILE *output = yyget_out(myscanner); // TODO: where do we want to redirect this?

        input = fopen(inputfile.c_str(),"r");
        if (input == NULL)
        {
            fprintf(stderr,"ERROR - cannot open input file\n");
            rtn = 1;
        }
        else
        {
            yy_switch_to_buffer( yy_create_buffer( input, YY_BUF_SIZE, myscanner ), myscanner);
        }

        if(rtn == 0)
        {
            rtn = yyparse(myscanner);
        }

        yylex_destroy(myscanner);

        if(rtn == 0)
        {
            if(PrintAST)
            {
                extra.codeGenContext->printDebugAST(extra.compilationUnit);
            }

            extra.codeGenContext->generateCode(extra.compilationUnit);

            if(OptimizationLevel != Debug)
            {
                extra.codeGenContext->runOptimizationPasses();
            }
            else
            {
                extra.codeGenContext->finalizeDebugInfo();
            }

            extra.codeGenContext->dumpModule();
            extra.codeGenContext->writeBitcodeToFile(llvmOut);
        }
    }

    if (llvmOut != &outs())
        delete llvmOut;

    llvm_shutdown();

    return rtn;
}


