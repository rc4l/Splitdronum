// ss_shm -- tiny cross-platform shared-memory shim used by the host/compositor.
//
// The host reads each client's framebuffer ("ZanDLLFB_<pid>") and writes each client's input ring
// ("ZanIN_<pid>") through named shared memory. On Windows that's a CreateFileMapping section; on
// POSIX (macOS) it's shm_open + mmap. The engine-side capture hook uses the SAME logical names, so the
// only platform difference the protocol must agree on is the POSIX leading-slash convention, applied
// uniformly here (host + hook both go through this rule).
//
// Header-only, no dependency beyond the OS. Two operations:
//   ShmOpenRead(name)        -- map an EXISTING section read-only (the host reading a client framebuffer)
//   ShmCreate(name, bytes)   -- create (or open) a section read/write (the host's per-seat input channel)
#pragma once
#include <cstddef>
#include <cstdio>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <fcntl.h>
  #include <unistd.h>
  #include <cstring>
#endif

namespace ss {

struct ShmView {
    void*  ptr  = nullptr;   // mapped base, or null on failure
    size_t size = 0;         // mapped bytes (0 = whole section, Windows read-open)
#ifdef _WIN32
    void*  handle = nullptr; // HANDLE to the file-mapping object
#else
    int    fd      = -1;
    bool   creator = false;  // we created it -> shm_unlink on close
    char   name[64] = "";    // POSIX shm name (incl. leading slash) for shm_unlink
#endif
};

inline bool ShmValid(const ShmView& v) { return v.ptr != nullptr; }

#ifndef _WIN32
// POSIX shm names are short (macOS PSHMNAMLEN = 31, incl. the leading '/'). Our names ("ZanDLLFB_<pid>",
// "ZanIN_<pid>") fit comfortably. Prefix a single '/' as POSIX requires.
inline void ShmPosixName(char* out, size_t cap, const char* name) {
    snprintf(out, cap, "/%s", name);
}
#endif

// Map an existing section read-only. Returns an invalid view (ptr == nullptr) if it doesn't exist yet.
inline ShmView ShmOpenRead(const char* name) {
    ShmView v;
#ifdef _WIN32
    HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
    if (!h) return v;
    void* p = MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0);   // 0 size = whole section
    if (!p) { CloseHandle(h); return v; }
    v.handle = h; v.ptr = p; v.size = 0;
#else
    char pn[64]; ShmPosixName(pn, sizeof(pn), name);
    int fd = shm_open(pn, O_RDONLY, 0);
    if (fd < 0) return v;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size == 0) { close(fd); return v; }
    void* p = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { close(fd); return v; }
    v.fd = fd; v.ptr = p; v.size = (size_t)st.st_size;
    snprintf(v.name, sizeof(v.name), "%s", pn);
#endif
    return v;
}

// Open an EXISTING section read/write (does NOT create or unlink). Used by the injected capture hook to
// access the host-created input channel ("ZanIN_<pid>"): it writes IN_MENU back and consumes mouse deltas.
inline ShmView ShmOpenRW(const char* name) {
    ShmView v;
#ifdef _WIN32
    HANDLE h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!h) return v;
    void* p = MapViewOfFile(h, FILE_MAP_WRITE, 0, 0, 0);
    if (!p) { CloseHandle(h); return v; }
    v.handle = h; v.ptr = p; v.size = 0;
#else
    char pn[64]; ShmPosixName(pn, sizeof(pn), name);
    int fd = shm_open(pn, O_RDWR, 0);
    if (fd < 0) return v;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size == 0) { close(fd); return v; }
    void* p = mmap(nullptr, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { close(fd); return v; }
    v.fd = fd; v.ptr = p; v.size = (size_t)st.st_size;   // creator stays false -> no unlink on close
    snprintf(v.name, sizeof(v.name), "%s", pn);
#endif
    return v;
}

// Create (or open) a section read/write of exactly `bytes`. Used for the host->client input channel.
inline ShmView ShmCreate(const char* name, size_t bytes) {
    ShmView v;
#ifdef _WIN32
    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                  0, (DWORD)bytes, name);
    if (!h) return v;
    void* p = MapViewOfFile(h, FILE_MAP_WRITE, 0, 0, bytes);
    if (!p) { CloseHandle(h); return v; }
    v.handle = h; v.ptr = p; v.size = bytes;
#else
    char pn[64]; ShmPosixName(pn, sizeof(pn), name);
    int fd = shm_open(pn, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return v;
    if (ftruncate(fd, (off_t)bytes) != 0) { close(fd); shm_unlink(pn); return v; }
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { close(fd); shm_unlink(pn); return v; }
    v.fd = fd; v.ptr = p; v.size = bytes; v.creator = true;
    snprintf(v.name, sizeof(v.name), "%s", pn);
    memset(p, 0, bytes);
#endif
    return v;
}

// Unmap + release. A creator also unlinks the POSIX name so it doesn't linger after exit.
inline void ShmClose(ShmView& v) {
    if (!v.ptr) return;
#ifdef _WIN32
    UnmapViewOfFile(v.ptr);
    if (v.handle) CloseHandle((HANDLE)v.handle);
    v.handle = nullptr;
#else
    munmap(v.ptr, v.size ? v.size : 1);
    if (v.fd >= 0) close(v.fd);
    if (v.creator && v.name[0]) shm_unlink(v.name);
    v.fd = -1; v.creator = false; v.name[0] = '\0';
#endif
    v.ptr = nullptr; v.size = 0;
}

}  // namespace ss
