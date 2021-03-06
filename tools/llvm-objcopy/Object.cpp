//===- Object.cpp -----------------------------------------------*- C++ -*-===//
//
//                      The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#include "Object.h"
#include "llvm-objcopy.h"

using namespace llvm;
using namespace object;
using namespace ELF;

template <class ELFT> void Segment::writeHeader(FileOutputBuffer &Out) const {
  typedef typename ELFT::Ehdr Elf_Ehdr;
  typedef typename ELFT::Phdr Elf_Phdr;

  uint8_t *Buf = Out.getBufferStart();
  Buf += sizeof(Elf_Ehdr) + Index * sizeof(Elf_Phdr);
  Elf_Phdr &Phdr = *reinterpret_cast<Elf_Phdr *>(Buf);
  Phdr.p_type = Type;
  Phdr.p_flags = Flags;
  Phdr.p_offset = Offset;
  Phdr.p_vaddr = VAddr;
  Phdr.p_paddr = PAddr;
  Phdr.p_filesz = FileSize;
  Phdr.p_memsz = MemSize;
  Phdr.p_align = Align;
}

void Segment::finalize() {
  auto FirstSec = firstSection();
  if (FirstSec) {
    // It is possible for a gap to be at the begining of a segment. Because of
    // this we need to compute the new offset based on how large this gap was
    // in the source file. Section layout should have already ensured that this
    // space is not used for something else.
    uint64_t OriginalOffset = Offset;
    Offset = FirstSec->Offset - (FirstSec->OriginalOffset - OriginalOffset);
  }
}

void Segment::writeSegment(FileOutputBuffer &Out) const {
  uint8_t *Buf = Out.getBufferStart() + Offset;
  // We want to maintain segments' interstitial data and contents exactly.
  // This lets us just copy segments directly.
  std::copy(std::begin(Contents), std::end(Contents), Buf);
}

void SectionBase::finalize() {}

template <class ELFT>
void SectionBase::writeHeader(FileOutputBuffer &Out) const {
  uint8_t *Buf = Out.getBufferStart();
  Buf += HeaderOffset;
  typename ELFT::Shdr &Shdr = *reinterpret_cast<typename ELFT::Shdr *>(Buf);
  Shdr.sh_name = NameIndex;
  Shdr.sh_type = Type;
  Shdr.sh_flags = Flags;
  Shdr.sh_addr = Addr;
  Shdr.sh_offset = Offset;
  Shdr.sh_size = Size;
  Shdr.sh_link = Link;
  Shdr.sh_info = Info;
  Shdr.sh_addralign = Align;
  Shdr.sh_entsize = EntrySize;
}

void Section::writeSection(FileOutputBuffer &Out) const {
  if (Type == SHT_NOBITS)
    return;
  uint8_t *Buf = Out.getBufferStart() + Offset;
  std::copy(std::begin(Contents), std::end(Contents), Buf);
}

void StringTableSection::addString(StringRef Name) {
  StrTabBuilder.add(Name);
  Size = StrTabBuilder.getSize();
}

uint32_t StringTableSection::findIndex(StringRef Name) const {
  return StrTabBuilder.getOffset(Name);
}

void StringTableSection::finalize() { StrTabBuilder.finalize(); }

void StringTableSection::writeSection(FileOutputBuffer &Out) const {
  StrTabBuilder.write(Out.getBufferStart() + Offset);
}

// Returns true IFF a section is wholly inside the range of a segment
static bool sectionWithinSegment(const SectionBase &Section,
                                 const Segment &Segment) {
  // If a section is empty it should be treated like it has a size of 1. This is
  // to clarify the case when an empty section lies on a boundary between two
  // segments and ensures that the section "belongs" to the second segment and
  // not the first.
  uint64_t SecSize = Section.Size ? Section.Size : 1;
  return Segment.Offset <= Section.OriginalOffset &&
         Segment.Offset + Segment.FileSize >= Section.OriginalOffset + SecSize;
}

template <class ELFT>
void Object<ELFT>::readProgramHeaders(const ELFFile<ELFT> &ElfFile) {
  uint32_t Index = 0;
  for (const auto &Phdr : unwrapOrError(ElfFile.program_headers())) {
    ArrayRef<uint8_t> Data{ElfFile.base() + Phdr.p_offset,
                           (size_t)Phdr.p_filesz};
    Segments.emplace_back(llvm::make_unique<Segment>(Data));
    Segment &Seg = *Segments.back();
    Seg.Type = Phdr.p_type;
    Seg.Flags = Phdr.p_flags;
    Seg.OriginalOffset = Phdr.p_offset;
    Seg.Offset = Phdr.p_offset;
    Seg.VAddr = Phdr.p_vaddr;
    Seg.PAddr = Phdr.p_paddr;
    Seg.FileSize = Phdr.p_filesz;
    Seg.MemSize = Phdr.p_memsz;
    Seg.Align = Phdr.p_align;
    Seg.Index = Index++;
    for (auto &Section : Sections) {
      if (sectionWithinSegment(*Section, Seg)) {
        Seg.addSection(&*Section);
        if (!Section->ParentSegment ||
            Section->ParentSegment->Offset > Seg.Offset) {
          Section->ParentSegment = &Seg;
        }
      }
    }
  }
}

template <class ELFT>
std::unique_ptr<SectionBase>
Object<ELFT>::makeSection(const llvm::object::ELFFile<ELFT> &ElfFile,
                          const Elf_Shdr &Shdr) {
  ArrayRef<uint8_t> Data;
  switch (Shdr.sh_type) {
  case SHT_STRTAB:
    return llvm::make_unique<StringTableSection>();
  case SHT_NOBITS:
    return llvm::make_unique<Section>(Data);
  default:
    Data = unwrapOrError(ElfFile.getSectionContents(&Shdr));
    return llvm::make_unique<Section>(Data);
  }
}

template <class ELFT>
void Object<ELFT>::readSectionHeaders(const ELFFile<ELFT> &ElfFile) {
  uint32_t Index = 0;
  for (const auto &Shdr : unwrapOrError(ElfFile.sections())) {
    if (Index == 0) {
      ++Index;
      continue;
    }
    SecPtr Sec = makeSection(ElfFile, Shdr);
    Sec->Name = unwrapOrError(ElfFile.getSectionName(&Shdr));
    Sec->Type = Shdr.sh_type;
    Sec->Flags = Shdr.sh_flags;
    Sec->Addr = Shdr.sh_addr;
    Sec->Offset = Shdr.sh_offset;
    Sec->OriginalOffset = Shdr.sh_offset;
    Sec->Size = Shdr.sh_size;
    Sec->Link = Shdr.sh_link;
    Sec->Info = Shdr.sh_info;
    Sec->Align = Shdr.sh_addralign;
    Sec->EntrySize = Shdr.sh_entsize;
    Sec->Index = Index++;
    Sections.push_back(std::move(Sec));
  }
}

template <class ELFT> Object<ELFT>::Object(const ELFObjectFile<ELFT> &Obj) {
  const auto &ElfFile = *Obj.getELFFile();
  const auto &Ehdr = *ElfFile.getHeader();

  std::copy(Ehdr.e_ident, Ehdr.e_ident + 16, Ident);
  Type = Ehdr.e_type;
  Machine = Ehdr.e_machine;
  Version = Ehdr.e_version;
  Entry = Ehdr.e_entry;
  Flags = Ehdr.e_flags;

  readSectionHeaders(ElfFile);
  readProgramHeaders(ElfFile);

  SectionNames =
      dyn_cast<StringTableSection>(Sections[Ehdr.e_shstrndx - 1].get());
}

template <class ELFT>
void Object<ELFT>::writeHeader(FileOutputBuffer &Out) const {
  uint8_t *Buf = Out.getBufferStart();
  Elf_Ehdr &Ehdr = *reinterpret_cast<Elf_Ehdr *>(Buf);
  std::copy(Ident, Ident + 16, Ehdr.e_ident);
  Ehdr.e_type = Type;
  Ehdr.e_machine = Machine;
  Ehdr.e_version = Version;
  Ehdr.e_entry = Entry;
  Ehdr.e_phoff = sizeof(Elf_Ehdr);
  Ehdr.e_shoff = SHOffset;
  Ehdr.e_flags = Flags;
  Ehdr.e_ehsize = sizeof(Elf_Ehdr);
  Ehdr.e_phentsize = sizeof(Elf_Phdr);
  Ehdr.e_phnum = Segments.size();
  Ehdr.e_shentsize = sizeof(Elf_Shdr);
  Ehdr.e_shnum = Sections.size() + 1;
  Ehdr.e_shstrndx = SectionNames->Index;
}

template <class ELFT>
void Object<ELFT>::writeProgramHeaders(FileOutputBuffer &Out) const {
  for (auto &Phdr : Segments)
    Phdr->template writeHeader<ELFT>(Out);
}

template <class ELFT>
void Object<ELFT>::writeSectionHeaders(FileOutputBuffer &Out) const {
  uint8_t *Buf = Out.getBufferStart() + SHOffset;
  // This reference serves to write the dummy section header at the begining
  // of the file.
  Elf_Shdr &Shdr = *reinterpret_cast<Elf_Shdr *>(Buf);
  Shdr.sh_name = 0;
  Shdr.sh_type = SHT_NULL;
  Shdr.sh_flags = 0;
  Shdr.sh_addr = 0;
  Shdr.sh_offset = 0;
  Shdr.sh_size = 0;
  Shdr.sh_link = 0;
  Shdr.sh_info = 0;
  Shdr.sh_addralign = 0;
  Shdr.sh_entsize = 0;

  for (auto &Section : Sections)
    Section->template writeHeader<ELFT>(Out);
}

template <class ELFT>
void Object<ELFT>::writeSectionData(FileOutputBuffer &Out) const {
  for (auto &Section : Sections)
    Section->writeSection(Out);
}

template <class ELFT> void ELFObject<ELFT>::sortSections() {
  // Put all sections in offset order. Maintain the ordering as closely as
  // possible while meeting that demand however.
  auto CompareSections = [](const SecPtr &A, const SecPtr &B) {
    return A->OriginalOffset < B->OriginalOffset;
  };
  std::stable_sort(std::begin(this->Sections), std::end(this->Sections),
                   CompareSections);
}

template <class ELFT> void ELFObject<ELFT>::assignOffsets() {
  // The size of ELF + program headers will not change so it is ok to assume
  // that the first offset of the first segment is a good place to start
  // outputting sections. This covers both the standard case and the PT_PHDR
  // case.
  uint64_t Offset;
  if (!this->Segments.empty()) {
    Offset = this->Segments[0]->Offset;
  } else {
    Offset = sizeof(Elf_Ehdr);
  }
  // The only way a segment should move is if a section was between two
  // segments and that section was removed. If that section isn't in a segment
  // then it's acceptable, but not ideal, to simply move it to after the
  // segments. So we can simply layout segments one after the other accounting
  // for alignment.
  for (auto &Segment : this->Segments) {
    Offset = alignTo(Offset, Segment->Align);
    Segment->Offset = Offset;
    Offset += Segment->FileSize;
  }
  // Now the offset of every segment has been set we can assign the offsets
  // of each section. For sections that are covered by a segment we should use
  // the segment's original offset and the section's original offset to compute
  // the offset from the start of the segment. Using the offset from the start
  // of the segment we can assign a new offset to the section. For sections not
  // covered by segments we can just bump Offset to the next valid location.
  uint32_t Index = 1;
  for (auto &Section : this->Sections) {
    Section->Index = Index++;
    if (Section->ParentSegment != nullptr) {
      auto Segment = Section->ParentSegment;
      Section->Offset =
          Segment->Offset + (Section->OriginalOffset - Segment->OriginalOffset);
    } else {
      Offset = alignTo(Offset, Section->Offset);
      Section->Offset = Offset;
      if (Section->Type != SHT_NOBITS)
        Offset += Section->Size;
    }
  }

  Offset = alignTo(Offset, sizeof(typename ELFT::Addr));
  this->SHOffset = Offset;
}

template <class ELFT> size_t ELFObject<ELFT>::totalSize() const {
  // We already have the section header offset so we can calculate the total
  // size by just adding up the size of each section header.
  return this->SHOffset + this->Sections.size() * sizeof(Elf_Shdr) +
         sizeof(Elf_Shdr);
}

template <class ELFT> void ELFObject<ELFT>::write(FileOutputBuffer &Out) const {
  this->writeHeader(Out);
  this->writeProgramHeaders(Out);
  this->writeSectionData(Out);
  this->writeSectionHeaders(Out);
}

template <class ELFT> void ELFObject<ELFT>::finalize() {
  for (const auto &Section : this->Sections) {
    this->SectionNames->addString(Section->Name);
  }

  sortSections();
  assignOffsets();

  // Finalize SectionNames first so that we can assign name indexes.
  this->SectionNames->finalize();
  // Finally now that all offsets and indexes have been set we can finalize any
  // remaining issues.
  uint64_t Offset = this->SHOffset + sizeof(Elf_Shdr);
  for (auto &Section : this->Sections) {
    Section->HeaderOffset = Offset;
    Offset += sizeof(Elf_Shdr);
    Section->NameIndex = this->SectionNames->findIndex(Section->Name);
    Section->finalize();
  }

  for (auto &Segment : this->Segments)
    Segment->finalize();
}

template <class ELFT> size_t BinaryObject<ELFT>::totalSize() const {
  return TotalSize;
}

template <class ELFT>
void BinaryObject<ELFT>::write(FileOutputBuffer &Out) const {
  for (auto &Segment : this->Segments) {
    // GNU objcopy does not output segments that do not cover a section. Such
    // segments can sometimes be produced by LLD due to how LLD handles PT_PHDR.
    if (Segment->Type == llvm::ELF::PT_LOAD &&
        Segment->firstSection() != nullptr) {
      Segment->writeSegment(Out);
    }
  }
}

template <class ELFT> void BinaryObject<ELFT>::finalize() {
  for (auto &Segment : this->Segments)
    Segment->finalize();

  // Put all segments in offset order.
  auto CompareSegments = [](const SegPtr &A, const SegPtr &B) {
    return A->Offset < B->Offset;
  };
  std::sort(std::begin(this->Segments), std::end(this->Segments),
            CompareSegments);

  uint64_t Offset = 0;
  for (auto &Segment : this->Segments) {
    if (Segment->Type == llvm::ELF::PT_LOAD &&
        Segment->firstSection() != nullptr) {
      Offset = alignTo(Offset, Segment->Align);
      Segment->Offset = Offset;
      Offset += Segment->FileSize;
    }
  }
  TotalSize = Offset;
}

template class Object<ELF64LE>;
template class Object<ELF64BE>;
template class Object<ELF32LE>;
template class Object<ELF32BE>;

template class ELFObject<ELF64LE>;
template class ELFObject<ELF64BE>;
template class ELFObject<ELF32LE>;
template class ELFObject<ELF32BE>;

template class BinaryObject<ELF64LE>;
template class BinaryObject<ELF64BE>;
template class BinaryObject<ELF32LE>;
template class BinaryObject<ELF32BE>;
