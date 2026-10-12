#ifdef RSX_FUNCTION_MACROS

#if RSX_UNSAFE
	#define RSX_FUNC(func)				rsx##func##Unsafe
#else
	#define RSX_FUNC(func)				rsx##func
#endif

#if RSX_INTERNAL

#if RSX_UNSAFE
	#define RSX_FUNC_INTERNAL(func)		__rsx##func##Unsafe
	#define RSX_CONTEXT_CURRENT_BEGIN(count) do {} while(0)	
#else
	s32 __attribute__((noinline)) rsxContextCallback(gcmContextData *context,u32 count)
	{
		if(context == NULL || context->callback == NULL)
			return -1;
		/* ATTRIBUTE_PRXPTR stores a 32-bit OPD32 descriptor address. Its
		 * entry and TOC are 32-bit words, so use the PS3 trampoline ABI rather
		 * than a native 64-bit ELFv1 indirect function-pointer call. Fixed
		 * register variables make the callback arguments and descriptor
		 * independent of compiler register allocation. */
		register u64 argContext asm("r3") = (u64)context;
		register u64 argCount asm("r4") = count;
		register u32 callbackOpd asm("r12") = (u32)context->callback;
		__asm__ __volatile__(
			"stdu 1,-128(1)\n"
			"std 2,40(1)\n"
			"lwz 0,0(12)\n"
			"lwz 2,4(12)\n"
			"mtctr 0\n"
			"bctrl\n"
			"ld 2,40(1)\n"
			"addi 1,1,128\n"
			: "+r"(argContext), "+r"(argCount), "+r"(callbackOpd)
			:
			: "r0", "r5", "r6", "r7", "r8", "r9", "r10", "r11",
			  "lr", "ctr", "cr0", "cr1", "cr2", "cr3", "cr4", "cr5",
			  "cr6", "cr7", "xer", "fr0", "fr1", "fr2", "fr3", "fr4",
			  "fr5", "fr6", "fr7", "fr8", "fr9", "fr10", "fr11", "fr12",
			  "fr13", "memory");
		return (s32)argContext;
	}
	
	#define RSX_FUNC_INTERNAL(func)		__rsx##func
	#define RSX_CONTEXT_CURRENT_BEGIN(count) do { \
		if((context->current + (count)) > context->end) { \
			if(rsxContextCallback(context,(count))!=0) return; \
		} \
	} while(0)
#endif

#endif

#endif

#ifndef RSX_FUNCTION_MACROS
#undef RSX_CONTEXT_CURRENT_BEGIN
#undef RSX_FUNC
#undef RSX_FUNC_INTERNAL
#endif
