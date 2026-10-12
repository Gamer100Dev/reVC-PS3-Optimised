#include <stdint.h>
#include <spu_intrinsics.h>
#include <mars/task.h>
#include <mars/module.h>

#define PARTICLE_BATCH_MAX 128
#define PARTICLE_POOL_MAX 750

typedef struct __attribute__((aligned(16))) ParticleSimInput {
	float position[4];
	float velocity[4];
	float timeStep;
	float friction;
	float gravity;
	uint32_t deathTime;
} ParticleSimInput;

typedef struct __attribute__((aligned(16))) ParticleSimOutput {
	float position[4];
	float moveStep[4];
	float velocity[4];
	uint32_t expired;
	uint32_t padding[3];
} ParticleSimOutput;

static ParticleSimInput inputRows[PARTICLE_BATCH_MAX] __attribute__((aligned(128)));
static ParticleSimOutput outputRows[PARTICLE_BATCH_MAX] __attribute__((aligned(128)));
// Keep a non-empty RW PT_LOAD segment for the MARS raw ELF loader.
static volatile uint32_t taskDataAnchor = 0x52575054;

void mars_module_main(void) {}

int
mars_task_main(const struct mars_task_args *args)
{
	if(taskDataAnchor != 0x52575054)
		return 6;
	uint64_t inputEA = args->type.u64[0];
	uint64_t outputEA = args->type.u64[1];
	uint32_t count = args->type.u32[4];
	uint32_t first = args->type.u32[5];
	uint32_t timeNow = args->type.u32[6];
	if(inputEA == 0 || outputEA == 0 || count == 0 ||
	   count > PARTICLE_BATCH_MAX || first > PARTICLE_POOL_MAX ||
	   count > PARTICLE_POOL_MAX - first)
		return 1;

	uint32_t inputBytes = count * sizeof(ParticleSimInput);
	uint32_t outputBytes = count * sizeof(ParticleSimOutput);
	if(mars_module_dma_get(inputRows, inputEA + first * sizeof(ParticleSimInput), inputBytes, 0))
		return 2;
	if(mars_module_dma_wait(0))
		return 3;

	for(uint32_t i = 0; i < count; i++){
		const vector float position = *(const vector float*)inputRows[i].position;
		const vector float velocity = *(const vector float*)inputRows[i].velocity;
		const vector float dt = spu_splats(inputRows[i].timeStep);
		const vector float friction = spu_splats(inputRows[i].friction);
		const vector float moveStep = spu_mul(velocity, dt);
		vector float nextVelocity = spu_mul(velocity, friction);
		float vz = spu_extract(nextVelocity, 2);
		if(inputRows[i].gravity > 0.0f &&
		   -50.0f * inputRows[i].gravity < vz)
			vz -= inputRows[i].gravity * inputRows[i].timeStep;
		else if(inputRows[i].gravity < 0.0f &&
		        -5.0f * inputRows[i].gravity > vz)
			vz -= inputRows[i].gravity * inputRows[i].timeStep;
		nextVelocity = spu_insert(vz, nextVelocity, 2);
		*(vector float*)outputRows[i].position = spu_add(position, moveStep);
		*(vector float*)outputRows[i].moveStep = moveStep;
		*(vector float*)outputRows[i].velocity = nextVelocity;
		outputRows[i].expired = timeNow > inputRows[i].deathTime;
		outputRows[i].padding[0] = outputRows[i].padding[1] = outputRows[i].padding[2] = 0;
	}

	if(mars_module_dma_put(outputRows, outputEA + first * sizeof(ParticleSimOutput), outputBytes, 1))
		return 4;
	if(mars_module_dma_wait(1))
		return 5;
	return 0;
}
