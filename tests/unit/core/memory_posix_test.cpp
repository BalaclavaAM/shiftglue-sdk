#include <cerrno>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <rex/memory/utils.h>

namespace {
thread_local bool legacy_mmap = false;
thread_local void* relocated_mapping = nullptr;

struct Mapping {
  explicit Mapping(size_t size) : size(size) {
    address = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(address != MAP_FAILED);
  }
  ~Mapping() { munmap(address, size); }
  void* address;
  size_t size;
};

struct LegacyKernel {
  LegacyKernel() {
    legacy_mmap = true;
    relocated_mapping = nullptr;
  }
  ~LegacyKernel() { legacy_mmap = false; }
};

void CheckRelocationReleased(size_t size) {
  REQUIRE(relocated_mapping != nullptr);
  unsigned char resident = 0;
  CHECK(mincore(relocated_mapping, size, &resident) == -1);
  CHECK(errno == ENOMEM);
}
}  // namespace

extern "C" void* __real_mmap(void*, size_t, int, int, int, off_t);
extern "C" void* __wrap_mmap(void* address, size_t size, int prot, int flags, int fd,
                             off_t offset) {
  // Linux before 4.17 ignores this flag, treating a fixed address as a hint.
  const bool emulate = legacy_mmap && (flags & MAP_FIXED_NOREPLACE);
  auto* result =
      __real_mmap(address, size, prot, emulate ? flags & ~MAP_FIXED_NOREPLACE : flags, fd, offset);
  if (emulate && result != MAP_FAILED && result != address)
    relocated_mapping = result;
  return result;
}

TEST_CASE("Fixed allocation handles a kernel that ignores NOREPLACE", "[memory][posix]") {
  using namespace rex::memory;
  const size_t page = page_size();
  Mapping mapping(page);
  std::memset(mapping.address, 0xBE, page);
  LegacyKernel legacy;

  SECTION("reserve collision fails without replacing data or leaking a mapping") {
    CHECK(AllocFixed(mapping.address, page, AllocationType::kReserve, PageAccess::kNoAccess) ==
          nullptr);
    CHECK(static_cast<unsigned char*>(mapping.address)[page - 1] == 0xBE);
    CheckRelocationReleased(page);
  }

  SECTION("commit reopens the requested guard page") {
    REQUIRE(mprotect(mapping.address, page, PROT_NONE) == 0);
    REQUIRE(AllocFixed(mapping.address, page, AllocationType::kCommit, PageAccess::kReadWrite) ==
            mapping.address);
    CheckRelocationReleased(page);
    CHECK(static_cast<unsigned char*>(mapping.address)[page - 1] == 0xBE);
    std::memset(mapping.address, 0xCD, page);
  }

  SECTION("reserve and commit preserves an existing reservation") {
    REQUIRE(mprotect(mapping.address, page, PROT_NONE) == 0);
    REQUIRE(AllocFixed(mapping.address, page, AllocationType::kReserveCommit,
                       PageAccess::kReadWrite) == mapping.address);
    CheckRelocationReleased(page);
    CHECK(static_cast<unsigned char*>(mapping.address)[page - 1] == 0xBE);
  }
}

TEST_CASE("Fixed allocation commits at the requested address on a current kernel",
          "[memory][posix]") {
  using namespace rex::memory;
  Mapping mapping(page_size());
  REQUIRE(mprotect(mapping.address, mapping.size, PROT_NONE) == 0);
  REQUIRE(AllocFixed(mapping.address, mapping.size, AllocationType::kCommit,
                     PageAccess::kReadWrite) == mapping.address);
  std::memset(mapping.address, 0xBE, mapping.size);
}
