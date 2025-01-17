/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2015 Mozilla Foundation
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

#include "wasm/WasmGenerator.h"

#include "mozilla/CheckedInt.h"
#include "mozilla/EnumeratedRange.h"
#include "mozilla/SHA1.h"

#include <algorithm>

#include "wasm/WasmBaselineCompile.h"
#include "wasm/WasmCompile.h"
#include "wasm/WasmIonCompile.h"
#include "wasm/WasmStubs.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

using mozilla::CheckedInt;
using mozilla::MakeEnumeratedRange;

bool
CompiledCode::swap(MacroAssembler& masm)
{
    MOZ_ASSERT(bytes.empty());
    if (!masm.swapBuffer(bytes))
        return false;

    callSites.swap(masm.callSites());
    callSiteTargets.swap(masm.callSiteTargets());
    trapSites.swap(masm.trapSites());
    callFarJumps.swap(masm.callFarJumps());
    trapFarJumps.swap(masm.trapFarJumps());
    memoryAccesses.swap(masm.memoryAccesses());
    symbolicAccesses.swap(masm.symbolicAccesses());
    codeLabels.swap(masm.codeLabels());
    return true;
}

// ****************************************************************************
// ModuleGenerator

static const unsigned GENERATOR_LIFO_DEFAULT_CHUNK_SIZE = 4 * 1024;
static const unsigned COMPILATION_LIFO_DEFAULT_CHUNK_SIZE = 64 * 1024;
static const uint32_t BAD_CODE_RANGE = UINT32_MAX;

ModuleGenerator::ModuleGenerator(const CompileArgs& args, ModuleEnvironment* env,
                                 Atomic<bool>* cancelled, UniqueChars* error)
  : compileArgs_(&args),
    error_(error),
    cancelled_(cancelled),
    env_(env),
    linkDataTier_(nullptr),
    metadataTier_(nullptr),
    taskState_(mutexid::WasmCompileTaskState),
    numFuncDefs_(0),
    numSigs_(0),
    numTables_(0),
    lifo_(GENERATOR_LIFO_DEFAULT_CHUNK_SIZE),
    masmAlloc_(&lifo_),
    masm_(MacroAssembler::WasmToken(), masmAlloc_),
    trapCodeOffsets_(),
    debugTrapCodeOffset_(),
    lastPatchedCallSite_(0),
    startOfUnpatchedCallsites_(0),
    parallel_(false),
    outstanding_(0),
    currentTask_(nullptr),
    batchedBytecode_(0),
    startedFuncDefs_(false),
    finishedFuncDefs_(false)
{
    MOZ_ASSERT(IsCompilingWasm());
    std::fill(trapCodeOffsets_.begin(), trapCodeOffsets_.end(), 0);
}

ModuleGenerator::~ModuleGenerator()
{
    MOZ_ASSERT_IF(finishedFuncDefs_, !batchedBytecode_);
    MOZ_ASSERT_IF(finishedFuncDefs_, !currentTask_);

    if (parallel_) {
        if (outstanding_) {
            // Remove any pending compilation tasks from the worklist.
            {
                AutoLockHelperThreadState lock;
                CompileTaskPtrVector& worklist = HelperThreadState().wasmWorklist(lock, mode());
                auto pred = [this](CompileTask* task) { return &task->state == &taskState_; };
                size_t removed = EraseIf(worklist, pred);
                MOZ_ASSERT(outstanding_ >= removed);
                outstanding_ -= removed;
            }

            // Wait until all active compilation tasks have finished.
            {
                auto taskState = taskState_.lock();
                while (true) {
                    MOZ_ASSERT(outstanding_ >= taskState->finished.length());
                    outstanding_ -= taskState->finished.length();
                    taskState->finished.clear();

                    MOZ_ASSERT(outstanding_ >= taskState->numFailed);
                    outstanding_ -= taskState->numFailed;
                    taskState->numFailed = 0;

                    if (!outstanding_)
                        break;

                    taskState.wait(taskState->failedOrFinished);
                }
            }
        }
    } else {
        MOZ_ASSERT(!outstanding_);
    }

    // Propagate error state.
    if (error_ && !*error_)
        *error_ = Move(taskState_.lock()->errorMessage);
}

bool
ModuleGenerator::initAsmJS(Metadata* asmJSMetadata)
{
    MOZ_ASSERT(env_->isAsmJS());

    if (!linkData_.initTier1(Tier::Ion, *asmJSMetadata))
        return false;
    linkDataTier_ = &linkData_.linkData(Tier::Ion);

    metadataTier_ = &asmJSMetadata->metadata(Tier::Ion);
    metadata_ = asmJSMetadata;
    MOZ_ASSERT(isAsmJS());

    // For asm.js, the Vectors in ModuleEnvironment are max-sized reservations
    // and will be initialized in a linear order via init* functions as the
    // module is generated.

    MOZ_ASSERT(env_->sigs.length() == AsmJSMaxTypes);
    MOZ_ASSERT(env_->tables.length() == AsmJSMaxTables);
    MOZ_ASSERT(env_->asmJSSigToTableIndex.length() == AsmJSMaxTypes);

    return true;
}

bool
ModuleGenerator::initWasm(size_t codeSectionSize)
{
    MOZ_ASSERT(!env_->isAsmJS());

    auto metadataTier = js::MakeUnique<MetadataTier>(tier());
    if (!metadataTier)
        return false;

    metadata_ = js_new<Metadata>(Move(metadataTier));
    if (!metadata_)
        return false;

    metadataTier_ = &metadata_->metadata(tier());

    if (!linkData_.initTier1(tier(), *metadata_))
        return false;
    linkDataTier_ = &linkData_.linkData(tier());

    MOZ_ASSERT(!isAsmJS());

    // For wasm, the amount of code, functions, signatures, imports, exports,
    // etc are known a priori.

    numSigs_ = env_->sigs.length();
    numTables_ = env_->tables.length();

    // When estimating the MacroAssembler buffer size, be extra conservative
    // since the price is low and the cost of an extra resize is high.

    if (!masm_.reserve(size_t(1.2 * EstimateCompiledCodeSize(tier(), codeSectionSize))))
        return false;

    // Although we could compute it more precisely (only the number of far jumps
    // is unknown), 2x number of functions is a good conservative estimate and
    // podResizeToFit will remove waste at the end.

    if (!metadataTier_->codeRanges.reserve(2 * env_->numFuncDefs()))
        return false;

    // Code can vary a lot, so use a conservative estimate of 1 load/store/call/trap
    // per 10 bytes of bytecode and rely on podResizeToFit() to remove waste.

    const size_t CallSitesPerByteCode = 10;
    if (!metadataTier_->callSites.reserve(codeSectionSize / CallSitesPerByteCode))
        return false;

    const size_t MemoryAccessesPerByteCode = 10;
    if (!metadataTier_->memoryAccesses.reserve(codeSectionSize / MemoryAccessesPerByteCode))
        return false;

    // Allocate space in TlsData for declarations that need it.

    for (size_t i = 0; i < env_->funcImportGlobalDataOffsets.length(); i++) {
        env_->funcImportGlobalDataOffsets[i] = metadata_->globalDataLength;
        metadata_->globalDataLength += sizeof(FuncImportTls);
        if (!addFuncImport(*env_->funcSigs[i], env_->funcImportGlobalDataOffsets[i]))
            return false;
    }

    for (TableDesc& table : env_->tables) {
        if (!allocateGlobalBytes(sizeof(TableTls), sizeof(void*), &table.globalDataOffset))
            return false;
    }

    for (uint32_t i = 0; i < numSigs_; i++) {
        SigWithId& sig = env_->sigs[i];
        if (SigIdDesc::isGlobal(sig)) {
            uint32_t globalDataOffset;
            if (!allocateGlobalBytes(sizeof(void*), sizeof(void*), &globalDataOffset))
                return false;

            sig.id = SigIdDesc::global(sig, globalDataOffset);

            Sig copy;
            if (!copy.clone(sig))
                return false;

            if (!metadata_->sigIds.emplaceBack(Move(copy), sig.id))
                return false;
        } else {
            sig.id = SigIdDesc::immediate(sig);
        }
    }

    for (GlobalDesc& global : env_->globals) {
        if (global.isConstant())
            continue;
        if (!allocateGlobal(&global))
            return false;
    }

    // Build a HashSet of all exported functions, whether by explicit export of
    // the function, or implicitly by being an element of an external (imported
    // or exported) table, or being the start function.

    for (const Export& exp : env_->exports) {
        if (exp.kind() == DefinitionKind::Function) {
            if (!exportedFuncs_.put(exp.funcIndex()))
                return false;
        }
    }

    if (env_->startFuncIndex) {
        metadata_->startFuncIndex.emplace(*env_->startFuncIndex);
        if (!exportedFuncs_.put(*env_->startFuncIndex))
            return false;
    }

    return true;
}

bool
ModuleGenerator::init(size_t codeSectionSize, Metadata* maybeAsmJSMetadata)
{
    if (!funcToCodeRange_.appendN(BAD_CODE_RANGE, env_->funcSigs.length()))
        return false;

    if (!assumptions_.clone(compileArgs_->assumptions))
        return false;

    if (!exportedFuncs_.init())
        return false;

    if (env_->isAsmJS() ? !initAsmJS(maybeAsmJSMetadata) : !initWasm(codeSectionSize))
        return false;

    if (compileArgs_->scriptedCaller.filename) {
        metadata_->filename = DuplicateString(compileArgs_->scriptedCaller.filename.get());
        if (!metadata_->filename)
            return false;
    }

    return true;
}

bool
ModuleGenerator::funcIsCompiled(uint32_t funcIndex) const
{
    return funcToCodeRange_[funcIndex] != BAD_CODE_RANGE;
}

const CodeRange&
ModuleGenerator::funcCodeRange(uint32_t funcIndex) const
{
    MOZ_ASSERT(funcIsCompiled(funcIndex));
    const CodeRange& cr = metadataTier_->codeRanges[funcToCodeRange_[funcIndex]];
    MOZ_ASSERT(cr.isFunction());
    return cr;
}

static bool
InRange(uint32_t caller, uint32_t callee)
{
    // We assume JumpImmediateRange is defined conservatively enough that the
    // slight difference between 'caller' (which is really the return address
    // offset) and the actual base of the relative displacement computation
    // isn't significant.
    uint32_t range = Min(JitOptions.jumpThreshold, JumpImmediateRange);
    if (caller < callee)
        return callee - caller < range;
    return caller - callee < range;
}

typedef HashMap<uint32_t, uint32_t, DefaultHasher<uint32_t>, SystemAllocPolicy> OffsetMap;
typedef EnumeratedArray<Trap, Trap::Limit, Maybe<uint32_t>> TrapOffsetArray;

bool
ModuleGenerator::linkCallSites()
{
    masm_.haltingAlign(CodeAlignment);

    // Create far jumps for calls that have relative offsets that may otherwise
    // go out of range. Far jumps are created for two cases: direct calls
    // between function definitions and calls to trap exits by trap out-of-line
    // paths. Far jump code is shared when possible to reduce bloat. This method
    // is called both between function bodies (at a frequency determined by the
    // ISA's jump range) and once at the very end of a module's codegen after
    // all possible calls/traps have been emitted.

    OffsetMap existingCallFarJumps;
    if (!existingCallFarJumps.init())
        return false;

    TrapOffsetArray existingTrapFarJumps;

    for (; lastPatchedCallSite_ < metadataTier_->callSites.length(); lastPatchedCallSite_++) {
        const CallSite& callSite = metadataTier_->callSites[lastPatchedCallSite_];
        const CallSiteTarget& target = callSiteTargets_[lastPatchedCallSite_];
        uint32_t callerOffset = callSite.returnAddressOffset();
        switch (callSite.kind()) {
          case CallSiteDesc::Dynamic:
          case CallSiteDesc::Symbolic:
            break;
          case CallSiteDesc::Func: {
            if (funcIsCompiled(target.funcIndex())) {
                uint32_t calleeOffset = funcCodeRange(target.funcIndex()).funcNormalEntry();
                if (InRange(callerOffset, calleeOffset)) {
                    masm_.patchCall(callerOffset, calleeOffset);
                    break;
                }
            }

            OffsetMap::AddPtr p = existingCallFarJumps.lookupForAdd(target.funcIndex());
            if (!p) {
                Offsets offsets;
                offsets.begin = masm_.currentOffset();
                if (!callFarJumps_.emplaceBack(target.funcIndex(), masm_.farJumpWithPatch()))
                    return false;
                offsets.end = masm_.currentOffset();
                if (masm_.oom())
                    return false;
                if (!metadataTier_->codeRanges.emplaceBack(CodeRange::FarJumpIsland, offsets))
                    return false;
                if (!existingCallFarJumps.add(p, target.funcIndex(), offsets.begin))
                    return false;
            }

            masm_.patchCall(callerOffset, p->value());
            break;
          }
          case CallSiteDesc::TrapExit: {
            if (!existingTrapFarJumps[target.trap()]) {
                // See MacroAssembler::wasmEmitTrapOutOfLineCode for why we must
                // reload the TLS register on this path.
                Offsets offsets;
                offsets.begin = masm_.currentOffset();
                masm_.loadPtr(Address(FramePointer, offsetof(Frame, tls)), WasmTlsReg);
                if (!trapFarJumps_.emplaceBack(target.trap(), masm_.farJumpWithPatch()))
                    return false;
                offsets.end = masm_.currentOffset();
                if (masm_.oom())
                    return false;
                if (!metadataTier_->codeRanges.emplaceBack(CodeRange::FarJumpIsland, offsets))
                    return false;
                existingTrapFarJumps[target.trap()] = Some(offsets.begin);
            }

            masm_.patchCall(callerOffset, *existingTrapFarJumps[target.trap()]);
            break;
          }
          case CallSiteDesc::Breakpoint:
          case CallSiteDesc::EnterFrame:
          case CallSiteDesc::LeaveFrame: {
            Uint32Vector& jumps = metadataTier_->debugTrapFarJumpOffsets;
            if (jumps.empty() || !InRange(jumps.back(), callerOffset)) {
                // See BaseCompiler::insertBreakablePoint for why we must
                // reload the TLS register on this path.
                Offsets offsets;
                offsets.begin = masm_.currentOffset();
                masm_.loadPtr(Address(FramePointer, offsetof(Frame, tls)), WasmTlsReg);
                CodeOffset jumpOffset = masm_.farJumpWithPatch();
                offsets.end = masm_.currentOffset();
                if (masm_.oom())
                    return false;
                if (!metadataTier_->codeRanges.emplaceBack(CodeRange::FarJumpIsland, offsets))
                    return false;
                if (!debugTrapFarJumps_.emplaceBack(jumpOffset))
                    return false;
                if (!jumps.emplaceBack(offsets.begin))
                    return false;
            }
            break;
          }
        }
    }

    masm_.flushBuffer();
    return !masm_.oom();
}

void
ModuleGenerator::noteCodeRange(uint32_t codeRangeIndex, const CodeRange& codeRange)
{
    switch (codeRange.kind()) {
      case CodeRange::Function:
        MOZ_ASSERT(funcToCodeRange_[codeRange.funcIndex()] == BAD_CODE_RANGE);
        funcToCodeRange_[codeRange.funcIndex()] = codeRangeIndex;
        break;
      case CodeRange::Entry:
        metadataTier_->lookupFuncExport(codeRange.funcIndex()).initEntryOffset(codeRange.begin());
        break;
      case CodeRange::ImportJitExit:
        metadataTier_->funcImports[codeRange.funcIndex()].initJitExitOffset(codeRange.begin());
        break;
      case CodeRange::ImportInterpExit:
        metadataTier_->funcImports[codeRange.funcIndex()].initInterpExitOffset(codeRange.begin());
        break;
      case CodeRange::TrapExit:
        MOZ_ASSERT(!trapCodeOffsets_[codeRange.trap()]);
        trapCodeOffsets_[codeRange.trap()] = codeRange.begin();
        break;
      case CodeRange::DebugTrap:
        MOZ_ASSERT(!debugTrapCodeOffset_);
        debugTrapCodeOffset_ = codeRange.begin();
        break;
      case CodeRange::OutOfBoundsExit:
        MOZ_ASSERT(!linkDataTier_->outOfBoundsOffset);
        linkDataTier_->outOfBoundsOffset = codeRange.begin();
        break;
      case CodeRange::UnalignedExit:
        MOZ_ASSERT(!linkDataTier_->unalignedAccessOffset);
        linkDataTier_->unalignedAccessOffset = codeRange.begin();
        break;
      case CodeRange::Interrupt:
        MOZ_ASSERT(!linkDataTier_->interruptOffset);
        linkDataTier_->interruptOffset = codeRange.begin();
        break;
      case CodeRange::Throw:
        // Jumped to by other stubs, so nothing to do.
        break;
      case CodeRange::FarJumpIsland:
      case CodeRange::BuiltinThunk:
        MOZ_CRASH("Unexpected CodeRange kind");
    }
}

template <class Vec, class Op>
static bool
AppendForEach(Vec* dstVec, const Vec& srcVec, Op op)
{
    if (!dstVec->growByUninitialized(srcVec.length()))
        return false;

    typedef typename Vec::ElementType T;

    const T* src = srcVec.begin();

    T* dstBegin = dstVec->begin();
    T* dstEnd = dstVec->end();
    T* dstStart = dstEnd - srcVec.length();

    for (T* dst = dstStart; dst != dstEnd; dst++, src++) {
        new(dst) T(*src);
        op(dst - dstBegin, dst);
    }

    return true;
}

bool
ModuleGenerator::linkCompiledCode(const CompiledCode& code)
{
    // All code offsets in 'code' must be incremented by their position in the
    // overall module when the code was appended.

    masm_.haltingAlign(CodeAlignment);
    const size_t offsetInModule = masm_.size();
    if (!masm_.appendRawCode(code.bytes.begin(), code.bytes.length()))
        return false;

    auto codeRangeOp = [=](uint32_t codeRangeIndex, CodeRange* codeRange) {
        codeRange->offsetBy(offsetInModule);
        noteCodeRange(codeRangeIndex, *codeRange);
    };
    if (!AppendForEach(&metadataTier_->codeRanges, code.codeRanges, codeRangeOp))
        return false;

    auto callSiteOp = [=](uint32_t, CallSite* cs) { cs->offsetBy(offsetInModule); };
    if (!AppendForEach(&metadataTier_->callSites, code.callSites, callSiteOp))
        return false;

    if (!callSiteTargets_.appendAll(code.callSiteTargets))
        return false;

    MOZ_ASSERT(code.trapSites.empty());

    auto trapFarJumpOp = [=](uint32_t, TrapFarJump* tfj) { tfj->offsetBy(offsetInModule); };
    if (!AppendForEach(&trapFarJumps_, code.trapFarJumps, trapFarJumpOp))
        return false;

    auto callFarJumpOp = [=](uint32_t, CallFarJump* cfj) { cfj->offsetBy(offsetInModule); };
    if (!AppendForEach(&callFarJumps_, code.callFarJumps, callFarJumpOp))
        return false;

    auto memoryOp = [=](uint32_t, MemoryAccess* ma) { ma->offsetBy(offsetInModule); };
    if (!AppendForEach(&metadataTier_->memoryAccesses, code.memoryAccesses, memoryOp))
        return false;

    for (const SymbolicAccess& access : code.symbolicAccesses) {
        uint32_t patchAt = offsetInModule + access.patchAt.offset();
        if (!linkDataTier_->symbolicLinks[access.target].append(patchAt))
            return false;
    }

    for (const CodeLabel& codeLabel : code.codeLabels) {
        LinkDataTier::InternalLink link;
        link.patchAtOffset = offsetInModule + codeLabel.patchAt().offset();
        link.targetOffset = offsetInModule + codeLabel.target().offset();
        if (!linkDataTier_->internalLinks.append(link))
            return false;
    }

    return true;
}

bool
ModuleGenerator::finishTask(CompileTask* task)
{
    masm_.haltingAlign(CodeAlignment);

    // Before merging in the new function's code, if calls in a prior code range
    // might go out of range, insert far jumps to extend the range.
    if (!InRange(startOfUnpatchedCallsites_, masm_.size() + task->output.bytes.length())) {
        startOfUnpatchedCallsites_ = masm_.size();
        if (!linkCallSites())
            return false;
    }

    if (!linkCompiledCode(task->output))
        return false;

    task->output.clear();

    MOZ_ASSERT(task->inputs.empty());
    MOZ_ASSERT(task->output.empty());
    MOZ_ASSERT(task->lifo.isEmpty());
    freeTasks_.infallibleAppend(task);
    return true;
}

bool
ModuleGenerator::finishFuncExports()
{
    // In addition to all the functions that were explicitly exported, any
    // element of an exported table is also exported.

    for (ElemSegment& elems : env_->elemSegments) {
        if (env_->tables[elems.tableIndex].external) {
            for (uint32_t funcIndex : elems.elemFuncIndices) {
                if (!exportedFuncs_.put(funcIndex))
                    return false;
            }
        }
    }

    // ModuleGenerator::exportedFuncs_ is an unordered HashSet. The
    // FuncExportVector stored in Metadata needs to be stored sorted by
    // function index to allow O(log(n)) lookup at runtime.

    Uint32Vector sorted;
    if (!sorted.reserve(exportedFuncs_.count()))
        return false;

    for (Uint32Set::Range r = exportedFuncs_.all(); !r.empty(); r.popFront())
        sorted.infallibleAppend(r.front());

    std::sort(sorted.begin(), sorted.end());

    MOZ_ASSERT(metadataTier_->funcExports.empty());
    if (!metadataTier_->funcExports.reserve(sorted.length()))
        return false;

    for (uint32_t funcIndex : sorted) {
        Sig sig;
        if (!sig.clone(funcSig(funcIndex)))
            return false;

        metadataTier_->funcExports.infallibleEmplaceBack(Move(sig), funcIndex);
    }

    return true;
}

bool
ModuleGenerator::addFuncImport(const Sig& sig, uint32_t globalDataOffset)
{
    MOZ_ASSERT(!finishedFuncDefs_);

    Sig copy;
    if (!copy.clone(sig))
        return false;

    return metadataTier_->funcImports.emplaceBack(Move(copy), globalDataOffset);
}

bool
ModuleGenerator::allocateGlobalBytes(uint32_t bytes, uint32_t align, uint32_t* globalDataOffset)
{
    CheckedInt<uint32_t> newGlobalDataLength(metadata_->globalDataLength);

    newGlobalDataLength += ComputeByteAlignment(newGlobalDataLength.value(), align);
    if (!newGlobalDataLength.isValid())
        return false;

    *globalDataOffset = newGlobalDataLength.value();
    newGlobalDataLength += bytes;

    if (!newGlobalDataLength.isValid())
        return false;

    metadata_->globalDataLength = newGlobalDataLength.value();
    return true;
}

bool
ModuleGenerator::allocateGlobal(GlobalDesc* global)
{
    MOZ_ASSERT(!startedFuncDefs_);
    unsigned width = 0;
    switch (global->type()) {
      case ValType::I32:
      case ValType::F32:
        width = 4;
        break;
      case ValType::I64:
      case ValType::F64:
        width = 8;
        break;
      case ValType::I8x16:
      case ValType::I16x8:
      case ValType::I32x4:
      case ValType::F32x4:
      case ValType::B8x16:
      case ValType::B16x8:
      case ValType::B32x4:
        width = 16;
        break;
    }

    uint32_t offset;
    if (!allocateGlobalBytes(width, width, &offset))
        return false;

    global->setOffset(offset);
    return true;
}

bool
ModuleGenerator::addGlobal(ValType type, bool isConst, uint32_t* index)
{
    MOZ_ASSERT(isAsmJS());
    MOZ_ASSERT(!startedFuncDefs_);

    *index = env_->globals.length();
    GlobalDesc global(type, !isConst, *index);
    if (!allocateGlobal(&global))
        return false;

    return env_->globals.append(global);
}

bool
ModuleGenerator::addExport(CacheableChars&& fieldName, uint32_t funcIndex)
{
    MOZ_ASSERT(isAsmJS());
    return env_->exports.emplaceBack(Move(fieldName), funcIndex, DefinitionKind::Function) &&
           exportedFuncs_.put(funcIndex);
}

void
ModuleGenerator::initSig(uint32_t sigIndex, Sig&& sig)
{
    MOZ_ASSERT(isAsmJS());
    MOZ_ASSERT(sigIndex == numSigs_);
    numSigs_++;

    MOZ_ASSERT(env_->sigs[sigIndex] == Sig());
    env_->sigs[sigIndex] = Move(sig);
}

const SigWithId&
ModuleGenerator::sig(uint32_t index) const
{
    MOZ_ASSERT(index < numSigs_);
    return env_->sigs[index];
}

void
ModuleGenerator::initFuncSig(uint32_t funcIndex, uint32_t sigIndex)
{
    MOZ_ASSERT(isAsmJS());
    MOZ_ASSERT(!env_->funcSigs[funcIndex]);

    env_->funcSigs[funcIndex] = &env_->sigs[sigIndex];
}

void
ModuleGenerator::initMemoryUsage(MemoryUsage memoryUsage)
{
    MOZ_ASSERT(isAsmJS());
    MOZ_ASSERT(env_->memoryUsage == MemoryUsage::None);

    env_->memoryUsage = memoryUsage;
}

void
ModuleGenerator::bumpMinMemoryLength(uint32_t newMinMemoryLength)
{
    MOZ_ASSERT(isAsmJS());
    MOZ_ASSERT(newMinMemoryLength >= env_->minMemoryLength);

    env_->minMemoryLength = newMinMemoryLength;
}

bool
ModuleGenerator::initImport(uint32_t funcIndex, uint32_t sigIndex)
{
    MOZ_ASSERT(isAsmJS());

    MOZ_ASSERT(!env_->funcSigs[funcIndex]);
    env_->funcSigs[funcIndex] = &env_->sigs[sigIndex];

    uint32_t globalDataOffset;
    if (!allocateGlobalBytes(sizeof(FuncImportTls), sizeof(void*), &globalDataOffset))
        return false;

    MOZ_ASSERT(!env_->funcImportGlobalDataOffsets[funcIndex]);
    env_->funcImportGlobalDataOffsets[funcIndex] = globalDataOffset;

    MOZ_ASSERT(funcIndex == metadataTier_->funcImports.length());
    return addFuncImport(sig(sigIndex), globalDataOffset);
}

const SigWithId&
ModuleGenerator::funcSig(uint32_t funcIndex) const
{
    MOZ_ASSERT(env_->funcSigs[funcIndex]);
    return *env_->funcSigs[funcIndex];
}

bool
ModuleGenerator::startFuncDefs()
{
    MOZ_ASSERT(!startedFuncDefs_);
    MOZ_ASSERT(!finishedFuncDefs_);

    GlobalHelperThreadState& threads = HelperThreadState();
    MOZ_ASSERT(threads.threadCount > 1);

    uint32_t numTasks;
    if (CanUseExtraThreads() && threads.cpuCount > 1) {
        parallel_ = true;
        numTasks = 2 * threads.maxWasmCompilationThreads();
    } else {
        numTasks = 1;
    }

    if (!tasks_.initCapacity(numTasks))
        return false;
    for (size_t i = 0; i < numTasks; i++)
        tasks_.infallibleEmplaceBack(*env_, taskState_, COMPILATION_LIFO_DEFAULT_CHUNK_SIZE);

    if (!freeTasks_.reserve(numTasks))
        return false;
    for (size_t i = 0; i < numTasks; i++)
        freeTasks_.infallibleAppend(&tasks_[i]);

    startedFuncDefs_ = true;
    MOZ_ASSERT(!finishedFuncDefs_);
    return true;
}

static bool
ExecuteCompileTask(CompileTask* task, UniqueChars* error)
{
    MOZ_ASSERT(task->lifo.isEmpty());
    MOZ_ASSERT(task->output.empty());

    switch (task->env.tier()) {
      case Tier::Ion:
        if (!IonCompileFunctions(task->env, task->lifo, task->inputs, &task->output, error))
            return false;
        break;
      case Tier::Baseline:
        if (!BaselineCompileFunctions(task->env, task->lifo, task->inputs, &task->output, error))
            return false;
        break;
    }

    MOZ_ASSERT(task->lifo.isEmpty());
    MOZ_ASSERT(task->inputs.length() == task->output.codeRanges.length());
    task->inputs.clear();
    return true;
}

void
wasm::ExecuteCompileTaskFromHelperThread(CompileTask* task)
{
    TraceLoggerThread* logger = TraceLoggerForCurrentThread();
    AutoTraceLog logCompile(logger, TraceLogger_WasmCompilation);

    UniqueChars error;
    bool ok = ExecuteCompileTask(task, &error);

    auto taskState = task->state.lock();

    if (!ok || !taskState->finished.append(task)) {
        taskState->numFailed++;
        if (!taskState->errorMessage)
            taskState->errorMessage = Move(error);
    }

    taskState->failedOrFinished.notify_one();
}

bool
ModuleGenerator::launchBatchCompile()
{
    MOZ_ASSERT(currentTask_);

    if (cancelled_ && *cancelled_)
        return false;

    if (parallel_) {
        if (!StartOffThreadWasmCompile(currentTask_, mode()))
            return false;
        outstanding_++;
    } else {
        if (!ExecuteCompileTask(currentTask_, error_))
            return false;
        if (!finishTask(currentTask_))
            return false;
    }

    currentTask_ = nullptr;
    batchedBytecode_ = 0;
    return true;
}

bool
ModuleGenerator::finishOutstandingTask()
{
    MOZ_ASSERT(parallel_);

    CompileTask* task = nullptr;
    {
        auto taskState = taskState_.lock();
        while (true) {
            MOZ_ASSERT(outstanding_ > 0);

            if (taskState->numFailed > 0)
                return false;

            if (!taskState->finished.empty()) {
                outstanding_--;
                task = taskState->finished.popCopy();
                break;
            }

            taskState.wait(taskState->failedOrFinished);
        }
    }

    // Call outside of the compilation lock.
    return finishTask(task);
}

bool
ModuleGenerator::compileFuncDef(uint32_t funcIndex, uint32_t lineOrBytecode,
                                Bytes&& bytes, const uint8_t* begin, const uint8_t* end,
                                Uint32Vector&& lineNums)
{
    MOZ_ASSERT(startedFuncDefs_);
    MOZ_ASSERT(!finishedFuncDefs_);
    MOZ_ASSERT_IF(mode() == CompileMode::Tier1, funcIndex < env_->numFuncs());

    numFuncDefs_++;

    if (!currentTask_) {
        if (freeTasks_.empty() && !finishOutstandingTask())
            return false;
        currentTask_ = freeTasks_.popCopy();
    }

    uint32_t funcBytecodeLength = end - begin;

    FuncCompileInputVector& inputs = currentTask_->inputs;
    if (!inputs.emplaceBack(funcIndex, lineOrBytecode, Move(bytes), begin, end, Move(lineNums)))
        return false;

    uint32_t threshold;
    switch (tier()) {
      case Tier::Baseline: threshold = JitOptions.wasmBatchBaselineThreshold; break;
      case Tier::Ion:      threshold = JitOptions.wasmBatchIonThreshold;      break;
      default:             MOZ_CRASH("Invalid tier value");                   break;
    }

    batchedBytecode_ += funcBytecodeLength;
    MOZ_ASSERT(batchedBytecode_ <= MaxModuleBytes);
    return batchedBytecode_ <= threshold || launchBatchCompile();
}

bool
ModuleGenerator::compileFuncDef(uint32_t funcIndex, uint32_t lineOrBytecode,
                                const uint8_t* begin, const uint8_t* end)
{
    return compileFuncDef(funcIndex, lineOrBytecode, Bytes(), begin, end, Uint32Vector());
}

bool
ModuleGenerator::compileFuncDef(uint32_t funcIndex, uint32_t lineOrBytecode,
                                Bytes&& bytes, Uint32Vector&& lineNums)
{
    return compileFuncDef(funcIndex, lineOrBytecode, Move(bytes), bytes.begin(), bytes.end(), Move(lineNums));
}

bool
ModuleGenerator::finishFuncDefs()
{
    MOZ_ASSERT(startedFuncDefs_);
    MOZ_ASSERT(!finishedFuncDefs_);

    if (currentTask_ && !launchBatchCompile())
        return false;

    while (outstanding_ > 0) {
        if (!finishOutstandingTask())
            return false;
    }

    MOZ_ASSERT_IF(!isAsmJS(), numFuncDefs_ == env_->numFuncDefs());
    finishedFuncDefs_ = true;
    return true;
}

bool
ModuleGenerator::initSigTableLength(uint32_t sigIndex, uint32_t length)
{
    MOZ_ASSERT(isAsmJS());
    MOZ_ASSERT(length != 0);
    MOZ_ASSERT(length <= MaxTableInitialLength);

    MOZ_ASSERT(env_->asmJSSigToTableIndex[sigIndex] == 0);
    env_->asmJSSigToTableIndex[sigIndex] = numTables_;

    TableDesc& table = env_->tables[numTables_++];
    table.kind = TableKind::TypedFunction;
    table.limits.initial = length;
    table.limits.maximum = Some(length);
    return allocateGlobalBytes(sizeof(TableTls), sizeof(void*), &table.globalDataOffset);
}

bool
ModuleGenerator::initSigTableElems(uint32_t sigIndex, Uint32Vector&& elemFuncIndices)
{
    MOZ_ASSERT(isAsmJS());
    MOZ_ASSERT(finishedFuncDefs_);

    uint32_t tableIndex = env_->asmJSSigToTableIndex[sigIndex];
    MOZ_ASSERT(env_->tables[tableIndex].limits.initial == elemFuncIndices.length());

    InitExpr offset(Val(uint32_t(0)));
    return env_->elemSegments.emplaceBack(tableIndex, offset, Move(elemFuncIndices));
}

bool
ModuleGenerator::finishLinking()
{
    // All functions and traps CodeRanges should have been processed.

#ifdef DEBUG
    if (isAsmJS()) {
        for (uint32_t i = 0; i < AsmJSFirstDefFuncIndex; i++)
            MOZ_ASSERT(funcToCodeRange_[i] == BAD_CODE_RANGE);
        for (uint32_t i = AsmJSFirstDefFuncIndex; i < AsmJSFirstDefFuncIndex + numFuncDefs_; i++)
            MOZ_ASSERT(funcToCodeRange_[i] != BAD_CODE_RANGE);
        for (uint32_t i = AsmJSFirstDefFuncIndex + numFuncDefs_; i < funcToCodeRange_.length(); i++)
            MOZ_ASSERT(funcToCodeRange_[i] == BAD_CODE_RANGE);
    } else {
        for (uint32_t codeRangeIndex : funcToCodeRange_)
            MOZ_ASSERT(codeRangeIndex != BAD_CODE_RANGE);
    }
#endif

    // Now that all functions and stubs are generated and their CodeRanges
    // known, patch all calls (which can emit far jumps) and far jumps. 

    if (!linkCallSites())
        return false;

    for (CallFarJump far : callFarJumps_)
        masm_.patchFarJump(far.jump, funcCodeRange(far.funcIndex).funcNormalEntry());

    for (TrapFarJump far : trapFarJumps_)
        masm_.patchFarJump(far.jump, trapCodeOffsets_[far.trap]);

    for (CodeOffset farJump : debugTrapFarJumps_)
        masm_.patchFarJump(farJump, debugTrapCodeOffset_);

    // None of the linking or far-jump operations should emit masm metadata.

    MOZ_ASSERT(masm_.callSites().empty());
    MOZ_ASSERT(masm_.callSiteTargets().empty());
    MOZ_ASSERT(masm_.trapSites().empty());
    MOZ_ASSERT(masm_.trapFarJumps().empty());
    MOZ_ASSERT(masm_.callFarJumps().empty());
    MOZ_ASSERT(masm_.memoryAccesses().empty());
    MOZ_ASSERT(masm_.symbolicAccesses().empty());
    MOZ_ASSERT(masm_.codeLabels().empty());

    masm_.finish();
    return !masm_.oom();
}

bool
ModuleGenerator::finishMetadata(const ShareableBytes& bytecode)
{
#ifdef DEBUG
    // Assert CodeRanges are sorted.
    uint32_t lastEnd = 0;
    for (const CodeRange& codeRange : metadataTier_->codeRanges) {
        MOZ_ASSERT(codeRange.begin() >= lastEnd);
        lastEnd = codeRange.end();
    }

    // Assert debugTrapFarJumpOffsets are sorted.
    uint32_t lastOffset = 0;
    for (uint32_t debugTrapFarJumpOffset : metadataTier_->debugTrapFarJumpOffsets) {
        MOZ_ASSERT(debugTrapFarJumpOffset >= lastOffset);
        lastOffset = debugTrapFarJumpOffset;
    }
#endif

    // Copy over data from the ModuleEnvironment.

    metadata_->memoryUsage = env_->memoryUsage;
    metadata_->minMemoryLength = env_->minMemoryLength;
    metadata_->maxMemoryLength = env_->maxMemoryLength;
    metadata_->tables = Move(env_->tables);
    metadata_->globals = Move(env_->globals);
    metadata_->funcNames = Move(env_->funcNames);
    metadata_->customSections = Move(env_->customSections);

    // Inflate the global bytes up to page size so that the total bytes are a
    // page size (as required by the allocator functions).

    metadata_->globalDataLength = AlignBytes(metadata_->globalDataLength, gc::SystemPageSize());

    // These Vectors can get large and the excess capacity can be significant,
    // so realloc them down to size.

    metadataTier_->memoryAccesses.podResizeToFit();
    metadataTier_->codeRanges.podResizeToFit();
    metadataTier_->callSites.podResizeToFit();
    metadataTier_->debugTrapFarJumpOffsets.podResizeToFit();
    metadataTier_->debugFuncToCodeRange.podResizeToFit();

    // For asm.js, the tables vector is over-allocated (to avoid resize during
    // parallel copilation). Shrink it back down to fit.

    if (isAsmJS() && !metadata_->tables.resize(numTables_))
        return false;

    // Complete function exports and element segments with code range indices,
    // now that every function has a code range.

    for (FuncExport& fe : metadataTier_->funcExports)
        fe.initCodeRangeIndex(funcToCodeRange_[fe.funcIndex()]);

    for (ElemSegment& elems : env_->elemSegments) {
        Uint32Vector& codeRangeIndices = elems.elemCodeRangeIndices(tier());
        MOZ_ASSERT(codeRangeIndices.empty());
        if (!codeRangeIndices.reserve(elems.elemFuncIndices.length()))
            return false;
        for (uint32_t funcIndex : elems.elemFuncIndices)
            codeRangeIndices.infallibleAppend(funcToCodeRange_[funcIndex]);
    }

    // Copy over additional debug information.

    if (env_->debugEnabled()) {
        metadata_->debugEnabled = true;

        const size_t numSigs = env_->funcSigs.length();
        if (!metadata_->debugFuncArgTypes.resize(numSigs))
            return false;
        if (!metadata_->debugFuncReturnTypes.resize(numSigs))
            return false;
        for (size_t i = 0; i < numSigs; i++) {
            if (!metadata_->debugFuncArgTypes[i].appendAll(env_->funcSigs[i]->args()))
                return false;
            metadata_->debugFuncReturnTypes[i] = env_->funcSigs[i]->ret();
        }
        metadataTier_->debugFuncToCodeRange = Move(funcToCodeRange_);

        static_assert(sizeof(ModuleHash) <= sizeof(mozilla::SHA1Sum::Hash),
                      "The ModuleHash size shall not exceed the SHA1 hash size.");
        mozilla::SHA1Sum::Hash hash;
        mozilla::SHA1Sum sha1Sum;
        sha1Sum.update(bytecode.begin(), bytecode.length());
        sha1Sum.finish(hash);
        memcpy(metadata_->debugHash, hash, sizeof(ModuleHash));
    }

    return true;
}

UniqueConstCodeSegment
ModuleGenerator::finishCodeSegment(const ShareableBytes& bytecode)
{
    MOZ_ASSERT(finishedFuncDefs_);

    // Because of asm.js, we can only generate the FuncExportVector at the end
    // of module generation (after we've seen the end of the exports object at
    // the end of the asm.js module).

    if (!finishFuncExports())
        return nullptr;

    // Now that all imports/exports are known, we can generate a special
    // CompiledCode containing stubs.

    CompiledCode& stubCode = tasks_[0].output;
    MOZ_ASSERT(stubCode.empty());

    if (!GenerateStubs(*env_, metadataTier_->funcImports, metadataTier_->funcExports, &stubCode))
        return nullptr;

    if (!linkCompiledCode(stubCode))
        return nullptr;

    // Now that all code is linked in masm_, patch calls and far jumps and
    // finish the metadata. Linking can emit tiny far-jump stubs, so there is an
    // ordering dependency here.

    if (!finishLinking())
        return nullptr;

    if (!finishMetadata(bytecode))
        return nullptr;

    return CodeSegment::create(tier(), masm_, bytecode, *linkDataTier_, *metadata_);
}

UniqueJumpTable
ModuleGenerator::createJumpTable(const CodeSegment& codeSegment)
{
    MOZ_ASSERT(mode() == CompileMode::Tier1);
    MOZ_ASSERT(!isAsmJS());

    uint32_t tableSize = env_->numFuncs();
    UniqueJumpTable jumpTable(js_pod_calloc<void*>(tableSize));
    if (!jumpTable)
        return nullptr;

    uint8_t* codeBase = codeSegment.base();
    for (const CodeRange& codeRange : metadataTier_->codeRanges) {
        if (codeRange.isFunction())
            jumpTable[codeRange.funcIndex()] = codeBase + codeRange.funcTierEntry();
    }

    return jumpTable;
}

SharedModule
ModuleGenerator::finishModule(const ShareableBytes& bytecode)
{
    MOZ_ASSERT(mode() == CompileMode::Once || mode() == CompileMode::Tier1);

    UniqueConstCodeSegment codeSegment = finishCodeSegment(bytecode);
    if (!codeSegment)
        return nullptr;

    UniqueJumpTable maybeJumpTable;
    if (mode() == CompileMode::Tier1) {
        maybeJumpTable = createJumpTable(*codeSegment);
        if (!maybeJumpTable)
            return nullptr;
    }

    UniqueConstBytes maybeDebuggingBytes;
    if (env_->debugEnabled()) {
        MOZ_ASSERT(mode() == CompileMode::Once);
        Bytes bytes;
        if (!bytes.resize(masm_.bytesNeeded()))
            return nullptr;
        masm_.executableCopy(bytes.begin(), /* flushICache = */ false);
        maybeDebuggingBytes = js::MakeUnique<Bytes>(Move(bytes));
        if (!maybeDebuggingBytes)
            return nullptr;
    }

    SharedCode code = js_new<Code>(Move(codeSegment), *metadata_, Move(maybeJumpTable));
    if (!code)
        return nullptr;

    SharedModule module(js_new<Module>(Move(assumptions_),
                                       *code,
                                       Move(maybeDebuggingBytes),
                                       Move(linkData_),
                                       Move(env_->imports),
                                       Move(env_->exports),
                                       Move(env_->dataSegments),
                                       Move(env_->elemSegments),
                                       bytecode));
    if (!module)
        return nullptr;

    if (mode() == CompileMode::Tier1)
        module->startTier2(*compileArgs_);

    return module;
}

bool
ModuleGenerator::finishTier2(Module& module)
{
    MOZ_ASSERT(mode() == CompileMode::Tier2);
    MOZ_ASSERT(tier() == Tier::Ion);
    MOZ_ASSERT(!env_->debugEnabled());

    if (cancelled_ && *cancelled_)
        return false;

    UniqueConstCodeSegment codeSegment = finishCodeSegment(module.bytecode());
    if (!codeSegment)
        return false;

    module.finishTier2(linkData_.takeLinkData(tier()),
                       metadata_->takeMetadata(tier()),
                       Move(codeSegment),
                       env_);
    return true;
}
