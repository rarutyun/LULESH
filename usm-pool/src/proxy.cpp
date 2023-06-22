/*
    Copyright (c) 2005-2022 Intel Corporation

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#define _CRT_SECURE_NO_WARNINGS 1

#if __unix__ && !__ANDROID__
// include <bits/c++config.h> indirectly so that <cstdlib> is not included
#include <cstddef>
// include <features.h> indirectly so that <stdlib.h> is not included
#include <unistd.h>
// Working around compiler issue with Anaconda's gcc 7.3 compiler package.
// New gcc ported for old libc may provide their inline implementation
// of aligned_alloc as required by new C++ standard, this makes it hard to
// redefine aligned_alloc here. However, running on systems with new libc
// version, it still needs it to be redefined, thus tricking system headers
#if defined(__GLIBC_PREREQ)
#if !__GLIBC_PREREQ(2, 16) && _GLIBCXX_HAVE_ALIGNED_ALLOC
// tell <cstdlib> that there is no aligned_alloc
#undef _GLIBCXX_HAVE_ALIGNED_ALLOC
// trick <stdlib.h> to define another symbol instead
#define aligned_alloc __hidden_redefined_aligned_alloc
// Fix the state and undefine the trick
#include <cstdlib>
#undef aligned_alloc
#endif // !__GLIBC_PREREQ(2, 16) && _GLIBCXX_HAVE_ALIGNED_ALLOC
#endif // defined(__GLIBC_PREREQ)
#include <cstdlib>
#endif // __unix__ && !__ANDROID__

#include "proxy.h"

#include "oneapi/tbb/detail/_config.h"
#include "oneapi/tbb/scalable_allocator.h"
#include "environment.h"

#if !defined(__EXCEPTIONS) && !defined(_CPPUNWIND) && !defined(__SUNPRO_CC)
    #if TBB_USE_EXCEPTIONS
        #error Compilation settings do not support exception handling. Please do not set TBB_USE_EXCEPTIONS macro or set it to 0.
    #elif !defined(TBB_USE_EXCEPTIONS)
        #define TBB_USE_EXCEPTIONS 0
    #endif
#elif !defined(TBB_USE_EXCEPTIONS)
    #define TBB_USE_EXCEPTIONS 1
#endif

#if MALLOC_UNIXLIKE_OVERLOAD_ENABLED || _WIN32 && !__TBB_WIN8UI_SUPPORT
/*** internal global operator new implementation (Linux, Windows) ***/
#include <new>

#if MALLOC_UNIXLIKE_OVERLOAD_ENABLED || MALLOC_ZONE_OVERLOAD_ENABLED
inline void InitOrigPointers();
#else
inline void InitOrigPointers() {}
#endif

// Synchronization primitives to protect original library pointers and new_handler
#include "Synchronize.h"
// Use MallocMutex implementation
typedef MallocMutex ProxyMutex;

// Adds aliasing and copy attributes to function if available
#if defined(__has_attribute)
    #if __has_attribute(__copy__)
        #define __TBB_ALIAS_ATTR_COPY(name) __attribute__((alias (#name), __copy__(name)))
    #endif
#endif

#ifndef __TBB_ALIAS_ATTR_COPY
    #define __TBB_ALIAS_ATTR_COPY(name) __attribute__((alias (#name)))
#endif

// Original (i.e., replaced) functions,
// they are never changed for MALLOC_ZONE_OVERLOAD_ENABLED.
static void *orig_free,
    *orig_realloc,
    *orig_malloc;

// In case there is no std::get_new_handler function
// which provides synchronized access to std::new_handler
#if !__TBB_CPP11_GET_NEW_HANDLER_PRESENT
static ProxyMutex new_lock;
#endif

static inline void* InternalOperatorNew(size_t sz) {
    InitOrigPointers();
    void *res = safer_aligned_malloc(sz, sizeof(void*));
#if TBB_USE_EXCEPTIONS
    while (!res) {
        std::new_handler handler;
#if __TBB_CPP11_GET_NEW_HANDLER_PRESENT
        handler = std::get_new_handler();
#else
        {
            ProxyMutex::scoped_lock lock(new_lock);
            handler = std::set_new_handler(0);
            std::set_new_handler(handler);
        }
#endif
        if (handler) {
            (*handler)();
        } else {
            throw std::bad_alloc();
        }
        res = safer_aligned_malloc(sz, sizeof(void*));
}
#endif /* TBB_USE_EXCEPTIONS */
    return res;
}
/*** end of internal global operator new implementation ***/
#endif // MALLOC_UNIXLIKE_OVERLOAD_ENABLED || _WIN32 && !__TBB_WIN8UI_SUPPORT

#if MALLOC_UNIXLIKE_OVERLOAD_ENABLED || MALLOC_ZONE_OVERLOAD_ENABLED

#ifndef __THROW
#define __THROW
#endif

/*** service functions and variables ***/
#include <string.h> // for memset
#include <unistd.h> // for sysconf

static long memoryPageSize;

static inline void initPageSize()
{
    memoryPageSize = sysconf(_SC_PAGESIZE);
}

#if MALLOC_UNIXLIKE_OVERLOAD_ENABLED
#include <dlfcn.h>
#include <malloc.h>    // mallinfo

/* __TBB_malloc_proxy used as a weak symbol by libtbbmalloc for:
   1) detection that the proxy library is loaded
   2) check that dlsym("malloc") found something different from our replacement malloc
*/

#if 0
extern "C" void *__TBB_malloc_proxy(size_t) __TBB_ALIAS_ATTR_COPY(malloc);
#endif

static void *orig_msize;

#elif MALLOC_ZONE_OVERLOAD_ENABLED

#include "proxy_overload_osx.h"

#endif // MALLOC_ZONE_OVERLOAD_ENABLED


#if MALLOC_UNIXLIKE_OVERLOAD_ENABLED
#define ZONE_ARG
#define PREFIX(name) name

static void *orig_libc_free,
    *orig_libc_realloc,
    *orig_aligned_alloc_shared,
    *orig_sycl_free,
    *orig_submit_impl;

using posix_memalign_t = int (*)(void **memptr, size_t alignment, size_t size);

static posix_memalign_t posix_memalign_ptr;

// We already tried to find ptr to original functions.
static std::atomic<bool> origFuncSearched{false};

inline void InitOrigPointers()
{
    // race is OK here, as different threads found same functions
    if (!origFuncSearched.load(std::memory_order_acquire)) {
        orig_malloc = dlsym(RTLD_NEXT, "malloc");
        posix_memalign_ptr = reinterpret_cast<posix_memalign_t>(dlsym(RTLD_NEXT, "posix_memalign"));
        orig_free = dlsym(RTLD_NEXT, "free");
        orig_realloc = dlsym(RTLD_NEXT, "realloc");
        orig_msize = dlsym(RTLD_NEXT, "malloc_usable_size");
        orig_libc_free = dlsym(RTLD_NEXT, "__libc_free");
        orig_libc_realloc = dlsym(RTLD_NEXT, "__libc_realloc");
        // find sycl::_V1::aligned_alloc_shared(unsigned long, unsigned long, sycl::_V1::device const&, sycl::_V1::context const&, sycl::_V1::property_list const&, sycl::_V1::detail::code_location const&)
        orig_aligned_alloc_shared = dlsym(RTLD_NEXT, "_ZN4sycl3_V120aligned_alloc_sharedEmmRKNS0_6deviceERKNS0_7contextERKNS0_13property_listERKNS0_6detail13code_locationE");
        // find sycl::_V1::free(void*, sycl::_V1::context const&, sycl::_V1::detail::code_location const&)
        orig_sycl_free = dlsym(RTLD_NEXT, "_ZN4sycl3_V14freeEPvRKNS0_7contextERKNS0_6detail13code_locationE");
        // find sycl::_V1::queue::submit_impl(std::function<void (sycl::_V1::handler&)>, sycl::_V1::detail::code_location const&)
        orig_submit_impl = dlsym(RTLD_NEXT, "_ZN4sycl3_V15queue11submit_implESt8functionIFvRNS0_7handlerEEERKNS0_6detail13code_locationE");

        origFuncSearched.store(true, std::memory_order_release);
    }
}

/*** replacements for malloc and the family ***/
extern "C" {
#elif MALLOC_ZONE_OVERLOAD_ENABLED

// each impl_* function has such 1st argument, it's unused
#define ZONE_ARG struct _malloc_zone_t *,
#define PREFIX(name) impl_##name
// not interested in original functions for zone overload
inline void InitOrigPointers() {}

#endif // MALLOC_UNIXLIKE_OVERLOAD_ENABLED and MALLOC_ZONE_OVERLOAD_ENABLED

void *PREFIX(malloc)(ZONE_ARG size_t size) __THROW
{
    InitOrigPointers();
    return safer_aligned_malloc(size, sizeof(void*));
}

void *PREFIX(calloc)(ZONE_ARG size_t num, size_t size) __THROW
{
    char *res = static_cast<char*>(safer_aligned_malloc(num * size, sizeof(void*)));
    return memset(res, 0, num * size);
}

void PREFIX(free)(ZONE_ARG void *object) __THROW
{
    InitOrigPointers();
    safer_free(object, (void (*)(void*))orig_free);
}

void *PREFIX(realloc)(ZONE_ARG void* ptr, size_t sz) __THROW
{
    InitOrigPointers();
    return safer_realloc(ptr, sz, (realloc_type)orig_realloc, orig_malloc);
}

/* The older *NIX interface for aligned allocations;
   it's formally substituted by posix_memalign and deprecated,
   so we do not expect it to cause cyclic dependency with C RTL. */
void *PREFIX(memalign)(ZONE_ARG size_t alignment, size_t size) __THROW
{
    return safer_aligned_malloc(size, alignment);
}

/* valloc allocates memory aligned on a page boundary */
void *PREFIX(valloc)(ZONE_ARG size_t size) __THROW
{
    if (! memoryPageSize) initPageSize();

    return safer_aligned_malloc(size, memoryPageSize);
}

#undef ZONE_ARG
#undef PREFIX

#if MALLOC_UNIXLIKE_OVERLOAD_ENABLED

// match prototype from system headers
#if __ANDROID__
size_t malloc_usable_size(const void *ptr) __THROW
#else
size_t malloc_usable_size(void *ptr) __THROW
#endif
{
    InitOrigPointers();
    return safer_msize(const_cast<void*>(ptr), (size_t (*)(void*))orig_msize);
}

int posix_memalign(void **memptr, size_t alignment, size_t size) __THROW
{
    *memptr = safer_aligned_malloc(size, alignment);
    return 0;
}

/* pvalloc allocates smallest set of complete pages which can hold
   the requested number of bytes. Result is aligned on page boundary. */
void *pvalloc(size_t size) __THROW
{
    if (! memoryPageSize) initPageSize();
    // align size up to the page size,
    // pvalloc(0) returns 1 page, see man libmpatrol
    size = size? ((size-1) | (memoryPageSize-1)) + 1 : memoryPageSize;

    return safer_aligned_malloc(size, memoryPageSize);
}

int mallopt(int /*param*/, int /*value*/) __THROW
{
    return 1;
}

#if defined(__GLIBC__) || defined(__ANDROID__)
struct mallinfo mallinfo() __THROW
{
    struct mallinfo m;
    memset(&m, 0, sizeof(struct mallinfo));

    return m;
}
#endif

#if __ANDROID__
// Android doesn't have malloc_usable_size, provide it to be compatible
// with Linux, in addition overload dlmalloc_usable_size() that presented
// under Android.
size_t dlmalloc_usable_size(const void *ptr) __TBB_ALIAS_ATTR_COPY(malloc_usable_size);
#else // __ANDROID__

// TODO: consider using __typeof__ to guarantee the correct declaration types
// C11 function, supported starting GLIBC 2.16
void *aligned_alloc(size_t alignment, size_t size) __TBB_ALIAS_ATTR_COPY(memalign);
// Those non-standard functions are exported by GLIBC, and might be used
// in conjunction with standard malloc/free, so we must overload them.
// Bionic doesn't have them. Not removing from the linker scripts,
// as absent entry points are ignored by the linker.

void *__libc_malloc(size_t size) __TBB_ALIAS_ATTR_COPY(malloc);
void *__libc_calloc(size_t num, size_t size) __TBB_ALIAS_ATTR_COPY(calloc);
void *__libc_memalign(size_t alignment, size_t size) __TBB_ALIAS_ATTR_COPY(memalign);
void *__libc_pvalloc(size_t size) __TBB_ALIAS_ATTR_COPY(pvalloc);
void *__libc_valloc(size_t size) __TBB_ALIAS_ATTR_COPY(valloc);

// call original __libc_* to support naive replacement of free via __libc_free etc
void __libc_free(void *ptr)
{
    InitOrigPointers();
    safer_free(ptr, (void (*)(void*))orig_libc_free);
}

void *__libc_realloc(void *ptr, size_t size)
{
    InitOrigPointers();
    return safer_realloc(ptr, size, (realloc_type)orig_libc_realloc, orig_malloc);
}

#endif // !__ANDROID__

} /* extern "C" */

void *orig_aligned_alloc(size_t size, size_t alignment)
{
    void *memptr;
    // posix_memalign rejects alignment less then sizeof(void*)
    int ret = posix_memalign_ptr(&memptr, std::max(alignment, sizeof(void*)), size);
    assert(!ret);
    return ret? nullptr : memptr;
}

/*** replacements for global operators new and delete ***/

void* operator new(size_t sz) {
    return InternalOperatorNew(sz);
}
void* operator new[](size_t sz) {
    return InternalOperatorNew(sz);
}
void operator delete(void* ptr) noexcept {
    InitOrigPointers();
    safer_free(ptr, (void (*)(void*))orig_free);
}
void operator delete[](void* ptr) noexcept {
    InitOrigPointers();
    safer_free(ptr, (void (*)(void*))orig_free);
}
void* operator new(size_t size, const std::nothrow_t&) noexcept {
    return safer_aligned_malloc(size, sizeof(void*));
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return safer_aligned_malloc(size, sizeof(void*));
}
void operator delete(void* ptr, const std::nothrow_t&) noexcept {
    InitOrigPointers();
    safer_free(ptr, (void (*)(void*))orig_free);
}
void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
    InitOrigPointers();
    safer_free(ptr, (void (*)(void*))orig_free);
}
void* operator new(std::size_t size, std::align_val_t al) {
    InitOrigPointers();
    return safer_aligned_malloc(size, size_t(al));
}
void* operator new[](std::size_t size, std::align_val_t al) {
    InitOrigPointers();
    return safer_aligned_malloc(size, size_t(al));
}
void operator delete(void* ptr, std::align_val_t /*al*/) noexcept {
    safer_free(ptr, (void (*)(void*))orig_free);
}
void operator delete[](void* ptr, std::align_val_t /*al*/) noexcept {
    safer_free(ptr, (void (*)(void*))orig_free);
}

namespace sycl {

void *safer_aligned_alloc_shared(size_t Alignment, size_t Size, const sycl::device &Dev,
                     const sycl::context &Ctxt,
                     const sycl::property_list &PropList _CODELOCPARAMDEF(&CodeLoc),
                     aligned_alloc_shared_type orig_aligned_alloc_shared)
{
    // aligned_alloc_shared holds per-context lock in piextUSMSharedAlloc
    // and calls system malloc internally. Because of that, the internal system
    // malloc call must not call aligned_alloc_shared. So, we set CallProtector
    // at this point.
    RecursiveMallocCallProtector scoped;
    return orig_aligned_alloc_shared(Alignment, Size, Dev, Ctxt, PropList, CodeLoc);
}

void safer_sycl_free(void *ptr, const queue &Q _CODELOCPARAMDEF(&CodeLoc),
                    sycl_free_type orig_sycl_free)
{
    RecursiveMallocCallProtector scoped;

    sycl::context dpcpp_default_context = Q.get_context();
    return orig_sycl_free(ptr, dpcpp_default_context, CodeLoc);
}

inline namespace _V1 {

void *aligned_alloc_shared(size_t Alignment, size_t Size, const sycl::device &Dev,
                     const sycl::context &Ctxt,
                     const sycl::property_list &PropList _CODELOCPARAMDEF(&CodeLoc))
{
    return safer_aligned_alloc_shared(Alignment, Size, Dev, Ctxt,
                     PropList, CodeLoc, (aligned_alloc_shared_type)orig_aligned_alloc_shared);
}

void *aligned_alloc(size_t Alignment, size_t Size, const device &Dev,
                    const context &Ctxt, sycl::_V1::usm::alloc Kind,
                    const property_list &PropList _CODELOCPARAMDEF(&CodeLoc))
{
    assert(Kind == sycl::_V1::usm::alloc::shared);
    return safer_aligned_alloc_shared(Alignment, Size, Dev, Ctxt,
                     PropList, CodeLoc, (aligned_alloc_shared_type)orig_aligned_alloc_shared);
}

void *malloc_shared(size_t Size, const queue &Q _CODELOCPARAMDEF(&CodeLoc))
{
    return safer_aligned_alloc_shared(sizeof(void*), Size, Q.get_device(), Q.get_context(),
                     property_list{}, CodeLoc, (aligned_alloc_shared_type)orig_aligned_alloc_shared);
}

void free(void *ptr, const sycl::_V1::queue &Q _CODELOCPARAMDEF(&CodeLoc))
{
    return safer_sycl_free(ptr, Q, CodeLoc, (sycl_free_type)orig_sycl_free);
}

using QueueSubmitImplPtr = sycl::_V1::event (*)(sycl::_V1::queue *, std::function<void(handler &)> CGH,
                         const detail::code_location &CodeLoc);

event queue::submit_impl(std::function<void(handler &)> CGH,
                         const detail::code_location &CodeLoc)
{
    // Our goal is to pervent mapping of allocations from
    // MemoryManager::allocateSystemMemory() to USM, as
    // Wddm::createAllocation() got ENOMEM in this case.
    RecursiveMallocCallProtector scoped;
    return ((QueueSubmitImplPtr)orig_submit_impl)(this, CGH, CodeLoc);
}

} // namespace _V1
}

#endif /* MALLOC_UNIXLIKE_OVERLOAD_ENABLED */
#endif /* MALLOC_UNIXLIKE_OVERLOAD_ENABLED || MALLOC_ZONE_OVERLOAD_ENABLED */

#ifdef _WIN32
#include <oneapi/dpl/execution>
#include <windows.h>

#if !__TBB_WIN8UI_SUPPORT

#include <stdio.h>

#include "function_replacement.h"

template<typename T, size_t N> // generic function to find length of array
inline size_t arrayLength(const T(&)[N]) {
    return N;
}

// we do not support _expand();
void* safer_expand( void *, size_t )
{
    return nullptr;
}

#define __TBB_ORIG_ALLOCATOR_REPLACEMENT_WRAPPER(CRTLIB)                                             \
void (*orig_free_##CRTLIB)(void*);                                                                   \
void __TBB_malloc_safer_free_##CRTLIB(void *ptr)                                                     \
{                                                                                                    \
    safer_free( ptr, orig_free_##CRTLIB );                                              \
}                                                                                                    \
                                                                                                     \
void (*orig__aligned_free_##CRTLIB)(void*);                                                          \
void __TBB_malloc_safer__aligned_free_##CRTLIB(void *ptr)                                            \
{                                                                                                    \
    safer_free( ptr, orig__aligned_free_##CRTLIB );                                     \
}                                                                                                    \
                                                                                                     \
size_t (*orig__msize_##CRTLIB)(void*);                                                               \
size_t __TBB_malloc_safer__msize_##CRTLIB(void *ptr)                                                 \
{                                                                                                    \
    return safer_msize( ptr, nullptr );                                    \
}                                                                                                    \
                                                                                                     \
size_t (*orig__aligned_msize_##CRTLIB)(void*, size_t, size_t);                                       \
size_t __TBB_malloc_safer__aligned_msize_##CRTLIB( void *ptr, size_t alignment, size_t offset)       \
{                                                                                                    \
    return safer_msize( ptr, nullptr ); \
}                                                                                                    \
                                                                                                     \
void* __TBB_malloc_safer_realloc_##CRTLIB( void *ptr, size_t size )                                  \
{                                                                                                    \
    orig_ptrs func_ptrs = {orig_free_##CRTLIB, orig__msize_##CRTLIB};                                \
    return safer_realloc( ptr, size, (realloc_type)orig_realloc, orig_malloc );                                      \
}                                                                                                    \
                                                                                                     \
void* __TBB_malloc_safer__aligned_realloc_##CRTLIB( void *ptr, size_t size, size_t alignment )       \
{                                                                                                    \
    orig_aligned_ptrs func_ptrs = {orig__aligned_free_##CRTLIB, orig__aligned_msize_##CRTLIB};       \
    return safer_aligned_realloc( ptr, size, alignment, nullptr );                   \
}

// Only for ucrtbase: substitution for _o_free
void (*orig__o_free)(void*);
void __TBB_malloc__o_free(void *ptr)
{
    safer_free( ptr, orig__o_free);
}
// Only for ucrtbase: substitution for _free_base
void(*orig__free_base)(void*);
void __TBB_malloc__free_base(void *ptr)
{
    safer_free(ptr, orig__free_base);
}

// Size limit is MAX_PATTERN_SIZE (28) byte codes / 56 symbols per line.
// * can be used to match any digit in byte codes.
// # followed by several * indicate a relative address that needs to be corrected.
// Purpose of the pattern is to mark an instruction bound; it should consist of several
// full instructions plus one extra byte code. It's not required for the patterns
// to be unique (i.e., it's OK to have same pattern for unrelated functions).
// TODO: use hot patch prologues if exist
const char* known_bytecodes[] = {
#if _WIN64
//  "========================================================" - 56 symbols
    "4883EC284885C974",       // release free()
    "4883EC284885C975",       // release _msize()
    "4885C974375348",         // release free() 8.0.50727.42, 10.0
    "E907000000CCCC",         // release _aligned_msize(), _aligned_free() ucrtbase.dll
    "C7442410000000008B",     // release free() ucrtbase.dll 10.0.14393.33
    "E90B000000CCCC",         // release _msize() ucrtbase.dll 10.0.14393.33
    "48895C24085748",         // release _aligned_msize() ucrtbase.dll 10.0.14393.33
    "E903000000CCCC",         // release _aligned_msize() ucrtbase.dll 10.0.16299.522
    "48894C24084883EC28BA",   // debug prologue
    "4C894424184889542410",   // debug _aligned_msize() 10.0
    "48894C24084883EC2848",   // debug _aligned_free 10.0
    "488BD1488D0D#*******E9", // _o_free(), ucrtbase.dll
 #if __TBB_OVERLOAD_OLD_MSVCR
    "48895C2408574883EC3049", // release _aligned_msize 9.0
    "4883EC384885C975",       // release _msize() 9.0
    "4C8BC1488B0DA6E4040033", // an old win64 SDK
 #endif
#else // _WIN32
//  "========================================================" - 56 symbols
    "8BFF558BEC8B",           // multiple
    "8BFF558BEC83",           // release free() & _msize() 10.0.40219.325, _msize() ucrtbase.dll
    "8BFF558BECFF",           // release _aligned_msize ucrtbase.dll
    "8BFF558BEC51",           // release free() & _msize() ucrtbase.dll 10.0.14393.33
    "558BEC8B450885C074",     // release _aligned_free 11.0
    "558BEC837D08000F",       // release _msize() 11.0.51106.1
    "558BEC837D08007419FF",   // release free() 11.0.50727.1
    "558BEC8B450885C075",     // release _aligned_msize() 11.0.50727.1
    "558BEC6A018B",           // debug free() & _msize() 11.0
    "558BEC8B451050",         // debug _aligned_msize() 11.0
    "558BEC8B450850",         // debug _aligned_free 11.0
    "8BFF558BEC6A",           // debug free() & _msize() 10.0.40219.325
 #if __TBB_OVERLOAD_OLD_MSVCR
    "6A1868********E8",       // release free() 8.0.50727.4053, 9.0
    "6A1C68********E8",       // release _msize() 8.0.50727.4053, 9.0
 #endif
#endif // _WIN64/_WIN32
    //  "========================================================" - 56 symbols
        "564883EC204889CEE8",       // release __sycl_register_lib
    nullptr
    };

#define __TBB_ORIG_ALLOCATOR_REPLACEMENT_CALL_ENTRY(CRT_VER,function_name,dbgsuffix) \
    ReplaceFunctionWithStore( #CRT_VER #dbgsuffix ".dll", #function_name, \
      (FUNCPTR)__TBB_malloc_safer_##function_name##_##CRT_VER##dbgsuffix, \
      known_bytecodes, (FUNCPTR*)&orig_##function_name##_##CRT_VER##dbgsuffix );

#define __TBB_ORIG_ALLOCATOR_REPLACEMENT_CALL_ENTRY_NO_FALLBACK(CRT_VER,function_name,dbgsuffix) \
    ReplaceFunctionWithStore( #CRT_VER #dbgsuffix ".dll", #function_name, \
      (FUNCPTR)__TBB_malloc_safer_##function_name##_##CRT_VER##dbgsuffix, 0, nullptr );

#define __TBB_ORIG_ALLOCATOR_REPLACEMENT_CALL_ENTRY_REDIRECT(CRT_VER,function_name,dest_func,dbgsuffix) \
    ReplaceFunctionWithStore( #CRT_VER #dbgsuffix ".dll", #function_name, \
      (FUNCPTR)__TBB_malloc_safer_##dest_func##_##CRT_VER##dbgsuffix, 0, nullptr );

#define __TBB_ORIG_ALLOCATOR_REPLACEMENT_CALL_IMPL(CRT_VER,dbgsuffix)                             \
    if (BytecodesAreKnown(#CRT_VER #dbgsuffix ".dll")) {                                          \
      __TBB_ORIG_ALLOCATOR_REPLACEMENT_CALL_ENTRY(CRT_VER,free,dbgsuffix)                         \
      __TBB_ORIG_ALLOCATOR_REPLACEMENT_CALL_ENTRY(CRT_VER,_msize,dbgsuffix)                       \
      __TBB_ORIG_ALLOCATOR_REPLACEMENT_CALL_ENTRY_NO_FALLBACK(CRT_VER,realloc,dbgsuffix)          \
      __TBB_ORIG_ALLOCATOR_REPLACEMENT_CALL_ENTRY(CRT_VER,_aligned_free,dbgsuffix)                \
      __TBB_ORIG_ALLOCATOR_REPLACEMENT_CALL_ENTRY(CRT_VER,_aligned_msize,dbgsuffix)               \
      __TBB_ORIG_ALLOCATOR_REPLACEMENT_CALL_ENTRY_NO_FALLBACK(CRT_VER,_aligned_realloc,dbgsuffix) \
    } else                                                                                        \
        SkipReplacement(#CRT_VER #dbgsuffix ".dll");

#define __TBB_ORIG_ALLOCATOR_REPLACEMENT_CALL_RELEASE(CRT_VER) __TBB_ORIG_ALLOCATOR_REPLACEMENT_CALL_IMPL(CRT_VER,)
#define __TBB_ORIG_ALLOCATOR_REPLACEMENT_CALL_DEBUG(CRT_VER) __TBB_ORIG_ALLOCATOR_REPLACEMENT_CALL_IMPL(CRT_VER,d)

#define __TBB_ORIG_ALLOCATOR_REPLACEMENT_CALL(CRT_VER)     \
    __TBB_ORIG_ALLOCATOR_REPLACEMENT_CALL_RELEASE(CRT_VER) \
    __TBB_ORIG_ALLOCATOR_REPLACEMENT_CALL_DEBUG(CRT_VER)

#if __TBB_OVERLOAD_OLD_MSVCR
__TBB_ORIG_ALLOCATOR_REPLACEMENT_WRAPPER(msvcr70d);
__TBB_ORIG_ALLOCATOR_REPLACEMENT_WRAPPER(msvcr70);
__TBB_ORIG_ALLOCATOR_REPLACEMENT_WRAPPER(msvcr71d);
__TBB_ORIG_ALLOCATOR_REPLACEMENT_WRAPPER(msvcr71);
__TBB_ORIG_ALLOCATOR_REPLACEMENT_WRAPPER(msvcr80d);
__TBB_ORIG_ALLOCATOR_REPLACEMENT_WRAPPER(msvcr80);
__TBB_ORIG_ALLOCATOR_REPLACEMENT_WRAPPER(msvcr90d);
__TBB_ORIG_ALLOCATOR_REPLACEMENT_WRAPPER(msvcr90);
#endif
__TBB_ORIG_ALLOCATOR_REPLACEMENT_WRAPPER(msvcr100d);
__TBB_ORIG_ALLOCATOR_REPLACEMENT_WRAPPER(msvcr100);
__TBB_ORIG_ALLOCATOR_REPLACEMENT_WRAPPER(msvcr110d);
__TBB_ORIG_ALLOCATOR_REPLACEMENT_WRAPPER(msvcr110);
__TBB_ORIG_ALLOCATOR_REPLACEMENT_WRAPPER(msvcr120d);
__TBB_ORIG_ALLOCATOR_REPLACEMENT_WRAPPER(msvcr120);
__TBB_ORIG_ALLOCATOR_REPLACEMENT_WRAPPER(ucrtbase);

/*** replacements for global operators new and delete ***/

#if _MSC_VER && !defined(__INTEL_COMPILER)
#pragma warning( push )
#pragma warning( disable : 4290 )
#endif

void __TBB_malloc_safer_delete(void* ptr)
{
    safer_free(ptr, orig_free_ucrtbase);
}

/*** operator new overloads internals (Linux, Windows) ***/

void* operator_new(size_t sz) {
    return InternalOperatorNew(sz);
}
void* operator_new_arr(size_t sz) {
    return InternalOperatorNew(sz);
}
void operator_delete(void* ptr) noexcept {
    __TBB_malloc_safer_delete(ptr);
}
#if _MSC_VER && !defined(__INTEL_COMPILER)
#pragma warning( pop )
#endif

void operator_delete_arr(void* ptr) noexcept {
    __TBB_malloc_safer_delete(ptr);
}
void* operator_new_t(size_t sz, const std::nothrow_t&) noexcept {
    return scalable_malloc(sz);
}
void* operator_new_arr_t(std::size_t sz, const std::nothrow_t&) noexcept {
    return scalable_malloc(sz);
}
void operator_delete_t(void* ptr, const std::nothrow_t&) noexcept {
    __TBB_malloc_safer_delete(ptr);
}
void operator_delete_arr_t(void* ptr, const std::nothrow_t&) noexcept {
    __TBB_malloc_safer_delete(ptr);
}

struct Module {
    const char *name;
    bool        doFuncReplacement; // do replacement in the DLL
};

Module modules_to_replace[] = {
    {"msvcr100d.dll", true},
    {"msvcr100.dll", true},
    {"msvcr110d.dll", true},
    {"msvcr110.dll", true},
    {"msvcr120d.dll", true},
    {"msvcr120.dll", true},
    {"ucrtbase.dll", true},
//    "ucrtbased.dll" is not supported because of problems with _dbg functions
#if __TBB_OVERLOAD_OLD_MSVCR
    {"msvcr90d.dll", true},
    {"msvcr90.dll", true},
    {"msvcr80d.dll", true},
    {"msvcr80.dll", true},
    {"msvcr70d.dll", true},
    {"msvcr70.dll", true},
    {"msvcr71d.dll", true},
    {"msvcr71.dll", true},
#endif
#if __TBB_TODO
    // TODO: Try enabling replacement for non-versioned system binaries below
    {"msvcrtd.dll", true},
    {"msvcrt.dll", true},
#endif
    };

/*
We need to replace following functions:
malloc
calloc
_aligned_malloc
_expand (by dummy implementation)
??2@YAPAXI@Z      operator new                         (ia32)
??_U@YAPAXI@Z     void * operator new[] (size_t size)  (ia32)
??3@YAXPAX@Z      operator delete                      (ia32)
??_V@YAXPAX@Z     operator delete[]                    (ia32)
??2@YAPEAX_K@Z    void * operator new(unsigned __int64)   (intel64)
??_V@YAXPEAX@Z    void * operator new[](unsigned __int64) (intel64)
??3@YAXPEAX@Z     operator delete                         (intel64)
??_V@YAXPEAX@Z    operator delete[]                       (intel64)
??2@YAPAXIABUnothrow_t@std@@@Z      void * operator new (size_t sz, const std::nothrow_t&) throw()  (optional)
??_U@YAPAXIABUnothrow_t@std@@@Z     void * operator new[] (size_t sz, const std::nothrow_t&) throw() (optional)

and these functions have runtime-specific replacement:
realloc
free
_msize
_aligned_realloc
_aligned_free
_aligned_msize
*/

typedef struct FRData_t {
    //char *_module;
    const char *_func;
    FUNCPTR _fptr;
    FRR_ON_ERROR _on_error;
} FRDATA;

FRDATA c_routines_to_replace[] = {
    { "malloc",  (FUNCPTR)safer_malloc, FRR_FAIL },
    { "calloc",  (FUNCPTR)safer_calloc, FRR_FAIL },
    { "_aligned_malloc",  (FUNCPTR)safer_aligned_malloc, FRR_FAIL },
    { "_expand",  (FUNCPTR)safer_expand, FRR_IGNORE },
};

FRDATA cxx_routines_to_replace[] = {
#if _WIN64
    { "??2@YAPEAX_K@Z", (FUNCPTR)operator_new, FRR_FAIL },
    { "??_U@YAPEAX_K@Z", (FUNCPTR)operator_new_arr, FRR_FAIL },
    { "??3@YAXPEAX@Z", (FUNCPTR)operator_delete, FRR_FAIL },
    { "??_V@YAXPEAX@Z", (FUNCPTR)operator_delete_arr, FRR_FAIL },
#else
    { "??2@YAPAXI@Z", (FUNCPTR)operator_new, FRR_FAIL },
    { "??_U@YAPAXI@Z", (FUNCPTR)operator_new_arr, FRR_FAIL },
    { "??3@YAXPAX@Z", (FUNCPTR)operator_delete, FRR_FAIL },
    { "??_V@YAXPAX@Z", (FUNCPTR)operator_delete_arr, FRR_FAIL },
#endif
    { "??2@YAPAXIABUnothrow_t@std@@@Z", (FUNCPTR)operator_new_t, FRR_IGNORE },
    { "??_U@YAPAXIABUnothrow_t@std@@@Z", (FUNCPTR)operator_new_arr_t, FRR_IGNORE }
};

#ifndef UNICODE
typedef char unicode_char_t;
#define WCHAR_SPEC "%s"
#else
typedef wchar_t unicode_char_t;
#define WCHAR_SPEC "%ls"
#endif

// Check that we recognize bytecodes that should be replaced by trampolines.
// If some functions have unknown prologue patterns, replacement should not be done.
bool BytecodesAreKnown(const unicode_char_t *dllName)
{
    const char *funcName[] = {"free", "_msize", "_aligned_free", "_aligned_msize", 0};
    HMODULE module = GetModuleHandle(dllName);

    if (!module)
        return false;
    for (int i=0; funcName[i]; i++)
        if (! IsPrologueKnown(dllName, funcName[i], known_bytecodes, module)) {
            fprintf(stderr, "TBBmalloc: skip allocation functions replacement in " WCHAR_SPEC
                    ": unknown prologue for function " WCHAR_SPEC "\n", dllName, funcName[i]);
            return false;
        }
    return true;
}

void SkipReplacement(const unicode_char_t *dllName)
{
#ifndef UNICODE
    const char *dllStr = dllName;
#else
    const size_t sz = 128; // all DLL name must fit

    char buffer[sz];
    size_t real_sz;
    char *dllStr = buffer;

    errno_t ret = wcstombs_s(&real_sz, dllStr, sz, dllName, sz-1);
    __TBB_ASSERT(!ret, "Dll name conversion failed");
#endif

    for (size_t i=0; i<arrayLength(modules_to_replace); i++)
        if (!strcmp(modules_to_replace[i].name, dllStr)) {
            modules_to_replace[i].doFuncReplacement = false;
            break;
        }
}

void ReplaceFunctionWithStore( const unicode_char_t *dllName, const char *funcName, FUNCPTR newFunc, const char ** opcodes, FUNCPTR* origFunc,  FRR_ON_ERROR on_error = FRR_FAIL )
{
    FRR_TYPE res = ReplaceFunction( dllName, funcName, newFunc, opcodes, origFunc );

    if (res == FRR_OK || res == FRR_NODLL || (res == FRR_NOFUNC && on_error == FRR_IGNORE))
        return;

    fprintf(stderr, "Failed to %s function %s in module %s\n",
            res==FRR_NOFUNC? "find" : "replace", funcName, dllName);

    // Unable to replace a required function
    // Aborting because incomplete replacement of memory management functions
    // may leave the program in an invalid state
    abort();
}

void doMallocReplacement()
{
    // Replace functions and keep backup of original code (separate for each runtime)
#if __TBB_OVERLOAD_OLD_MSVCR
    __TBB_ORIG_ALLOCATOR_REPLACEMENT_CALL(msvcr70)
    __TBB_ORIG_ALLOCATOR_REPLACEMENT_CALL(msvcr71)
    __TBB_ORIG_ALLOCATOR_REPLACEMENT_CALL(msvcr80)
    __TBB_ORIG_ALLOCATOR_REPLACEMENT_CALL(msvcr90)
#endif
    __TBB_ORIG_ALLOCATOR_REPLACEMENT_CALL(msvcr100)
    __TBB_ORIG_ALLOCATOR_REPLACEMENT_CALL(msvcr110)
    __TBB_ORIG_ALLOCATOR_REPLACEMENT_CALL(msvcr120)
    __TBB_ORIG_ALLOCATOR_REPLACEMENT_CALL_RELEASE(ucrtbase)

    // Replace functions without storing original code
    for (size_t j = 0; j < arrayLength(modules_to_replace); j++) {
        if (!modules_to_replace[j].doFuncReplacement)
            continue;
        for (size_t i = 0; i < arrayLength(c_routines_to_replace); i++)
        {
            ReplaceFunctionWithStore( modules_to_replace[j].name, c_routines_to_replace[i]._func, c_routines_to_replace[i]._fptr, nullptr, nullptr,  c_routines_to_replace[i]._on_error );
        }
        if ( strcmp(modules_to_replace[j].name, "ucrtbase.dll") == 0 ) {
            HMODULE ucrtbase_handle = GetModuleHandle("ucrtbase.dll");
            if (!ucrtbase_handle)
                continue;
            // If _o_free function is present and patchable, redirect it to tbbmalloc as well
            // This prevents issues with other _o_* functions which might allocate memory with malloc
            if ( IsPrologueKnown("ucrtbase.dll", "_o_free", known_bytecodes, ucrtbase_handle)) {
                ReplaceFunctionWithStore( "ucrtbase.dll", "_o_free", (FUNCPTR)__TBB_malloc__o_free, known_bytecodes, (FUNCPTR*)&orig__o_free,  FRR_FAIL );
            }
            // Similarly for _free_base
            if (IsPrologueKnown("ucrtbase.dll", "_free_base", known_bytecodes, ucrtbase_handle)) {
                ReplaceFunctionWithStore("ucrtbase.dll", "_free_base", (FUNCPTR)__TBB_malloc__free_base, known_bytecodes, (FUNCPTR*)&orig__free_base, FRR_FAIL);
            }
            // ucrtbase.dll does not export operator new/delete, so skip the rest of the loop.
            continue;
        }

        for (size_t i = 0; i < arrayLength(cxx_routines_to_replace); i++)
        {
#if !_WIN64
            // in Microsoft* Visual Studio* 2012 and 2013 32-bit operator delete consists of 2 bytes only: short jump to free(ptr);
            // replacement should be skipped for this particular case.
            if ( ((strcmp(modules_to_replace[j].name, "msvcr110.dll") == 0) || (strcmp(modules_to_replace[j].name, "msvcr120.dll") == 0)) && (strcmp(cxx_routines_to_replace[i]._func, "??3@YAXPAX@Z") == 0) ) continue;
            // in Microsoft* Visual Studio* 2013 32-bit operator delete[] consists of 2 bytes only: short jump to free(ptr);
            // replacement should be skipped for this particular case.
            if ( (strcmp(modules_to_replace[j].name, "msvcr120.dll") == 0) && (strcmp(cxx_routines_to_replace[i]._func, "??_V@YAXPAX@Z") == 0) ) continue;
#endif
            ReplaceFunctionWithStore( modules_to_replace[j].name, cxx_routines_to_replace[i]._func, cxx_routines_to_replace[i]._fptr, nullptr, nullptr,  cxx_routines_to_replace[i]._on_error );
        }
    }
}

FUNCPTR real_sycl_register_lib;

extern "C" void __sycl_register_lib(pi_device_binaries desc)
{
    // At the moment of __sycl_register_lib call we are under loader lock,
    // so can't use USM allocation and have to set RecursiveMallocCallProtector.
    RecursiveMallocCallProtector scoped;
    ((void (*)(pi_device_binaries))real_sycl_register_lib)(desc);
}

bool SyclBytecodesAreKnown(const unicode_char_t* dllName)
{
    const char* funcName[] = { "__sycl_register_lib", 0 };
    HMODULE module = GetModuleHandle(dllName);

    if (!module)
        return false;
    for (int i = 0; funcName[i]; i++)
        if (!IsPrologueKnown(dllName, funcName[i], known_bytecodes, module)) {
            fprintf(stderr, "TBBmalloc: skip allocation functions replacement in " WCHAR_SPEC
                ": unknown prologue for function " WCHAR_SPEC "\n", dllName, funcName[i]);
            return false;
        }
    return true;
}

void doSyclReplacement()
{
    if (SyclBytecodesAreKnown("sycl6.dll")) {
        ReplaceFunctionWithStore("sycl6.dll", "__sycl_register_lib", (FUNCPTR)__sycl_register_lib,
            known_bytecodes, &real_sycl_register_lib);
    }
}

#endif // !__TBB_WIN8UI_SUPPORT

#if defined(_MSC_VER) && !defined(__INTEL_COMPILER)
    // Suppress warning for UWP build ('main' signature found without threading model)
    #pragma warning(push)
    #pragma warning(disable:4447)
#endif

extern "C" BOOL WINAPI DllMain( HINSTANCE hInst, DWORD callReason, LPVOID reserved )
{

    if ( callReason==DLL_PROCESS_ATTACH && reserved && hInst ) {
#if !__TBB_WIN8UI_SUPPORT
        if (!tbb::detail::r1::GetBoolEnvironmentVariable("TBB_MALLOC_DISABLE_REPLACEMENT"))
        {
            doMallocReplacement();
            doSyclReplacement();
        }
#endif // !__TBB_WIN8UI_SUPPORT
    }
    else if (callReason == DLL_PROCESS_DETACH)
        // leaks here, but we are in DLL unload,
        // so can't call dpcpp_default dtor
        oneapi::dpl::execution::dpcpp_default.release();

    return TRUE;
}

#if defined(_MSC_VER) && !defined(__INTEL_COMPILER)
    #pragma warning(pop)
#endif

// Just to make the linker happy and link the DLL to the application
extern "C" __declspec(dllexport) void __TBB_malloc_proxy()
{

}

#endif //_WIN32
