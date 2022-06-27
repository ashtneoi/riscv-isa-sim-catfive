#include "devices.h"
#include "mmu.h"
#include <stdexcept>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

void bus_t::add_device(reg_t addr, abstract_device_t* dev)
{
  // Searching devices via lower_bound/upper_bound
  // implicitly relies on the underlying std::map 
  // container to sort the keys and provide ordered
  // iteration over this sort, which it does. (python's
  // SortedDict is a good analogy)
  devices[addr] = dev;
}

bool bus_t::load(reg_t addr, size_t len, uint8_t* bytes)
{
  // Find the device with the base address closest to but
  // less than addr (price-is-right search)
  auto it = devices.upper_bound(addr);
  if (devices.empty() || it == devices.begin()) {
    // Either the bus is empty, or there weren't 
    // any items with a base address <= addr
    return false;
  }
  // Found at least one item with base address <= addr
  // The iterator points to the device after this, so
  // go back by one item.
  it--;
  return it->second->load(addr - it->first, len, bytes);
}

bool bus_t::store(reg_t addr, size_t len, const uint8_t* bytes)
{
  // See comments in bus_t::load
  auto it = devices.upper_bound(addr);
  if (devices.empty() || it == devices.begin()) {
    return false;
  }
  it--;
  return it->second->store(addr - it->first, len, bytes);
}

std::pair<reg_t, abstract_device_t*> bus_t::find_device(reg_t addr)
{
  // See comments in bus_t::load
  auto it = devices.upper_bound(addr);
  if (devices.empty() || it == devices.begin()) {
    return std::make_pair((reg_t)0, (abstract_device_t*)NULL);
  }
  it--;
  return std::make_pair(it->first, it->second);
}

// Type for holding all registered MMIO plugins by name.
using mmio_plugin_map_t = std::map<std::string, mmio_plugin_t>;

// Simple singleton instance of an mmio_plugin_map_t.
static mmio_plugin_map_t& mmio_plugin_map()
{
  static mmio_plugin_map_t instance;
  return instance;
}

void register_mmio_plugin(const char* name_cstr,
                          const mmio_plugin_t* mmio_plugin)
{
  std::string name(name_cstr);
  if (!mmio_plugin_map().emplace(name, *mmio_plugin).second) {
    throw std::runtime_error("Plugin \"" + name + "\" already registered!");
  }
}

mmio_plugin_device_t::mmio_plugin_device_t(const std::string& name,
                                           const std::string& args)
  : plugin(mmio_plugin_map().at(name)), user_data((*plugin.alloc)(args.c_str()))
{
}

mmio_plugin_device_t::~mmio_plugin_device_t()
{
  (*plugin.dealloc)(user_data);
}

bool mmio_plugin_device_t::load(reg_t addr, size_t len, uint8_t* bytes)
{
  return (*plugin.load)(user_data, addr, len, bytes);
}

bool mmio_plugin_device_t::store(reg_t addr, size_t len, const uint8_t* bytes)
{
  return (*plugin.store)(user_data, addr, len, bytes);
}

mem_t::mem_t(reg_t size)
  : sz(size)
{
  if (size == 0 || size % PGSIZE != 0)
    throw std::runtime_error("memory size must be a positive multiple of 4 KiB");
}

mem_t::~mem_t()
{
  for (auto& entry : sparse_memory_map)
    free(entry.second);
}

bool mem_t::load_store(reg_t addr, size_t len, uint8_t* bytes, bool store)
{
  if (addr + len < addr || addr + len > sz)
    return false;

  while (len > 0) {
    auto n = std::min(PGSIZE - (addr % PGSIZE), reg_t(len));

    if (store)
      memcpy(this->contents(addr), bytes, n);
    else
      memcpy(bytes, this->contents(addr), n);

    addr += n;
    bytes += n;
    len -= n;
  }

  return true;
}

char* mem_t::contents(reg_t addr) {
  reg_t ppn = addr >> PGSHIFT, pgoff = addr % PGSIZE;
  auto search = sparse_memory_map.find(ppn);
  if (search == sparse_memory_map.end()) {
    auto res = (char*)calloc(PGSIZE, 1);
    if (res == nullptr)
      throw std::bad_alloc();
    sparse_memory_map[ppn] = res;
    return res + pgoff;
  }
  return search->second + pgoff;
}


struct file_plugin_data {
  int fd;
  char* addr;
  size_t length;
  bool writable;
};

void* file_plugin_alloc(const char* args) {
  const char* const colon = strchr(args, ':');

  const char* filename;
  bool writable = false;
  if (colon == NULL) {
    filename = args;
  } else {
    filename = colon + 1;
    for (const char* flag = args; flag < colon; flag++) {
      if (*flag == 'w') {
        writable = true;
      } else {
        return NULL;
      }
    }
  }

  const int fd = open(filename, writable ? O_RDWR : O_RDONLY);
  if (fd < 0) {
    return NULL;
  }

  const off_t length = lseek(fd, 0, SEEK_END);
  if (length == 0) {
    (void) close(fd); // ignore error
    return NULL;
  }

  char* const addr = (char*) mmap(NULL, length, writable ? PROT_READ|PROT_WRITE : PROT_READ, MAP_SHARED, fd, 0);
  if (addr == NULL) {
    (void) close(fd); // ignore error
    return NULL;
  }

  if (close(fd) != 0) {
    return NULL;
  }

  struct file_plugin_data* const data = (struct file_plugin_data*) malloc(sizeof(struct file_plugin_data));
  if (data == NULL) {
    // jesus christ
    (void) munmap(addr, length);
    return NULL;
  }
  data->fd = fd;
  data->addr = addr;
  data->length = length;
  data->writable = writable;
  return data;
}

bool file_plugin_load(void* data, reg_t offset, size_t width, uint8_t* buffer) {
  struct file_plugin_data* data2 = (struct file_plugin_data*) data;
  if (offset >= data2->length) {
    return false;
  }
  memmove(buffer, data2->addr + offset, width);
  return true;
}

bool file_plugin_store(void* data, reg_t offset, size_t width, const uint8_t* buffer) {
  struct file_plugin_data* data2 = (struct file_plugin_data*) data;
  if (!data2->writable || offset >= data2->length) {
    return false;
  }
  memmove(data2->addr + offset, buffer, width);
  return true;
}

void file_plugin_dealloc(void* data) {
  if (data != NULL) {
    struct file_plugin_data* data2 = (struct file_plugin_data*) data;
    (void) munmap(data2->addr, data2->length); // ignore error
  }
  free(data);
}

mmio_plugin_t file_plugin {
  file_plugin_alloc,
  file_plugin_load,
  file_plugin_store,
  file_plugin_dealloc
};

__attribute__((constructor))
void init_file_plugin(void) {
  register_mmio_plugin("file", &file_plugin);
}
