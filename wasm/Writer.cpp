//===- Writer.cpp ---------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Writer.h"
#include "Config.h"
#include "InputChunks.h"
#include "InputGlobal.h"
#include "OutputSections.h"
#include "OutputSegment.h"
#include "SymbolTable.h"
#include "WriterUtils.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "lld/Common/Strings.h"
#include "lld/Common/Threads.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/BinaryFormat/Wasm.h"
#include "llvm/Object/WasmTraits.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/Path.h"

#include <cstdarg>
#include <map>
#include <snax/abimerge.hpp>
#include <snax/utils.hpp>

#define DEBUG_TYPE "lld"

using namespace llvm;
using namespace llvm::wasm;
using namespace lld;
using namespace lld::wasm;

static constexpr int kStackAlignment = 16;
static constexpr int kInitialTableOffset = 1;
static constexpr const char *kFunctionTableName = "__indirect_function_table";

namespace {

// An init entry to be written to either the synthetic init func or the
// linking metadata.
struct WasmInitEntry {
  const FunctionSymbol *Sym;
  uint32_t Priority;
};

// The writer writes a SymbolTable result to a file.
class Writer {
public:
  void run(bool is_entry_defined);

private:
  void openFile();

  uint32_t lookupType(const WasmSignature &Sig);
  uint32_t registerType(const WasmSignature &Sig);

  void createCtorFunction();
  void createDispatchFunction();
  void calculateInitFunctions();
  void assignIndexes();
  void calculateImports();
  void calculateExports();
  void calculateCustomSections();
  void assignSymtab();
  void calculateTypes();
  void createOutputSegments();
  void layoutMemory();
  void createHeader();
  void createSections();
  SyntheticSection *createSyntheticSection(uint32_t Type, StringRef Name = "");

  // Builtin sections
  void createTypeSection();
  void createFunctionSection();
  void createTableSection();
  void createGlobalSection();
  void createExportSection();
  void createImportSection();
  void createMemorySection();
  void createElemSection();
  void createCodeSection();
  void createDataSection();
  void createCustomSections();

  // Custom sections
  void createRelocSections();
  void createLinkingSection();
  void createNameSection();

  void writeHeader();
  void writeSections();
  void writeABI() {
     if (abis.empty())
        return;
     try {
        ABIMerger merger(ojson::parse(abis.back()));
        for (auto abi : abis) {
           merger.set_abi(merger.merge(ojson::parse(abi)));
        }
        SmallString<64> output_file = Config->OutputFile;
        llvm::sys::path::replace_extension(output_file, ".abi");
        Expected<std::unique_ptr<FileOutputBuffer>> BufferOrErr =
          FileOutputBuffer::create(output_file, merger.get_abi_string().size());

        if (!BufferOrErr)
          error("failed to open " + Config->OutputFile + ": " +
                toString(BufferOrErr.takeError()));
        else {
          auto Buffer = std::move(*BufferOrErr);
          memcpy(Buffer->getBufferStart(), merger.get_abi_string().c_str(), merger.get_abi_string().size());
          if (Error E = Buffer->commit())
             fatal("failed to write the output file: " + toString(std::move(E)));
        }
     } catch (std::runtime_error& err) {
        fatal(std::string(std::string("failed to write abi: ") + err.what()).c_str());
     } catch (jsoncons::json_exception& ex) {
        fatal(std::string(std::string("failed to write abi") + ex.what()).c_str());
     }
  }

  uint64_t FileSize = 0;
  uint32_t NumMemoryPages = 0;
  uint32_t MaxMemoryPages = 0;

  std::vector<const WasmSignature *> Types;
  DenseMap<WasmSignature, int32_t> TypeIndices;
  std::vector<const Symbol *> ImportedSymbols;
  unsigned NumImportedFunctions = 0;
  unsigned NumImportedGlobals = 0;
  std::vector<WasmExport> Exports;
  std::vector<const DefinedData *> DefinedFakeGlobals;
  std::vector<InputGlobal *> InputGlobals;
  std::vector<InputFunction *> InputFunctions;
  std::vector<const FunctionSymbol *> IndirectFunctions;
  std::vector<const Symbol *> SymtabEntries;
  std::vector<WasmInitEntry> InitFunctions;
  std::vector<std::string> abis;

  llvm::StringMap<std::vector<InputSection *>> CustomSectionMapping;
  llvm::StringMap<SectionSymbol *> CustomSectionSymbols;

  // Elements that are used to construct the final output
  std::string Header;
  std::vector<OutputSection *> OutputSections;

  std::unique_ptr<FileOutputBuffer> Buffer;

  std::vector<OutputSegment *> Segments;
  llvm::SmallDenseMap<StringRef, OutputSegment *> SegmentMap;
};

} // anonymous namespace

void Writer::createImportSection() {
  uint32_t NumImports = ImportedSymbols.size();
  if (Config->ImportMemory)
    ++NumImports;
  if (Config->ImportTable)
    ++NumImports;

  if (NumImports == 0)
    return;

  SyntheticSection *Section = createSyntheticSection(WASM_SEC_IMPORT);
  raw_ostream &OS = Section->getStream();

  writeUleb128(OS, NumImports, "import count");

  if (Config->ImportMemory) {
    WasmImport Import;
    Import.Module = "env";
    Import.Field = "memory";
    Import.Kind = WASM_EXTERNAL_MEMORY;
    Import.Memory.Flags = 0;
    Import.Memory.Initial = NumMemoryPages;
    if (MaxMemoryPages != 0) {
      Import.Memory.Flags |= WASM_LIMITS_FLAG_HAS_MAX;
      Import.Memory.Maximum = MaxMemoryPages;
    }
    writeImport(OS, Import);
  }

  if (Config->ImportTable) {
    uint32_t TableSize = kInitialTableOffset + IndirectFunctions.size();
    WasmImport Import;
    Import.Module = "env";
    Import.Field = kFunctionTableName;
    Import.Kind = WASM_EXTERNAL_TABLE;
    Import.Table.ElemType = WASM_TYPE_ANYFUNC;
    Import.Table.Limits = {WASM_LIMITS_FLAG_HAS_MAX, TableSize, TableSize};
    writeImport(OS, Import);
  }

  for (const Symbol *Sym : ImportedSymbols) {
    WasmImport Import;
    Import.Module = "env";
    Import.Field = Sym->getName();
    if (auto *FunctionSym = dyn_cast<FunctionSymbol>(Sym)) {
      Import.Kind = WASM_EXTERNAL_FUNCTION;
      Import.SigIndex = lookupType(*FunctionSym->getFunctionType());
    } else {
      auto *GlobalSym = cast<GlobalSymbol>(Sym);
      Import.Kind = WASM_EXTERNAL_GLOBAL;
      Import.Global = *GlobalSym->getGlobalType();
    }
    writeImport(OS, Import);
  }
}

void Writer::createTypeSection() {
  SyntheticSection *Section = createSyntheticSection(WASM_SEC_TYPE);
  raw_ostream &OS = Section->getStream();
  writeUleb128(OS, Types.size(), "type count");
  for (const WasmSignature *Sig : Types)
    writeSig(OS, *Sig);
}

void Writer::createFunctionSection() {
  if (InputFunctions.empty())
    return;

  SyntheticSection *Section = createSyntheticSection(WASM_SEC_FUNCTION);
  raw_ostream &OS = Section->getStream();

  writeUleb128(OS, InputFunctions.size(), "function count");
  for (const InputFunction *Func : InputFunctions) {
    writeUleb128(OS, lookupType(Func->Signature), "sig index");
  }
}

void Writer::createMemorySection() {
  if (Config->ImportMemory)
    return;

  SyntheticSection *Section = createSyntheticSection(WASM_SEC_MEMORY);
  raw_ostream &OS = Section->getStream();

  bool HasMax = MaxMemoryPages != 0;
  writeUleb128(OS, 1, "memory count");
  writeUleb128(OS, HasMax ? static_cast<unsigned>(WASM_LIMITS_FLAG_HAS_MAX) : 0,
               "memory limits flags");
  writeUleb128(OS, NumMemoryPages, "initial pages");
  if (HasMax)
    writeUleb128(OS, MaxMemoryPages, "max pages");
}

void Writer::createGlobalSection() {
  unsigned NumGlobals = InputGlobals.size() + DefinedFakeGlobals.size();
  if (NumGlobals == 0)
    return;

  SyntheticSection *Section = createSyntheticSection(WASM_SEC_GLOBAL);
  raw_ostream &OS = Section->getStream();

  writeUleb128(OS, NumGlobals, "global count");
  for (const InputGlobal *G : InputGlobals)
    writeGlobal(OS, G->Global);
  for (const DefinedData *Sym : DefinedFakeGlobals) {
    WasmGlobal Global;
    Global.Type = {WASM_TYPE_I32, false};
    Global.InitExpr.Opcode = WASM_OPCODE_I32_CONST;
    Global.InitExpr.Value.Int32 = Sym->getVirtualAddress();
    writeGlobal(OS, Global);
  }
}

void Writer::createTableSection() {
  if (Config->ImportTable)
    return;

  // Always output a table section (or table import), even if there are no
  // indirect calls.  There are two reasons for this:
  //  1. For executables it is useful to have an empty table slot at 0
  //     which can be filled with a null function call handler.
  //  2. If we don't do this, any program that contains a call_indirect but
  //     no address-taken function will fail at validation time since it is
  //     a validation error to include a call_indirect instruction if there
  //     is not table.
  uint32_t TableSize = kInitialTableOffset + IndirectFunctions.size();

  SyntheticSection *Section = createSyntheticSection(WASM_SEC_TABLE);
  raw_ostream &OS = Section->getStream();

  writeUleb128(OS, 1, "table count");
  WasmLimits Limits = {WASM_LIMITS_FLAG_HAS_MAX, TableSize, TableSize};
  writeTableType(OS, WasmTable{WASM_TYPE_ANYFUNC, Limits});
}

void Writer::createExportSection() {
  if (!Exports.size())
    return;

  SyntheticSection *Section = createSyntheticSection(WASM_SEC_EXPORT);
  raw_ostream &OS = Section->getStream();

  std::vector<WasmExport> filtered_exports;
  for (auto ex : Exports) {
     if (Config->should_export(ex))
        filtered_exports.push_back(ex);
  }
  writeUleb128(OS, filtered_exports.size(), "export count");
  for (const WasmExport &Export : filtered_exports) {
     writeExport(OS, Export);
  }
  Exports = filtered_exports;
}

void Writer::calculateCustomSections() {
  log("calculateCustomSections");
  bool StripDebug = Config->StripDebug || Config->StripAll;
  for (ObjFile *File : Symtab->ObjectFiles) {
    for (InputSection *Section : File->CustomSections) {
      StringRef Name = Section->getName();
      // These custom sections are known the linker and synthesized rather than
      // blindly copied
      if (Name == "linking" || Name == "name" || Name.startswith("reloc."))
        continue;
      // .. or it is a debug section
      if (StripDebug && Name.startswith(".debug_"))
        continue;
      CustomSectionMapping[Name].push_back(Section);
    }
  }
}

void Writer::createCustomSections() {
  log("createCustomSections");
  for (auto &Pair : CustomSectionMapping) {
    StringRef Name = Pair.first();

    auto P = CustomSectionSymbols.find(Name);
    if (P != CustomSectionSymbols.end()) {
      uint32_t SectionIndex = OutputSections.size();
      P->second->setOutputSectionIndex(SectionIndex);
    }

    LLVM_DEBUG(dbgs() << "createCustomSection: " << Name << "\n");
    OutputSections.push_back(make<CustomSection>(Name, Pair.second));
  }
}

void Writer::createElemSection() {
  if (IndirectFunctions.empty())
    return;

  SyntheticSection *Section = createSyntheticSection(WASM_SEC_ELEM);
  raw_ostream &OS = Section->getStream();

  writeUleb128(OS, 1, "segment count");
  writeUleb128(OS, 0, "table index");
  WasmInitExpr InitExpr;
  InitExpr.Opcode = WASM_OPCODE_I32_CONST;
  InitExpr.Value.Int32 = kInitialTableOffset;
  writeInitExpr(OS, InitExpr);
  writeUleb128(OS, IndirectFunctions.size(), "elem count");

  uint32_t TableIndex = kInitialTableOffset;
  for (const FunctionSymbol *Sym : IndirectFunctions) {
    assert(Sym->getTableIndex() == TableIndex);
    writeUleb128(OS, Sym->getFunctionIndex(), "function index");
    ++TableIndex;
  }
}

void Writer::createCodeSection() {
  if (InputFunctions.empty())
    return;

  log("createCodeSection");

  auto Section = make<CodeSection>(InputFunctions);
  OutputSections.push_back(Section);
}

void Writer::createDataSection() {
  if (!Segments.size())
    return;

  log("createDataSection");
  auto Section = make<DataSection>(Segments);
  OutputSections.push_back(Section);
}

// Create relocations sections in the final output.
// These are only created when relocatable output is requested.
void Writer::createRelocSections() {
  log("createRelocSections");
  // Don't use iterator here since we are adding to OutputSection
  size_t OrigSize = OutputSections.size();
  for (size_t I = 0; I < OrigSize; I++) {
    OutputSection *OSec = OutputSections[I];
    uint32_t Count = OSec->numRelocations();
    if (!Count)
      continue;

    StringRef Name;
    if (OSec->Type == WASM_SEC_DATA)
      Name = "reloc.DATA";
    else if (OSec->Type == WASM_SEC_CODE)
      Name = "reloc.CODE";
    else if (OSec->Type == WASM_SEC_CUSTOM)
      Name = Saver.save("reloc." + OSec->Name);
    else
      llvm_unreachable(
          "relocations only supported for code, data, or custom sections");

    SyntheticSection *Section = createSyntheticSection(WASM_SEC_CUSTOM, Name);
    raw_ostream &OS = Section->getStream();
    writeUleb128(OS, I, "reloc section");
    writeUleb128(OS, Count, "reloc count");
    OSec->writeRelocations(OS);
  }
}

static uint32_t getWasmFlags(const Symbol *Sym) {
  uint32_t Flags = 0;
  if (Sym->isLocal())
    Flags |= WASM_SYMBOL_BINDING_LOCAL;
  if (Sym->isWeak())
    Flags |= WASM_SYMBOL_BINDING_WEAK;
  if (Sym->isHidden())
    Flags |= WASM_SYMBOL_VISIBILITY_HIDDEN;
  if (Sym->isUndefined())
    Flags |= WASM_SYMBOL_UNDEFINED;
  return Flags;
}

// Some synthetic sections (e.g. "name" and "linking") have subsections.
// Just like the synthetic sections themselves these need to be created before
// they can be written out (since they are preceded by their length). This
// class is used to create subsections and then write them into the stream
// of the parent section.
class SubSection {
public:
  explicit SubSection(uint32_t Type) : Type(Type) {}

  void writeTo(raw_ostream &To) {
    OS.flush();
    writeUleb128(To, Type, "subsection type");
    writeUleb128(To, Body.size(), "subsection size");
    To.write(Body.data(), Body.size());
  }

private:
  uint32_t Type;
  std::string Body;

public:
  raw_string_ostream OS{Body};
};

// Create the custom "linking" section containing linker metadata.
// This is only created when relocatable output is requested.
void Writer::createLinkingSection() {
  SyntheticSection *Section =
      createSyntheticSection(WASM_SEC_CUSTOM, "linking");
  raw_ostream &OS = Section->getStream();

  writeUleb128(OS, WasmMetadataVersion, "Version");

  if (!SymtabEntries.empty()) {
    SubSection Sub(WASM_SYMBOL_TABLE);
    writeUleb128(Sub.OS, SymtabEntries.size(), "num symbols");

    for (const Symbol *Sym : SymtabEntries) {
      assert(Sym->isDefined() || Sym->isUndefined());
      WasmSymbolType Kind = Sym->getWasmType();
      uint32_t Flags = getWasmFlags(Sym);

      writeU8(Sub.OS, Kind, "sym kind");
      writeUleb128(Sub.OS, Flags, "sym flags");

      if (auto *F = dyn_cast<FunctionSymbol>(Sym)) {
        writeUleb128(Sub.OS, F->getFunctionIndex(), "index");
        if (Sym->isDefined())
          writeStr(Sub.OS, Sym->getName(), "sym name");
      } else if (auto *G = dyn_cast<GlobalSymbol>(Sym)) {
        writeUleb128(Sub.OS, G->getGlobalIndex(), "index");
        if (Sym->isDefined())
          writeStr(Sub.OS, Sym->getName(), "sym name");
      } else if (isa<DataSymbol>(Sym)) {
        writeStr(Sub.OS, Sym->getName(), "sym name");
        if (auto *DataSym = dyn_cast<DefinedData>(Sym)) {
          writeUleb128(Sub.OS, DataSym->getOutputSegmentIndex(), "index");
          writeUleb128(Sub.OS, DataSym->getOutputSegmentOffset(),
                       "data offset");
          writeUleb128(Sub.OS, DataSym->getSize(), "data size");
        }
      } else {
        auto *S = cast<SectionSymbol>(Sym);
        writeUleb128(Sub.OS, S->getOutputSectionIndex(), "sym section index");
      }
    }

    Sub.writeTo(OS);
  }

  if (Segments.size()) {
    SubSection Sub(WASM_SEGMENT_INFO);
    writeUleb128(Sub.OS, Segments.size(), "num data segments");
    for (const OutputSegment *S : Segments) {
      writeStr(Sub.OS, S->Name, "segment name");
      writeUleb128(Sub.OS, S->Alignment, "alignment");
      writeUleb128(Sub.OS, 0, "flags");
    }
    Sub.writeTo(OS);
  }

  if (!InitFunctions.empty()) {
    SubSection Sub(WASM_INIT_FUNCS);
    writeUleb128(Sub.OS, InitFunctions.size(), "num init functions");
    for (const WasmInitEntry &F : InitFunctions) {
      writeUleb128(Sub.OS, F.Priority, "priority");
      writeUleb128(Sub.OS, F.Sym->getOutputSymbolIndex(), "function index");
    }
    Sub.writeTo(OS);
  }

  struct ComdatEntry {
    unsigned Kind;
    uint32_t Index;
  };
  std::map<StringRef, std::vector<ComdatEntry>> Comdats;

  for (const InputFunction *F : InputFunctions) {
    StringRef Comdat = F->getComdatName();
    if (!Comdat.empty())
      Comdats[Comdat].emplace_back(
          ComdatEntry{WASM_COMDAT_FUNCTION, F->getFunctionIndex()});
  }
  for (uint32_t I = 0; I < Segments.size(); ++I) {
    const auto &InputSegments = Segments[I]->InputSegments;
    if (InputSegments.empty())
      continue;
    StringRef Comdat = InputSegments[0]->getComdatName();
#ifndef NDEBUG
    for (const InputSegment *IS : InputSegments)
      assert(IS->getComdatName() == Comdat);
#endif
    if (!Comdat.empty())
      Comdats[Comdat].emplace_back(ComdatEntry{WASM_COMDAT_DATA, I});
  }

  if (!Comdats.empty()) {
    SubSection Sub(WASM_COMDAT_INFO);
    writeUleb128(Sub.OS, Comdats.size(), "num comdats");
    for (const auto &C : Comdats) {
      writeStr(Sub.OS, C.first, "comdat name");
      writeUleb128(Sub.OS, 0, "comdat flags"); // flags for future use
      writeUleb128(Sub.OS, C.second.size(), "num entries");
      for (const ComdatEntry &Entry : C.second) {
        writeU8(Sub.OS, Entry.Kind, "entry kind");
        writeUleb128(Sub.OS, Entry.Index, "entry index");
      }
    }
    Sub.writeTo(OS);
  }
}

// Create the custom "name" section containing debug symbol names.
void Writer::createNameSection() {
  unsigned NumNames = NumImportedFunctions;
  for (const InputFunction *F : InputFunctions)
    if (!F->getName().empty() || !F->getDebugName().empty())
      ++NumNames;

  if (NumNames == 0)
    return;

  SyntheticSection *Section = createSyntheticSection(WASM_SEC_CUSTOM, "name");

  SubSection Sub(WASM_NAMES_FUNCTION);
  writeUleb128(Sub.OS, NumNames, "name count");

  // Names must appear in function index order.  As it happens ImportedSymbols
  // and InputFunctions are numbered in order with imported functions coming
  // first.
  for (const Symbol *S : ImportedSymbols) {
    if (auto *F = dyn_cast<FunctionSymbol>(S)) {
      writeUleb128(Sub.OS, F->getFunctionIndex(), "func index");
      Optional<std::string> Name = demangleItanium(F->getName());
      writeStr(Sub.OS, Name ? StringRef(*Name) : F->getName(), "symbol name");
    }
  }
  for (const InputFunction *F : InputFunctions) {
    if (!F->getName().empty()) {
      writeUleb128(Sub.OS, F->getFunctionIndex(), "func index");
      if (!F->getDebugName().empty()) {
        writeStr(Sub.OS, F->getDebugName(), "symbol name");
      } else {
        Optional<std::string> Name = demangleItanium(F->getName());
        writeStr(Sub.OS, Name ? StringRef(*Name) : F->getName(), "symbol name");
      }
    }
  }

  Sub.writeTo(Section->getStream());
}

void Writer::writeHeader() {
  memcpy(Buffer->getBufferStart(), Header.data(), Header.size());
}

void Writer::writeSections() {
  uint8_t *Buf = Buffer->getBufferStart();
  parallelForEach(OutputSections, [Buf](OutputSection *S) { S->writeTo(Buf); });
}

// Fix the memory layout of the output binary.  This assigns memory offsets
// to each of the input data sections as well as the explicit stack region.
// The default memory layout is as follows, from low to high.
//
//  - initialized data (starting at Config->GlobalBase)
//  - BSS data (not currently implemented in llvm)
//  - explicit stack (Config->ZStackSize)
//  - heap start / unallocated
//
// The --stack-first option means that stack is placed before any static data.
// This can be useful since it means that stack overflow traps immediately rather
// than overwriting global data, but also increases code size since all static
// data loads and stores requires larger offsets.
void Writer::layoutMemory() {
  createOutputSegments();

  uint32_t MemoryPtr = 0;

  auto PlaceStack = [&]() {
    if (Config->Relocatable)
      return;
    MemoryPtr = alignTo(MemoryPtr, kStackAlignment);
    if (Config->ZStackSize != alignTo(Config->ZStackSize, kStackAlignment))
      error("stack size must be " + Twine(kStackAlignment) + "-byte aligned");
    log("mem: stack size  = " + Twine(Config->ZStackSize));
    log("mem: stack base  = " + Twine(MemoryPtr));
    MemoryPtr += Config->ZStackSize;
    WasmSym::StackPointer->Global->Global.InitExpr.Value.Int32 = MemoryPtr;
    log("mem: stack top   = " + Twine(MemoryPtr));
  };

  if (Config->StackFirst) {
    PlaceStack();
  } else {
    MemoryPtr = Config->GlobalBase;
    log("mem: global base = " + Twine(Config->GlobalBase));
  }

  uint32_t DataStart = MemoryPtr;

  // Arbitrarily set __dso_handle handle to point to the start of the data
  // segments.
  if (WasmSym::DsoHandle)
    WasmSym::DsoHandle->setVirtualAddress(DataStart);

  for (OutputSegment *Seg : Segments) {
    MemoryPtr = alignTo(MemoryPtr, Seg->Alignment);
    Seg->StartVA = MemoryPtr;
    log(formatv("mem: {0,-15} offset={1,-8} size={2,-8} align={3}", Seg->Name,
                MemoryPtr, Seg->Size, Seg->Alignment));
    MemoryPtr += Seg->Size;
  }

  // TODO: Add .bss space here.
  if (WasmSym::DataEnd)
    WasmSym::DataEnd->setVirtualAddress(MemoryPtr);

  log("mem: static data = " + Twine(MemoryPtr - DataStart));

  if (!Config->StackFirst)
    PlaceStack();

  // Set `__heap_base` to directly follow the end of the stack or global data.
  // The fact that this comes last means that a malloc/brk implementation
  // can grow the heap at runtime.
  if (!Config->Relocatable) {
    WasmSym::HeapBase->setVirtualAddress(MemoryPtr);
    log("mem: heap base   = " + Twine(MemoryPtr));
  }

  if (Config->InitialMemory != 0) {
    if (Config->InitialMemory != alignTo(Config->InitialMemory, WasmPageSize))
      error("initial memory must be " + Twine(WasmPageSize) + "-byte aligned");
    if (MemoryPtr > Config->InitialMemory)
      error("initial memory too small, " + Twine(MemoryPtr) + " bytes needed");
    else
      MemoryPtr = Config->InitialMemory;
  }
  uint32_t MemSize = alignTo(MemoryPtr, WasmPageSize);
  NumMemoryPages = MemSize / WasmPageSize;
  log("mem: total pages = " + Twine(NumMemoryPages));

  if (Config->MaxMemory != 0) {
    if (Config->MaxMemory != alignTo(Config->MaxMemory, WasmPageSize))
      error("maximum memory must be " + Twine(WasmPageSize) + "-byte aligned");
    if (MemoryPtr > Config->MaxMemory)
      error("maximum memory too small, " + Twine(MemoryPtr) + " bytes needed");
    MaxMemoryPages = Config->MaxMemory / WasmPageSize;
    log("mem: max pages   = " + Twine(MaxMemoryPages));
  }
}

SyntheticSection *Writer::createSyntheticSection(uint32_t Type,
                                                 StringRef Name) {
  auto Sec = make<SyntheticSection>(Type, Name);
  log("createSection: " + toString(*Sec));
  OutputSections.push_back(Sec);
  return Sec;
}

void Writer::createSections() {
  // Known sections
  createTypeSection();
  createImportSection();
  createFunctionSection();
  createTableSection();
  createMemorySection();
  createGlobalSection();
  createExportSection();
  createElemSection();
  createCodeSection();
  createDataSection();
  createCustomSections();

  // Custom sections
  if (Config->Relocatable) {
    createLinkingSection();
    createRelocSections();
  }
  if (!Config->StripDebug && !Config->StripAll)
    createNameSection();

  for (OutputSection *S : OutputSections) {
    S->setOffset(FileSize);
    S->finalizeContents();
    FileSize += S->getSize();
  }
}

void Writer::calculateImports() {
  for (Symbol *Sym : Symtab->getSymbols()) {
    if (!Sym->isUndefined())
      continue;
    if (isa<DataSymbol>(Sym))
      continue;
    if (Sym->isWeak() && !Config->Relocatable)
      continue;
    if (!Sym->isLive())
      continue;
    if (!Sym->IsUsedInRegularObj)
      continue;

    LLVM_DEBUG(dbgs() << "import: " << Sym->getName() << "\n");
    ImportedSymbols.emplace_back(Sym);
    if (auto *F = dyn_cast<FunctionSymbol>(Sym))
      F->setFunctionIndex(NumImportedFunctions++);
    else
      cast<GlobalSymbol>(Sym)->setGlobalIndex(NumImportedGlobals++);
  }
}

void Writer::calculateExports() {
  if (Config->Relocatable)
    return;

  if (!Config->Relocatable && !Config->ImportMemory)
    Exports.push_back(WasmExport{"memory", WASM_EXTERNAL_MEMORY, 0});

  if (!Config->Relocatable && Config->ExportTable)
    Exports.push_back(WasmExport{kFunctionTableName, WASM_EXTERNAL_TABLE, 0});

  unsigned FakeGlobalIndex = NumImportedGlobals + InputGlobals.size();

  for (Symbol *Sym : Symtab->getSymbols()) {
    if (!Sym->isDefined())
      continue;
    if (Sym->isHidden() && !Config->ExportAll)
      continue;
    if (Sym->isLocal())
      continue;
    if (!Sym->isLive())
      continue;

    StringRef Name = Sym->getName();
    WasmExport Export;
    if (auto *F = dyn_cast<DefinedFunction>(Sym)) {
      Export = {Name, WASM_EXTERNAL_FUNCTION, F->getFunctionIndex()};
    } else if (auto *G = dyn_cast<DefinedGlobal>(Sym)) {
      // TODO(sbc): Remove this check once to mutable global proposal is
      // implement in all major browsers.
      // See: https://github.com/WebAssembly/mutable-global
      if (G->getGlobalType()->Mutable) {
        // Only the __stack_pointer should ever be create as mutable.
        assert(G == WasmSym::StackPointer);
        continue;
      }
      Export = {Name, WASM_EXTERNAL_GLOBAL, G->getGlobalIndex()};
    } else {
      auto *D = cast<DefinedData>(Sym);
      DefinedFakeGlobals.emplace_back(D);
      Export = {Name, WASM_EXTERNAL_GLOBAL, FakeGlobalIndex++};
    }

    LLVM_DEBUG(dbgs() << "Export: " << Name << "\n");
    Exports.push_back(Export);
  }
}

void Writer::assignSymtab() {
  if (!Config->Relocatable)
    return;

  StringMap<uint32_t> SectionSymbolIndices;

  unsigned SymbolIndex = SymtabEntries.size();
  for (ObjFile *File : Symtab->ObjectFiles) {
    LLVM_DEBUG(dbgs() << "Symtab entries: " << File->getName() << "\n");
    for (Symbol *Sym : File->getSymbols()) {
      if (Sym->getFile() != File)
        continue;

      if (auto *S = dyn_cast<SectionSymbol>(Sym)) {
        StringRef Name = S->getName();
        if (CustomSectionMapping.count(Name) == 0)
          continue;

        auto SSI = SectionSymbolIndices.find(Name);
        if (SSI != SectionSymbolIndices.end()) {
          Sym->setOutputSymbolIndex(SSI->second);
          continue;
        }

        SectionSymbolIndices[Name] = SymbolIndex;
        CustomSectionSymbols[Name] = cast<SectionSymbol>(Sym);

        Sym->markLive();
      }

      // (Since this is relocatable output, GC is not performed so symbols must
      // be live.)
      assert(Sym->isLive());
      Sym->setOutputSymbolIndex(SymbolIndex++);
      SymtabEntries.emplace_back(Sym);
    }
  }

  // For the moment, relocatable output doesn't contain any synthetic functions,
  // so no need to look through the Symtab for symbols not referenced by
  // Symtab->ObjectFiles.
}

uint32_t Writer::lookupType(const WasmSignature &Sig) {
  auto It = TypeIndices.find(Sig);
  if (It == TypeIndices.end()) {
    error("type not found: " + toString(Sig));
    return 0;
  }
  return It->second;
}

uint32_t Writer::registerType(const WasmSignature &Sig) {
  auto Pair = TypeIndices.insert(std::make_pair(Sig, Types.size()));
  if (Pair.second) {
    LLVM_DEBUG(dbgs() << "type " << toString(Sig) << "\n");
    Types.push_back(&Sig);
  }
  return Pair.first->second;
}

void Writer::calculateTypes() {
  // The output type section is the union of the following sets:
  // 1. Any signature used in the TYPE relocation
  // 2. The signatures of all imported functions
  // 3. The signatures of all defined functions

  for (ObjFile *File : Symtab->ObjectFiles) {
    ArrayRef<WasmSignature> Types = File->getWasmObj()->types();
    for (uint32_t I = 0; I < Types.size(); I++)
      if (File->TypeIsUsed[I])
        File->TypeMap[I] = registerType(Types[I]);
  }

  for (const Symbol *Sym : ImportedSymbols)
    if (auto *F = dyn_cast<FunctionSymbol>(Sym))
      registerType(*F->getFunctionType());

  for (const InputFunction *F : InputFunctions)
    registerType(F->Signature);
}

void Writer::assignIndexes() {
  uint32_t FunctionIndex = NumImportedFunctions + InputFunctions.size();
  auto AddDefinedFunction = [&](InputFunction *Func) {
    if (!Func->Live)
      return;
    InputFunctions.emplace_back(Func);
    Func->setFunctionIndex(FunctionIndex++);
  };

  for (InputFunction *Func : Symtab->SyntheticFunctions)
    AddDefinedFunction(Func);

  for (ObjFile *File : Symtab->ObjectFiles) {
    LLVM_DEBUG(dbgs() << "Functions: " << File->getName() << "\n");
    for (InputFunction *Func : File->Functions)
      AddDefinedFunction(Func);
  }

  uint32_t TableIndex = kInitialTableOffset;
  auto HandleRelocs = [&](InputChunk *Chunk) {
    if (!Chunk->Live)
      return;
    ObjFile *File = Chunk->File;
    ArrayRef<WasmSignature> Types = File->getWasmObj()->types();
    for (const WasmRelocation &Reloc : Chunk->getRelocations()) {
      if (Reloc.Type == R_WEBASSEMBLY_TABLE_INDEX_I32 ||
          Reloc.Type == R_WEBASSEMBLY_TABLE_INDEX_SLEB) {
        FunctionSymbol *Sym = File->getFunctionSymbol(Reloc.Index);
        if (Sym->hasTableIndex() || !Sym->hasFunctionIndex())
          continue;
        Sym->setTableIndex(TableIndex++);
        IndirectFunctions.emplace_back(Sym);
      } else if (Reloc.Type == R_WEBASSEMBLY_TYPE_INDEX_LEB) {
        // Mark target type as live
        File->TypeMap[Reloc.Index] = registerType(Types[Reloc.Index]);
        File->TypeIsUsed[Reloc.Index] = true;
      }
    }
  };

  for (ObjFile *File : Symtab->ObjectFiles) {
    LLVM_DEBUG(dbgs() << "Handle relocs: " << File->getName() << "\n");
    for (InputChunk *Chunk : File->Functions)
      HandleRelocs(Chunk);
    for (InputChunk *Chunk : File->Segments)
      HandleRelocs(Chunk);
    for (auto &P : File->CustomSections)
      HandleRelocs(P);
  }

  uint32_t GlobalIndex = NumImportedGlobals + InputGlobals.size();
  auto AddDefinedGlobal = [&](InputGlobal *Global) {
    if (Global->Live) {
      LLVM_DEBUG(dbgs() << "AddDefinedGlobal: " << GlobalIndex << "\n");
      Global->setGlobalIndex(GlobalIndex++);
      InputGlobals.push_back(Global);
    }
  };

  for (InputGlobal *Global : Symtab->SyntheticGlobals)
    AddDefinedGlobal(Global);

  for (ObjFile *File : Symtab->ObjectFiles) {
    LLVM_DEBUG(dbgs() << "Globals: " << File->getName() << "\n");
    for (InputGlobal *Global : File->Globals)
      AddDefinedGlobal(Global);
  }
}

static StringRef getOutputDataSegmentName(StringRef Name) {
  if (!Config->MergeDataSegments)
    return Name;
  if (Name.startswith(".text."))
    return ".text";
  if (Name.startswith(".data."))
    return ".data";
  if (Name.startswith(".bss."))
    return ".bss";
  return Name;
}

void Writer::createOutputSegments() {
  for (ObjFile *File : Symtab->ObjectFiles) {
    if (!File->getSnaxABI().empty())
       abis.push_back(File->getSnaxABI());
    for (InputSegment *Segment : File->Segments) {
      if (!Segment->Live)
        continue;
      StringRef Name = getOutputDataSegmentName(Segment->getName());
      OutputSegment *&S = SegmentMap[Name];
      if (S == nullptr) {
        LLVM_DEBUG(dbgs() << "new segment: " << Name << "\n");
        S = make<OutputSegment>(Name, Segments.size());
        Segments.push_back(S);
      }
      S->addInputSegment(Segment);
      LLVM_DEBUG(dbgs() << "added data: " << Name << ": " << S->Size << "\n");
    }
  }
}

static constexpr int OPCODE_CALL = 0x10;
static constexpr int OPCODE_IF   = 0x4;
static constexpr int OPCODE_ELSE = 0x5;
static constexpr int OPCODE_END  = 0xb;
static constexpr int OPCODE_GET_LOCAL = 0x20;
static constexpr int OPCODE_I32_EQ    = 0x46;
static constexpr int OPCODE_I64_EQ    = 0x51;
static constexpr int OPCODE_I64_NE    = 0x52;
static constexpr int OPCODE_I32_CONST = 0x41;
static constexpr int OPCODE_I64_CONST = 0x42;
static constexpr uint64_t SNAX_COMPILER_ERROR_BASE = 8000000000000000000ull;
static constexpr uint64_t SNAX_ERROR_NO_ACTION     = SNAX_COMPILER_ERROR_BASE;
static constexpr uint64_t SNAX_ERROR_ONERROR       = SNAX_COMPILER_ERROR_BASE+1;

void Writer::createDispatchFunction() {

   auto get_function = [&](std::string func_name) -> int64_t {
      for (ObjFile *File : Symtab->ObjectFiles) {
         for (auto func : File->Functions) {
            if (func->getName().str() == func_name) {
               return func->getFunctionIndex();
            }
         }
      }
      return -1;
   };

   auto create_if = [&](raw_string_ostream& os, std::string str, bool& need_else) {
      if (need_else) {
         writeU8(os, OPCODE_ELSE, "ELSE");
      }
      need_else = true;
      uint64_t nm = snax::cdt::string_to_name(str.substr(0, str.find(":")).c_str());
      writeU8(os, OPCODE_I64_CONST, "I64 CONST");
      encodeSLEB128((int64_t)nm, os);
      writeU8(os, OPCODE_GET_LOCAL, "GET_LOCAL");
      writeUleb128(os, 2, "action");
      writeU8(os, OPCODE_I64_EQ, "I64_EQ");
      writeU8(os, OPCODE_IF, "IF action == name");
      writeU8(os, 0x40, "none");
      writeU8(os, OPCODE_GET_LOCAL, "GET_LOCAL");
      writeUleb128(os, 0, "receiver");
      writeU8(os, OPCODE_GET_LOCAL, "GET_LOCAL");
      writeUleb128(os, 1, "code");
      writeU8(os, OPCODE_CALL, "CALL");
      auto func_sym = (FunctionSymbol*)Symtab->find(str.substr(str.find(":")+1));
      uint32_t index = func_sym->getFunctionIndex();
      if (index >= 0)
         writeUleb128(os, index, "index");
      else
         throw std::runtime_error("wasm_ld internal error function not found");
   };

   auto assert_sym = (FunctionSymbol*)Symtab->find("snax_assert_code");
   uint32_t assert_idx = assert_sym->getFunctionIndex();
   auto post_sym = (FunctionSymbol*)Symtab->find("post_dispatch");

   auto create_action_dispatch = [&](raw_string_ostream& OS) {
      // count how many total actions we have
      int act_cnt = 0;

      // create the dispatching for the actions
      std::set<StringRef> has_dispatched;
      bool need_else = false;
      for (ObjFile *File : Symtab->ObjectFiles) {
        if (!File->getSnaxActions().empty()) {
            for (auto act : File->getSnaxActions()) {
              if (has_dispatched.insert(act).second) {
                create_if(OS, act.str(), need_else);
                act_cnt++;
              }
            }
        }
      }
      if (act_cnt > 0)
        writeU8(OS, OPCODE_ELSE, "ELSE");

      // do not fail if self == snax
      writeU8(OS, OPCODE_GET_LOCAL, "GET_LOCAL");
      writeUleb128(OS, 0, "self");
      writeU8(OS, OPCODE_I64_CONST, "I64.CONST");
      encodeSLEB128((int64_t)snax::cdt::string_to_name("snax"), OS);
      writeU8(OS, OPCODE_I64_NE, "I64.NE");
      writeU8(OS, OPCODE_IF, "if receiver != snax");
      writeU8(OS, 0x40, "none");

      // assert that no action was found
      writeU8(OS, OPCODE_I32_CONST, "I32.CONST");
      writeUleb128(OS, 0, "false");
      writeU8(OS, OPCODE_I64_CONST, "I64.CONST");
      writeUleb128(OS, SNAX_ERROR_NO_ACTION, "error code");
      writeU8(OS, OPCODE_CALL, "CALL");
      writeUleb128(OS, assert_idx, "code");

      if (post_sym) {
         writeU8(OS, OPCODE_ELSE, "ELSE");
         uint32_t post_idx  = post_sym->getFunctionIndex();
         writeU8(OS, OPCODE_GET_LOCAL, "GET_LOCAL");
         writeUleb128(OS, 0, "receiver");
         writeU8(OS, OPCODE_GET_LOCAL, "GET_LOCAL");
         writeUleb128(OS, 1, "code");
         writeU8(OS, OPCODE_GET_LOCAL, "GET_LOCAL");
         writeUleb128(OS, 2, "action");
         writeU8(OS, OPCODE_CALL, "CALL");
         writeUleb128(OS, post_idx, "post_dispatch call");
      }
      writeU8(OS, OPCODE_END, "END");

      for (int i=0; i < act_cnt; i++) {
         writeU8(OS, OPCODE_END, "END");
      }
   };

   auto create_notify_dispatch = [&](raw_string_ostream& OS) {
      // count how many total notify handlers we have and register them
      int not_cnt = 0;
      std::set<StringRef> has_dispatched;
      std::map<std::string, std::vector<std::string>> notify_handlers;
      for (ObjFile *File : Symtab->ObjectFiles) {
         if (!File->getSnaxNotify().empty()) {
            for (auto notif : File->getSnaxNotify()) {
              if (has_dispatched.insert(notif).second) {
                not_cnt++;
                std::string snotif = notif.str();
                size_t idx = snotif.find(":");
                // <code_name>::<action>:<generated_notify_dispatch_func>
                auto code_name = snotif.substr(0, idx);
                auto rest      = snotif.substr(idx+2);
                notify_handlers[code_name].push_back(rest);
              }
            }
         }
      }

      writeU8(OS, OPCODE_GET_LOCAL, "GET_LOCAL");
      writeUleb128(OS, 0, "self");
      writeU8(OS, OPCODE_I64_CONST, "I64.CONST");
      encodeSLEB128((int64_t)snax::cdt::string_to_name("snax"), OS);
      writeU8(OS, OPCODE_I64_NE, "I64.NE");
      writeU8(OS, OPCODE_IF, "if receiver != snax");
      writeU8(OS, 0x40, "none");
      
      // check for onerror first
      bool has_onerror_handler = false;
      if (not_cnt > 0) {
         for (auto const& notif0 : notify_handlers) {
            uint64_t nm = snax::cdt::string_to_name(notif0.first.c_str());
            if (notif0.first == "snax")
               for (auto const& notif1 : notif0.second)
                  if (notif1 == "onerror")
                     has_onerror_handler = true;
         }
      }

      if (!has_onerror_handler) {
         // assert on onerror
         writeU8(OS, OPCODE_I64_CONST, "I64.CONST");
         uint64_t acnt = snax::cdt::string_to_name("snax");
         encodeSLEB128((int64_t)acnt, OS);
         writeU8(OS, OPCODE_GET_LOCAL, "GET_LOCAL");
         writeUleb128(OS, 1, "code");
         writeU8(OS, OPCODE_I64_EQ, "I64.EQ");
         writeU8(OS, OPCODE_IF, "IF code == snax");
         writeU8(OS, 0x40, "none");

         writeU8(OS, OPCODE_I64_CONST, "I64.CONST");
         uint64_t nm = snax::cdt::string_to_name("onerror");
         encodeSLEB128((int64_t)nm, OS);
         writeU8(OS, OPCODE_GET_LOCAL, "GET_LOCAL");
         writeUleb128(OS, 2, "action");
         writeU8(OS, OPCODE_I64_EQ, "I64.EQ");
         writeU8(OS, OPCODE_IF, "IF action==onerror");
         writeU8(OS, 0x40, "none");
         writeU8(OS, OPCODE_I32_CONST, "I32.CONST");
         writeUleb128(OS, 0, "false");
         writeU8(OS, OPCODE_I64_CONST, "I64.CONST");
         writeUleb128(OS, SNAX_ERROR_ONERROR, "error code");
         writeU8(OS, OPCODE_CALL, "CALL");
         writeUleb128(OS, assert_idx, "code");
         writeU8(OS, OPCODE_END, "END");
         writeU8(OS, OPCODE_END, "END");
      }

      // dispatch notification handlers
      bool notify0_need_else = false;
      if (not_cnt > 0) {
         for (auto const& notif0 : notify_handlers) {
            uint64_t nm = snax::cdt::string_to_name(notif0.first.c_str());
            if (notif0.first == "*")
               continue;
            if (notify0_need_else)
               writeU8(OS, OPCODE_ELSE, "ELSE");
            writeU8(OS, OPCODE_I64_CONST, "I64.CONST");
            encodeSLEB128((int64_t)nm, OS);
            writeU8(OS, OPCODE_GET_LOCAL, "GET_LOCAL");
            writeUleb128(OS, 1, "code");
            writeU8(OS, OPCODE_I64_EQ, "I64.EQ");
            writeU8(OS, OPCODE_IF, "IF code==?");
            writeU8(OS, 0x40, "none");
            bool need_else = false;
            for (auto const& notif1 : notif0.second)
               create_if(OS, notif1, need_else);
            notify0_need_else = true;
         }
         writeU8(OS, OPCODE_END, "END");
         writeU8(OS, OPCODE_ELSE, "ELSE");
      }

      if (!notify_handlers["*"].empty()) {
         bool need_else = false;
         for (auto const& notif1 : notify_handlers["*"]) {
            create_if(OS, notif1, need_else);
         }
      }

      if (post_sym) {
         writeU8(OS, OPCODE_ELSE, "ELSE");
         uint32_t post_idx  = post_sym->getFunctionIndex();
         writeU8(OS, OPCODE_GET_LOCAL, "GET_LOCAL");
         writeUleb128(OS, 0, "receiver");
         writeU8(OS, OPCODE_GET_LOCAL, "GET_LOCAL");
         writeUleb128(OS, 1, "code");
         writeU8(OS, OPCODE_GET_LOCAL, "GET_LOCAL");
         writeUleb128(OS, 2, "action");
         writeU8(OS, OPCODE_CALL, "CALL");
         writeUleb128(OS, post_idx, "post_dispatch call");
      }

      for (int i=0; i < not_cnt; i++)
        writeU8(OS, OPCODE_END, "END");
   };

   std::string BodyContent;
   {
      raw_string_ostream OS(BodyContent);
      writeUleb128(OS, 0, "num locals");

      // create ctors call
      auto ctors_sym = (FunctionSymbol*)Symtab->find("__wasm_call_ctors");
      if (ctors_sym) {
         uint32_t ctors_idx = ctors_sym->getFunctionIndex();
         if (ctors_idx != 0) {
            writeU8(OS, OPCODE_CALL, "CALL");
            writeUleb128(OS, ctors_idx, "__wasm_call_ctors");
         }

      }


      // create the pre_dispatch function call
      auto pre_sym = (FunctionSymbol*)Symtab->find("pre_dispatch");
      if (pre_sym) {
         uint32_t pre_idx  = pre_sym->getFunctionIndex();
         writeU8(OS, OPCODE_GET_LOCAL, "GET_LOCAL");
         writeUleb128(OS, 0, "receiver");
         writeU8(OS, OPCODE_GET_LOCAL, "GET_LOCAL");
         writeUleb128(OS, 1, "code");
         writeU8(OS, OPCODE_GET_LOCAL, "GET_LOCAL");
         writeUleb128(OS, 2, "action");
         writeU8(OS, OPCODE_CALL, "CALL");
         writeUleb128(OS, pre_idx, "pre_dispatch call");
         writeU8(OS, OPCODE_IF, "IF pre_dispatch -> T");
         writeU8(OS, 0x40, "none");
      }

      // create the preamble for apply if (code == receiver)
      writeU8(OS, OPCODE_GET_LOCAL, "GET_LOCAL");
      writeUleb128(OS, 0, "receiver");
      writeU8(OS, OPCODE_GET_LOCAL, "GET_LOCAL");
      writeUleb128(OS, 1, "code");

      writeU8(OS, OPCODE_I64_EQ, "I64.EQ");
      writeU8(OS, OPCODE_IF, "IF code==receiver");
      writeU8(OS, 0x40, "none");

      create_action_dispatch(OS);

      // now doing notification handling
      writeU8(OS, OPCODE_ELSE, "ELSE");

      create_notify_dispatch(OS);

      writeU8(OS, OPCODE_END, "END");

      auto dtors_sym = (FunctionSymbol*)Symtab->find("__cxa_finalize");
      if (dtors_sym) {
         uint32_t dtors_idx = dtors_sym->getFunctionIndex();
         if (dtors_idx != 0) {
            writeU8(OS, OPCODE_I32_CONST, "I32.CONST");
            writeUleb128(OS, (uint32_t)0, "NULL");
            writeU8(OS, OPCODE_CALL, "CALL");
            writeUleb128(OS, dtors_idx, "__cxa_finalize");
         }
      }
      if (pre_sym)
         writeU8(OS, OPCODE_END, "END");
      writeU8(OS, OPCODE_END, "END");
   }

   std::string FunctionBody;
   {
      raw_string_ostream OS(FunctionBody);
      writeUleb128(OS, BodyContent.size(), "function size");
      OS << BodyContent;
   }

   ArrayRef<uint8_t> Body = toArrayRef(Saver.save(FunctionBody));
   cast<SyntheticFunction>(WasmSym::EntryFunc->Function)->setBody(Body);
}

// Create synthetic "__wasm_call_ctors" function based on ctor functions
// in input object.
void Writer::createCtorFunction() {
  // First write the body's contents to a string.
  std::string BodyContent;
  {
    raw_string_ostream OS(BodyContent);
    writeUleb128(OS, 0, "num locals");
    for (const WasmInitEntry &F : InitFunctions) {
      writeU8(OS, OPCODE_CALL, "CALL");
      writeUleb128(OS, F.Sym->getFunctionIndex(), "function index");
    }
    writeU8(OS, OPCODE_END, "END");
  }

  // Once we know the size of the body we can create the final function body
  std::string FunctionBody;
  {
    raw_string_ostream OS(FunctionBody);
    writeUleb128(OS, BodyContent.size(), "function size");
    OS << BodyContent;
  }

  ArrayRef<uint8_t> Body = toArrayRef(Saver.save(FunctionBody));
  cast<SyntheticFunction>(WasmSym::CallCtors->Function)->setBody(Body);
}

// Populate InitFunctions vector with init functions from all input objects.
// This is then used either when creating the output linking section or to
// synthesize the "__wasm_call_ctors" function.
void Writer::calculateInitFunctions() {
  for (ObjFile *File : Symtab->ObjectFiles) {
    const WasmLinkingData &L = File->getWasmObj()->linkingData();
    for (const WasmInitFunc &F : L.InitFunctions) {
      FunctionSymbol *Sym = File->getFunctionSymbol(F.Symbol);
      if (*Sym->getFunctionType() != WasmSignature{{}, WASM_TYPE_NORESULT})
        error("invalid signature for init func: " + toString(*Sym));
      InitFunctions.emplace_back(WasmInitEntry{Sym, F.Priority});
    }
  }

  // Sort in order of priority (lowest first) so that they are called
  // in the correct order.
  std::stable_sort(InitFunctions.begin(), InitFunctions.end(),
                   [](const WasmInitEntry &L, const WasmInitEntry &R) {
                     return L.Priority < R.Priority;
                   });
}

void Writer::run(bool is_entry_defined) {
  if (Config->Relocatable)
    Config->GlobalBase = 0;

  log("-- calculateImports");
  calculateImports();
  log("-- assignIndexes");
  assignIndexes();
  log("-- calculateInitFunctions");
  calculateInitFunctions();
  if (!Config->Relocatable)
    createCtorFunction();

  if (Symtab->EntryIsUndefined)
     createDispatchFunction();

  log("-- calculateTypes");
  calculateTypes();
  log("-- layoutMemory");
  layoutMemory();
  log("-- calculateExports");
  calculateExports();
  log("-- calculateCustomSections");
  calculateCustomSections();
  log("-- assignSymtab");
  assignSymtab();

  if (errorHandler().Verbose) {
    log("Defined Functions: " + Twine(InputFunctions.size()));
    log("Defined Globals  : " + Twine(InputGlobals.size()));
    log("Function Imports : " + Twine(NumImportedFunctions));
    log("Global Imports   : " + Twine(NumImportedGlobals));
    for (ObjFile *File : Symtab->ObjectFiles)
      File->dumpInfo();
  }

  createHeader();
  log("-- createSections");
  createSections();

  log("-- openFile");
  openFile();
  if (errorCount())
    return;

  writeHeader();

  log("-- writeSections");
  writeSections();
  if (errorCount())
    return;

  if (Error E = Buffer->commit())
    fatal("failed to write the output file: " + toString(std::move(E)));

  writeABI();
}

// Open a result file.
void Writer::openFile() {
  log("writing: " + Config->OutputFile);

  Expected<std::unique_ptr<FileOutputBuffer>> BufferOrErr =
      FileOutputBuffer::create(Config->OutputFile, FileSize,
                               FileOutputBuffer::F_executable);

  if (!BufferOrErr)
    error("failed to open " + Config->OutputFile + ": " +
          toString(BufferOrErr.takeError()));
  else
    Buffer = std::move(*BufferOrErr);
}

void Writer::createHeader() {
  raw_string_ostream OS(Header);
  writeBytes(OS, WasmMagic, sizeof(WasmMagic), "wasm magic");
  writeU32(OS, WasmVersion, "wasm version");
  OS.flush();
  FileSize += Header.size();
}

void lld::wasm::writeResult(bool is_entry_defined) { Writer().run(is_entry_defined); }
