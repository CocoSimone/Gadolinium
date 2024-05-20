#include <Mem.hpp>
#include <core/registers/Registers.hpp>
#include <core/registers/Cop0.hpp>
#include <core/Interpreter.hpp>
#include <backend/RomHelpers.hpp>
#include <File.hpp>
#include <unarr.h>
#include <cassert>

namespace n64 {
Mem::Mem(Registers& regs, ParallelRDP& parallel) : flash(saveData), mmio(*this, regs, parallel) {
  memset(readPages, 0, PAGE_COUNT);
  memset(writePages, 0, PAGE_COUNT);

  for(u64 i = 0; i < RDRAM_SIZE / PAGE_SIZE; i++) {
    const auto addr = (i * PAGE_SIZE) & RDRAM_DSIZE;
    const auto pointer = (uintptr_t) &mmio.rdp.rdram[addr];
    readPages[i] = pointer;
    writePages[i] = pointer;
  }

  rom.cart.resize(CART_SIZE);
  std::fill(rom.cart.begin(), rom.cart.end(), 0);
}

void Mem::Reset() {
  std::fill(rom.cart.begin(), rom.cart.end(), 0);
  flash.Reset();
  if (saveData.is_mapped()) {
    std::error_code error;
    saveData.sync(error);
    if (error) { Util::panic("Could not sync save data"); }
    saveData.unmap();
  }
  mmio.Reset();
}

void Mem::LoadSRAM(SaveType save_type, fs::path path) {
  if(save_type == SAVE_SRAM_256k) {
    std::error_code error;
    if (!savePath.empty()) {
      path = savePath / path.filename();
    }
    sramPath = path.replace_extension(".sram").string();
    if(saveData.is_mapped()) {
      saveData.sync(error);
      if(error) { Util::panic("Could not sync {}", sramPath); }
      saveData.unmap();
    }

    auto sramVec = Util::ReadFileBinary(sramPath);
    if(sramVec.empty()) {
      Util::WriteFileBinary(std::array<u8, SRAM_SIZE>{}, sramPath);
      sramVec = Util::ReadFileBinary(sramPath);
    }

    if (sramVec.size() != SRAM_SIZE) {
      Util::panic("Corrupt SRAM!");
    }

    saveData = mio::make_mmap_sink(
      sramPath, 0, mio::map_entire_file, error);
    if (error) { Util::panic("Could not mmap {}", sramPath); }
  }
}

FORCE_INLINE void SetROMCIC(u32 checksum, ROM& rom) {
  switch (checksum) {
    case 0xEC8B1325: rom.cicType = CIC_NUS_7102; break; // 7102
    case 0x1DEB51A9: rom.cicType = CIC_NUS_6101; break; // 6101
    case 0xC08E5BD6: rom.cicType = CIC_NUS_6102_7101; break;
    case 0x03B8376A: rom.cicType = CIC_NUS_6103_7103; break;
    case 0xCF7F41DC: rom.cicType = CIC_NUS_6105_7105; break;
    case 0xD1059C6A: rom.cicType = CIC_NUS_6106_7106; break;
    default:
      Util::warn("Could not determine CIC TYPE! Checksum: 0x{:08X} is unknown!", checksum);
      rom.cicType = UNKNOWN_CIC_TYPE;
      break;
  }
}

std::vector<u8> Mem::OpenArchive(const std::string &path, size_t& sizeAdjusted) {
  auto stream = ar_open_file(fs::path(path).u8string().c_str());

  if(!stream) {
    Util::panic("Could not open archive! Are you sure it's an archive?");
  }

  ar_archive* archive = ar_open_zip_archive(stream, false);

  if(!archive) archive = ar_open_rar_archive(stream);
  if(!archive) archive = ar_open_7z_archive(stream);
  if(!archive) archive = ar_open_tar_archive(stream);

  if(!archive) {
    ar_close(stream);
    Util::panic("Could not open archive! Are you sure it's a supported archive? (7z, zip, rar and tar are supported)");
  }

  std::vector<u8> buf{};

  std::vector<std::string> rom_exts{".n64",".z64",".v64",".N64",".Z64",".V64"};

  while(ar_parse_entry(archive)) {
    auto filename = ar_entry_get_name(archive);
    auto extension = fs::path(filename).extension();

    if(std::any_of(rom_exts.begin(), rom_exts.end(), [&](auto x) {
      return extension == x;
    })) {
      auto size = ar_entry_get_size(archive);
      sizeAdjusted = Util::NextPow2(size);
      buf.resize(sizeAdjusted);
      ar_entry_uncompress(archive, buf.data(), size);
      break;
    } else {
      ar_close_archive(archive);
      ar_close(stream);
      Util::panic("Could not find any rom image in the archive!");
    }
  }

  ar_close_archive(archive);
  ar_close(stream);
  return buf;
}

std::vector<u8> Mem::OpenROM(const std::string& filename, size_t& sizeAdjusted) {
  auto buf = Util::ReadFileBinary(filename);
  sizeAdjusted = Util::NextPow2(buf.size());
  return buf;
}

void Mem::LoadROM(bool isArchive, const std::string& filename) {
  size_t sizeAdjusted;
  u32 endianness;
  {
    std::vector<u8> buf{};
    if (isArchive) {
      buf = OpenArchive(filename, sizeAdjusted);
    } else {
      buf = OpenROM(filename, sizeAdjusted);
    }

    endianness = be32toh(*reinterpret_cast<u32*>(buf.data()));
    Util::SwapN64Rom<true>(buf, endianness);

    std::copy(buf.begin(), buf.end(), rom.cart.begin());
    rom.mask = sizeAdjusted - 1;
    memcpy(&rom.header, buf.data(), sizeof(ROMHeader));
  }
  memcpy(rom.gameNameCart, rom.header.imageName, sizeof(rom.header.imageName));

  rom.header.clockRate = be32toh(rom.header.clockRate);
  rom.header.programCounter = be32toh(rom.header.programCounter);
  rom.header.release = be32toh(rom.header.release);
  rom.header.crc1 = be32toh(rom.header.crc1);
  rom.header.crc2 = be32toh(rom.header.crc2);
  rom.header.unknown = be64toh(rom.header.unknown);
  rom.header.unknown2 = be32toh(rom.header.unknown2);
  rom.header.manufacturerId = be32toh(rom.header.manufacturerId);
  rom.header.cartridgeId = be16toh(rom.header.cartridgeId);

  rom.code[0] = rom.header.manufacturerId & 0xFF;
  rom.code[1] = (rom.header.cartridgeId >> 8) & 0xFF;
  rom.code[2] = rom.header.cartridgeId & 0xFF;
  rom.code[3] = '\0';

  for (int i = sizeof(rom.header.imageName) - 1; rom.gameNameCart[i] == ' '; i--) {
    rom.gameNameCart[i] = '\0';
  }

  u32 checksum = Util::crc32(0, &rom.cart[0x40], 0x9c0);
  SetROMCIC(checksum, rom);
  endianness = be32toh(*reinterpret_cast<u32*>(rom.cart.data()));
  Util::SwapN64Rom(rom.cart, endianness);
  rom.pal = IsROMPAL();
}

template<> u8 Mem::Read(n64::Registers &regs, u32 paddr) {
  const auto page = paddr >> 12;
  const auto offset = paddr & 0xFFF;
  const auto pointer = readPages[page];
  SI& si = mmio.si;

  if(pointer) {
    return ((u8*)pointer)[BYTE_ADDRESS(offset)];
  } else {
    switch (paddr) {
      case RDRAM_REGION:
        return mmio.rdp.rdram[BYTE_ADDRESS(paddr)];
      case RSP_MEM_REGION: {
        auto& src = paddr & 0x1000 ? mmio.rsp.imem : mmio.rsp.dmem;
        return src[BYTE_ADDRESS(paddr & 0xfff)];
      }
      case REGION_CART:
        return mmio.pi.BusRead<u8, false>(paddr);
      case 0x04040000 ... 0x040FFFFF:
      case 0x04100000 ... 0x041FFFFF:
      case 0x04600000 ... 0x048FFFFF:
      case 0x04300000 ... 0x044FFFFF:
        Util::panic("MMIO Read<u8>!\n");
      case AI_REGION: {
        u32 w = mmio.ai.Read(paddr & ~3);
        int offs = 3 - (paddr & 3);
        return (w >> (offs * 8)) & 0xff;
      }
      case PIF_ROM_REGION:
        return si.pif.bootrom[BYTE_ADDRESS(paddr) - PIF_ROM_REGION_START];
      case PIF_RAM_REGION:
        return si.pif.ram[paddr - PIF_RAM_REGION_START];
      case 0x00800000 ... 0x03EFFFFF: // unused
      case 0x04200000 ... 0x042FFFFF: // unused
      case 0x04900000 ... 0x04FFFFFF: // unused
      case 0x1FC00800 ... 0xFFFFFFFF: // unused
        return 0;
      default:
        Util::panic("Unimplemented 8-bit read at address {:08X} (PC = {:016X})", paddr, (u64) regs.pc);
    }
  }
}

template<> u16 Mem::Read(n64::Registers &regs, u32 paddr) {
  const auto page = paddr >> 12;
  const auto offset = paddr & 0xFFF;
  const auto pointer = readPages[page];
  SI& si = mmio.si;

  if(pointer) {
    return Util::ReadAccess<u16>((u8*)pointer, HALF_ADDRESS(offset));
  } else {
    switch (paddr) {
      case RDRAM_REGION:
        return Util::ReadAccess<u16>(mmio.rdp.rdram, HALF_ADDRESS(paddr));
      case RSP_MEM_REGION: {
        auto& src = paddr & 0x1000 ? mmio.rsp.imem : mmio.rsp.dmem;
        return Util::ReadAccess<u16>(src, HALF_ADDRESS(paddr & 0xfff));
      }
      case MMIO_REGION:
        return mmio.Read(paddr);
      case REGION_CART:
        return mmio.pi.BusRead<u16, false>(paddr);
      case PIF_ROM_REGION:
        return Util::ReadAccess<u16>(si.pif.bootrom, HALF_ADDRESS(paddr) - PIF_ROM_REGION_START);
      case PIF_RAM_REGION:
        return be16toh(Util::ReadAccess<u16>(si.pif.ram, paddr - PIF_RAM_REGION_START));
      case 0x00800000 ... 0x03EFFFFF:
      case 0x04200000 ... 0x042FFFFF:
      case 0x04900000 ... 0x04FFFFFF:
      case 0x1FC00800 ... 0xFFFFFFFF:
        return 0;
      default:
        Util::panic("Unimplemented 16-bit read at address {:08X} (PC = {:016X})", paddr, (u64) regs.pc);
    }
  }
}

template<> u32 Mem::Read(n64::Registers &regs, u32 paddr) {
  const auto page = paddr >> 12;
  const auto offset = paddr & 0xFFF;
  const auto pointer = readPages[page];
  SI& si = mmio.si;

  if(pointer) {
    return Util::ReadAccess<u32>((u8*)pointer, offset);
  } else {
    switch(paddr) {
      case RDRAM_REGION:
        return Util::ReadAccess<u32>(mmio.rdp.rdram, paddr);
      case RSP_MEM_REGION: {
        auto& src = paddr & 0x1000 ? mmio.rsp.imem : mmio.rsp.dmem;
        return Util::ReadAccess<u32>(src, paddr & 0xfff);
      }
      case MMIO_REGION:
        return mmio.Read(paddr);
      case REGION_CART:
        return mmio.pi.BusRead<u32, false>(paddr);
      case PIF_ROM_REGION:
        return Util::ReadAccess<u32>(si.pif.bootrom, paddr - PIF_ROM_REGION_START);
      case PIF_RAM_REGION:
        return be32toh(Util::ReadAccess<u32>(si.pif.ram, paddr - PIF_RAM_REGION_START));
      case 0x00800000 ... 0x03EFFFFF: case 0x04200000 ... 0x042FFFFF:
      case 0x04900000 ... 0x04FFFFFF: case 0x1FC00800 ... 0xFFFFFFFF: return 0;
      default:
        Util::panic("Unimplemented 32-bit read at address {:08X} (PC = {:016X})", paddr, (u64) regs.pc);
    }
  }
}

template<> u64 Mem::Read(n64::Registers &regs, u32 paddr) {
  const auto page = paddr >> 12;
  const auto offset = paddr & 0xFFF;
  const auto pointer = readPages[page];
  SI& si = mmio.si;

  if(pointer) {
    return Util::ReadAccess<u64>((u8*)pointer, offset);
  } else {
    switch (paddr) {
      case RDRAM_REGION:
        return Util::ReadAccess<u64>(mmio.rdp.rdram, paddr);
      case RSP_MEM_REGION: {
        auto& src = paddr & 0x1000 ? mmio.rsp.imem : mmio.rsp.dmem;
        return Util::ReadAccess<u64>(src, paddr & 0xfff);
      }
      case MMIO_REGION:
        return mmio.Read(paddr);
      case REGION_CART:
        return mmio.pi.BusRead<u64, false>(paddr);
      case PIF_ROM_REGION:
        return Util::ReadAccess<u64>(si.pif.bootrom, paddr - PIF_ROM_REGION_START);
      case PIF_RAM_REGION:
        return be64toh(Util::ReadAccess<u64>(si.pif.ram, paddr - PIF_RAM_REGION_START));
      case 0x00800000 ... 0x03EFFFFF:
      case 0x04200000 ... 0x042FFFFF:
      case 0x04900000 ... 0x04FFFFFF:
      case 0x1FC00800 ... 0xFFFFFFFF:
        return 0;
      default:
        Util::panic("Unimplemented 32-bit read at address {:08X} (PC = {:016X})", paddr, (u64) regs.pc);
    }
  }
}

template<> void Mem::Write<u8>(Registers& regs, u32 paddr, u32 val) {
  const auto page = paddr >> 12;
  const auto offset = paddr & 0xFFF;
  const auto pointer = writePages[page];
  SI& si = mmio.si;

  if(pointer) {
    ((u8*)pointer)[BYTE_ADDRESS(offset)] = val;
  } else {
    switch (paddr) {
      case RDRAM_REGION:
        mmio.rdp.rdram[BYTE_ADDRESS(paddr)] = val;
        break;
      case RSP_MEM_REGION: {
        val = val << (8 * (3 - (paddr & 3)));
        auto& dest = paddr & 0x1000 ? mmio.rsp.imem : mmio.rsp.dmem;
        paddr = (paddr & 0xFFF) & ~3;
        Util::WriteAccess<u32>(dest, paddr, val);
      } break;
      case REGION_CART:
        Util::trace("BusWrite<u8> @ {:08X} = {:02X}", paddr, val);
        mmio.pi.BusWrite<u8, false>(paddr, val);
        break;
      case MMIO_REGION:
        Util::panic("MMIO Write<u8>!");
      case PIF_RAM_REGION:
        val = val << (8 * (3 - (paddr & 3)));
        paddr = (paddr - PIF_RAM_REGION_START) & ~3;
        Util::WriteAccess<u32>(si.pif.ram, paddr, htobe32(val));
        si.pif.ProcessCommands(*this);
        break;
      case 0x00800000 ... 0x03EFFFFF:
      case 0x04200000 ... 0x042FFFFF:
      case 0x04900000 ... 0x04FFFFFF:
      case PIF_ROM_REGION:
      case 0x1FC00800 ... 0x7FFFFFFF:
      case 0x80000000 ... 0xFFFFFFFF:
        break;
      default:
        Util::panic("Unimplemented 8-bit write at address {:08X} with value {:02X} (PC = {:016X})", paddr, val,
                    (u64) regs.pc);
    }
  }
}

template<> void Mem::Write<u16>(Registers& regs, u32 paddr, u32 val) {
  const auto page = paddr >> 12;
  const auto offset = paddr & 0xFFF;
  const auto pointer = writePages[page];
  SI& si = mmio.si;

  if(pointer) {
    Util::WriteAccess<u16>((u8*)pointer, HALF_ADDRESS(offset), val);
  } else {
    switch (paddr) {
      case RDRAM_REGION:
        Util::WriteAccess<u16>(mmio.rdp.rdram, HALF_ADDRESS(paddr), val);
        break;
      case RSP_MEM_REGION: {
        val = val << (16 * !(paddr & 2));
        auto& dest = paddr & 0x1000 ? mmio.rsp.imem : mmio.rsp.dmem;
        paddr = (paddr & 0xFFF) & ~3;
        Util::WriteAccess<u32>(dest, paddr, val);
      } break;
      case REGION_CART:
        Util::trace("BusWrite<u8> @ {:08X} = {:04X}", paddr, val);
        mmio.pi.BusWrite<u16, false>(paddr, val);
        break;
      case MMIO_REGION:
        Util::panic("MMIO Write<u16>!");
      case PIF_RAM_REGION:
        val = val << (16 * !(paddr & 2));
        paddr &= ~3;
        Util::WriteAccess<u32>(si.pif.ram, paddr - PIF_RAM_REGION_START, htobe32(val));
        si.pif.ProcessCommands(*this);
        break;
      case 0x00800000 ... 0x03EFFFFF:
      case 0x04200000 ... 0x042FFFFF:
      case 0x04900000 ... 0x04FFFFFF:
      case PIF_ROM_REGION:
      case 0x1FC00800 ... 0x7FFFFFFF:
      case 0x80000000 ... 0xFFFFFFFF:
        break;
      default:
        Util::panic("Unimplemented 16-bit write at address {:08X} with value {:04X} (PC = {:016X})", paddr, val,
                    (u64) regs.pc);
    }
  }
}

template<> void Mem::Write<u32>(Registers& regs, u32 paddr, u32 val) {
  const auto page = paddr >> 12;
  const auto offset = paddr & 0xFFF;
  const auto pointer = writePages[page];
  SI& si = mmio.si;

  if(pointer) {
    Util::WriteAccess<u32>((u8*)pointer, offset, val);
  } else {
    switch(paddr) {
      case RDRAM_REGION:
        Util::WriteAccess<u32>(mmio.rdp.rdram, paddr, val);
        break;
      case RSP_MEM_REGION: {
        auto& dest = paddr & 0x1000 ? mmio.rsp.imem : mmio.rsp.dmem;
        Util::WriteAccess<u32>(dest, paddr & 0xfff, val);
      } break;
      case REGION_CART:
        Util::trace("BusWrite<u8> @ {:08X} = {:08X}", paddr, val);
        mmio.pi.BusWrite<u32, false>(paddr, val);
        break;
      case MMIO_REGION:
        mmio.Write(paddr, val);
        break;
      case PIF_RAM_REGION:
        Util::WriteAccess<u32>(si.pif.ram, paddr - PIF_RAM_REGION_START, htobe32(val));
        si.pif.ProcessCommands(*this);
        break;
      case 0x00800000 ... 0x03EFFFFF:
      case 0x04200000 ... 0x042FFFFF:
      case 0x04900000 ... 0x04FFFFFF:
      case PIF_ROM_REGION:
      case 0x1FC00800 ... 0x7FFFFFFF:
      case 0x80000000 ... 0xFFFFFFFF: break;
      default: Util::panic("Unimplemented 32-bit write at address {:08X} with value {:0X} (PC = {:016X})", paddr, val, (u64)regs.pc);
    }
  }
}

void Mem::Write(Registers& regs, u32 paddr, u64 val) {
  const auto page = paddr >> 12;
  const auto offset = paddr & 0xFFF;
  const auto pointer = writePages[page];
  SI& si = mmio.si;

  if(pointer) {
    Util::WriteAccess<u64>((u8*)pointer, offset, val);
  } else {
    switch (paddr) {
      case RDRAM_REGION:
        Util::WriteAccess<u64>(mmio.rdp.rdram, paddr, val);
        break;
      case RSP_MEM_REGION: {
        auto& dest = paddr & 0x1000 ? mmio.rsp.imem : mmio.rsp.dmem;
        val >>= 32;
        Util::WriteAccess<u32>(dest, paddr & 0xfff, val);
      } break;
      case REGION_CART:
        Util::trace("BusWrite<u8> @ {:08X} = {:016X}", paddr, val);
        mmio.pi.BusWrite<false>(paddr, val);
        break;
      case MMIO_REGION:
        Util::panic("MMIO Write!");
      case PIF_RAM_REGION:
        Util::WriteAccess<u64>(si.pif.ram, paddr - PIF_RAM_REGION_START, htobe64(val));
        si.pif.ProcessCommands(*this);
        break;
      case 0x00800000 ... 0x03EFFFFF:
      case 0x04200000 ... 0x042FFFFF:
      case 0x04900000 ... 0x04FFFFFF:
      case 0x1FC00000 ... 0x1FC007BF:
      case 0x1FC00800 ... 0x7FFFFFFF:
      case 0x80000000 ... 0xFFFFFFFF: break;
      default:
        Util::panic("Unimplemented 64-bit write at address {:08X} with value {:0X} (PC = {:016X})", paddr, val,
                    (u64) regs.pc);
    }
  }
}

template <> u32 Mem::BackupRead<u32>(u32 addr) {
  switch(saveType) {
    case SAVE_NONE: return 0;
    case SAVE_EEPROM_4k: case SAVE_EEPROM_16k:
      Util::warn("Accessing cartridge backup type SAVE_EEPROM, returning 0 for word read");
      return 0;
    case SAVE_FLASH_1m:
      return flash.Read<u32>(addr);
    case SAVE_SRAM_256k:
      return 0xFFFFFFFF;
    default:
      Util::panic("Backup read word with unknown save type");
  }
}

template <> u8 Mem::BackupRead<u8>(u32 addr) {
  switch (saveType) {
  case SAVE_NONE: return 0;
  case SAVE_EEPROM_4k: case SAVE_EEPROM_16k:
    Util::warn("Accessing cartridge backup type SAVE_EEPROM, returning 0 for word read");
    return 0;
  case SAVE_FLASH_1m:
    return flash.Read<u8>(addr);
  case SAVE_SRAM_256k:
    if (saveData.is_mapped()) {
      assert(addr < saveData.size());
      return saveData[addr];
    }
    else {
      Util::panic("Invalid backup Read<u8> if save data is not initialized");
    }
  default:
    Util::panic("Backup read word with unknown save type");
  }
}

template <> void Mem::BackupWrite<u32>(u32 addr, u32 val) {
  switch(saveType) {
    case SAVE_NONE:
      Util::warn("Accessing cartridge with save type SAVE_NONE in write word");
      break;
    case SAVE_EEPROM_4k: case SAVE_EEPROM_16k:
      Util::panic("Accessing cartridge with save type SAVE_EEPROM in write word");
    case SAVE_FLASH_1m:
      flash.Write<u32>(addr, val);
      break;
    case SAVE_SRAM_256k:
      break;
    default:
      Util::panic("Backup read word with unknown save type");
  }
}

template <> void Mem::BackupWrite<u8>(u32 addr, u8 val) {
  switch(saveType) {
    case SAVE_NONE:
      Util::warn("Accessing cartridge with save type SAVE_NONE in write word");
      break;
    case SAVE_EEPROM_4k: case SAVE_EEPROM_16k:
      Util::panic("Accessing cartridge with save type SAVE_EEPROM in write word");
    case SAVE_FLASH_1m:
      flash.Write<u8>(addr, val);
      break;
    case SAVE_SRAM_256k:
      if(saveData.is_mapped()) {
        assert(addr < saveData.size());
        saveData[addr] = val;
      } else {
        Util::panic("Invalid backup Write<u8> if save data is not initialized");
      }
      break;
    default:
      Util::panic("Backup read word with unknown save type");
  }
}

std::vector<u8> Mem::Serialize() {
  std::vector<u8> res{};

  auto sMMIO = mmio.Serialize();
  auto sFLASH = flash.Serialize();
  mmioSize = sMMIO.size();
  flashSize = sFLASH.size();

  res.insert(res.begin(), sMMIO.begin(), sMMIO.end());
  res.insert(res.end(), sFLASH.begin(), sFLASH.end());
  res.insert(res.end(), saveData.begin(), saveData.end());

  return res;
}

void Mem::Deserialize(const std::vector<u8>& data) {
  mmio.Deserialize(std::vector<u8>(data.begin(), data.begin() + mmioSize));
  flash.Deserialize(std::vector<u8>(data.begin() + mmioSize, data.begin() + mmioSize + flashSize));
  memcpy(saveData.data(), data.data() + mmioSize + flashSize, saveData.size());
}
}