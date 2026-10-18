#ifndef DOAS_SYS_SECRET_HPP
#define DOAS_SYS_SECRET_HPP

#include <cstddef>
#include <memory>
#include <span>

namespace sys {

// Owner of a heap buffer holding secret material. The destructor wipes the
// buffer with explicit_bzero(3) before releasing it. Note that destructors do
// not run on exit()/err() paths, so those paths still wipe explicitly, exactly
// like the original C code did.
class Secret {
  public:
    explicit Secret(size_t size);

    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;
    Secret(Secret&&) = delete;
    Secret& operator=(Secret&&) = delete;

    ~Secret();

    char* data();
    std::span<char> span();

  private:
    std::unique_ptr<char[]> buf_;
    size_t size_ = 0;
};

} // namespace sys

#endif // DOAS_SYS_SECRET_HPP
