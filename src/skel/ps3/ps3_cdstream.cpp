// ps3_cdstream.cpp -- CdStream (the .img streaming reads) for the PS3.
//
// Image reads run on one low-priority PPU worker. Only raw seek/read work is
// moved off the game thread; CStreaming still converts assets and creates
// RenderWare/GCM resources on the game thread.

#include "common.h"
#include "crossplatform.h"

#include <sys/file.h>
#include <sys/thread.h>
#include <sys/sem.h>
#include <sys/mutex.h>
#include <string.h>

#include "CdStream.h"
#include "rwcore.h"

#include "ps3_platform.h"

extern "C" void PS3_Logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef FLUSHABLE_STREAMING
bool flushStream[MAX_CDCHANNELS];
#endif

struct CdChannel
{
	int32 nStatus;
	int32 jobState; // 0 idle, 1 queued, 2 reading, 3 complete
	uint32 offset, size;
	void *buffer;
};

static char gCdImageNames[MAX_CDIMAGES+1][64];
static s32 gImgFd[MAX_CDIMAGES];
static bool gImgOpen[MAX_CDIMAGES];
static int32 gNumImages;
static int32 gNumChannels;
static CdChannel *gChannels;
static int32 lastPosnRead;
static sys_ppu_thread_t gReadThread;
static sys_mutex_t gReadMutex;
static sys_sem_t gReadWake, gReadDone;
static bool gReadWorkerReady, gReadStop;
static int32 gReadQueue[MAX_CDCHANNELS + 1];
static int32 gReadHead, gReadTail, gReadCount;
uint32 gPS3CdReads, gPS3CdErrors;
uint64 gPS3CdBytes;

static void
readImage(int32 channel)
{
	CdChannel *ch = &gChannels[channel];
	uint32 offset = ch->offset, size = ch->size;
	void *buffer = ch->buffer;
	int32 img = _GET_INDEX(offset);
	int32 status = STREAM_NONE;
	uint64 pos = 0, nread = 0;
	uint64 bytes = (uint64)size * CDSTREAM_SECTOR_SIZE;
	s32 ret = -1;
	if(img >= 0 && img < MAX_CDIMAGES && gImgOpen[img]){
		ret = sysLv2FsLSeek64(gImgFd[img], (u64)_GET_OFFSET(offset) * CDSTREAM_SECTOR_SIZE, 0, &pos);
		if(ret == 0)
			ret = sysLv2FsRead(gImgFd[img], buffer, bytes, &nread);
	}
	gPS3CdReads++;
	gPS3CdBytes += nread;
	if(ret != 0 || nread == 0){
		status = STREAM_ERROR;
		gPS3CdErrors++;
		if(gPS3CdErrors <= 20)
			PS3_Logf("[cd] read error 0x%08x: image %d sector %u, %u sectors, got %llu bytes",
			         (unsigned)ret, img, _GET_OFFSET(offset), size, (unsigned long long)nread);
	}
	sysMutexLock(gReadMutex, 0);
	ch->nStatus = status;
	ch->jobState = 3;
	sysMutexUnlock(gReadMutex);
	sysSemPost(gReadDone, 1);
}

static void
readWorker(void *arg)
{
	(void)arg;
	for(;;){
		sysSemWait(gReadWake, 0);
		sysMutexLock(gReadMutex, 0);
		if(gReadStop){
			sysMutexUnlock(gReadMutex);
			break;
		}
		if(gReadCount == 0){
			sysMutexUnlock(gReadMutex);
			continue;
		}
		int32 channel = gReadQueue[gReadHead];
		gReadHead = (gReadHead + 1) % (MAX_CDCHANNELS + 1);
		gReadCount--;
		if(channel < 0 || channel >= gNumChannels || gChannels[channel].jobState != 1){
			sysMutexUnlock(gReadMutex);
			continue;
		}
		gChannels[channel].jobState = 2;
		sysMutexUnlock(gReadMutex);
		readImage(channel);
	}
	sysThreadExit(0);
}

void
CdStreamInitThread(void)
{
	if(gReadWorkerReady || gChannels == nil)
		return;
	sys_mutex_attr_t mutexAttr;
	sys_mutex_attr_t *mutexAttrPtr = &mutexAttr;
	sysMutexAttrInitialize(mutexAttr);
	sys_sem_attr_t semAttr = {};
	semAttr.attr_protocol = SYS_SEM_ATTR_PROTOCOL;
	semAttr.attr_pshared = SYS_SEM_ATTR_PSHARED;
	memcpy(semAttr.name, "cdread", 6);
	if(sysMutexCreate(&gReadMutex, mutexAttrPtr) != 0){
		PS3_Logf("[cd] async reader mutex unavailable; using synchronous reads");
		return;
	}
	if(sysSemCreate(&gReadWake, &semAttr, 0, MAX_CDCHANNELS) != 0){
		sysMutexDestroy(gReadMutex);
		PS3_Logf("[cd] async reader semaphore unavailable; using synchronous reads");
		return;
	}
	semAttr.name[0] = 'c'; semAttr.name[1] = 'd'; semAttr.name[2] = 'd';
	if(sysSemCreate(&gReadDone, &semAttr, 0, MAX_CDCHANNELS) != 0){
		sysSemDestroy(gReadWake);
		sysMutexDestroy(gReadMutex);
		PS3_Logf("[cd] async completion semaphore unavailable; using synchronous reads");
		return;
	}
	gReadStop = false;
	gReadHead = gReadTail = gReadCount = 0;
	if(sysThreadCreate(&gReadThread, readWorker, nil, 3000, 16*1024,
	                   THREAD_JOINABLE, (char*)"cd image read") != 0){
		sysSemDestroy(gReadDone);
		sysSemDestroy(gReadWake);
		sysMutexDestroy(gReadMutex);
		PS3_Logf("[cd] async reader thread unavailable; using synchronous reads");
		return;
	}
	gReadWorkerReady = true;
	PS3_Logf("[cd] asynchronous IMG reader ready (%d channels, bounded queue)", gNumChannels);
}

void
CdStreamInit(int32 numChannels)
{
	gNumImages = 0;
	gNumChannels = numChannels;
	gChannels = (CdChannel*)calloc(numChannels, sizeof(CdChannel));
	if(gChannels == nil){
		PS3_Logf("[cd] channel allocation failed; streaming unavailable");
		return;
	}
	CdStreamInitThread();
}

uint32
GetGTA3ImgSize(void)
{
	char abs[1024];
	long long size;
	if (!PS3_ResolvePath(gCdImageNames[0], abs, sizeof(abs)) || !PS3_FileSize(abs, &size))
		return 0;
	return (uint32)size;
}

void
CdStreamShutdown(void)
{
	CdStreamRemoveImages();
	if(gReadWorkerReady){
		sysMutexLock(gReadMutex, 0);
		gReadStop = true;
		sysMutexUnlock(gReadMutex);
		sysSemPost(gReadWake, 1);
		u64 result;
		sysThreadJoin(gReadThread, &result);
		sysSemDestroy(gReadDone);
		sysSemDestroy(gReadWake);
		sysMutexDestroy(gReadMutex);
		gReadWorkerReady = false;
	}
	free(gChannels);
	gChannels = nil;
}

int32
CdStreamRead(int32 channel, void *buffer, uint32 offset, uint32 size)
{
	ASSERT(channel < gNumChannels);
	lastPosnRead = size + offset;
	if(channel < 0 || channel >= gNumChannels || buffer == nil || size == 0)
		return STREAM_NONE;
	if(gReadWorkerReady){
		sysMutexLock(gReadMutex, 0);
		CdChannel *ch = &gChannels[channel];
		if(ch->jobState == 1 || ch->jobState == 2 || gReadCount >= MAX_CDCHANNELS){
			sysMutexUnlock(gReadMutex);
			return STREAM_NONE;
		}
		ch->offset = offset;
		ch->size = size;
		ch->buffer = buffer;
		ch->nStatus = STREAM_NONE;
		ch->jobState = 1;
		gReadQueue[gReadTail] = channel;
		gReadTail = (gReadTail + 1) % (MAX_CDCHANNELS + 1);
		gReadCount++;
		sysMutexUnlock(gReadMutex);
		sysSemPost(gReadWake, 1);
		return STREAM_SUCCESS;
	}

	int32 img = _GET_INDEX(offset);
	CdChannel *ch = &gChannels[channel];
	if (img >= MAX_CDIMAGES || !gImgOpen[img]) {
		ch->nStatus = STREAM_ERROR;
		gPS3CdErrors++;
		return STREAM_SUCCESS;
	}

	u64 pos = 0, nread = 0;
	u64 bytes = (u64)size * CDSTREAM_SECTOR_SIZE;
	s32 ret = sysLv2FsLSeek64(gImgFd[img], (u64)_GET_OFFSET(offset) * CDSTREAM_SECTOR_SIZE, 0, &pos);
	if (ret == 0)
		ret = sysLv2FsRead(gImgFd[img], buffer, bytes, &nread);
	gPS3CdReads++;
	gPS3CdBytes += nread;
	// a short read at the end of the image is fine (the last file)
	if (ret != 0 || nread == 0) {
		ch->nStatus = STREAM_ERROR;
		gPS3CdErrors++;
		if (gPS3CdErrors <= 20)
			PS3_Logf("[cd] read error 0x%08x: image %d sector %u, %u sectors, got %llu bytes",
			         (unsigned)ret, img, _GET_OFFSET(offset), size, (unsigned long long)nread);
	} else
		ch->nStatus = STREAM_NONE;
	return STREAM_SUCCESS;
}

int32
CdStreamGetStatus(int32 channel)
{
	if(channel < 0 || channel >= gNumChannels)
		return STREAM_ERROR;
	CdChannel *ch = &gChannels[channel];
	if(gReadWorkerReady){
		sysMutexLock(gReadMutex, 0);
		int32 state = ch->jobState;
		if(state == 1 || state == 2){
			sysMutexUnlock(gReadMutex);
			return state == 1 ? STREAM_WAITING : STREAM_READING;
		}
		if(state == 3){
			int32 status = ch->nStatus;
			ch->jobState = 0;
			ch->buffer = nil;
			ch->nStatus = STREAM_NONE;
			sysMutexUnlock(gReadMutex);
			return status;
		}
		sysMutexUnlock(gReadMutex);
		return STREAM_NONE;
	}
	int32 status = ch->nStatus;
	ch->nStatus = STREAM_NONE;
	return status;
}

int32
CdStreamGetLastPosn(void)
{
	return lastPosnRead;
}

int32
CdStreamSync(int32 channel)
{
	if(channel < 0 || channel >= gNumChannels)
		return STREAM_ERROR;
	if(gReadWorkerReady){
		for(;;){
			sysMutexLock(gReadMutex, 0);
			int32 state = gChannels[channel].jobState;
			int32 status = gChannels[channel].nStatus;
			sysMutexUnlock(gReadMutex);
			if(state != 1 && state != 2)
				return status;
			sysSemWait(gReadDone, 0);
		}
	}
#ifdef FLUSHABLE_STREAMING
	flushStream[channel] = false;
#endif
	return gChannels[channel].nStatus;
}

void
AddToQueue(Queue *queue, int32 item)
{
	queue->items[queue->tail] = item;
	queue->tail = (queue->tail + 1) % queue->size;
}

int32
GetFirstInQueue(Queue *queue)
{
	if (queue->head == queue->tail)
		return -1;
	return queue->items[queue->head];
}

void
RemoveFirstInQueue(Queue *queue)
{
	if (queue->head == queue->tail)
		return;
	queue->head = (queue->head + 1) % queue->size;
}

bool
CdStreamAddImage(char const *path)
{
	char abs[1024];
	s32 fd;

	ASSERT(gNumImages < MAX_CDIMAGES);
	if (!PS3_ResolvePath(path, abs, sizeof(abs)) ||
	    sysLv2FsOpen(abs, SYS_O_RDONLY, &fd, 0, NULL, 0) != 0) {
		PS3_Logf("[cd] can't open image %s (%s)", path, abs);
		return false;
	}
	gImgFd[gNumImages] = fd;
	gImgOpen[gNumImages] = true;
	snprintf(gCdImageNames[gNumImages], sizeof(gCdImageNames[0]), "%s", path);
	PS3_Logf("[cd] image %d: %s", gNumImages, abs);
	gNumImages++;
	return true;
}

char*
CdStreamGetImageName(int32 cd)
{
	ASSERT(cd < MAX_CDIMAGES);
	if (gImgOpen[cd])
		return gCdImageNames[cd];
	return nil;
}

void
CdStreamRemoveImages(void)
{
	// Image descriptors and channel buffers remain owned by the reader until
	// every in-flight request has completed.
	for(int32 ch = 0; ch < gNumChannels; ch++)
		CdStreamSync(ch);
	for (int32 i = 0; i < gNumImages; i++) {
		if (gImgOpen[i])
			sysLv2FsClose(gImgFd[i]);
		gImgOpen[i] = false;
	}
	gNumImages = 0;
}

int32
CdStreamGetNumImages(void)
{
	return gNumImages;
}
