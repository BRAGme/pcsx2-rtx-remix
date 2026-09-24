// SPDX-License-Identifier: GPL-3.0+
#pragma once
#include "common/Pcsx2Types.h"

namespace RemixCameraTrace
{
enum Kind : u32
{
  NativeCommit = 1, JobEnqueue, PacketEnqueue, ProducerVSync,
  ConsumerVSync, RawTransfer, RawDraw, RecoveryCamera, SubmittedCamera,
  EndFrame, Boundary, WorkerPacket, APICamera
};

// These are observations at a hook, not ownership assigned to guest geometry.
struct alignas(16) PoseObservation
{
  u32 generation = 0, kind = 0, pc = 0, ee_cycle = 0;
  u64 commit_epoch = 0, kick_seq = 0;
  u32 valid = 0, viewport = 0, engine = 0, level = 0;
  u32 client = 0, controller = 0, width = 0, height = 0;
  u32 position[4] = {}, rotation[3] = {}, hfov = 0;
  u32 detail[3] = {}, native_s3 = 0;
  u32 job_id = 0;
  char map[32] = {};
};
static_assert(sizeof(PoseObservation) % 16 == 0);

bool Capturing();
// Called by the settings UI; the GS thread consumes this edge even after a pause.
void RequestRearm();
void SetRequested(bool requested, bool ready = false);
void Stop(u32 reason);
struct JobDescriptor { u32 generation = 0, job_id = 0; u64 commit_epoch = 0; };
struct WorkerObservation
{
  JobDescriptor job;
  u64 first_kick = 0, last_kick = 0;
};
JobDescriptor OnEEPoint(u32 kind, u32 pc, u32 a = 0, u32 b = 0, u32 c = 0);
inline void OnNativeCommit(u32 pc) { OnEEPoint(NativeCommit, pc); }
void AcceptPose(const PoseObservation& observation);
void RecordWorker(const WorkerObservation& observation);
void Record(u32 kind, const void* data, u32 bytes);
void RecordTransfer(u32 path, const void* data, u32 qwc);
void RecordDraw(u64 draw, u32 buffer, u32 head, u32 tail, u32 next,
                const u64* regs, const void* vertices, u32 vertex_bytes,
                const void* indices, u32 index_bytes);
void RecordCamera(u32 kind, u64 frame, u64 hash, bool valid, bool held,
                  const float* position, const float* view, const float* projection);
void RecordAPI(u64 frame, u32 type, u32 status, bool has_ext,
               const float* view, const float* projection);
void EndVSync();
}
