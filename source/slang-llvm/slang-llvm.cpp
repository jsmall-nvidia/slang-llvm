
#include "clang/Basic/Stack.h"
#include "clang/Basic/TargetOptions.h"
#include "clang/CodeGen/ObjectFilePCHContainerOperations.h"
#include "clang/Config/config.h"
#include "clang/Driver/DriverDiagnostic.h"
#include "clang/Driver/Options.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendDiagnostic.h"
#include "clang/Frontend/TextDiagnosticBuffer.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Frontend/Utils.h"
#include "clang/FrontendTool/Utils.h"

#include "clang/Lex/PreprocessorOptions.h"

#include "clang/Frontend/FrontendAction.h"
#include "clang/CodeGen/CodeGenAction.h"
#include "clang/Basic/Version.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/LinkAllPasses.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Support/BuryPointer.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/Timer.h"

#include "llvm/Support/raw_ostream.h"

#include "llvm/Target/TargetMachine.h"

// Jit
#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/ExecutionEngine/JITLink/JITLinkMemoryManager.h"

#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"

#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"

#include "llvm/ExecutionEngine/JITSymbol.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IRReader/IRReader.h"

// Slang

#include <slang.h>
#include <slang-com-helper.h>
#include <slang-com-ptr.h>

#include <core/slang-list.h>
#include <core/slang-string.h>

#include <compiler-core/slang-downstream-compiler.h>

#include <stdio.h>

// We want to make math functions available to the JIT
#if SLANG_GCC_FAMILY && __GNUC__ < 6
#   include <cmath>
#   define SLANG_LLVM_STD std::
#else
#   include <math.h>
#   define SLANG_LLVM_STD
#endif

namespace slang_llvm {

using namespace clang;

using namespace llvm::opt;
using namespace llvm;
using namespace llvm::orc;

using namespace Slang;

class LLVMDownstreamCompiler : public DownstreamCompiler
{
public:
    typedef DownstreamCompiler Super;

    /// Compile using the specified options. The result is in resOut
    virtual SlangResult compile(const CompileOptions& options, RefPtr<DownstreamCompileResult>& outResult) SLANG_OVERRIDE;
    virtual SlangResult disassemble(SlangCompileTarget sourceBlobTarget, const void* blob, size_t blobSize, ISlangBlob** out) SLANG_OVERRIDE;
    virtual bool isFileBased() SLANG_OVERRIDE { return false; }

    LLVMDownstreamCompiler()
    {
        Desc desc;

        desc.type = SLANG_PASS_THROUGH_LLVM;
        desc.majorVersion = LLVM_VERSION_MAJOR;
        desc.minorVersion = LLVM_VERSION_MINOR;

        m_desc = desc;
    }
};

SlangResult LLVMDownstreamCompiler::disassemble(SlangCompileTarget sourceBlobTarget, const void* blob, size_t blobSize, ISlangBlob** out)
{
    return SLANG_E_NOT_IMPLEMENTED;
}

class LLVMDownstreamCompileResult : public ISlangSharedLibrary, public DownstreamCompileResult
{
public:
    typedef DownstreamCompileResult Super;

    // ISlangUnknown
    SLANG_REF_OBJECT_IUNKNOWN_QUERY_INTERFACE
    SLANG_REF_OBJECT_IUNKNOWN_ADD_REF
    SLANG_REF_OBJECT_IUNKNOWN_RELEASE

    // ISlangSharedLibrary impl
    virtual SLANG_NO_THROW void* SLANG_MCALL findSymbolAddressByName(char const* name) SLANG_OVERRIDE;

    // DownstreamCompileResult impl
    virtual SlangResult getHostCallableSharedLibrary(ComPtr<ISlangSharedLibrary>& outLibrary) SLANG_OVERRIDE;
    virtual SlangResult getBinary(ComPtr<ISlangBlob>& outBlob) SLANG_OVERRIDE { return SLANG_E_NOT_IMPLEMENTED;  }

    LLVMDownstreamCompileResult(DownstreamDiagnostics& diagnostics,
        std::unique_ptr<llvm::orc::LLJIT> jit) :
        Super(diagnostics),
        m_jit(std::move(jit))
    {
    }

protected:
    ISlangUnknown* LLVMDownstreamCompileResult::getInterface(const Guid& guid);

    std::unique_ptr<llvm::orc::LLJIT> m_jit;
};

SLANG_NO_THROW void* SLANG_MCALL LLVMDownstreamCompileResult::findSymbolAddressByName(char const* name) 
{
    auto fnExpected = m_jit->lookup(name);
    if (fnExpected)
    {
        auto fn = std::move(*fnExpected);
        return (void*)fn.getAddress();
    }
    return nullptr;
}

SlangResult LLVMDownstreamCompileResult::getHostCallableSharedLibrary(ComPtr<ISlangSharedLibrary>& outLibrary)
{
    outLibrary = this;
    return SLANG_OK;
}

ISlangUnknown* LLVMDownstreamCompileResult::getInterface(const Guid& guid)
{
    return guid == ISlangUnknown::getTypeGuid() || guid == ISlangSharedLibrary::getTypeGuid() ? static_cast<ISlangSharedLibrary*>(this) : nullptr;
}


static void _ensureSufficientStack() {}

static void _llvmErrorHandler(void* userData, const std::string& message, bool genCrashDiag)
{
    DiagnosticsEngine& diags = *static_cast<DiagnosticsEngine*>(userData);
    diags.Report(diag::err_fe_error_backend) << message;

    // Run the interrupt handlers to make sure any special cleanups get done, in
    // particular that we remove files registered with RemoveFileOnSignal.
    llvm::sys::RunInterruptHandlers();

    // We cannot recover from llvm errors.  (!)
    // 
    // Returning nothing, will still cause LLVM to exit the process.
}

static Slang::DownstreamDiagnostic::Severity _getSeverity(DiagnosticsEngine::Level level)
{
    typedef DownstreamDiagnostic::Severity Severity;
    typedef DiagnosticsEngine::Level Level;
    switch (level)
    {
        default:
        case Level::Ignored:
        case Level::Note:
        case Level::Remark:
        {
            return Severity::Info;
        }
        case Level::Warning:
        {
            return Severity::Warning;
        }
        case Level::Error:
        case Level::Fatal:
        {
            return Severity::Error;
        }
    }
}

class BufferedDiagnosticConsumer : public clang::DiagnosticConsumer
{
public:

    void HandleDiagnostic(DiagnosticsEngine::Level level, const Diagnostic& info) override
    {

        SmallString<100> text;
        info.FormatDiagnostic(text);

        DownstreamDiagnostic diagnostic;
        diagnostic.severity = _getSeverity(level);
        diagnostic.stage = DownstreamDiagnostic::Stage::Compile;
        diagnostic.text = text.c_str();

        auto location = info.getLocation();

        // Work out what the location is
        auto& sourceManager = info.getSourceManager();

        // Gets the file/line number 
        const bool useLineDirectives = true;
        const PresumedLoc presumedLoc = sourceManager.getPresumedLoc(location, useLineDirectives);

        diagnostic.fileLine = presumedLoc.getLine();
        diagnostic.filePath = presumedLoc.getFilename();

        m_diagnostics.diagnostics.add(diagnostic);
    }

    bool hasError() const { return m_diagnostics.getCountByMinSeverity(DownstreamDiagnostic::Severity::Error) > 0; }

    DownstreamDiagnostics m_diagnostics;
};

/*
* A question is how to make the prototypes available for these functions. They would need to be defined before the
* the prelude - or potentially in the prelude.
*
* I could just define the prototypes in the prelude, and only impl, if needed. Here though I require that all the functions
* implemented here, use C style names (ie unmanagled) to simplify lookup.
*/

struct NameAndFunc
{
    typedef void (*Func)();

    const char* name;
    Func func;
};

#define SLANG_LLVM_EXPAND(x) x

#define SLANG_LLVM_FUNC(name, cppName, retType, paramTypes) NameAndFunc{ #name, (NameAndFunc::Func)static_cast<retType (*) paramTypes>(&SLANG_LLVM_EXPAND(cppName)) },

// Implementations of maths functions available to JIT
static float F32_frexp(float x, float* e)
{
    int ei;
    float m = ::frexpf(x, &ei);
    *e = float(ei);
    return m;
}

static double F64_frexp(double x, double* e)
{
    int ei;
    double m = ::frexp(x, &ei);
    *e = float(ei);
    return m;
}

// These are only the functions that cannot be implemented with 'reasonable performance' in the prelude.
// It is assumed that calling from JIT to C function whilst not super expensive, is an issue. 

// name, cppName, retType, paramTypes
#define SLANG_LLVM_FUNCS(x) \
    x(F64_ceil, ceil, double, (double)) \
    x(F64_floor, floor, double, (double)) \
    x(F64_round, round, double, (double)) \
    x(F64_sin, sin, double, (double)) \
    x(F64_cos, cos, double, (double)) \
    x(F64_tan, tan, double, (double)) \
    x(F64_asin, asin, double, (double)) \
    x(F64_acos, acos, double, (double)) \
    x(F64_atan, atan, double, (double)) \
    x(F64_sinh, sinh, double, (double)) \
    x(F64_cosh, cosh, double, (double)) \
    x(F64_tanh, tanh, double, (double)) \
    x(F64_log2, log2, double, (double)) \
    x(F64_log, log, double, (double)) \
    x(F64_log10, log10, double, (double)) \
    x(F64_exp2, exp2, double, (double)) \
    x(F64_exp, exp, double, (double)) \
    x(F64_fabs, fabs, double, (double)) \
    x(F64_trunc, trunc, double, (double)) \
    x(F64_sqrt, sqrt, double, (double)) \
    \
    x(F64_isnan, SLANG_LLVM_STD isnan, bool, (double)) \
    x(F64_isfinite, SLANG_LLVM_STD isfinite, bool, (double)) \
    x(F64_isinf, SLANG_LLVM_STD isinf, bool, (double)) \
    \
    x(F64_atan2, atan2, double, (double, double)) \
    \
    x(F64_frexp, F64_frexp, double, (double, double*)) \
    x(F64_pow, pow, double, (double, double)) \
    \
    x(F64_modf, modf, double, (double, double*)) \
    x(F64_fmod, fmod, double, (double, double)) \
    x(F64_remainder, remainder, double, (double, double)) \
    \
    x(F32_ceil, ceilf, float, (float)) \
    x(F32_floor, floorf, float, (float)) \
    x(F32_round, roundf, float, (float)) \
    x(F32_sin, sinf, float, (float)) \
    x(F32_cos, cosf, float, (float)) \
    x(F32_tan, tanf, float, (float)) \
    x(F32_asin, asinf, float, (float)) \
    x(F32_acos, acosf, float, (float)) \
    x(F32_atan, atanf, float, (float)) \
    x(F32_sinh, sinhf, float, (float)) \
    x(F32_cosh, coshf, float, (float)) \
    x(F32_tanh, tanhf, float, (float)) \
    x(F32_log2, log2f, float, (float)) \
    x(F32_log, logf, float, (float)) \
    x(F32_log10, log10f, float, (float)) \
    x(F32_exp2, exp2f, float, (float)) \
    x(F32_exp, expf, float, (float)) \
    x(F32_fabs, fabsf, float, (float)) \
    x(F32_trunc, truncf, float, (float)) \
    x(F32_sqrt, sqrtf, float, (float)) \
    \
    x(F32_isnan, SLANG_LLVM_STD isnan, bool, (float)) \
    x(F32_isfinite, SLANG_LLVM_STD isfinite, bool, (float)) \
    x(F32_isinf, SLANG_LLVM_STD isinf, bool, (float)) \
    \
    x(F32_atan2, atan2f, float, (float, float)) \
    \
    x(F32_frexp, F32_frexp, float, (float, float*)) \
    x(F32_pow, powf, float, (float, float)) \
    \
    x(F32_modf, modff, float, (float, float*)) \
    x(F32_fmod, fmodf, float, (float, float)) \
    x(F32_remainder, remainderf, float, (float, float)) 

static void _appendBuiltinPrototypes(Slang::StringBuilder& out)
{
    // Make all function names unmangled that are implemented externally.
    out << "extern \"C\" { \n";

#define SLANG_LLVM_APPEND_PROTOTYPE(name, cppName, retType, paramTypes)     out << #retType << " " << #name << #paramTypes << ";\n";
    SLANG_LLVM_FUNCS(SLANG_LLVM_APPEND_PROTOTYPE)

        out << "}\n\n";
}

static int _getOptimizationLevel(DownstreamCompiler::OptimizationLevel level)
{
    typedef DownstreamCompiler::OptimizationLevel OptimizationLevel;
    switch (level)
    {
        case OptimizationLevel::None:     return 0;
        default:
        case OptimizationLevel::Default:  return 1;
        case OptimizationLevel::High:     return 2;
        case OptimizationLevel::Maximal:  return 3;
    }
}

static SlangResult _initLLVM()
{
    // Initialize targets first, so that --version shows registered targets.
#if 0
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmPrinters();
    llvm::InitializeAllAsmParsers();
#else
    // Just initialize items needed for this target.

    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    llvm::InitializeNativeTargetDisassembler();
#endif

    return SLANG_OK;
}

SlangResult LLVMDownstreamCompiler::compile(const CompileOptions& options, RefPtr<DownstreamCompileResult>& outResult)
{
    _ensureSufficientStack();

    static const SlangResult initLLVMResult = _initLLVM();
    SLANG_RETURN_ON_FAIL(initLLVMResult);

    std::unique_ptr<CompilerInstance> clang(new CompilerInstance());
    IntrusiveRefCntPtr<DiagnosticIDs> diagID(new DiagnosticIDs());

    // Register the support for object-file-wrapped Clang modules.
    auto pchOps = clang->getPCHContainerOperations();
    pchOps->registerWriter(std::make_unique<ObjectFilePCHContainerWriter>());
    pchOps->registerReader(std::make_unique<ObjectFilePCHContainerReader>());

    IntrusiveRefCntPtr<DiagnosticOptions> diagOpts = new DiagnosticOptions();

    // TODO(JS): We might just want this to talk directly to the listener.
    // For now we just buffer up. 
    BufferedDiagnosticConsumer diagsBuffer;

    IntrusiveRefCntPtr<DiagnosticsEngine> diags = new DiagnosticsEngine(diagID, diagOpts, &diagsBuffer, false);

    //Slang::StringBuilder source;
    //_appendBuiltinPrototypes(source);
    //source << "\n\n";
    //source << cppSource;

    const auto& source = options.sourceContents;
    StringRef sourceStringRef(source.getBuffer(), source.getLength());

    auto sourceBuffer = llvm::MemoryBuffer::getMemBuffer(sourceStringRef);

    auto& invocation = clang->getInvocation();

    std::string verboseOutputString;

    // Capture all of the verbose output into a buffer, so not writen to stdout
    clang->setVerboseOutputStream(std::make_unique<llvm::raw_string_ostream>(verboseOutputString));

    SmallVector<char> output;
    clang->setOutputStream(std::make_unique<llvm::raw_svector_ostream>(output));

    frontend::ActionKind action = frontend::ActionKind::EmitLLVMOnly;

    // EmitCodeGenOnly doesn't appear to actually emit anything
    // EmitLLVM outputs LLVM assembly
    // EmitLLVMOnly doesn't 'emit' anything, but the IR that is produced is accessible, from the 'action'.

    action = frontend::ActionKind::EmitLLVMOnly;

    //action = frontend::ActionKind::EmitBC;
    //action = frontend::ActionKind::EmitLLVM;
    // 
    //action = frontend::ActionKind::EmitCodeGenOnly;
    //action = frontend::ActionKind::EmitObj;
    //action = frontend::ActionKind::EmitAssembly;

    Language language;
    LangStandard::Kind langStd;
    switch (options.sourceLanguage)
    {
        case SLANG_SOURCE_LANGUAGE_CPP:
        {
            language = Language::CXX;
            langStd = LangStandard::Kind::lang_cxx17;
            break;
        }
        case SLANG_SOURCE_LANGUAGE_C:
        {
            language = Language::C;
            langStd = LangStandard::Kind::lang_c17;
            break;
        }
        default:
        {
            return SLANG_E_NOT_AVAILABLE;
        }
    }

    const InputKind inputKind(language, InputKind::Format::Source);

    {
        auto& opts = invocation.getFrontendOpts();

        // Add the source
        // TODO(JS): For the moment this kind of include does *NOT* show a input source filename
        // not super surprising as one isn't set, but it's not clear how one would be set when the input is a memory buffer.
        // For Slang usage, this probably isn't an issue, because it's *output* typically holds #line directives.
        {
            
            FrontendInputFile inputFile(*sourceBuffer, inputKind);

            opts.Inputs.push_back(inputFile);
        }

        opts.ProgramAction = action;
    }

    {
        auto& opts = invocation.getPreprocessorOpts();

        // Add definition so that 'LLVM/Clang' compilations can be recognized
        opts.addMacroDef("SLANG_LLVM");

        for (const auto& define : options.defines)
        {
            const Index index = define.nameWithSig.indexOf('(');
            if (index >= 0)
            {
                // Interface does not support having a signature.
                return SLANG_E_NOT_AVAILABLE;
            }

            // TODO(JS): NOTE! The options do not support setting a *value* just that a macro is defined.
            // So strictly speaking, we should probably have a warning/error if the value is not appropriate
            opts.addMacroDef(define.nameWithSig.getBuffer());
        }
    }


    llvm::Triple targetTriple;
    {
        auto& opts = invocation.getTargetOpts();

        opts.Triple = LLVM_DEFAULT_TARGET_TRIPLE;

        // A code model isn't set by default, "default" seems to fit the bill here 
        opts.CodeModel = "default";

        targetTriple = llvm::Triple(opts.Triple);
    }

    {
        auto opts = invocation.getLangOpts();

        std::vector<std::string> includes;
        for (const auto& includePath : options.includePaths)
        {
            includes.push_back(includePath.getBuffer());
        }

        clang::CompilerInvocation::setLangDefaults(*opts, inputKind, targetTriple, includes, langStd);

        if (options.floatingPointMode == DownstreamCompiler::FloatingPointMode::Fast)
        {
            opts->FastMath = true;
        }
    }

    {
        auto& opts = invocation.getHeaderSearchOpts();

        // These only work if the resource directory is setup (or a virtual file system points to it)
        opts.UseBuiltinIncludes = true;
        opts.UseStandardSystemIncludes = true;
        opts.UseStandardCXXIncludes = true;

        /// Use libc++ instead of the default libstdc++.
        //opts.UseLibcxx = true;
    }


    {
        auto& opts = invocation.getCodeGenOpts();

        // Set to -O optimization level
        opts.OptimizationLevel = _getOptimizationLevel(options.optimizationLevel);

        // Copy over the targets CodeModel
        opts.CodeModel = invocation.getTargetOpts().CodeModel;
    }

    //const llvm::opt::OptTable& opts = clang::driver::getDriverOptTable();

    // TODO(JS): Need a way to find in system search paths, for now we just don't bother
    //
    // The system search paths are for includes for compiler intrinsics it seems. 
    // Infer the builtin include path if unspecified.
#if 0
    {
        auto& searchOpts = clang->getHeaderSearchOpts();
        if (searchOpts.UseBuiltinIncludes && searchOpts.ResourceDir.empty())
        {
            // TODO(JS): Hack - hard coded path such that we can test out the
            // resource directory functionality.

            StringRef binaryPath = "F:/dev/llvm-12.0/llvm-project-llvmorg-12.0.1/build.vs/Release/bin";

            // Dir is bin/ or lib/, depending on where BinaryPath is.

            // On Windows, libclang.dll is in bin/.
            // On non-Windows, libclang.so/.dylib is in lib/.
            // With a static-library build of libclang, LibClangPath will contain the
            // path of the embedding binary, which for LLVM binaries will be in bin/.
            // ../lib gets us to lib/ in both cases.
            SmallString<128> path = llvm::sys::path::parent_path(binaryPath);
            llvm::sys::path::append(path, Twine("lib") + CLANG_LIBDIR_SUFFIX, "clang", CLANG_VERSION_STRING);

            searchOpts.ResourceDir = path.c_str();
        }
    }
#endif

    // Create the actual diagnostics engine.
    clang->createDiagnostics();
    clang->setDiagnostics(diags.get());

    if (!clang->hasDiagnostics())
        return SLANG_FAIL;

    //
    clang->createFileManager();
    clang->createSourceManager(clang->getFileManager());

    // Set an error handler, so that any LLVM backend diagnostics go through our
    // error handler.
    llvm::install_fatal_error_handler(_llvmErrorHandler, static_cast<void*>(&clang->getDiagnostics()));

    std::unique_ptr<LLVMContext> llvmContext = std::make_unique<LLVMContext>();

    clang::CodeGenAction* codeGenAction = nullptr;
    std::unique_ptr<FrontendAction> act;

    {
        // If we are going to just emit IR, we need to have access to the underlying type
        if (action == frontend::ActionKind::EmitLLVMOnly)
        {
            EmitLLVMOnlyAction* llvmOnlyAction = new EmitLLVMOnlyAction(llvmContext.get());
            codeGenAction = llvmOnlyAction;
            // Make act the owning ptr
            act = std::unique_ptr<FrontendAction>(llvmOnlyAction);
        }
        else
        {
            act = CreateFrontendAction(*clang);
        }

        if (!act)
        {
            return SLANG_FAIL;
        }

        const bool compileSucceeded = clang->ExecuteAction(*act);

        // If the compilation failed make sure, we have an error
        if (!compileSucceeded)
        {
            diagsBuffer.m_diagnostics.requireErrorDiagnostic();
        }

        if (!compileSucceeded || diagsBuffer.hasError())
        {
            outResult = new BlobDownstreamCompileResult(diagsBuffer.m_diagnostics, nullptr);
            return SLANG_FAIL;
        }
    }

    std::unique_ptr<llvm::Module> module;

    switch (action)
    {
        case frontend::ActionKind::EmitLLVM:
        {
            // LLVM output is text, that must be zero terminated
            output.push_back(char(0));

            StringRef identifier;
            StringRef data(output.begin(), output.size() - 1);

            MemoryBufferRef memoryBufferRef(data, identifier);

            SMDiagnostic err;
            module = llvm::parseIR(memoryBufferRef, err, *llvmContext);
            break;
        }
        case frontend::ActionKind::EmitBC:
        {
            StringRef identifier;
            StringRef data(output.begin(), output.size());

            MemoryBufferRef memoryBufferRef(data, identifier);

            SMDiagnostic err;
            module = llvm::parseIR(memoryBufferRef, err, *llvmContext);
            break;
        }
        case frontend::ActionKind::EmitLLVMOnly:
        {
            // Get the module produced by the action
            module = codeGenAction->takeModule();
            break;
        }
    }

    switch (options.targetType)
    {
        case SLANG_SHARED_LIBRARY:
        case SLANG_HOST_CALLABLE:
        {
            // Try running something in the module on the JIT
            std::unique_ptr<llvm::orc::LLJIT> jit;
            {
                // Create the JIT

                LLJITBuilder jitBuilder;

                Expected<std::unique_ptr< llvm::orc::LLJIT>> expectJit = jitBuilder.create();
                if (!expectJit)
                {
                    return SLANG_FAIL;
                }
                jit = std::move(*expectJit);
            }

            // Used the following link to test this out
            // https://www.llvm.org/docs/ORCv2.html
            // https://www.llvm.org/docs/ORCv2.html#processandlibrarysymbols

            {
                auto& es = jit->getExecutionSession();

                const DataLayout& dl = jit->getDataLayout();
                MangleAndInterner mangler(es, dl);

                // The name of the lib must be unique. Should be here as we are only thing adding libs
                auto stdcLibExpected = es.createJITDylib("stdc");

                if (stdcLibExpected)
                {
                    auto& stdcLib = *stdcLibExpected;

                    // Add all the symbolmap
                    SymbolMap symbolMap;

                    //symbolMap.insert(std::make_pair(mangler("sin"), JITEvaluatedSymbol::fromPointer(static_cast<double (*)(double)>(&sin))));

                    static const NameAndFunc funcs[] =
                    {
                        SLANG_LLVM_FUNCS(SLANG_LLVM_FUNC)
                    };

                    for (auto& func : funcs)
                    {
                        symbolMap.insert(std::make_pair(mangler(func.name), JITEvaluatedSymbol::fromPointer(func.func)));
                    }

                    stdcLib.define(absoluteSymbols(symbolMap));

                    // Required or the symbols won't be found
                    jit->getMainJITDylib().addToLinkOrder(stdcLib);
                }
            }

            ThreadSafeModule threadSafeModule(std::move(module), std::move(llvmContext));

            jit->addIRModule(std::move(threadSafeModule));

            outResult = new LLVMDownstreamCompileResult(diagsBuffer.m_diagnostics, std::move(jit));
            return SLANG_OK;
        }
    }

    return SLANG_FAIL;
}

} // namespace slang_llvm

extern "C" SLANG_DLL_EXPORT SlangResult createLLVMDownstreamCompiler(Slang::RefPtr<Slang::DownstreamCompiler>&out)
{
    Slang::RefPtr<slang_llvm::LLVMDownstreamCompiler> compiler(new slang_llvm::LLVMDownstreamCompiler);
    out = compiler;
    return SLANG_OK;
}
