#include <stdio.h>
#include "../rwbase.h"
#include "ps3spu.h"

#ifdef RW_PS3_RSX
#include <stdlib.h>
#include <string.h>
#include <mars/base.h>
#include <mars/context.h>
#include <mars/task.h>
#include <sys/spu.h>
#include <sys/mutex.h>
#include <sys/systime.h>

extern "C" {
// MARS mars_task_create expects the raw ELF image, embedded by PSL1GHT's
// bin2s path just like its spumars example.
extern const unsigned char ps3mip_task[];
extern const unsigned char ps3particle_task[];
}

namespace rw { namespace ps3 {
extern void rsxLog(const char *fmt, ...);
static_assert(sizeof(ParticleSimInput) == 48, "SPU particle input DMA layout changed");
static_assert(sizeof(ParticleSimOutput) == 64, "SPU particle output DMA layout changed");
static const uint32 MAX_WORKERS = 6;
static const uint32 MIN_PIXELS = 128 * 128;
static const uint32 MAX_WIDTH = 1024;
// Eight output rows (sixteen input rows) reduce MARS scheduling and DMA
// overhead while keeping the task's buffers at 96 KiB in local store.
static const uint32 ROWS_PER_TASK = 8;
// A particle batch normally completes in far less than one frame. If an MPU
// stops responding, release the game thread and leave its static DMA buffers
// mapped for the remainder of the process.
static const uint64 PARTICLE_WAIT_TIMEOUT_US = 250000;
static uint64 timebaseFrequency;
static mars_context *context;
static mars_task_id tasks[MAX_WORKERS];
static mars_task_id particleTasks[MAX_WORKERS];
static uint32 workerCount;
static uint32 particleWorkerCount;
static uint32 particleNextOffset, particleInFlight, particleCount, particleTimeNow;
static uint64 particleInputEA, particleOutputEA;
static size_t particleInputBytes, particleOutputBytes;
static bool particleBatchActive;
static sys_mutex_t lock;
static bool lockReady;
static bool spuRuntimeReady;
static uint32 completedTasks, completedPixels, fallbackCount;
static uint32 submittedTasks;
static volatile uint32 initGate;
// Particle tasks are optional accelerators. Once their setup or execution
// fails, keep the game on its known-good PPU path for the rest of the process.
static bool particleSpuDisabled;
static bool particleFailureLogged;
static bool marsQuarantined;

static uint64
timebaseUS(void)
{
	if(timebaseFrequency == 0)
		timebaseFrequency = sysGetTimebaseFrequency();
	if(timebaseFrequency == 0) return 0;
	uint64 ticks = __gettime();
	return (ticks / timebaseFrequency) * 1000000ULL +
	       ((ticks % timebaseFrequency) * 1000000ULL) / timebaseFrequency;
}

static void
disableParticleSpu(const char *reason, int result)
{
	particleSpuDisabled = true;
	if(!particleFailureLogged){
		rsxLog("[spu] particle offload disabled (%s, %d); PPU fallback active",
		       reason, result);
		particleFailureLogged = true;
	}
}

#ifdef RW_PS3_SPU_SELFTEST
static uint8 selfTestSrc[256*256*4] __attribute__((aligned(128)));
static uint8 selfTestDst[128*128*4] __attribute__((aligned(128)));
static uint8 selfTestRef[128*128*4] __attribute__((aligned(128)));

static void
runMipSelfTest(void)
{
	for(uint32 y = 0; y < 256; y++)
		for(uint32 x = 0; x < 256; x++)
			for(uint32 c = 0; c < 4; c++)
				selfTestSrc[(y*256 + x)*4 + c] = (uint8)(x*3 + y*5 + c*53);
	for(uint32 y = 0; y < 128; y++)
		for(uint32 x = 0; x < 128; x++)
			for(uint32 c = 0; c < 4; c++){
				const uint8 *a = selfTestSrc + ((y*2)*256 + x*2)*4 + c;
				selfTestRef[(y*128 + x)*4 + c] = (a[0] + a[4] + a[256*4] + a[256*4+4] + 2) >> 2;
			}
	bool ok = spuDownsampleRGBA8(selfTestDst, 128, 128, selfTestSrc, 256, 256);
	if(ok && memcmp(selfTestDst, selfTestRef, sizeof(selfTestDst)) == 0)
		rsxLog("[spu-test] mip batch passed; 128x128 outputs match the PPU reference");
	else
		rsxLog("[spu-test] mip batch FAILED: worker or PPU reference mismatch");
}
#endif

static bool
ensureLock(void)
{
	while(__sync_lock_test_and_set(&initGate, 1)) {}
	if(!lockReady){
		sys_mutex_attr_t attr;
		sysMutexAttrInitialize(attr);
		lockReady = sysMutexCreate(&lock, &attr) == 0;
	}
	__sync_lock_release(&initGate);
	return lockReady;
}

static bool
initWorkers(void)
{
	if(context) return true;
	int result = mars_context_create(&context, 0, 100, 100);
	if(result != MARS_SUCCESS){
		rsxLog("[spu] MARS context creation failed (%d); mip work stays on PPU", result);
		return false;
	}
	workerCount = mars_context_get_num_mpus(context);
	if(workerCount > MAX_WORKERS) workerCount = MAX_WORKERS;
	if(workerCount == 0){
		rsxLog("[spu] MARS context created without usable workers; mip work stays on PPU");
		mars_context_destroy(context);
		context = nil;
		return false;
	}
	for(uint32 i = 0; i < workerCount; i++)
		if((result = mars_task_create(context, &tasks[i], "rw mip", ps3mip_task,
		                              MARS_TASK_CONTEXT_SAVE_SIZE_MAX)) != MARS_SUCCESS){
			rsxLog("[spu] worker task creation failed at slot %u (%d); disabling SPU mip path", i, result);
			workerCount = i;
			for(uint32 j = 0; j < workerCount; j++){
				int32 code;
				mars_task_wait(&tasks[j], &code);
				mars_task_destroy(&tasks[j]);
			}
			mars_context_destroy(context);
			context = nil;
			workerCount = 0;
			return false;
		}
	rsxLog("[spu] MARS mip workers ready: %u/%u available MPUs",
	       workerCount, mars_context_get_num_mpus(context));
	return true;
}

static bool
initRuntimeAndWorkers(void)
{
	if(!spuRuntimeReady){
		// PSL1GHT's MARS sample and runtime require SPU management to be
		// initialized before context creation.
		int result = sysSpuInitialize(6, 0);
		if(result != 0){
			rsxLog("[spu] sysSpuInitialize(6,0) failed (%d); PPU fallback active", result);
			return false;
		}
		spuRuntimeReady = true;
	}
	return initWorkers();
}

static bool
initParticleWorkers(void)
{
	if(particleSpuDisabled || marsQuarantined)
		return false;
	if(particleWorkerCount)
		return true;
	if(!initRuntimeAndWorkers()){
		disableParticleSpu("MARS runtime initialization failed", -1);
		return false;
	}
	for(uint32 i = 0; i < workerCount; i++){
		int result = mars_task_create(context, &particleTasks[i], "rw particle",
		                              ps3particle_task, MARS_TASK_CONTEXT_SAVE_SIZE_MAX);
		if(result != MARS_SUCCESS){
			disableParticleSpu("task creation failed", result);
			for(uint32 j = 0; j < i; j++){
				mars_task_destroy(&particleTasks[j]);
			}
			return false;
		}
	}
	particleWorkerCount = workerCount;
	rsxLog("[spu] particle workers ready: %u", particleWorkerCount);
	return true;
}

static bool
scheduleParticleWave(void)
{
	particleInFlight = 0;
	for(uint32 i = 0; i < particleWorkerCount && particleNextOffset < particleCount; i++){
		uint32 count = particleCount - particleNextOffset;
		if(count > 128) count = 128;
		mars_task_args args;
		memset(&args, 0, sizeof(args));
		args.type.u64[0] = particleInputEA;
		args.type.u64[1] = particleOutputEA;
		args.type.u32[4] = count;
		args.type.u32[5] = particleNextOffset;
		args.type.u32[6] = particleTimeNow;
		if(mars_task_schedule(&particleTasks[i], &args, 128) != MARS_SUCCESS)
			return false;
		particleNextOffset += count;
		particleInFlight++;
	}
	return particleInFlight != 0;
}

static bool
waitParticleWave(void)
{
	bool finished[MAX_WORKERS] = {};
	uint32 remaining = particleInFlight;
	bool ok = true;
	#ifdef RW_PS3_PARTICLE_FAULT_INJECT
	// Build with SPU_FAULT_INJECT=1 to exercise the same quarantine/fallback
	// path while real jobs are still outstanding. Their static DMA buffers stay
	// pinned, so a late completion cannot expose partial results to the PPU.
	static bool injected;
	if(remaining && !injected){
		injected = true;
		marsQuarantined = true;
		rsxLog("[spu-test] injected outstanding particle timeout; quarantining buffers");
		return false;
	}
	#endif
	const uint64 start = timebaseUS();
	uint32 polls = 0;
	while(remaining){
		for(uint32 i = 0; i < particleInFlight; i++){
			if(finished[i]) continue;
			int32 exitCode = -1;
			int result = mars_task_try_wait(&particleTasks[i], &exitCode);
			if(result == MARS_SUCCESS){
				finished[i] = true;
				remaining--;
				if(exitCode != 0) ok = false;
			}else if(result != MARS_ERROR_BUSY){
				ok = false;
				// An unexpected wait error does not prove that the task stopped.
				marsQuarantined = true;
				return false;
			}
		}
		const uint64 now = timebaseUS();
		const uint64 elapsed = now >= start ? now - start : PARTICLE_WAIT_TIMEOUT_US;
		// Keep a poll-count limit as well as the monotonic deadline. It makes
		// the wait finite even if the platform timebase is unavailable.
		if(remaining && (elapsed >= PARTICLE_WAIT_TIMEOUT_US || ++polls >= 300)){
			marsQuarantined = true;
			rsxLog("[spu] particle MARS wait timed out: %u/%u tasks completed; quarantining shared buffers", particleInFlight - remaining, particleInFlight);
			return false;
		}
		if(remaining) sysUsleep(1000);
	}
	particleInFlight = 0;
	return ok;
}

bool
spuInit(void)
{
	if(!ensureLock()){
		rsxLog("[spu] MARS worker lock initialization failed; PPU fallback active");
		return false;
	}
	sysMutexLock(lock, 0);
	bool ok = initRuntimeAndWorkers();
	sysMutexUnlock(lock);
#ifdef RW_PS3_SPU_SELFTEST
	if(ok)
		runMipSelfTest();
#endif
	return ok;
}

bool
spuParticleInit(void)
{
	if(!ensureLock())
		return false;
	sysMutexLock(lock, 0);
	bool ok = !particleSpuDisabled && initParticleWorkers();
	sysMutexUnlock(lock);
	return ok;
}

bool
spuParticleAvailable(void)
{
	return !particleSpuDisabled && !marsQuarantined;
}

bool
spuParticleBegin(const ParticleSimInput *input, ParticleSimOutput *output,
                 uint32 count, uint32 timeNow)
{
	if(!input || !output || count == 0 || count > 750 ||
	   ((uintptr)input & 127) || ((uintptr)output & 127) || !ensureLock())
		return false;
	sysMutexLock(lock, 0);
	if(particleSpuDisabled || marsQuarantined || particleBatchActive || !initParticleWorkers()){
		sysMutexUnlock(lock);
		return false;
	}
	particleInputBytes = (size_t)count * sizeof(ParticleSimInput);
	particleOutputBytes = (size_t)count * sizeof(ParticleSimOutput);
	particleInputEA = mars_ea_map((void*)input, particleInputBytes);
	particleOutputEA = mars_ea_map(output, particleOutputBytes);
	if(!particleInputEA || !particleOutputEA){
		if(particleInputEA) mars_ea_unmap(particleInputEA, particleInputBytes);
		if(particleOutputEA) mars_ea_unmap(particleOutputEA, particleOutputBytes);
		particleInputEA = particleOutputEA = 0;
		disableParticleSpu("effective-address mapping failed", -1);
		sysMutexUnlock(lock);
		return false;
	}
	mars_ea_sync();
	particleNextOffset = 0;
	particleCount = count;
	particleTimeNow = timeNow;
	particleBatchActive = true;
	if(!scheduleParticleWave()){
		bool waited = waitParticleWave();
		if(waited){
			mars_ea_sync();
			mars_ea_unmap(particleInputEA, particleInputBytes);
			mars_ea_unmap(particleOutputEA, particleOutputBytes);
			particleInputEA = particleOutputEA = 0;
		}
		particleBatchActive = false;
		disableParticleSpu("task scheduling failed", waited ? -1 : -2);
		sysMutexUnlock(lock);
		return false;
	}
	// Keep the MARS lock until spuParticleWait: mipmap and particle work may
	// share the context but must not schedule against the same task set at once.
	return true;
}

bool
spuParticleWait(void)
{
	if(!particleBatchActive)
		return false;
	bool ok = true;
	while(particleNextOffset < particleCount){
		if(!waitParticleWave()){
			ok = false;
			break;
		}
		if(!scheduleParticleWave()){
			bool waited = waitParticleWave();
			if(!waited)
				ok = false;
			ok = false;
			disableParticleSpu("task scheduling failed", waited ? -1 : -2);
			break;
		}
	}
	if(particleInFlight && !marsQuarantined && !waitParticleWave()){
		ok = false;
		disableParticleSpu("task wait or task exit failed", -1);
	}
	if(!ok && !particleSpuDisabled)
		disableParticleSpu("task wait or task exit failed", -1);
	if(!marsQuarantined){
		mars_ea_sync();
		mars_ea_unmap(particleInputEA, particleInputBytes);
		mars_ea_unmap(particleOutputEA, particleOutputBytes);
		particleInputEA = particleOutputEA = 0;
	}
	particleBatchActive = false;
	sysMutexUnlock(lock);
	return ok;
}

bool
spuDownsampleRGBA8(uint8 *dst, int32 dw, int32 dh,
                   const uint8 *src, int32 sw, int32 sh)
{
	if(!dst || !src || sw <= 0 || sh <= 0 || dw != sw/2 || dh != sh/2 ||
	   sw > (int32)MAX_WIDTH || (sw & 1) || (dw & 3) || (sh & 1) ||
	   ((uintptr)src & 15) || ((uintptr)dst & 15) ||
	   (uint64)sw*sh < MIN_PIXELS){
		__sync_fetch_and_add(&fallbackCount, 1);
		return false;
	}
	uint64 srcSize64 = (uint64)sw * sh * 4;
	uint64 dstSize64 = (uint64)dw * dh * 4;
	if(srcSize64 > SIZE_MAX || dstSize64 > SIZE_MAX){ __sync_fetch_and_add(&fallbackCount, 1); return false; }
	if(!ensureLock()){ __sync_fetch_and_add(&fallbackCount, 1); return false; }
	if(marsQuarantined){ __sync_fetch_and_add(&fallbackCount, 1); return false; }
	sysMutexLock(lock, 0);
	if(!initRuntimeAndWorkers()){
		__sync_fetch_and_add(&fallbackCount, 1);
		sysMutexUnlock(lock);
		return false;
	}
	uint64 srcEA = mars_ea_map((void*)src, (size_t)srcSize64);
	uint64 dstEA = mars_ea_map(dst, (size_t)dstSize64);
	if(!srcEA || !dstEA){
		if(srcEA) mars_ea_unmap(srcEA, (size_t)srcSize64);
		if(dstEA) mars_ea_unmap(dstEA, (size_t)dstSize64);
		fallbackCount++;
		sysMutexUnlock(lock);
		return false;
	}
	mars_ea_sync();
	uint32 nextY = 0;
	bool ok = true;
	while(nextY < (uint32)dh && ok){
		uint32 batch = 0;
		for(; batch < workerCount && nextY < (uint32)dh; batch++, nextY += ROWS_PER_TASK){
			mars_task_args args;
			memset(&args, 0, sizeof(args));
			args.type.u64[0] = srcEA;
			args.type.u64[1] = dstEA;
			args.type.u32[4] = sw;
			args.type.u32[5] = sh;
			args.type.u32[6] = dw;
			args.type.u32[7] = nextY;
			if(mars_task_schedule(&tasks[batch], &args, 128) != MARS_SUCCESS){ ok = false; break; }
			__sync_fetch_and_add(&submittedTasks, 1);
		}
		for(uint32 i = 0; i < batch; i++){
			int32 exitCode = -1;
			if(mars_task_wait(&tasks[i], &exitCode) != MARS_SUCCESS || exitCode != 0) ok = false;
			else __sync_fetch_and_add(&completedTasks, 1);
		}
	}
	mars_ea_sync();
	mars_ea_unmap(srcEA, (size_t)srcSize64);
	mars_ea_unmap(dstEA, (size_t)dstSize64);
	if(ok) __sync_fetch_and_add(&completedPixels, (uint32)((uint64)dw * dh));
	else __sync_fetch_and_add(&fallbackCount, 1);
	sysMutexUnlock(lock);
	return ok;
}

void
spuShutdown(void)
{
	if(particleBatchActive)
		spuParticleWait();
	if(marsQuarantined)
		return;
	if(context){
		for(uint32 i = 0; i < particleWorkerCount; i++){
			int32 code;
			mars_task_wait(&particleTasks[i], &code);
			mars_task_destroy(&particleTasks[i]);
		}
		particleWorkerCount = 0;
		for(uint32 i = 0; i < workerCount; i++){
			int32 code;
			mars_task_wait(&tasks[i], &code);
			mars_task_destroy(&tasks[i]);
		}
		mars_context_destroy(context);
		context = nil;
		workerCount = 0;
	}
}

void
spuGetMipStats(uint32 *submittedOut, uint32 *completedOut,
               uint32 *pixelsOut, uint32 *fallbacksOut)
{
	if(submittedOut) *submittedOut = __sync_fetch_and_add(&submittedTasks, 0);
	if(completedOut) *completedOut = __sync_fetch_and_add(&completedTasks, 0);
	if(pixelsOut) *pixelsOut = __sync_fetch_and_add(&completedPixels, 0);
	if(fallbacksOut) *fallbacksOut = __sync_fetch_and_add(&fallbackCount, 0);
}
} }
#else
namespace rw { namespace ps3 {
bool spuDownsampleRGBA8(uint8*,int32,int32,const uint8*,int32,int32){ return false; }
bool spuInit(void){return false;}
bool spuParticleInit(void){return false;}
bool spuParticleAvailable(void){return false;}
bool spuParticleBegin(const ParticleSimInput*,ParticleSimOutput*,uint32,uint32){return false;}
bool spuParticleWait(void){return false;}
void spuShutdown(void){}
void spuGetMipStats(uint32 *s,uint32 *t,uint32 *p,uint32 *f){ if(s)*s=0;if(t)*t=0;if(p)*p=0;if(f)*f=0; }
} }
#endif
