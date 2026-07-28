#include <stdio.h>
 
#ifdef WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <psapi.h>
#else
    #include <dlfcn.h>
    #include <sys/types.h>
    #include <sys/stat.h> 
#endif
 
#include "sigscan.h"
 
/* There is no ANSI ustrncpy */
unsigned char* ustrncpy(unsigned char *dest, const unsigned char *src, int len) {
    while(len--)
        dest[len] = src[len];
 
    return dest;
}
 
/* //////////////////////////////////////
    CSigScan Class
    ////////////////////////////////////// */
unsigned char* CSigScan::base_addr;
size_t CSigScan::base_len;
void *(*CSigScan::sigscan_dllfunc)(const char *pName, int *pReturnCode);
 
/* Initialize the Signature Object */
void CSigScan::Init(unsigned char *sig, const char *mask, size_t len) {
    is_set = 0;
 
    if (len >= SIG_STRING_MAX)
        return;

    sig_len = len;
    ustrncpy(sig_str, sig, sig_len);
 
    strncpy(sig_mask, mask, sig_len);
    sig_mask[sig_len+1] = 0;
 
    if(!base_addr)
        return ; // GetDllMemInfo() Failed
 
    if((sig_addr = FindSignature()) == NULL)
        return ; // FindSignature() Failed
 
    is_set = 1;
    // SigScan Successful!
}
 
/* Get base address of the server module (base_addr) and get its ending offset (base_len) */
bool CSigScan::GetDllMemInfo(void) {
    void *pAddr = (void*)sigscan_dllfunc;
    base_addr = 0;
    base_len = 0;
 
    #ifdef WIN32
    MEMORY_BASIC_INFORMATION mem;
 
    if(!pAddr)
        return false; // GetDllMemInfo failed!pAddr
 
    if(!VirtualQuery(pAddr, &mem, sizeof(mem)))
        return false;
 
    base_addr = (unsigned char*)mem.AllocationBase;
 
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER*)mem.AllocationBase;
    IMAGE_NT_HEADERS *pe = (IMAGE_NT_HEADERS*)((unsigned long)dos+(unsigned long)dos->e_lfanew);
 
    if(pe->Signature != IMAGE_NT_SIGNATURE) {
        base_addr = 0;
        return false; // GetDllMemInfo failedpe points to a bad location
    }
 
    base_len = (size_t)pe->OptionalHeader.SizeOfImage;
 
    #else
 
    Dl_info info;
    struct stat buf;
 
    if(!dladdr(pAddr, &info))
        return false;
 
    if(!info.dli_fbase || !info.dli_fname)
        return false;
 
    if(stat(info.dli_fname, &buf) != 0)
        return false;
 
    base_addr = (unsigned char*)info.dli_fbase;
    base_len = buf.st_size;
    #endif
 
    return true;
}

bool CSigScan::GetDllMemInfo(const char* moduleName) {
    base_addr = 0;
    base_len = 0;

#ifdef WIN32
    HMODULE module = GetModuleHandle(moduleName);
    if (module == NULL)
        return false; // GetDllMemInfo failed!

    base_addr = (unsigned char*)module;

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)module;
    IMAGE_NT_HEADERS* pe = (IMAGE_NT_HEADERS*)((unsigned long)dos + (unsigned long)dos->e_lfanew);

    if (pe->Signature != IMAGE_NT_SIGNATURE) {
        base_addr = 0;
        return false; // GetDllMemInfo failedpe points to a bad location
    }

    base_len = (size_t)pe->OptionalHeader.SizeOfImage;

#else
    #error not intended for linux!
#endif

    return true;
}
 
/* Scan for the signature in memory then return the starting position's address */
void* CSigScan::FindSignature(void) {
    unsigned char *pBasePtr = base_addr;
    unsigned char *pEndPtr = base_addr+base_len;
    size_t i;
 
    while(pBasePtr < pEndPtr) {
        for(i = 0;i < sig_len;i++) {
            if((sig_mask[i] != '?') && (sig_str[i] != pBasePtr[i]))
                break;
        }
 
        // If 'i' reached the end, we know we have a match!
        if(i == sig_len)
            return (void*)pBasePtr;
 
        pBasePtr++;
    }
 
    return NULL;
}

void WriteByte(void* dst, unsigned char src)
{
    DWORD oldProtect;
    VirtualProtect(dst, 1, PAGE_EXECUTE_READWRITE, &oldProtect);
    memset(dst, src, 1);
    VirtualProtect(dst, 1, oldProtect, &oldProtect);
}

void WriteBytes(void* dst, const void* src, size_t size)
{
	DWORD oldProtect;
	VirtualProtect(dst, size, PAGE_EXECUTE_READWRITE, &oldProtect);
	memcpy(dst, src, size);
	VirtualProtect(dst, size, oldProtect, &oldProtect);
}