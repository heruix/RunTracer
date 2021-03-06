/*
 * redflag.c: detect writes to heap memory outside allocated chunks
 *
 * (c) 2011, the grugq <the.grugq@gmail.com>
 */


#include "pin.H"
#include <stdio.h>
#include <algorithm>
#include <iostream>
#include <list>
#include <map>

namespace WINDOWS
{
#include <windows.h>
}

/*
 * GLOBALS
 */

KNOB<string> KnobOutputFile(KNOB_MODE_WRITEONCE, "pintool",
                "o", "redflag.txt", "specifity trace file name");
static FILE * LogFile;

// forward declaration
class chunklist_t;

/*
 * housekeeping classes
 */
class chunk_t {
public:
	chunk_t(unsigned long address, unsigned long size)
		: mAddress(address), mSize(size)
	{};
	chunk_t(const chunk_t &chunk)
		: mAddress(chunk.mAddress), mSize(chunk.mSize)
	{};
	chunk_t(const chunk_t *chunk)
		: mAddress(chunk->mAddress), mSize(chunk->mSize)
	{};
	~chunk_t() {};

	bool contains(unsigned long addr) {
		if (addr >= mAddress && addr <= (mAddress+mSize))
			return true;
		return false;
	}

	bool operator< (chunk_t *rhs) {
		return mAddress < rhs->mAddress;
	}
	bool operator< (const chunk_t &rhs) {
		return mAddress < rhs.mAddress;
	};
	bool operator< (const unsigned long address) {
		return mAddress < address;
	}

	const unsigned long address() { return mAddress; };
	const unsigned long size() { return mSize; };

private:
	unsigned long	mAddress;
	unsigned long	mSize;
};


class chunklist_t {
public:
	chunklist_t() 
		: mInserts(0), mRemoves(0)
	{};
	~chunklist_t() {};

	void insert(unsigned long address, unsigned long size);
	void remove(unsigned long address);
	bool contains(unsigned long address);
	bool has_address(unsigned long address);
	bool in_range(unsigned long address);

	void set_ntdll(unsigned long low, unsigned long high) {
		mNTDLL_low = low; mNTDLL_high =high;
	};
	bool is_ntdll(unsigned long address) {
		if (address >= mNTDLL_low && address <= mNTDLL_high)
			return true;
		return false;
	};

	std::list<chunk_t>::iterator	begin() {return mChunks.begin(); };
	std::list<chunk_t>::iterator	end() {return mChunks.end(); };
	unsigned int size() const { return mChunks.size(); }

	unsigned long ntdll_low() const { return mNTDLL_low; }
	unsigned long ntdll_high() const { return mNTDLL_high; }

private:
	std::list<chunk_t>	mChunks;
	unsigned int		mInserts;
	unsigned int		mRemoves;
	unsigned long		mNTDLL_low;
	unsigned long		mNTDLL_high;
};

class heap_t {
public:
	heap_t()
		: mStart(0), mEnd(0)
	{};
	heap_t(const heap_t &heap)
		: mStart(heap.mStart), mEnd(heap.mEnd)
	{};
	~heap_t() {};

	unsigned long start() const { return mStart; };
	unsigned long end() const { return mEnd; };
	unsigned long start(unsigned long s) { mStart = s; return mStart; };
	unsigned long end(unsigned long s) { mEnd = s; return mEnd; };
private:
	unsigned long	mStart;
	unsigned long	mEnd;
};

class heaplist_t
{
public:
	heaplist_t() {};
	~heaplist_t() {};

	void update(unsigned long handle, unsigned long addr, unsigned long size) {
		heap_t	&heap = mHeaps[handle];


		if (heap.start() == 0)
			heap.start(addr);
		if (heap.end() == 0) 
			heap.end(addr+size);

		if (heap.end() < (addr+size))
			heap.end(addr+size);
		else if (heap.start() > addr)
			heap.start(addr);
	};

	bool contains(unsigned long addr) {
		std::map<unsigned long, heap_t>::iterator	it;
		for (it = mHeaps.begin(); it != mHeaps.end(); it++) {
			heap_t & heap = (*it).second;

			if (addr >= heap.start() && addr <= heap.end())
				return true;
		}
		return false;
	};

	std::map<unsigned long, heap_t>::iterator begin() { return mHeaps.begin(); };
	std::map<unsigned long, heap_t>::iterator end() { return mHeaps.end(); };

private:
	std::map<unsigned long, heap_t> mHeaps;
};


/* 
 * GLOBALS (again)
 */
chunklist_t	 ChunksList;
heaplist_t	 HeapsList;

void
chunklist_t::insert(unsigned long address, unsigned long size)
{
	chunk_t	chunk(address, size);
	std::list<chunk_t>::iterator	low;

	low = std::lower_bound(mChunks.begin(), mChunks.end(), chunk);

	mChunks.insert(low, chunk);
}

void
chunklist_t::remove(unsigned long address)
{
	std::list<chunk_t>::iterator	low;

	low = std::lower_bound(mChunks.begin(), mChunks.end(), address);

	if (low != mChunks.end() && (*low).address() == address)
		mChunks.erase(low);
}

// address is in a chunk range
bool
chunklist_t::contains(unsigned long address)
{
	std::list<chunk_t>::iterator	low;

	low = std::lower_bound(mChunks.begin(), mChunks.end(), address);

	if (low != mChunks.end() && ((*low).contains(address)))
		return true;

	low--; // preceding chunk ? 
	if (low != mChunks.end() && (*low).contains(address))
		return true;
	return false;
}

// has the exact address
bool
chunklist_t::has_address(unsigned long address)
{
	std::list<chunk_t>::iterator	low;

	low = std::lower_bound(mChunks.begin(), mChunks.end(), address);

	if (low != mChunks.end() && ((*low).address() == address))
		return true;
	return false;
}

bool
chunklist_t::in_range(unsigned long address)
{
	if (address >= mChunks.front().address() && 
	    address <= (mChunks.back().address() + mChunks.back().size()))
		return true;
	return false;
}

static void
log_redflag(ADDRINT address, ADDRINT ea, unsigned int size)
{
	unsigned long long	value;

	switch (size) {
	case 1: value = *(unsigned char *)ea; break;
	case 2: value = *(unsigned short *)ea; break;
	case 4: value = *(unsigned int *)ea; break;
	case 8: value = *(unsigned long long *)ea; break;
	default: value = *(unsigned long *)ea; break;
	}

	fprintf(LogFile, "%#x %#x %#x\n", address, ea, value);
}

#define STACKSHIFT	(3*8)
#define	is_stack(EA, SP)	(((SP)>>STACKSHIFT)==((EA)>>STACKSHIFT))

static void
write_ins(ADDRINT eip, ADDRINT esp, ADDRINT ea, unsigned int size)
{
	// is it on the stack?
	if (is_stack(ea, esp))
		return;

	// is it in a known heap region?
	if (!HeapsList.contains(ea))
		return;

	// is it in the heap?
	if (ChunksList.contains(ea))
		return;

	// is it normal access by heap management code?
	if (ChunksList.is_ntdll(eip))
		return;

	// is it in a mmap() file?
	// ... not sure how to test this

	log_redflag(eip, ea, size);
}
#undef STACKSHIFT
#undef is_stack

static void
trace_instructions(INS ins, VOID *arg)
{
	if (INS_IsMemoryWrite(ins))
		INS_InsertCall(ins, IPOINT_BEFORE,
				AFUNPTR(write_ins),
				IARG_INST_PTR,
				IARG_REG_VALUE, REG_STACK_PTR,
				IARG_MEMORYWRITE_EA,
				IARG_MEMORYWRITE_SIZE,
				IARG_END
			       );
}

// on malloc() chunklist.insert(address, size)
// on free() chunklist.remove(address)
// on realloc() chunklist.remove(address), insert(naddress, size)

static WINDOWS::PVOID
replacementRtlAllocateHeap(
		AFUNPTR rtlAllocateHeap,
		WINDOWS::PVOID heapHandle,
		WINDOWS::ULONG flags,
		WINDOWS::SIZE_T size,
		CONTEXT * ctx)
{
	WINDOWS::PVOID	retval;

	PIN_CallApplicationFunction(ctx, PIN_ThreadId(),
			CALLINGSTD_STDCALL, rtlAllocateHeap,
			PIN_PARG(void *), &retval,
			PIN_PARG(WINDOWS::PVOID), heapHandle,
			PIN_PARG(WINDOWS::ULONG), flags,
			PIN_PARG(WINDOWS::SIZE_T), size,
			PIN_PARG_END()
			);

	// adjust for heap management structures
	ChunksList.insert((unsigned long) retval-8, size+8);
	HeapsList.update((unsigned long) heapHandle, (unsigned long) retval-8,
			size+8);

	return retval;
};

static WINDOWS::PVOID
replacementRtlReAllocateHeap(
		AFUNPTR rtlReAllocateHeap,
		WINDOWS::PVOID heapHandle,
		WINDOWS::ULONG flags,
		WINDOWS::PVOID memoryPtr,
		WINDOWS::SIZE_T size,
		CONTEXT *ctx)
{
	WINDOWS::PVOID	retval;

	PIN_CallApplicationFunction(ctx, PIN_ThreadId(),
			CALLINGSTD_STDCALL, rtlReAllocateHeap,
			PIN_PARG(void *), &retval,
			PIN_PARG(WINDOWS::PVOID), heapHandle,
			PIN_PARG(WINDOWS::ULONG), flags,
			PIN_PARG(WINDOWS::PVOID), memoryPtr,
			PIN_PARG(WINDOWS::SIZE_T), size,
			PIN_PARG_END()
			);
	// XXX should we check for retval == NULL ?

	ChunksList.remove((unsigned long)memoryPtr-8);
	ChunksList.insert((unsigned long)retval-8, size+8);
	HeapsList.update((unsigned long) heapHandle, (unsigned long) retval-8,
			size+8);

	return retval;
}

static WINDOWS::BOOL
replacementRtlFreeHeap(
		AFUNPTR rtlFreeHeap,
		WINDOWS::PVOID heapHandle,
		WINDOWS::ULONG flags,
		WINDOWS::PVOID memoryPtr,
		CONTEXT *ctx)
{
	WINDOWS::BOOL 	retval;

	PIN_CallApplicationFunction(ctx, PIN_ThreadId(),
			CALLINGSTD_STDCALL, rtlFreeHeap,
			PIN_PARG(WINDOWS::BOOL), &retval,
			PIN_PARG(WINDOWS::PVOID), heapHandle,
			PIN_PARG(WINDOWS::ULONG), flags,
			PIN_PARG(WINDOWS::PVOID), memoryPtr,
			PIN_PARG_END()
			);

	ChunksList.remove((unsigned long)memoryPtr);

	return retval;
}

VOID
image_load(IMG img, VOID *v)
{
	RTN rtn;

	// check this image actually has the heap functions.
	if ((rtn = RTN_FindByName(img, "RtlAllocateHeap")) == RTN_Invalid())
		return;

	ChunksList.set_ntdll(IMG_LowAddress(img), IMG_HighAddress(img));

	// hook RtlAllocateHeap
	RTN rtlAllocate = RTN_FindByName(img, "RtlAllocateHeap");

	PROTO protoRtlAllocateHeap = 
		PROTO_Allocate( PIN_PARG(void *),
				CALLINGSTD_STDCALL,
				"RtlAllocateHeap",
				PIN_PARG(WINDOWS::PVOID), // HeapHandle
				PIN_PARG(WINDOWS::ULONG),    // Flags
				PIN_PARG(WINDOWS::SIZE_T),   // Size
				PIN_PARG_END()
				);

	RTN_ReplaceSignature(rtlAllocate,(AFUNPTR)replacementRtlAllocateHeap,
			IARG_PROTOTYPE, protoRtlAllocateHeap,
			IARG_ORIG_FUNCPTR,
			IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
			IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
			IARG_FUNCARG_ENTRYPOINT_VALUE, 2,
			IARG_CONTEXT,
			IARG_END
			);

	PROTO_Free(protoRtlAllocateHeap);

	// replace RtlReAllocateHeap()
	RTN rtlReallocate = RTN_FindByName(img, "RtlReAllocateHeap");
	PROTO protoRtlReAllocateHeap = 
			PROTO_Allocate( PIN_PARG(void *), CALLINGSTD_STDCALL,
					"RtlReAllocateHeap",
					PIN_PARG(WINDOWS::PVOID), // HeapHandle
					PIN_PARG(WINDOWS::ULONG), // Flags
					PIN_PARG(WINDOWS::PVOID), // MemoryPtr
					PIN_PARG(WINDOWS::SIZE_T),// Size
					PIN_PARG_END()
					);

	RTN_ReplaceSignature(rtlReallocate,
			(AFUNPTR)replacementRtlReAllocateHeap,
			IARG_PROTOTYPE, protoRtlReAllocateHeap,
			IARG_ORIG_FUNCPTR,
			IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
			IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
			IARG_FUNCARG_ENTRYPOINT_VALUE, 2,
			IARG_FUNCARG_ENTRYPOINT_VALUE, 3,
			IARG_CONTEXT,
			IARG_END
			);

	PROTO_Free(protoRtlReAllocateHeap);

	// replace RtlFreeHeap
	RTN rtlFree = RTN_FindByName(img, "RtlFreeHeap");
	PROTO protoRtlFreeHeap = 
		PROTO_Allocate( PIN_PARG(void *),
				CALLINGSTD_STDCALL,
				"RtlFreeHeap",
				PIN_PARG(WINDOWS::PVOID), // HeapHandle
				PIN_PARG(WINDOWS::ULONG),    // Flags
				PIN_PARG(WINDOWS::PVOID),   // MemoryPtr
				PIN_PARG_END()
				);

	RTN_ReplaceSignature(rtlFree,(AFUNPTR)replacementRtlFreeHeap,
			IARG_PROTOTYPE, protoRtlFreeHeap,
			IARG_ORIG_FUNCPTR,
			IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
			IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
			IARG_FUNCARG_ENTRYPOINT_VALUE, 2,
			IARG_CONTEXT,
			IARG_END
			);

	PROTO_Free(protoRtlFreeHeap);
}

VOID
finish(int ignored, VOID *arg)
{
	std::map<unsigned long, heap_t>::iterator	it;

	for (it = HeapsList.begin(); it != HeapsList.end(); it++)
		fprintf(LogFile, "# %#x %#x\n",
				(*it).second.start(), (*it).second.end());

	fflush(LogFile);
	fclose(LogFile);
}

UINT32
usage()
{
	std::cout << "RedFlag: tool to detecting out of bounds heap writes";
	std::cout << std::endl;
	std::cout << KNOB_BASE::StringKnobSummary();
	std::cout << std::endl;

	return 2;
}

int
main(int argc, char **argv)
{
	PIN_InitSymbols();

	if (PIN_Init(argc, argv))
		return usage();

	LogFile = fopen(KnobOutputFile.Value().c_str(), "wb+");

	INS_AddInstrumentFunction(trace_instructions, NULL);

	IMG_AddInstrumentFunction(image_load, NULL);

	PIN_AddFiniFunction(finish, NULL);

	// never returns..
	PIN_StartProgram();

	return 0;
}
