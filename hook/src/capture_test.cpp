#undef NDEBUG
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <array>
#include <sys/mman.h>
#include <sys/uio.h>
#include "safe_memory.h"
#include "navigation_cache.h"
static int reads = 0;
static bool raw(void *to, uintptr_t from, size_t size) {
    ++reads; iovec l{to,size}, r{reinterpret_cast<void *>(from),size};
    return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == ssize_t(size);
}
int main() {
    auto memory=static_cast<unsigned char *>(mmap(nullptr,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    assert(memory != MAP_FAILED); memory[0]=7;memory[4095]=8;memory[4096]=9;
    SafeMemoryReader reader(raw);unsigned char value=0;reads=0;reader.begin();
    assert(reader.read(&value,reinterpret_cast<uintptr_t>(memory),1) && value==7);
    assert(reader.read(&value,reinterpret_cast<uintptr_t>(memory),1) && reads==1);
    std::array<unsigned char,2> boundary{};
    assert(reader.read(boundary.data(),reinterpret_cast<uintptr_t>(memory+4095),2));
    assert(boundary[0]==8 && boundary[1]==9 && reads==2);
    reader.end();memory[0]=42;reader.begin();
    assert(reader.read(&value,reinterpret_cast<uintptr_t>(memory),1) && value==42);
    assert(!reader.read(&value,1,1));reader.end();
    assert(mprotect(memory+4096,4096,PROT_NONE)==0);reader.begin();
    assert(!reader.read(boundary.data(),reinterpret_cast<uintptr_t>(memory+4095),2));reader.end();
    munmap(memory,8192);
    char file[]="/tmp/xenomit-nav-test-XXXXXX";int fd=mkstemp(file);assert(fd>=0);
    const std::string body="{\"map_id\":6,\"geometry_ready\":true,\"obstacles\":[]}";
    NavigationHeader h{0x56414e58,1,10000,6,uint32_t(body.size())};
    assert(write(fd,&h,sizeof(h))==ssize_t(sizeof(h)));assert(write(fd,body.data(),body.size())==ssize_t(body.size()));close(fd);
    NavigationCache nav;
    assert(nav.get(file,6,10000,1000)==body);
    assert(nav.get(file,7,10000,1001).find("false")!=std::string::npos);
    assert(nav.get(file,6,10751,1002).find("false")!=std::string::npos);
    assert(nav.get(file,6,8999,1003).find("false")!=std::string::npos);
    assert(truncate(file,sizeof(h)+1)==0);
    assert(nav.get(file,6,10000,1200).find("false")!=std::string::npos);
    unlink(file);
    std::puts("safe capture read and navigation cache tests passed");
}
