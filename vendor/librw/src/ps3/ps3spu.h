#ifndef RW_PS3_SPU_H
#define RW_PS3_SPU_H

namespace rw { namespace ps3 {
struct ParticleSimInput {
	float position[4];
	float velocity[4];
	float timeStep;
	float friction;
	float gravity;
	unsigned int deathTime;
};

struct ParticleSimOutput {
	float position[4];
	float moveStep[4];
	float velocity[4];
	unsigned int expired;
	unsigned int padding[3];
};

bool spuInit(void);
bool spuDownsampleRGBA8(unsigned char *dst, int dw, int dh,
                        const unsigned char *src, int sw, int sh);
bool spuParticleInit(void);
bool spuParticleAvailable(void);
bool spuParticleBegin(const ParticleSimInput *input, ParticleSimOutput *output,
                      unsigned int count, unsigned int timeNow);
bool spuParticleWait(void);
void spuShutdown(void);
void spuGetMipStats(unsigned int *submitted, unsigned int *completed,
                    unsigned int *pixels, unsigned int *fallbacks);
} }

#endif
