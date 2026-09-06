// Fixed-address Mach-O compatibility host for the pinned original core.
// Unknown dependencies abort; they never silently return success.
#define _GNU_SOURCE 1
#include "../common/json.hpp"
#include <atomic>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <map>
#include <malloc.h>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <vector>

static std::map<std::string, uintptr_t> syms;
static std::vector<std::pair<uintptr_t, std::string>> address_names;
static uintptr_t tls_data = 0, tls_size = 0, tls_total = 0;
static uintptr_t stack_guard = 0xa35197bd82e436c1ULL;
static bool service_mode = false;
alignas(16) static unsigned char default_rune_locale[4096];
static uint32_t rune_mask(int c) {
  uint32_t r = 0;
  if (c >= 0 && c < 256) {
    if (isalpha(c))
      r |= 0x100;
    if (iscntrl(c))
      r |= 0x200;
    if (isdigit(c))
      r |= 0x400;
    if (islower(c))
      r |= 0x1000;
    if (ispunct(c))
      r |= 0x2000;
    if (isspace(c))
      r |= 0x4000;
    if (isupper(c))
      r |= 0x8000;
    if (isxdigit(c))
      r |= 0x10000;
    if (isblank(c))
      r |= 0x20000;
    if (isprint(c))
      r |= 0x40000;
  }
  return r;
}
extern "C" unsigned long shim_maskrune(int c, unsigned long mask) {
  return rune_mask(c) & mask;
}
static void initialize_rune_locale() {
  memcpy(default_rune_locale, "RuneMagA", 8);
  memcpy(default_rune_locale + 8, "UTF-8", 6);
  *(int32_t *)(default_rune_locale + 56) = -1;
  for (int c = 0; c < 256; c++) {
    *(uint32_t *)(default_rune_locale + 60 + c * 4) = rune_mask(c);
    *(int32_t *)(default_rune_locale + 1084 + c * 4) = tolower(c);
    *(int32_t *)(default_rune_locale + 2108 + c * 4) = toupper(c);
  }
}
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static std::map<void *, pthread_mutex_t *> mutexes;
static std::map<void *, pthread_cond_t *> conditions;
static std::map<void *, pthread_rwlock_t *> rwlocks;
static pthread_rwlock_t *rwlock_for(void *p) {
  pthread_mutex_lock(&registry_lock);
  auto &r = rwlocks[p];
  if (!r) {
    r = new pthread_rwlock_t;
    pthread_rwlock_init(r, nullptr);
  }
  pthread_mutex_unlock(&registry_lock);
  return r;
}
extern "C" int shim_rw_init(void *p, const void *a) {
  if (a) {
    errno = ENOTSUP;
    return ENOTSUP;
  }
  rwlock_for(p);
  return 0;
}
extern "C" int shim_rw_rdlock(void *p) {
  return pthread_rwlock_rdlock(rwlock_for(p));
}
extern "C" int shim_rw_wrlock(void *p) {
  return pthread_rwlock_wrlock(rwlock_for(p));
}
extern "C" int shim_rw_tryrdlock(void *p) {
  return pthread_rwlock_tryrdlock(rwlock_for(p));
}
extern "C" int shim_rw_trywrlock(void *p) {
  return pthread_rwlock_trywrlock(rwlock_for(p));
}
extern "C" int shim_rw_unlock(void *p) {
  return pthread_rwlock_unlock(rwlock_for(p));
}
extern "C" int shim_rw_destroy(void *p) {
  pthread_mutex_lock(&registry_lock);
  auto it = rwlocks.find(p);
  int r = 0;
  if (it != rwlocks.end()) {
    r = pthread_rwlock_destroy(it->second);
    if (!r) {
      delete it->second;
      rwlocks.erase(it);
    }
  }
  pthread_mutex_unlock(&registry_lock);
  return r;
}
static pthread_mutex_t *mutex_for(void *key) {
  pthread_mutex_lock(&registry_lock);
  auto &m = mutexes[key];
  if (!m) {
    m = new pthread_mutex_t;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    // Darwin recursive static initialization uses signature 0x32AAABA2.
    if (*(uint32_t *)key == 0x32aaaba2)
      pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
  }
  pthread_mutex_unlock(&registry_lock);
  return m;
}
static pthread_cond_t *condition_for(void *key) {
  pthread_mutex_lock(&registry_lock);
  auto &c = conditions[key];
  if (!c) {
    c = new pthread_cond_t;
    pthread_cond_init(c, nullptr);
  }
  pthread_mutex_unlock(&registry_lock);
  return c;
}
extern "C" int shim_mutex_init(void *p, const void *attr) {
  pthread_mutex_lock(&registry_lock);
  auto &mutex = mutexes[p];
  if (!mutex) {
    mutex = new pthread_mutex_t;
    pthread_mutexattr_t native_attr;
    pthread_mutexattr_init(&native_attr);
    if (attr)
      pthread_mutexattr_settype(&native_attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(mutex, &native_attr);
    pthread_mutexattr_destroy(&native_attr);
  }
  pthread_mutex_unlock(&registry_lock);
  return 0;
}
extern "C" int shim_mutex_lock(void *p) {
  return pthread_mutex_lock(mutex_for(p));
}
extern "C" int shim_mutex_unlock(void *p) {
  return pthread_mutex_unlock(mutex_for(p));
}
extern "C" int shim_mutex_trylock(void *p) {
  return pthread_mutex_trylock(mutex_for(p));
}
extern "C" int shim_mutex_destroy(void *p) {
  pthread_mutex_lock(&registry_lock);
  auto it = mutexes.find(p);
  int r = 0;
  if (it != mutexes.end()) {
    r = pthread_mutex_destroy(it->second);
    if (!r) {
      delete it->second;
      mutexes.erase(it);
    }
  }
  pthread_mutex_unlock(&registry_lock);
  return r;
}
extern "C" int shim_cond_init(void *p, const void *a) {
  if (a) {
    fputs("UNSUPPORTED cond attribute\n", stderr);
    _exit(86);
  }
  condition_for(p);
  return 0;
}
extern "C" int shim_cond_signal(void *p) {
  return pthread_cond_signal(condition_for(p));
}
extern "C" int shim_cond_broadcast(void *p) {
  return pthread_cond_broadcast(condition_for(p));
}
extern "C" int shim_cond_wait(void *p, void *m) {
  return pthread_cond_wait(condition_for(p), mutex_for(m));
}
extern "C" int shim_cond_timedwait(void *p, void *m, const timespec *t) {
  return pthread_cond_timedwait(condition_for(p), mutex_for(m), t);
}
extern "C" int shim_cond_relative(void *p, void *m, const timespec *t) {
  timespec abs;
  clock_gettime(CLOCK_REALTIME, &abs);
  abs.tv_sec += t->tv_sec;
  abs.tv_nsec += t->tv_nsec;
  if (abs.tv_nsec >= 1000000000) {
    abs.tv_nsec -= 1000000000;
    ++abs.tv_sec;
  }
  return shim_cond_timedwait(p, m, &abs);
}
extern "C" int shim_cond_destroy(void *p) {
  pthread_mutex_lock(&registry_lock);
  auto it = conditions.find(p);
  int r = 0;
  if (it != conditions.end()) {
    r = pthread_cond_destroy(it->second);
    if (!r) {
      delete it->second;
      conditions.erase(it);
    }
  }
  pthread_mutex_unlock(&registry_lock);
  return r;
}
extern "C" void shim_cpp_cv_wait(void *p, void *lock) {
  int r = shim_cond_wait(p, *(void **)lock);
  if (r)
    _exit(86);
}
extern "C" int shim_threadid(pthread_t t, uint64_t *out) {
  if (t && !pthread_equal(t, pthread_self()))
    return ENOTSUP;
  *out = syscall(SYS_gettid);
  return 0;
}
extern "C" int *shim_error() { return &errno; }
extern "C" int shim_toupper(int c) {
  return c >= -1 && c <= 255 ? toupper(c) : towupper(c);
}
extern "C" int shim_tolower(int c) {
  return c >= -1 && c <= 255 ? tolower(c) : towlower(c);
}
extern "C" void shim_pattern16(void *d, const void *p, size_t n) {
  unsigned char pattern[16];
  memcpy(pattern, p, 16);
  auto q = (unsigned char *)d;
  while (n) {
    size_t part = std::min(n, size_t(16));
    memcpy(q, pattern, part);
    q += part;
    n -= part;
  }
}
static int timezone_object;
static std::string timezone_name;
extern "C" void *shim_timezone_default() {
  const char *tz = getenv("TZ");
  timezone_name = (tz && *tz) ? tz : "Etc/UTC";
  return &timezone_object;
}
extern "C" void *shim_timezone_name(void *p) {
  if (p != &timezone_object)
    _exit(86);
  return &timezone_name;
}
extern "C" const char *shim_cf_string_ptr(void *p, uint32_t enc) {
  if (p != &timezone_name)
    _exit(86);
  if (enc != 0x08000100 && enc != 0x600)
    return nullptr;
  return timezone_name.c_str();
}
extern "C" bool shim_cf_string_copy(void *p, char *out, long cap,
                                    uint32_t enc) {
  auto s = shim_cf_string_ptr(p, enc);
  if (!s || cap <= (long)strlen(s))
    return false;
  strcpy(out, s);
  return true;
}
extern "C" long shim_cf_string_length(void *p) {
  if (p != &timezone_name)
    _exit(86);
  return timezone_name.size();
}
extern "C" long shim_cf_string_max(long n, uint32_t enc) {
  if (n < 0)
    return -1;
  if (enc == 0x08000100)
    return n <= INT64_MAX / 3 ? n * 3 : -1;
  if (enc == 0x600)
    return n;
  return -1;
}
extern "C" void shim_cf_release(void *p) {
  if (p != &timezone_object && p != &timezone_name) {
    fprintf(stderr, "UNSUPPORTED CFRelease object\n");
    _exit(86);
  }
}
extern "C" void *shim_cf_retain(void *p) {
  shim_cf_release(p);
  return p;
}
extern "C" double shim_cf_time() {
  timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  return t.tv_sec - 978307200.0 + t.tv_nsec / 1e9;
}
struct DarwinStat {
  int32_t dev;
  uint16_t mode, nlink;
  uint64_t ino;
  uint32_t uid, gid;
  int32_t rdev;
  uint32_t pad;
  timespec atime, mtime, ctime, birthtime;
  int64_t size, blocks;
  int32_t blksize;
  uint32_t flags, gen;
  int32_t spare;
  int64_t qspare[2];
};
static_assert(sizeof(DarwinStat) == 144);
static void convert_stat(const struct stat &s, DarwinStat *d) {
  memset(d, 0, sizeof(*d));
  d->dev = s.st_dev;
  d->mode = s.st_mode;
  d->nlink = s.st_nlink;
  d->ino = s.st_ino;
  d->uid = s.st_uid;
  d->gid = s.st_gid;
  d->rdev = s.st_rdev;
  d->atime = s.st_atim;
  d->mtime = s.st_mtim;
  d->ctime = s.st_ctim;
  d->size = s.st_size;
  d->blocks = s.st_blocks;
  d->blksize = s.st_blksize;
}
extern "C" int shim_fstat(int fd, DarwinStat *d) {
  struct stat s;
  int r = fstat(fd, &s);
  if (!r)
    convert_stat(s, d);
  return r;
}
extern "C" int shim_stat(const char *p, DarwinStat *d) {
  struct stat s;
  int r = stat(p, &s);
  if (!r)
    convert_stat(s, d);
  return r;
}
extern "C" int shim_lstat(const char *p, DarwinStat *d) {
  struct stat s;
  int r = lstat(p, &s);
  if (!r)
    convert_stat(s, d);
  return r;
}
struct DarwinStatFs {
  uint32_t block_size;
  int32_t io_size;
  uint64_t blocks, free_blocks, available_blocks, files, free_files;
  int32_t fsid[2];
  uint32_t owner, type, flags, subtype;
  char type_name[16];
  char mount_on[1024];
  char mount_from[1024];
  uint32_t extended_flags;
  uint32_t reserved[7];
};
static_assert(sizeof(DarwinStatFs) == 2168);
static void convert_statfs(const struct statvfs &source, DarwinStatFs *target) {
  memset(target, 0, sizeof(*target));
  target->block_size = source.f_bsize;
  target->io_size = source.f_frsize;
  target->blocks = source.f_blocks;
  target->free_blocks = source.f_bfree;
  target->available_blocks = source.f_bavail;
  target->files = source.f_files;
  target->free_files = source.f_ffree;
  target->flags = source.f_flag;
  memcpy(target->type_name, "linux", 6);
}
extern "C" int shim_fstatfs(int fd, DarwinStatFs *target) {
  struct statvfs source;
  int result = fstatvfs(fd, &source);
  if (!result)
    convert_statfs(source, target);
  return result;
}
extern "C" int shim_statfs(const char *path, DarwinStatFs *target) {
  struct statvfs source;
  int result = statvfs(path, &source);
  if (!result)
    convert_statfs(source, target);
  return result;
}
static int open_flags(int f) {
  int n = f & 3;
  const std::pair<int, int> m[] = {
      {4, O_NONBLOCK},       {8, O_APPEND},       {0x80, O_SYNC},
      {0x100, O_NOFOLLOW},   {0x200, O_CREAT},    {0x400, O_TRUNC},
      {0x800, O_EXCL},       {0x20000, O_NOCTTY}, {0x100000, O_DIRECTORY},
      {0x1000000, O_CLOEXEC}};
  int known = 3;
  for (auto [a, b] : m) {
    known |= a;
    if (f & a)
      n |= b;
  }
  if (f & ~known) {
    fprintf(stderr, "UNSUPPORTED open flags %x\n", f);
    errno = EINVAL;
    return -1;
  }
  return n;
}
extern "C" int shim_open(const char *p, int f, ...) {
  mode_t mode = 0;
  if (f & 0x200) {
    va_list a;
    va_start(a, f);
    mode = va_arg(a, int);
    va_end(a);
  }
  int flags = open_flags(f);
  if (flags < 0)
    return -1;
  return open(p, flags, mode);
}
struct DarwinDirent {
  uint64_t ino, seek;
  uint16_t reclen, namlen;
  uint8_t type;
  char name[1024];
};
extern "C" DarwinDirent *shim_readdir(DIR *d) {
  auto e = readdir(d);
  if (!e)
    return nullptr;
  static thread_local DarwinDirent out{};
  out.ino = e->d_ino;
  out.seek = e->d_off;
  out.namlen = strlen(e->d_name);
  out.type = e->d_type;
  memcpy(out.name, e->d_name, out.namlen + 1);
  out.reclen = (21 + out.namlen + 1 + 3) & ~3;
  return &out;
}
struct DarwinFlock {
  int64_t start, len;
  int32_t pid;
  int16_t type, whence;
};
extern "C" int shim_fcntl(int fd, int cmd, ...) {
  va_list ap;
  va_start(ap, cmd);
  long arg = 0;
  if (cmd != 1 && cmd != 3)
    arg = va_arg(ap, long);
  va_end(ap);
  if (cmd == 7 || cmd == 8 || cmd == 9) {
    auto d = (DarwinFlock *)arg;
    struct flock f{};
    f.l_start = d->start;
    f.l_len = d->len;
    f.l_pid = d->pid;
    f.l_type = d->type == 1 ? F_RDLCK : d->type == 2 ? F_UNLCK : F_WRLCK;
    f.l_whence = d->whence;
    int r = fcntl(fd, cmd == 7 ? F_GETLK : cmd == 8 ? F_SETLK : F_SETLKW, &f);
    if (!r && cmd == 7) {
      d->type = f.l_type == F_RDLCK ? 1 : f.l_type == F_UNLCK ? 2 : 3;
      d->pid = f.l_pid;
      d->start = f.l_start;
      d->len = f.l_len;
    }
    return r;
  }
  if (cmd == 50) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(path, (char *)arg, 1023);
    if (n < 0)
      return -1;
    ((char *)arg)[n] = 0;
    return 0;
  }
  if (cmd == 51)
    return fsync(fd);
  if (cmd == 67)
    return fcntl(fd, F_DUPFD_CLOEXEC, arg);
  if (cmd == 0 || cmd == 1 || cmd == 2)
    return fcntl(fd, cmd, arg);
  if (cmd == 4) {
    int flags = open_flags(arg);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags);
  }
  if (cmd == 3) {
    int f = fcntl(fd, F_GETFL);
    if (f < 0)
      return f;
    return (f & 3) | ((f & O_NONBLOCK) ? 4 : 0) | ((f & O_APPEND) ? 8 : 0);
  }
  fprintf(stderr, "UNSUPPORTED fcntl %d\n", cmd);
  errno = EINVAL;
  return -1;
}
extern "C" void *shim_mmap(void *p, size_t n, int prot, int flags, int fd,
                           off_t offset) {
  int native = flags & 0x13; // SHARED, PRIVATE, FIXED are equal.
  if (flags & 0x1000)
    native |= MAP_ANONYMOUS;
  if (flags & 0x40)
    native |= MAP_NORESERVE;
  if (flags & ~0x1053) {
    fprintf(stderr, "UNSUPPORTED mmap flags=%x\n", flags);
    errno = EINVAL;
    return MAP_FAILED;
  }
  return mmap(p, n, prot, native, fd, offset);
}
extern "C" void *shim_dlsym(void *h, const char *n) {
  if (h == (void *)-1 || h == (void *)-2 || h == (void *)-3 || h == (void *)-5)
    h = RTLD_DEFAULT;
  if (!strcmp(n, "__cxa_throw")) {
    static void *abi = dlopen("libc++abi.so.1", RTLD_NOW);
    return dlsym(abi, n);
  }
  return dlsym(h, n);
}
extern "C" void shim_bzero(void *p, size_t n) { memset(p, 0, n); }
extern "C" int shim_atexit(void (*function)()) { return atexit(function); }
static void *sec_random_default = nullptr;
extern "C" int shim_sec_random_copy_bytes(void *, size_t size,
                                           unsigned char *output) {
  size_t offset = 0;
  while (offset < size) {
    ssize_t count = getrandom(output + offset, size - offset, 0);
    if (count < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    offset += static_cast<size_t>(count);
  }
  return 0;
}
extern "C" void *shim_malloc_zone_malloc(void *, size_t size) {
  return malloc(size);
}
extern "C" void *shim_malloc_zone_calloc(void *, size_t count, size_t size) {
  return calloc(count, size);
}
extern "C" void *shim_malloc_zone_valloc(void *, size_t size) {
  void *result = nullptr;
  return posix_memalign(&result, 4096, size) ? nullptr : result;
}
extern "C" void shim_malloc_zone_free(void *, void *pointer) { free(pointer); }
extern "C" void *shim_malloc_zone_realloc(void *, void *pointer, size_t size) {
  return realloc(pointer, size);
}
extern "C" size_t shim_malloc_size(const void *pointer) {
  return pointer ? malloc_usable_size(const_cast<void *>(pointer)) : 0;
}
extern "C" size_t shim_malloc_zone_size(void *, const void *pointer) {
  return shim_malloc_size(pointer);
}
extern "C" void shim_malloc_zone_destroy(void *) {}
struct DarwinMallocZone {
  void *reserved1;
  void *reserved2;
  size_t (*size)(void *, const void *);
  void *(*malloc_fn)(void *, size_t);
  void *(*calloc_fn)(void *, size_t, size_t);
  void *(*valloc_fn)(void *, size_t);
  void (*free_fn)(void *, void *);
  void *(*realloc_fn)(void *, void *, size_t);
  void (*destroy_fn)(void *);
  const char *zone_name;
};
static DarwinMallocZone default_malloc_zone{
    nullptr,
    nullptr,
    shim_malloc_zone_size,
    shim_malloc_zone_malloc,
    shim_malloc_zone_calloc,
    shim_malloc_zone_valloc,
    shim_malloc_zone_free,
    shim_malloc_zone_realloc,
    shim_malloc_zone_destroy,
    "fcitx5-wetypex"};
extern "C" void *shim_malloc_default_zone() { return &default_malloc_zone; }
extern "C" void *shim_malloc_create_zone(size_t, unsigned) {
  return &default_malloc_zone;
}
extern "C" void shim_malloc_set_zone_name(void *, const char *) {}
extern "C" void *shim_reallocf(void *pointer, size_t size) {
  void *result = realloc(pointer, size);
  if (!result && size)
    free(pointer);
  return result;
}
static int sysctl_copy(const void *value, size_t size, void *old_value,
                       size_t *old_size) {
  if (!old_size) {
    errno = EINVAL;
    return -1;
  }
  if (!old_value) {
    *old_size = size;
    return 0;
  }
  if (*old_size < size) {
    *old_size = size;
    errno = ENOMEM;
    return -1;
  }
  memcpy(old_value, value, size);
  *old_size = size;
  return 0;
}
extern "C" int shim_sysctlbyname(const char *name, void *old_value,
                                  size_t *old_size, const void *, size_t) {
  if (!name) {
    errno = EINVAL;
    return -1;
  }
  if (!strcmp(name, "hw.ncpu") || !strcmp(name, "hw.logicalcpu") ||
      !strcmp(name, "hw.logicalcpu_max") ||
      !strcmp(name, "hw.physicalcpu") || !strcmp(name, "hw.physicalcpu_max")) {
    int count = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
    if (count < 1)
      count = 1;
    return sysctl_copy(&count, sizeof(count), old_value, old_size);
  }
  if (!strcmp(name, "hw.memsize")) {
    uint64_t memory = static_cast<uint64_t>(sysconf(_SC_PHYS_PAGES)) *
                      static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
    return sysctl_copy(&memory, sizeof(memory), old_value, old_size);
  }
  const char *text = nullptr;
  if (!strcmp(name, "hw.model")) {
    text = getenv("WETYPE_DEVICE_MODEL");
    if (!text || !*text)
      text = "LINUX";
  } else if (!strcmp(name, "hw.machine")) {
    text = "x86_64";
  } else if (!strcmp(name, "kern.osrelease")) {
    text = "24.2.0";
  } else if (!strcmp(name, "kern.osproductversion")) {
    text = "15.2";
  } else if (!strcmp(name, "machdep.cpu.brand_string")) {
    text = "Linux x86_64";
  }
  if (text)
    return sysctl_copy(text, strlen(text) + 1, old_value, old_size);
  fprintf(stderr, "UNSUPPORTED sysctlbyname %s\n", name);
  errno = ENOENT;
  return -1;
}
extern "C" __attribute__((used, noinline)) void *shim_tlv_impl(void *d) {
  struct Desc {
    void *p;
    uintptr_t key, off;
  };
  auto desc = (Desc *)d;
  static thread_local void *block = nullptr;
  if (!block) {
    block = calloc(1, tls_total);
    memcpy(block, (void *)tls_data, tls_size);
  }
  if (desc->off >= tls_total) {
    fputs("BAD TLV offset\n", stderr);
    _exit(86);
  }
  return (char *)block + desc->off;
}
extern "C" void shim_tlv();
extern "C" void shim_chkstk();
asm(".text\n.global shim_chkstk\nshim_chkstk:\npush %rcx\npush %rax\nlea "
    "24(%rsp),%rcx\n"
    "cmp $4096,%rax\njb 2f\n1: sub $4096,%rcx\ntestb $0,(%rcx)\nsub "
    "$4096,%rax\ncmp $4096,%rax\njae 1b\n"
    "2: sub %rax,%rcx\ntestb $0,(%rcx)\npop %rax\npop %rcx\nret\n");
asm(".text\n.global shim_tlv\nshim_tlv:\n"
    "push %rdi\npush %rsi\npush %rdx\npush %rcx\npush %r8\npush %r9\npush "
    "%r10\npush %r11\n"
    "sub $264,%rsp\nmovdqu %xmm0,0(%rsp)\nmovdqu %xmm1,16(%rsp)\nmovdqu "
    "%xmm2,32(%rsp)\nmovdqu %xmm3,48(%rsp)\n"
    "movdqu %xmm4,64(%rsp)\nmovdqu %xmm5,80(%rsp)\nmovdqu "
    "%xmm6,96(%rsp)\nmovdqu %xmm7,112(%rsp)\n"
    "movdqu %xmm8,128(%rsp)\nmovdqu %xmm9,144(%rsp)\nmovdqu "
    "%xmm10,160(%rsp)\nmovdqu %xmm11,176(%rsp)\n"
    "movdqu %xmm12,192(%rsp)\nmovdqu %xmm13,208(%rsp)\nmovdqu "
    "%xmm14,224(%rsp)\nmovdqu %xmm15,240(%rsp)\ncall shim_tlv_impl\n"
    "movdqu 0(%rsp),%xmm0\nmovdqu 16(%rsp),%xmm1\nmovdqu "
    "32(%rsp),%xmm2\nmovdqu 48(%rsp),%xmm3\n"
    "movdqu 64(%rsp),%xmm4\nmovdqu 80(%rsp),%xmm5\nmovdqu "
    "96(%rsp),%xmm6\nmovdqu 112(%rsp),%xmm7\n"
    "movdqu 128(%rsp),%xmm8\nmovdqu 144(%rsp),%xmm9\nmovdqu "
    "160(%rsp),%xmm10\nmovdqu 176(%rsp),%xmm11\n"
    "movdqu 192(%rsp),%xmm12\nmovdqu 208(%rsp),%xmm13\nmovdqu "
    "224(%rsp),%xmm14\nmovdqu 240(%rsp),%xmm15\n"
    "add $264,%rsp\npop %r11\npop %r10\npop %r9\npop %r8\npop %rcx\npop "
    "%rdx\npop %rsi\npop %rdi\nret\n");
extern "C" int __cxa_thread_atexit_impl(void (*)(void *), void *, void *);
extern "C" int shim_tlv_atexit(void (*f)(void *), void *p) {
  return __cxa_thread_atexit_impl(f, p, (void *)&stack_guard);
}
static void show_address(uintptr_t pc) {
  auto i =
      std::upper_bound(address_names.begin(), address_names.end(), pc,
                       [](uintptr_t v, const auto &r) { return v < r.first; });
  if (i != address_names.begin()) {
    --i;
    fprintf(stderr, "0x%lx %s +0x%lx\n", pc, i->second.c_str(), pc - i->first);
  } else
    fprintf(stderr, "0x%lx\n", pc);
}
static void crash(int sig, siginfo_t *info, void *ctx) {
  auto uc = (ucontext_t *)ctx;
  fprintf(stderr, "FAULT signal=%d address=%p rip=", sig, info->si_addr);
  show_address(uc->uc_mcontext.gregs[REG_RIP]);
  fprintf(stderr, "REG rdi=%llx rsi=%llx rdx=%llx rax=%llx rbp=%llx rsp=%llx\n",
          (long long)uc->uc_mcontext.gregs[REG_RDI],
          (long long)uc->uc_mcontext.gregs[REG_RSI],
          (long long)uc->uc_mcontext.gregs[REG_RDX],
          (long long)uc->uc_mcontext.gregs[REG_RAX],
          (long long)uc->uc_mcontext.gregs[REG_RBP],
          (long long)uc->uc_mcontext.gregs[REG_RSP]);
  auto sp = (uintptr_t *)uc->uc_mcontext.gregs[REG_RSP];
  for (int i = 0; i < 12; ++i) {
    if (sp[i] >= 0x100000000 && sp[i] < 0x104000000)
      show_address(sp[i]);
  }
  _exit(128 + sig);
}
extern "C" void missing_symbol(const char *n) {
  fprintf(stderr, "UNIMPLEMENTED %s\n", n);
  _exit(85);
}
static unsigned char *trap_arena = nullptr;
static size_t trap_offset = 0;
static void *trap(const std::string &n) {
  if (!trap_arena) {
    trap_arena = (unsigned char *)mmap(nullptr, 1048576, PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (trap_arena == MAP_FAILED)
      _exit(92);
  }
  if (trap_offset + 32 > 1048576)
    _exit(92);
  auto code = trap_arena + trap_offset;
  trap_offset += 32;
  char *name = strdup(n.c_str());
  code[0] = 0x48;
  code[1] = 0xbf;
  memcpy(code + 2, &name, 8);
  void *fn = (void *)&missing_symbol;
  code[10] = 0x48;
  code[11] = 0xb8;
  memcpy(code + 12, &fn, 8);
  code[20] = 0xff;
  code[21] = 0xe0;
  return code;
}
static void *resolve(const std::string &name) {
  static std::map<std::string, void *> overrides = {
      {"___stack_chk_guard", &stack_guard},
      {"___error", (void *)shim_error},
      {"___bzero", (void *)shim_bzero},
      {"_atexit", (void *)shim_atexit},
      {"_SecRandomCopyBytes", (void *)shim_sec_random_copy_bytes},
      {"_kSecRandomDefault", &sec_random_default},
      {"___stderrp", &stderr},
      {"___stdoutp", &stdout},
      {"___stdinp", &stdin},
      {"__tlv_bootstrap", (void *)shim_tlv},
      {"__tlv_atexit", (void *)shim_tlv_atexit},
      {"____chkstk_darwin", (void *)shim_chkstk},
      {"_dlsym", (void *)shim_dlsym},
      {"_malloc_create_zone", (void *)shim_malloc_create_zone},
      {"_malloc_default_zone", (void *)shim_malloc_default_zone},
      {"_malloc_set_zone_name", (void *)shim_malloc_set_zone_name},
      {"_malloc_size", (void *)shim_malloc_size},
      {"_malloc_zone_free", (void *)shim_malloc_zone_free},
      {"_malloc_zone_malloc", (void *)shim_malloc_zone_malloc},
      {"_malloc_zone_realloc", (void *)shim_malloc_zone_realloc},
      {"_reallocf", (void *)shim_reallocf},
      {"_sysctlbyname", (void *)shim_sysctlbyname},
      {"_mmap", (void *)shim_mmap},
      {"_fstat$INODE64", (void *)shim_fstat},
      {"_fstatfs$INODE64", (void *)shim_fstatfs},
      {"_stat$INODE64", (void *)shim_stat},
      {"_statfs$INODE64", (void *)shim_statfs},
      {"_lstat$INODE64", (void *)shim_lstat},
      {"_open", (void *)shim_open},
      {"_opendir$INODE64", (void *)opendir},
      {"_readdir$INODE64", (void *)shim_readdir},
      {"_fcntl", (void *)shim_fcntl},
      {"_pthread_rwlock_init", (void *)shim_rw_init},
      {"_pthread_rwlock_rdlock", (void *)shim_rw_rdlock},
      {"_pthread_rwlock_wrlock", (void *)shim_rw_wrlock},
      {"_pthread_rwlock_tryrdlock", (void *)shim_rw_tryrdlock},
      {"_pthread_rwlock_trywrlock", (void *)shim_rw_trywrlock},
      {"_pthread_rwlock_unlock", (void *)shim_rw_unlock},
      {"_pthread_rwlock_destroy", (void *)shim_rw_destroy},
      {"_CFTimeZoneCopyDefault", (void *)shim_timezone_default},
      {"_CFTimeZoneGetName", (void *)shim_timezone_name},
      {"_CFStringGetCStringPtr", (void *)shim_cf_string_ptr},
      {"_CFStringGetCString", (void *)shim_cf_string_copy},
      {"_CFStringGetLength", (void *)shim_cf_string_length},
      {"_CFRelease", (void *)shim_cf_release},
      {"_CFRetain", (void *)shim_cf_retain},
      {"_CFAbsoluteTimeGetCurrent", (void *)shim_cf_time},
      {"_CFStringGetMaximumSizeForEncoding", (void *)shim_cf_string_max},
      {"___toupper", (void *)shim_toupper},
      {"___tolower", (void *)shim_tolower},
      {"__DefaultRuneLocale", default_rune_locale},
      {"___maskrune", (void *)shim_maskrune},
      {"_memset_pattern16", (void *)shim_pattern16},
      {"_pthread_mutex_init", (void *)shim_mutex_init},
      {"_pthread_mutex_lock", (void *)shim_mutex_lock},
      {"_pthread_mutex_unlock", (void *)shim_mutex_unlock},
      {"_pthread_mutex_trylock", (void *)shim_mutex_trylock},
      {"_pthread_mutex_destroy", (void *)shim_mutex_destroy},
      {"_pthread_cond_init", (void *)shim_cond_init},
      {"_pthread_cond_signal", (void *)shim_cond_signal},
      {"_pthread_cond_broadcast", (void *)shim_cond_broadcast},
      {"_pthread_cond_wait", (void *)shim_cond_wait},
      {"_pthread_cond_timedwait", (void *)shim_cond_timedwait},
      {"_pthread_cond_timedwait_relative_np", (void *)shim_cond_relative},
      {"_pthread_cond_destroy", (void *)shim_cond_destroy},
      {"_pthread_threadid_np", (void *)shim_threadid},
      {"__ZNSt3__15mutex4lockEv", (void *)shim_mutex_lock},
      {"__ZNSt3__15mutex6unlockEv", (void *)shim_mutex_unlock},
      {"__ZNSt3__15mutexD1Ev", (void *)shim_mutex_destroy},
      {"__ZNSt3__118condition_variable4waitERNS_11unique_lockINS_5mutexEEE",
       (void *)shim_cpp_cv_wait},
      {"__ZNSt3__118condition_variable10notify_oneEv",
       (void *)shim_cond_signal},
      {"__ZNSt3__118condition_variable10notify_allEv",
       (void *)shim_cond_broadcast},
      {"__ZNSt3__118condition_variableD1Ev", (void *)shim_cond_destroy}};
  auto it = overrides.find(name);
  if (it != overrides.end())
    return it->second;
  if (name == "__ZNKSt3__16locale9use_facetERNS0_2idE")
    return dlsym(RTLD_DEFAULT, "wetype_locale_use_facet");
  if (name == "__ZNSt3__115__get_classnameEPKcb")
    return dlsym(RTLD_DEFAULT, "wetype_get_classname");
  // Never attempt to use macOS framework objects as native ELF objects.
  if (name.rfind("_OBJC_", 0) == 0 || name.rfind("_$s", 0) == 0)
    return nullptr;
  return dlsym(RTLD_DEFAULT, name.c_str() + 1);
}
static void exception_callback(const char *s, unsigned len) {
  fprintf(stderr, "ENGINE_EXCEPTION %.*s\n", (int)len, s);
}
static void engine_log(int level, const char *s) {
  if (!service_mode)
    fprintf(stderr, "IME[%d] %s\n", level, s ? s : "(null)");
}
#include "service.inc"
#include "business_services.inc"
struct LlmResultItem {
  const unsigned char *data;
  uint32_t data_length, pad0;
  const char *text;
  uint32_t text_length;
  int32_t type;
  uint32_t reserved;
  bool flag;
  unsigned char pad2[3];
};
static_assert(sizeof(LlmResultItem) == 40);
struct LlmEvent {
  int32_t error_code, chat_id, frame;
  bool is_end;
  unsigned char pad0[3];
  LlmResultItem *results;
  uint32_t result_count, pad1;
  const unsigned char *cookie;
  uint32_t cookie_length, pad2;
  const char *additional;
  uint32_t additional_length, pad3;
};
static_assert(sizeof(LlmEvent) == 64);
static std::mutex llm_service_mutex;
static std::vector<unsigned char> llm_service_cookie;
static unsigned llm_service_events = 0;
static std::atomic<bool> llm_service_finished{false};
static void llm_service_callback(LlmEvent event) {
  fprintf(stdout,
          "LLM_EVENT chat=%d frame=%d error=%d end=%d results=%u\n",
          event.chat_id, event.frame, event.error_code, event.is_end,
          event.result_count);
  fprintf(stdout, "LLM_COOKIE bytes=%u\n", event.cookie_length);
  if (event.result_count > 64 || (event.result_count && !event.results))
    _exit(89);
  for (unsigned i = 0; i < event.result_count; ++i) {
    auto &item = event.results[i];
    if (item.data_length > 1048576 || (!item.data && item.data_length) ||
        item.text_length > 1048576 || (!item.text && item.text_length))
      _exit(89);
    fprintf(stdout, "LLM_DATA index=%u bytes=%u text=%.*s\n", i,
            item.data_length, int(item.data_length),
            item.data ? reinterpret_cast<const char *>(item.data) : "");
    fprintf(stdout, "LLM_RESULT index=%u type=%d text=%.*s\n", i, item.type,
            int(item.text_length), item.text ? item.text : "");
    std::string metadata(item.text ? item.text : "", item.text_length);
    if (metadata.find("\"is_answer_end\":1") != std::string::npos ||
        metadata.find("\"is_answer_end\":true") != std::string::npos)
      llm_service_finished = true;
  }
  {
    std::lock_guard lock(llm_service_mutex);
    llm_service_cookie.assign(event.cookie,
                            event.cookie ? event.cookie + event.cookie_length
                                         : event.cookie);
    ++llm_service_events;
  }
  fflush(stdout);
}
struct LlmSearchParam {
  const char *text;
  uint32_t text_length, pad0;
  const unsigned char *cookie;
  uint32_t cookie_length;
  int32_t accept_text_type;
  uint64_t wechat_ability;
  int32_t return_key_type, pad1;
  const char *app_name;
  uint32_t app_name_length;
  bool warm_up;
  unsigned char pad2[3];
};
static_assert(sizeof(LlmSearchParam) == 64);
static void run_llm_service(const char *question) {
  const char *app = "LINUX";
  using Search = uint32_t (*)(decltype(&llm_service_callback), LlmSearchParam);
  auto search = (Search)syms.at("_wxime_cloud_llm_search_without_session");
  auto invoke = [&](const char *text, const std::vector<unsigned char> &cookie) {
    LlmSearchParam parameter{text,
                             uint32_t(strlen(text)),
                             0,
                             cookie.empty() ? nullptr : cookie.data(),
                             uint32_t(cookie.size()),
                             1,
                             0,
                             0,
                             0,
                             app,
                             uint32_t(strlen(app)),
                             false,
                             {}};
    return search(llm_service_callback, parameter);
  };
  uint32_t request = invoke(question, {});
  fprintf(stdout, "LLM_REQUEST id=%u\n", request);
  fflush(stdout);
  unsigned handled = 0;
  for (unsigned tick = 0; tick < 600 && !llm_service_finished; ++tick) {
    std::vector<unsigned char> cookie;
    {
      std::lock_guard lock(llm_service_mutex);
      if (llm_service_events > handled) {
        handled = llm_service_events;
        cookie = llm_service_cookie;
      }
    }
    if (!cookie.empty()) {
      request = invoke("", cookie);
      fprintf(stdout, "LLM_CONTINUE id=%u frame=%u\n", request, handled);
      fflush(stdout);
    }
    usleep(50000);
  }
}
static std::vector<unsigned char> ehframe;
static void emit32(std::vector<unsigned char> &b, uint32_t n) {
  for (int i = 0; i < 4; i++)
    b.push_back(n >> (8 * i));
}
static void emit64(std::vector<unsigned char> &b, uint64_t n) {
  for (int i = 0; i < 8; i++)
    b.push_back(n >> (8 * i));
}
static void uleb(std::vector<unsigned char> &b, uint64_t n) {
  do {
    unsigned char c = n & 127;
    n >>= 7;
    b.push_back(c | (n ? 128 : 0));
  } while (n);
}
static void record(std::vector<unsigned char> &b) {
  while (b.size() % 8 != 4)
    b.push_back(0);
  emit32(ehframe, b.size());
  ehframe.insert(ehframe.end(), b.begin(), b.end());
}
static void setup_unwind(const std::vector<std::string> &entries) {
  std::vector<unsigned char> cie;
  emit32(cie, 0);
  for (auto c : {1, 'z' + 0, 'P' + 0, 'L' + 0, 'R' + 0, 0, 1, 0x78, 16, 11, 0})
    cie.push_back(c);
  emit64(cie, (uintptr_t)dlsym(RTLD_DEFAULT, "__gxx_personality_v0"));
  cie.insert(cie.end(), {0, 0, 0x0c, 7, 8, 0x90, 1});
  record(cie);
  const unsigned regs[] = {0, 3, 12, 13, 14, 15, 6};
  for (auto &line : entries) {
    std::istringstream s(line);
    std::string tag;
    uintptr_t addr, len, enc, lsda;
    s >> tag >> std::hex >> addr >> len >> enc >> lsda;
    std::vector<unsigned char> b;
    emit32(b, ehframe.size() + 4);
    emit64(b, addr);
    emit64(b, len);
    b.push_back(8);
    emit64(b, lsda);
    b.insert(b.end(), {0x0c, 6, 16, 0x86, 2});
    unsigned offset = (enc >> 16) & 255;
    for (unsigned i = 0; i < 5; i++) {
      unsigned reg = (enc >> (3 * i)) & 7;
      if (reg && reg <= 6) {
        b.push_back(0x80 | regs[reg]);
        uleb(b, offset + 2 - i);
      }
    }
    record(b);
  }
  emit32(ehframe, 0);
  auto fn = (void (*)(void *))dlsym(RTLD_DEFAULT, "__register_frame");
  if (!fn) {
    fputs("missing ELF unwind registration\n", stderr);
    _exit(86);
  }
  fn(ehframe.data());
  fprintf(stderr, "UNWIND registered %zu RBP frame entries\n", entries.size());
}
int main(int argc, char **argv) {
  if (argc != 3 && argc != 4) {
    fprintf(stderr, "usage: host PREPARED_DIR MODE [ASCII_PINYIN]\n");
    return 2;
  }
  service_mode = !strcmp(argv[2], "serve");
  if (!service_mode)
    alarm(!strcmp(argv[2], "llm-service") ? 45 : 15);
  initialize_rune_locale();
  setvbuf(stderr, nullptr, _IONBF, 0);
  struct sigaction sa{};
  sa.sa_sigaction = crash;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);
  sigaction(SIGILL, &sa, nullptr);
  sigaction(SIGABRT, &sa, nullptr);
  if (!dlopen("libc++.so.1", RTLD_NOW | RTLD_GLOBAL) ||
      !dlopen("libc++abi.so.1", RTLD_NOW | RTLD_GLOBAL)) {
    fputs(dlerror(), stderr);
    return 2;
  }
  const char *support = getenv("WETYPE_SUPPORT_DIR");
  std::string support_dir = support ? support : argv[1];
  if (!dlopen((support_dir + "/locale.so").c_str(), RTLD_NOW | RTLD_GLOBAL)) {
    fputs(dlerror(), stderr);
    return 2;
  }
  if (!dlopen((support_dir + "/libkqueue.so").c_str(),
              RTLD_NOW | RTLD_GLOBAL)) {
    fputs(dlerror(), stderr);
    return 2;
  }
  if (!dlopen((support_dir + "/wcwss_bridge.so").c_str(),
              RTLD_NOW | RTLD_GLOBAL)) {
    fputs(dlerror(), stderr);
    return 2;
  }
  std::string dir = argv[1], mode = argv[2], line;
  std::ifstream symbols(dir + "/symbols.txt");
  while (std::getline(symbols, line)) {
    std::istringstream s(line);
    uintptr_t a;
    std::string n;
    s >> std::hex >> a >> n;
    if (!n.empty())
      address_names.emplace_back(a, n);
  }
  std::ifstream file(dir + "/manifest.txt");
  std::vector<std::string> binds, unwinds;
  std::vector<std::pair<uintptr_t, std::string>> ctors;
  int fd = open((dir + "/image.macho").c_str(), O_RDONLY);
  if (fd < 0)
    return 2;
  while (std::getline(file, line)) {
    std::istringstream s(line);
    std::string type;
    s >> type;
    if (type == "SEG") {
      uintptr_t addr, size, off, len;
      int prot;
      std::string n;
      s >> std::hex >> addr >> size >> off >> len >> std::dec >> prot >> n;
      size = (size + 4095) & ~4095ULL;
      if (n == "__LINKEDIT")
        continue; // Symbol/relocation metadata is already prepared.
      void *m = mmap((void *)addr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
      if (m == MAP_FAILED) {
        perror("map segment");
        return 2;
      }
      if (len) {
        if (len % 4096 || off % 4096) {
          fputs("unsupported segment alignment\n", stderr);
          return 2;
        }
        int protection =
            (prot & 4) ? PROT_READ | PROT_EXEC : PROT_READ | PROT_WRITE;
        if (mmap(m, len, protection, MAP_PRIVATE | MAP_FIXED, fd, off) ==
            MAP_FAILED) {
          perror("map file segment");
          return 2;
        }
      }
    } else if (type == "BIND")
      binds.push_back(line);
    else if (type == "OWN") {
      uintptr_t a, v;
      long add;
      s >> std::hex >> a >> v >> std::dec >> add;
      *(uintptr_t *)a = v + add;
    } else if (type == "UNWIND")
      unwinds.push_back(line);
    else if (type == "SYM") {
      uintptr_t a;
      std::string n;
      s >> std::hex >> a >> n;
      syms[n] = a;
    } else if (type == "CTOR") {
      uintptr_t a;
      std::string n;
      s >> std::hex >> a >> n;
      ctors.emplace_back(a, n);
    } else if (type == "SECTION") {
      uintptr_t a, size;
      std::string n;
      s >> std::hex >> a >> size >> n;
      if (n == "__thread_data") {
        tls_data = a;
        tls_size = size;
      }
      if (n == "__thread_bss")
        tls_total = a + size;
    }
  }
  close(fd);
  tls_total -= tls_data;
  std::map<std::string, void *> cache;
  unsigned unresolved = 0;
  for (auto &ln : binds) {
    std::istringstream s(ln);
    std::string tag, n;
    uintptr_t a;
    long add;
    s >> tag >> std::hex >> a >> std::dec >> add >> n;
    void *p;
    auto it = cache.find(n);
    if (it != cache.end())
      p = it->second;
    else {
      p = resolve(n);
      if (!p) {
        ++unresolved;
        p = trap(n);
      }
      cache[n] = p;
    }
    *(uintptr_t *)a = (uintptr_t)p + add;
  }
  if (trap_arena && mprotect(trap_arena, 1048576, PROT_READ | PROT_EXEC))
    return 2;
  fprintf(stderr, "MAPPED imports=%zu unresolved=%u tls=%lu\n", cache.size(),
          unresolved, tls_total);
  fprintf(stderr, "VERSION %s\n",
          ((const char *(*)())syms.at("_wxime_get_version"))());
  if (mode == "version")
    return 0;
  setup_unwind(unwinds);
  for (auto &[a, n] : address_names)
    if (n == "__ZN5wxime5utils26SetReportExceptionCallBackEPFvPKcjE")
      ((void (*)(void (*)(const char *, unsigned)))a)(exception_callback);
  // Initialize the original C/C++ core region. AppKit/Swift UI constructors
  // remain outside this compatibility boundary.
  const bool verbose_host = getenv("WETYPE_HOST_DEBUG");
  for (auto &[addr, name] : ctors) {
    if (addr < 0x1005f51c0 || addr >= 0x101b00000)
      continue;
    if (verbose_host)
      fprintf(stderr, "CTOR %lx %s\n", addr, name.c_str());
    ((void (*)())addr)();
  }
  fprintf(stderr, "CORE_CONSTRUCTORS_RETURNED\n");
  if (mode == "business-service")
    business_service();
  // Version-pinned wxime initialization ABI.
  alignas(16) unsigned char config[1024]{};
  const char *work = getenv("WETYPE_WORK_DIR");
  if (!work || !*work)
    work = "/work/user";
  memcpy(config + 0x18, &work, 8);
  void *logger = (void *)engine_log;
  memcpy(config, &logger, 8);
  config[8] = 1;
  auto setString = [&](size_t offset, const char *value) {
    memcpy(config + offset, &value, sizeof(value));
    *(uint32_t *)(config + offset + 8) = strlen(value);
  };
  // Recovered from Android 3.5.4's InitInfo -> wxime_init_config bridge and
  // cross-checked against this Mac build's shared C ABI.
  // Account identity belongs to wxime_network_login.  The desktop init ABI
  // leaves this field empty; putting an account UIN here makes the core enter
  // its device-code generation path before network-login information exists.
  setString(0x58, "2.2.3(657)");
  // The upstream protocol has no Linux platform enum, so use the original Mac
  // platform value while presenting an honest Linux device name.
  const char *device_name = getenv("WETYPE_DEVICE_MODEL");
  setString(0x68, device_name && *device_name ? device_name : "LINUX");
  setString(0x78, "15.2.0");
  *(uint32_t *)(config + 0x50) = 5; // observed original desktop platform config
  *(uint32_t *)(config + 0xc0) = 3; // observed package config
  // +0x10 is JSON logging configuration, NOT the dictionary resource path.
  // Original dictionary loading is a separate wxime_config_dict operation.
  void *callback = (void *)exception_callback;
  memcpy(config + 0x88, &callback, 8);
  fprintf(stderr, "CALL wxime_initialize config=%p\n", config);
  try {
    ((void (*)(void *))syms.at("_wxime_initialize"))(config);
  } catch (const std::exception &e) {
    fprintf(stderr, "INITIALIZE_EXCEPTION %s\n", e.what());
    _exit(87);
  }
  fprintf(stderr, "WXIME_INITIALIZE_RETURNED (not candidate proof)\n");
  if (mode == "llm-service")
    network_login_service(false);
  if (mode == "llm-service") {
    run_llm_service(argc == 4 ? argv[3] : "用一句话介绍Linux");
    _exit(0);
  }
  if (mode == "serve" && getenv("WETYPE_NETWORK_LIVE"))
    network_login_service(false, false, false);
  struct Dict {
    void *asset;
    const char *path;
    uint32_t id, version;
  };
  struct DictConfig {
    Dict *dicts;
    uint32_t count;
    uint32_t padding;
    const char *user_path;
  };
  static_assert(sizeof(Dict) == 24 && sizeof(DictConfig) == 24);
  std::vector<std::string> paths;
  paths.reserve(100);
  std::vector<Dict> dicts;
  std::ifstream dictfile(dir + "/dicts.txt");
  while (std::getline(dictfile, line)) {
    std::istringstream s(line);
    uint32_t id, version;
    std::string name;
    s >> id >> version >> name;
    if (name.empty())
      continue;
    paths.push_back("/input/resources/" + name);
    dicts.push_back({nullptr, paths.back().c_str(), id, version});
  }
  DictConfig dc{dicts.data(), (uint32_t)dicts.size(), 0, "/work/userDict"};
  fprintf(stderr, "CALL wxime_config_dict count=%u\n", dc.count);
  bool loaded =
      ((bool (*)(const DictConfig *))syms.at("_wxime_config_dict"))(&dc);
  fprintf(stderr, "WXIME_CONFIG_DICT_RETURNED %d\n", loaded);
  if (!loaded)
    _exit(88);
  if (mode == "validate")
    _exit(0);
  if (mode == "serve" && getenv("WETYPE_NETWORK_LIVE")) {
    const auto groupId =
        strtoull(getenv("WETYPE_GROUP_ID") ? getenv("WETYPE_GROUP_ID") : "0",
                 nullptr, 10);
    const auto functions = strtoull(getenv("WETYPE_GROUP_FUNCTIONS")
                                         ? getenv("WETYPE_GROUP_FUNCTIONS")
                                         : "0",
                                     nullptr, 10);
    const auto phraseVersion = strtoull(getenv("WETYPE_HOTWORD_VERSION")
                                            ? getenv("WETYPE_HOTWORD_VERSION")
                                            : "0",
                                        nullptr, 10);
    if (groupId) {
      // Recovered from Windows 2.1.3.18's only call site and the macOS
      // 2.2.3.657 implementation/log labels: unknown, debug, group id,
      // function mask, common-phrase version, personal-dictionary version.
      struct GroupSyncInfo {
        bool unknown;
        bool debug;
        unsigned char padding[6];
        uint64_t groupId;
        uint64_t functions;
        uint64_t phraseVersion;
        uint64_t dictionaryVersion;
      } info{false, false, {}, groupId, functions, phraseVersion, 0};
      static_assert(sizeof(GroupSyncInfo) == 40);
      ((void (*)(GroupSyncInfo))syms.at("_wxime_group_sync_info_changed"))(
          info);
      fprintf(stderr, "GROUP_SYNC_CONFIGURED group=%llu functions=%llu\n",
              groupId, functions);
    }
  }
  if (mode == "serve") {
    service_loop();
    _exit(0);
  }
  fprintf(stderr, "Unsupported engine-host mode: %s\n", mode.c_str());
  return 64;
}
