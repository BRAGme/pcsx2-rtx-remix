// GS-thread-only storage. EE observations arrive as inline MTGS data packets.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>

namespace RemixCameraTrace
{
namespace
{
constexpr u32 capacity = 64 * 1024 * 1024;
constexpr u32 max_fields = 240;
std::atomic<bool> capturing{false};
std::atomic<u32> generation{0};
std::unique_ptr<u8[]> storage;
u32 used = 0, fields = 0;
u64 sequence = 0;
bool previous_request = false, pending_start = false;
std::atomic<bool> rearm_requested{false};
struct Header { u32 kind, bytes; u64 sequence; };
static_assert(sizeof(Header) == 16);
struct FileHeader
{
  char magic[8];
  u32 version, generation, bytes, records, fields, stop_reason;
};
bool HasRoom(u64 bytes)
{
  return Capturing() && bytes <= capacity - used;
}
bool Append(u32 kind, const void* data, u32 bytes)
{
  if (!HasRoom(sizeof(Header) + static_cast<u64>(bytes)))
    return false;
  const Header h{kind, bytes, ++sequence};
  std::memcpy(storage.get() + used, &h, sizeof(h));
  used += sizeof(h);
  if (bytes) std::memcpy(storage.get() + used, data, bytes);
  used += bytes;
  return true;
}
}

bool Capturing() { return capturing.load(std::memory_order_acquire); }

void Stop(u32 reason)
{
  if (!capturing.exchange(false, std::memory_order_acq_rel)) return;
  const char* base = std::getenv("PCSX2_REMIX_CAMTRACEFILE");
  if (base && *base)
  {
    const std::string path = std::string(base) + "." + std::to_string(generation.load()) + ".bin";
    if (std::FILE* f = std::fopen(path.c_str(), "wb"))
    {
      const FileHeader h{{'R','6','C','A','M','T','R','C'}, 1, generation.load(), used,
                         static_cast<u32>(sequence), fields, reason};
      const bool ok = std::fwrite(&h, sizeof(h), 1, f) == 1 &&
                      std::fwrite(storage.get(), 1, used, f) == used;
      std::fclose(f);
      INFO_LOG("Remix: camera trace bytes {} fields {} records {} saved {}", used, fields, sequence, ok);
    }
    else INFO_LOG("Remix: camera trace output could not be opened");
  }
  storage.reset();
}

void RequestRearm()
{
  rearm_requested.store(true, std::memory_order_release);
}

void SetRequested(bool requested, bool ready)
{
  const bool rearm = rearm_requested.exchange(false, std::memory_order_acq_rel);
  const bool start = requested && (!previous_request || rearm);
  previous_request = requested;
  if (!requested) { pending_start = false; Stop(1); return; }
  if (start)
  {
    Stop(1);
    pending_start = true;
    INFO_LOG("Remix: camera trace armed; waiting for a valid RS3 camera and world geometry");
  }
  // A valid map and even a cached viewport can exist throughout loading. Preserve
  // the request without consuming fields until this frame actually drew the world.
  if (!pending_start || !ready) return;
  const char* path = std::getenv("PCSX2_REMIX_CAMTRACEFILE");
  if (!path || !*path) return;
  storage.reset(new (std::nothrow) u8[capacity]);
  if (!storage) return;
  used = fields = 0; sequence = 0;
  generation.fetch_add(1, std::memory_order_acq_rel);
  pending_start = false;
  capturing.store(true, std::memory_order_release);
  INFO_LOG("Remix: camera trace started generation {} after world geometry", generation.load());
}

void Record(u32 kind, const void* data, u32 bytes)
{
  if (!Capturing()) return;
  if (!Append(kind, data, bytes)) Stop(2);
}

void AcceptPose(const PoseObservation& p)
{
  if (!Capturing() || p.generation != generation.load(std::memory_order_acquire)) return;
  Record(p.kind, &p, sizeof(p));
}

void RecordWorker(const WorkerObservation& p)
{
  if (!Capturing() || p.job.generation != generation.load(std::memory_order_acquire)) return;
  Record(WorkerPacket, &p, sizeof(p));
}

void RecordTransfer(u32 path, const void* data, u32 qwc)
{
  if (!Capturing()) return;
  const u64 size = static_cast<u64>(qwc) * 16;
  if (!HasRoom(sizeof(Header) + 8 + size)) { Stop(2); return; }
  const Header h{RawTransfer, static_cast<u32>(8 + size), ++sequence};
  const u32 detail[2] = {path, qwc};
  std::memcpy(storage.get()+used, &h, sizeof(h)); used += sizeof(h);
  std::memcpy(storage.get()+used, detail, sizeof(detail)); used += sizeof(detail);
  if (size) { std::memcpy(storage.get()+used, data, static_cast<size_t>(size)); used += static_cast<u32>(size); }
}

void RecordDraw(u64 draw, u32 buffer, u32 head, u32 tail, u32 next,
                const u64* regs, const void* vertices, u32 vertex_bytes,
                const void* indices, u32 index_bytes)
{
  if (!Capturing()) return;
  struct DrawHeader
  {
    u64 draw;
    u32 buffer, head, tail, next, vertex_bytes, index_bytes;
    u64 regs[10];
  } h{draw, buffer, head, tail, next, vertex_bytes, index_bytes, {}};
  std::memcpy(h.regs, regs, sizeof(h.regs));
  // Reserve the whole draw: a budget stop never leaves a partial vertex/index record.
  const u64 bytes = sizeof(h) + static_cast<u64>(vertex_bytes) + index_bytes;
  if (!HasRoom(sizeof(Header) + bytes)) { Stop(2); return; }
  const Header rh{RawDraw, static_cast<u32>(bytes), ++sequence};
  std::memcpy(storage.get()+used, &rh, sizeof(rh)); used += sizeof(rh);
  std::memcpy(storage.get()+used, &h, sizeof(h)); used += sizeof(h);
  if (vertex_bytes) { std::memcpy(storage.get()+used, vertices, vertex_bytes); used += vertex_bytes; }
  if (index_bytes) { std::memcpy(storage.get()+used, indices, index_bytes); used += index_bytes; }
}

void RecordCamera(u32 kind, u64 frame, u64 hash, bool valid, bool held,
                  const float* position, const float* view, const float* projection)
{
  if (!Capturing()) return;
  struct Camera { u64 frame, hash; u32 valid, held; float position[3], view[16], projection[16]; }
    c{frame, hash, static_cast<u32>(valid), static_cast<u32>(held), {}, {}, {}};
  std::memcpy(c.position, position, sizeof(c.position));
  std::memcpy(c.view, view, sizeof(c.view));
  std::memcpy(c.projection, projection, sizeof(c.projection));
  Record(kind, &c, sizeof(c));
}

void EndVSync()
{
  if (Capturing() && ++fields >= max_fields) Stop(3);
}

void RecordAPI(u64 frame, u32 type, u32 status, bool has_ext,
               const float* view, const float* projection)
{
  if (!Capturing()) return;
  struct API { u64 frame; u32 type, status, has_ext, pad; float view[16], projection[16]; } c{};
  c.frame = frame; c.type = type; c.status = status; c.has_ext = has_ext;
  std::memcpy(c.view, view, sizeof(c.view));
  std::memcpy(c.projection, projection, sizeof(c.projection));
  Record(APICamera, &c, sizeof(c));
}
}
