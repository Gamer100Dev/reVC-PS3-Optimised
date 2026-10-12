#include <stdint.h>
#include <mars/task.h>
#include <mars/module.h>

#define ROWS_PER_TASK 8
#define MAX_SRC_BYTES (1024 * ROWS_PER_TASK * 2 * 4)
#define MAX_DST_BYTES (1024 * ROWS_PER_TASK * 4)

static unsigned char srcRows[MAX_SRC_BYTES] __attribute__((aligned(128)));
static unsigned char dstRows[MAX_DST_BYTES] __attribute__((aligned(128)));
// The MARS task ELF loader requires a non-empty RW PT_LOAD segment. Without
// initialized data this translation unit emitted only .bss (p_filesz == 0),
// which mars_task_create rejects as MARS_ERROR_FORMAT.
static volatile uint32_t taskDataAnchor = 0x52574d50;

/* libspumars groups DMA helpers with its module entry object. MARS task
 * executables enter through libspumarstask's main wrapper, so this symbol is
 * retained only to satisfy the unused module-entry reference. */
void mars_module_main(void) {}

int
mars_task_main(const struct mars_task_args *args)
{
	if(taskDataAnchor != 0x52574d50) return 6;
	uint64_t srcEA = args->type.u64[0];
	uint64_t dstEA = args->type.u64[1];
	uint32_t sw = args->type.u32[4];
	uint32_t sh = args->type.u32[5];
	uint32_t dw = args->type.u32[6];
	uint32_t y0 = args->type.u32[7];
	uint32_t outRows = sh / 2;
	if(sw == 0 || sh == 0 || dw == 0 || dw > 1024 || y0 >= outRows ||
	   dw != sw / 2 || (uint64_t)sw * ROWS_PER_TASK * 8 > MAX_SRC_BYTES ||
	   (uint64_t)dw * ROWS_PER_TASK * 4 > MAX_DST_BYTES)
		return 1;

	uint32_t rows = outRows - y0;
	if(rows > ROWS_PER_TASK) rows = ROWS_PER_TASK;
	uint32_t first = y0 * 2;
	uint32_t last = first + rows * 2;
	if(last > sh) last = sh;
	uint32_t srcBegin = first * sw * 4;
	uint32_t srcBytes = (last - first) * sw * 4;
	uint32_t dmaSrcBytes = (srcBytes + 15u) & ~15u;
	if(mars_module_dma_get(srcRows, srcEA + srcBegin, dmaSrcBytes, 0)) return 2;
	if(mars_module_dma_wait(0)) return 3;

	for(uint32_t y = 0; y < rows; y++){
		uint32_t sy0 = y * 2;
		uint32_t sy1 = sy0 + 1 < last - first ? sy0 + 1 : sy0;
		for(uint32_t x = 0; x < dw; x++){
			const unsigned char *a = srcRows + (sy0 * sw + x * 2) * 4;
			const unsigned char *b = a + 4;
			const unsigned char *c = srcRows + (sy1 * sw + x * 2) * 4;
			const unsigned char *d = c + 4;
			for(uint32_t k = 0; k < 4; k++)
				dstRows[(y * dw + x) * 4 + k] = (a[k]+b[k]+c[k]+d[k]+2)>>2;
		}
	}
	uint32_t dstBytes = rows * dw * 4;
	uint32_t dmaDstBytes = (dstBytes + 15u) & ~15u;
	if(mars_module_dma_put(dstRows, dstEA + y0 * dw * 4, dmaDstBytes, 1)) return 4;
	if(mars_module_dma_wait(1)) return 5;
	return 0;
}
