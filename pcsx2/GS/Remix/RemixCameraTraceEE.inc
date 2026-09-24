#include "MemoryTypes.h"
#include "R5900.h"

namespace RemixCameraTrace
{
namespace
{
bool ReadPose(PoseObservation& p)
{
  if (!eeMem || VMManager::GetCurrentCRC() != 0x21CC1EC3 ||
      VMManager::GetDiscSerial() != "SLUS-20883") return false;
  const u8* mem = eeMem->Main;
  constexpr u32 ram = 0x02000000;
  const auto bounded = [](u32 x, u32 size) { return x >= 0x100000 && !(x & 3) && x <= ram - size; };
  const auto word = [mem](u32 x) { u32 v; std::memcpy(&v, mem + x, 4); return v; };
  p.engine = word(0x6546C8);
  if (!bounded(p.engine, 0x468) || word(p.engine) != 0x61A220) return false;
  p.level = word(p.engine + 0x45C); p.client = word(p.engine + 0x44);
  if (word(p.engine + 0x464) || !bounded(p.level, 0x34) || word(p.level) != 0x615EE0 ||
      !bounded(p.client, 0x94) || word(p.client) != 0x60EAF0) return false;
  const u32 package = word(p.level + 0x18);
  if (!bounded(package, 0x2C) || word(package) != 0x60F950) return false;
  const u32 name = word(package + 0x28);
  if (!name || name > ram - 31) return false;
  std::memcpy(p.map, mem + name, 31); p.map[31] = 0;
  bool terminated = false;
  for (u32 i = 0; i < 31; ++i)
  {
    char c = p.map[i];
    if (!c) { terminated = i > 0; break; }
    if (c >= 'a' && c <= 'z') c = p.map[i] = c - 'a' + 'A';
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
  }
  if (!terminated || std::strcmp(p.map, "ENTRY") == 0) return false;
  p.viewport = word(p.client + 0x90);
  const u32 array = word(p.client + 0x30), count = word(p.client + 0x34), cap = word(p.client + 0x38);
  if (!count || count > 2 || cap < count || cap > 2 || !bounded(array, cap * 4) ||
      !bounded(p.viewport, 0x1E4) || word(p.viewport) != 0x60EBD0) return false;
  bool member = false;
  for (u32 i = 0; i < count; ++i) member |= word(array + 4 * i) == p.viewport;
  if (!member) return false;
  p.controller = word(p.viewport + 0x34);
  if (!bounded(p.controller, 0x57C) || word(p.controller) != 0x621610 ||
      word(p.controller + 0x56C) != p.viewport || word(p.controller + 0x18) != package) return false;
  p.width = word(p.viewport + 0x90); p.height = word(p.viewport + 0x94);
  if (p.width < 64 || p.width > 4096 || p.height < 64 || p.height > 4096) return false;
  std::memcpy(p.position, mem + p.viewport + 0x1C0, sizeof(p.position));
  std::memcpy(p.rotation, mem + p.viewport + 0x1D0, sizeof(p.rotation));
  p.hfov = word(p.viewport + 0x1DC);
  return word(0x6546C8) == p.engine && word(p.engine + 0x45C) == p.level &&
    word(p.engine + 0x464) == 0 && word(p.engine + 0x44) == p.client &&
    word(p.client + 0x90) == p.viewport && word(p.viewport + 0x34) == p.controller;
}
}

// Called only from EE execution. MTVU workers neither read native EE pose nor publish this packet.
JobDescriptor OnEEPoint(u32 kind, u32 pc, u32 a, u32 b, u32 c)
{
  if (!Capturing() || !RemixVU1Capture::Armed()) return {};
  PoseObservation p{};
  p.generation = generation.load(std::memory_order_acquire);
  p.kind = kind; p.pc = pc; p.ee_cycle = cpuRegs.cycle;
  p.kick_seq = RemixVU1Capture::KickSeq();
  p.detail[0] = a; p.detail[1] = b; p.detail[2] = c;
  p.native_s3 = cpuRegs.GPR.n.s3.UL[0];
  static u32 ee_generation = 0;
  static u64 epoch = 0;
  static u32 jobs = 0;
  if (ee_generation != p.generation) { ee_generation = p.generation; epoch = jobs = 0; }
  p.valid = ReadPose(p);
  if (kind == NativeCommit)
  {
    // The callback runs before 2EF410, after the final HFOV store at2EF40C.
    p.valid = p.valid && pc == 0x2EF410 && p.native_s3 == p.viewport &&
      *reinterpret_cast<const u32*>(eeMem->Main + 0x2EF40C) == 0xE66201DC &&
      *reinterpret_cast<const u32*>(eeMem->Main + 0x2EF410) == 0x8E620034;
    ++epoch;
  }
  p.commit_epoch = epoch; // Zero means no native commit has been observed in this capture.
  if (kind == JobEnqueue) p.job_id = ++jobs;
  // Inline storage is retained safely by the FIFO and needs no discarded-pointer cleanup.
  MTGS::PrepDataPacket(MTGS::Command::RemixCameraTrace, sizeof(p) / 16);
  MemCopy_WrappedDest(reinterpret_cast<const u128*>(&p), MTGS::RingBuffer.m_Ring,
                     MTGS::s_packet_writepos, MTGS::RingBufferSize, sizeof(p) / 16);
  MTGS::SendDataPacket();
  return {p.generation, p.job_id, p.commit_epoch};
}
}
