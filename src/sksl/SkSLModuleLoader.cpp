/*
 * Copyright 2022 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkTypes.h"
#include "include/private/SkMutex.h"
#include "include/private/SkSLModifiers.h"
#include "include/private/SkSLProgramElement.h"
#include "include/private/SkSLProgramKind.h"
#include "include/sksl/SkSLPosition.h"
#include "src/sksl/SkSLBuiltinTypes.h"
#include "src/sksl/SkSLCompiler.h"
#include "src/sksl/SkSLModifiersPool.h"
#include "src/sksl/SkSLModuleLoader.h"
#include "src/sksl/ir/SkSLSymbolTable.h"
#include "src/sksl/ir/SkSLType.h"
#include "src/sksl/ir/SkSLVariable.h"

#include <algorithm>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#if SKSL_STANDALONE

    // In standalone mode, we load the original SkSL source files. GN is responsible for copying
    // these files from src/sksl/ to the output directory.
    #include <fstream>

    static std::string load_module_file(const char* moduleFilename) {
        std::ifstream in(std::string{moduleFilename});
        std::string moduleSource{std::istreambuf_iterator<char>(in),
                                 std::istreambuf_iterator<char>()};
        if (in.rdstate()) {
            SK_ABORT("Error reading %s\n", moduleFilename);
        }
        return moduleSource;
    }

    #define MODULE_DATA(name) #name, load_module_file(#name ".sksl")

#else

    // We include minified SkSL module code and pass it directly to the compiler.
    #if defined(SK_ENABLE_OPTIMIZE_SIZE) || !defined(SK_DEBUG)
        #include "src/sksl/generated/sksl_shared.minified.sksl"
        #include "src/sksl/generated/sksl_compute.minified.sksl"
        #include "src/sksl/generated/sksl_frag.minified.sksl"
        #include "src/sksl/generated/sksl_gpu.minified.sksl"
        #include "src/sksl/generated/sksl_public.minified.sksl"
        #include "src/sksl/generated/sksl_rt_shader.minified.sksl"
        #include "src/sksl/generated/sksl_vert.minified.sksl"
        #if defined(SK_GRAPHITE_ENABLED)
        #include "src/sksl/generated/sksl_graphite_frag.minified.sksl"
        #include "src/sksl/generated/sksl_graphite_vert.minified.sksl"
        #endif
    #else
        #include "src/sksl/generated/sksl_shared.unoptimized.sksl"
        #include "src/sksl/generated/sksl_compute.unoptimized.sksl"
        #include "src/sksl/generated/sksl_frag.unoptimized.sksl"
        #include "src/sksl/generated/sksl_gpu.unoptimized.sksl"
        #include "src/sksl/generated/sksl_public.unoptimized.sksl"
        #include "src/sksl/generated/sksl_rt_shader.unoptimized.sksl"
        #include "src/sksl/generated/sksl_vert.unoptimized.sksl"
        #if defined(SK_GRAPHITE_ENABLED)
        #include "src/sksl/generated/sksl_graphite_frag.unoptimized.sksl"
        #include "src/sksl/generated/sksl_graphite_vert.unoptimized.sksl"
        #endif
    #endif

    #define MODULE_DATA(name) #name, std::string(SKSL_MINIFIED_##name)

#endif

namespace SkSL {

#define TYPE(t) &BuiltinTypes::f ## t

static constexpr BuiltinTypePtr kRootTypes[] = {
    TYPE(Void),

    TYPE( Float), TYPE( Float2), TYPE( Float3), TYPE( Float4),
    TYPE(  Half), TYPE(  Half2), TYPE(  Half3), TYPE(  Half4),
    TYPE(   Int), TYPE(   Int2), TYPE(   Int3), TYPE(   Int4),
    TYPE(  UInt), TYPE(  UInt2), TYPE(  UInt3), TYPE(  UInt4),
    TYPE( Short), TYPE( Short2), TYPE( Short3), TYPE( Short4),
    TYPE(UShort), TYPE(UShort2), TYPE(UShort3), TYPE(UShort4),
    TYPE(  Bool), TYPE(  Bool2), TYPE(  Bool3), TYPE(  Bool4),

    TYPE(Float2x2), TYPE(Float2x3), TYPE(Float2x4),
    TYPE(Float3x2), TYPE(Float3x3), TYPE(Float3x4),
    TYPE(Float4x2), TYPE(Float4x3), TYPE(Float4x4),

    TYPE(Half2x2),  TYPE(Half2x3),  TYPE(Half2x4),
    TYPE(Half3x2),  TYPE(Half3x3),  TYPE(Half3x4),
    TYPE(Half4x2),  TYPE(Half4x3),  TYPE(Half4x4),

    TYPE(SquareMat), TYPE(SquareHMat),
    TYPE(Mat),       TYPE(HMat),

    // TODO(skia:12349): generic short/ushort
    TYPE(GenType),   TYPE(GenIType), TYPE(GenUType),
    TYPE(GenHType),   /* (GenSType)      (GenUSType) */
    TYPE(GenBType),
    TYPE(IntLiteral),
    TYPE(FloatLiteral),

    TYPE(Vec),     TYPE(IVec),     TYPE(UVec),
    TYPE(HVec),    TYPE(SVec),     TYPE(USVec),
    TYPE(BVec),

    TYPE(ColorFilter),
    TYPE(Shader),
    TYPE(Blender),
};

static constexpr BuiltinTypePtr kPrivateTypes[] = {
    TYPE(Sampler2D), TYPE(SamplerExternalOES), TYPE(Sampler2DRect),

    TYPE(SubpassInput), TYPE(SubpassInputMS),

    TYPE(Sampler),
    TYPE(Texture2D),
    TYPE(ReadWriteTexture2D), TYPE(ReadOnlyTexture2D), TYPE(WriteOnlyTexture2D),
    TYPE(GenTexture2D), TYPE(ReadableTexture2D), TYPE(WritableTexture2D),
};

#undef TYPE

struct ModuleLoader::Impl {
    Impl();

    void makeRootSymbolTable();

    // This mutex is taken when ModuleLoader::Get is called, and released when the returned
    // ModuleLoader object falls out of scope.
    SkMutex fMutex;
    const BuiltinTypes fBuiltinTypes;
    ModifiersPool fCoreModifiers;

    std::unique_ptr<const Module> fRootModule;

    std::unique_ptr<const Module> fSharedModule;            // [Root] + Public intrinsics
    std::unique_ptr<const Module> fGPUModule;               // [Shared] + Non-public intrinsics/
                                                            //     helper functions
    std::unique_ptr<const Module> fVertexModule;            // [GPU] + Vertex stage decls
    std::unique_ptr<const Module> fFragmentModule;          // [GPU] + Fragment stage decls
    std::unique_ptr<const Module> fComputeModule;           // [GPU] + Compute stage decls
    std::unique_ptr<const Module> fGraphiteVertexModule;    // [Vert] + Graphite vertex helpers
    std::unique_ptr<const Module> fGraphiteFragmentModule;  // [Frag] + Graphite fragment helpers

    std::unique_ptr<const Module> fPublicModule;            // [Shared] minus Private types +
                                                            //     Runtime effect intrinsics
    std::unique_ptr<const Module> fRuntimeShaderModule;     // [Public] + Runtime shader decls
};

ModuleLoader ModuleLoader::Get() {
    static ModuleLoader::Impl* sModuleLoaderImpl = new ModuleLoader::Impl;
    return ModuleLoader(*sModuleLoaderImpl);
}

ModuleLoader::ModuleLoader(ModuleLoader::Impl& m) : fModuleLoader(m) {
    fModuleLoader.fMutex.acquire();
}

ModuleLoader::~ModuleLoader() {
    fModuleLoader.fMutex.release();
}

void ModuleLoader::unloadModules() {
    fModuleLoader.fSharedModule           = nullptr;
    fModuleLoader.fGPUModule              = nullptr;
    fModuleLoader.fVertexModule           = nullptr;
    fModuleLoader.fFragmentModule         = nullptr;
    fModuleLoader.fComputeModule          = nullptr;
    fModuleLoader.fGraphiteVertexModule   = nullptr;
    fModuleLoader.fGraphiteFragmentModule = nullptr;
    fModuleLoader.fPublicModule           = nullptr;
    fModuleLoader.fRuntimeShaderModule    = nullptr;
}

ModuleLoader::Impl::Impl() {
    this->makeRootSymbolTable();
}

static void add_public_type_aliases(SkSL::SymbolTable* symbols, const SkSL::BuiltinTypes& types) {
    // Add some aliases to the runtime effect modules so that it's friendlier, and more like GLSL.
    symbols->addWithoutOwnership(types.fVec2.get());
    symbols->addWithoutOwnership(types.fVec3.get());
    symbols->addWithoutOwnership(types.fVec4.get());

    symbols->addWithoutOwnership(types.fIVec2.get());
    symbols->addWithoutOwnership(types.fIVec3.get());
    symbols->addWithoutOwnership(types.fIVec4.get());

    symbols->addWithoutOwnership(types.fBVec2.get());
    symbols->addWithoutOwnership(types.fBVec3.get());
    symbols->addWithoutOwnership(types.fBVec4.get());

    symbols->addWithoutOwnership(types.fMat2.get());
    symbols->addWithoutOwnership(types.fMat3.get());
    symbols->addWithoutOwnership(types.fMat4.get());

    symbols->addWithoutOwnership(types.fMat2x2.get());
    symbols->addWithoutOwnership(types.fMat2x3.get());
    symbols->addWithoutOwnership(types.fMat2x4.get());
    symbols->addWithoutOwnership(types.fMat3x2.get());
    symbols->addWithoutOwnership(types.fMat3x3.get());
    symbols->addWithoutOwnership(types.fMat3x4.get());
    symbols->addWithoutOwnership(types.fMat4x2.get());
    symbols->addWithoutOwnership(types.fMat4x3.get());
    symbols->addWithoutOwnership(types.fMat4x4.get());

    // Hide all the private symbols by aliasing them all to "invalid". This will prevent code from
    // using built-in names like `sampler2D` as variable names.
    for (BuiltinTypePtr privateType : kPrivateTypes) {
        symbols->add(Type::MakeAliasType((types.*privateType)->name(), *types.fInvalid));
    }
}

static void add_compute_type_aliases(SkSL::SymbolTable* symbols, const SkSL::BuiltinTypes& types) {
    // A `texture2D` in a compute shader should generally mean "read-write" texture access, not
    // "sample" texture access. Remap the name `texture2D` to point to `readWriteTexture2D`.
    symbols->add(Type::MakeAliasType("texture2D", *types.fReadWriteTexture2D));
}

static std::unique_ptr<Module> compile_and_shrink(SkSL::Compiler* compiler,
                                                  ProgramKind kind,
                                                  const char* moduleName,
                                                  std::string moduleSource,
                                                  const Module* parent,
                                                  ModifiersPool& modifiersPool) {
    std::unique_ptr<Module> m = compiler->compileModule(kind,
                                                        moduleName,
                                                        std::move(moduleSource),
                                                        parent,
                                                        modifiersPool,
                                                        /*shouldInline=*/true);

    // We can eliminate FunctionPrototypes without changing the meaning of the module; the function
    // declaration is still safely in the symbol table. This only impacts our ability to recreate
    // the input verbatim, which we don't care about at runtime.
    m->fElements.erase(std::remove_if(m->fElements.begin(), m->fElements.end(),
                                      [](const std::unique_ptr<ProgramElement>& element) {
                                          switch (element->kind()) {
                                              case ProgramElement::Kind::kFunction:
                                              case ProgramElement::Kind::kGlobalVar:
                                              case ProgramElement::Kind::kInterfaceBlock:
                                                  // We need to preserve these.
                                                  return false;

                                              case ProgramElement::Kind::kFunctionPrototype:
                                                  // These are already in the symbol table; the
                                                  // ProgramElement isn't needed anymore.
                                                  return true;

                                              default:
                                                  SkDEBUGFAILF("Unsupported element: %s\n",
                                                               element->description().c_str());
                                                  return false;
                                          }
                                      }),
                       m->fElements.end());

    m->fElements.shrink_to_fit();
    return m;
}

const BuiltinTypes& ModuleLoader::builtinTypes() {
    return fModuleLoader.fBuiltinTypes;
}

ModifiersPool& ModuleLoader::coreModifiers() {
    return fModuleLoader.fCoreModifiers;
}

const Module* ModuleLoader::rootModule() {
    return fModuleLoader.fRootModule.get();
}

const Module* ModuleLoader::loadPublicModule(SkSL::Compiler* compiler) {
    if (!fModuleLoader.fPublicModule) {
        const Module* sharedModule = this->loadSharedModule(compiler);
        fModuleLoader.fPublicModule = compile_and_shrink(compiler,
                                                         ProgramKind::kGeneric,
                                                         MODULE_DATA(sksl_public),
                                                         sharedModule,
                                                         this->coreModifiers());
        add_public_type_aliases(fModuleLoader.fPublicModule->fSymbols.get(), this->builtinTypes());
    }
    return fModuleLoader.fPublicModule.get();
}

const Module* ModuleLoader::loadPrivateRTShaderModule(SkSL::Compiler* compiler) {
    if (!fModuleLoader.fRuntimeShaderModule) {
        const Module* publicModule = this->loadPublicModule(compiler);
        fModuleLoader.fRuntimeShaderModule = compile_and_shrink(compiler,
                                                                ProgramKind::kFragment,
                                                                MODULE_DATA(sksl_rt_shader),
                                                                publicModule,
                                                                this->coreModifiers());
    }
    return fModuleLoader.fRuntimeShaderModule.get();
}

const Module* ModuleLoader::loadSharedModule(SkSL::Compiler* compiler) {
    if (!fModuleLoader.fSharedModule) {
        const Module* rootModule = this->rootModule();
        fModuleLoader.fSharedModule = compile_and_shrink(compiler,
                                                         ProgramKind::kFragment,
                                                         MODULE_DATA(sksl_shared),
                                                         rootModule,
                                                         this->coreModifiers());
    }
    return fModuleLoader.fSharedModule.get();
}

const Module* ModuleLoader::loadGPUModule(SkSL::Compiler* compiler) {
    if (!fModuleLoader.fGPUModule) {
        const Module* sharedModule = this->loadSharedModule(compiler);
        fModuleLoader.fGPUModule = compile_and_shrink(compiler,
                                                      ProgramKind::kFragment,
                                                      MODULE_DATA(sksl_gpu),
                                                      sharedModule,
                                                      this->coreModifiers());
    }
    return fModuleLoader.fGPUModule.get();
}

const Module* ModuleLoader::loadFragmentModule(SkSL::Compiler* compiler) {
    if (!fModuleLoader.fFragmentModule) {
        const Module* gpuModule = this->loadGPUModule(compiler);
        fModuleLoader.fFragmentModule = compile_and_shrink(compiler,
                                                           ProgramKind::kFragment,
                                                           MODULE_DATA(sksl_frag),
                                                           gpuModule,
                                                           this->coreModifiers());
    }
    return fModuleLoader.fFragmentModule.get();
}

const Module* ModuleLoader::loadVertexModule(SkSL::Compiler* compiler) {
    if (!fModuleLoader.fVertexModule) {
        const Module* gpuModule = this->loadGPUModule(compiler);
        fModuleLoader.fVertexModule = compile_and_shrink(compiler,
                                                         ProgramKind::kVertex,
                                                         MODULE_DATA(sksl_vert),
                                                         gpuModule,
                                                         this->coreModifiers());
    }
    return fModuleLoader.fVertexModule.get();
}

const Module* ModuleLoader::loadComputeModule(SkSL::Compiler* compiler) {
    if (!fModuleLoader.fComputeModule) {
        const Module* gpuModule = this->loadGPUModule(compiler);
        fModuleLoader.fComputeModule = compile_and_shrink(compiler,
                                                          ProgramKind::kCompute,
                                                          MODULE_DATA(sksl_compute),
                                                          gpuModule,
                                                          this->coreModifiers());
        add_compute_type_aliases(fModuleLoader.fComputeModule->fSymbols.get(),
                                 this->builtinTypes());
    }
    return fModuleLoader.fComputeModule.get();
}

const Module* ModuleLoader::loadGraphiteFragmentModule(SkSL::Compiler* compiler) {
#if defined(SK_GRAPHITE_ENABLED)
    if (!fModuleLoader.fGraphiteFragmentModule) {
        const Module* fragmentModule = this->loadFragmentModule(compiler);
        fModuleLoader.fGraphiteFragmentModule = compile_and_shrink(compiler,
                                                                   ProgramKind::kGraphiteFragment,
                                                                   MODULE_DATA(sksl_graphite_frag),
                                                                   fragmentModule,
                                                                   this->coreModifiers());
    }
    return fModuleLoader.fGraphiteFragmentModule.get();
#else
    return this->loadFragmentModule(compiler);
#endif
}

const Module* ModuleLoader::loadGraphiteVertexModule(SkSL::Compiler* compiler) {
#if defined(SK_GRAPHITE_ENABLED)
    if (!fModuleLoader.fGraphiteVertexModule) {
        const Module* vertexModule = this->loadVertexModule(compiler);
        fModuleLoader.fGraphiteVertexModule = compile_and_shrink(compiler,
                                                                 ProgramKind::kGraphiteVertex,
                                                                 MODULE_DATA(sksl_graphite_vert),
                                                                 vertexModule,
                                                                 this->coreModifiers());
    }
    return fModuleLoader.fGraphiteVertexModule.get();
#else
    return this->loadVertexModule(compiler);
#endif
}

void ModuleLoader::Impl::makeRootSymbolTable() {
    auto rootModule = std::make_unique<Module>();
    rootModule->fSymbols = std::make_shared<SymbolTable>(/*builtin=*/true);

    for (BuiltinTypePtr rootType : kRootTypes) {
        rootModule->fSymbols->addWithoutOwnership((fBuiltinTypes.*rootType).get());
    }

    for (BuiltinTypePtr privateType : kPrivateTypes) {
        rootModule->fSymbols->addWithoutOwnership((fBuiltinTypes.*privateType).get());
    }

    // sk_Caps is "builtin", but all references to it are resolved to Settings, so we don't need to
    // treat it as builtin (ie, no need to clone it into the Program).
    rootModule->fSymbols->add(std::make_unique<Variable>(/*pos=*/Position(),
                                                          /*modifiersPosition=*/Position(),
                                                          fCoreModifiers.add(Modifiers{}),
                                                          "sk_Caps",
                                                          fBuiltinTypes.fSkCaps.get(),
                                                          /*builtin=*/false,
                                                          Variable::Storage::kGlobal));
    fRootModule = std::move(rootModule);
}

}  // namespace SkSL
