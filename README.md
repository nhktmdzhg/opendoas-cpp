# doas-common

Một binary **multicall** duy nhất, chứa cả ba applet. Không có `doas`, `doasedit`,
`sudo` riêng: chỉ có `doas-common` cộng ba symlink trỏ về nó.

| Applet     | Symlink    | Việc                                                                                                                                         |
| ---------- | ---------- | -------------------------------------------------------------------------------------------------------------------------------------------- |
| `doas`     | `doas`     | Chạy lệnh với quyền user khác (port của [OpenDoas](https://github.com/Duncaen/OpenDoas), gốc là `doas` của OpenBSD)                          |
| `doasedit` | `doasedit` | Sửa file bằng quyền root nhưng dùng editor chạy dưới quyền user (port của [`doasedit`](https://github.com/nhktmdzhg/doasedit) viết bằng Rust) |
| `sudo`     | `sudo`     | Dịch command line kiểu `sudo` sang tham số `doas` rồi chạy (port của `fudo`)                                                                 |

Hành vi, thông điệp, exit code và cú pháp `doas.conf` giữ **y nguyên** so với bản
gốc: cùng đầu vào phải cho cùng stdout/stderr/exit code, từng byte một.

## Yêu cầu

- Linux + glibc (dùng trực tiếp PAM, `getspnam(3)`, `closefrom(3)`; **không** hỗ
  trợ BSD/macOS).
- CMake ≥ 3.22, Ninja, Bison ≥ 3.8, toolchain LLVM (`clang++`, `lld`, `llvm-ar`,
  `llvm-ranlib`), tuỳ chọn `ccache`.
- `libpam` (mặc định) hoặc `libcrypt` nếu chọn provider SHADOW.

## Build

```sh
cmake --preset default      # Ninja + clang + lld + ccache, Release
cmake --build build
ctest --preset default
```

Cấu hình ví dụ:

```sh
cmake -S . -B build -G Ninja \
      -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

### Tuỳ chọn CMake

| Tuỳ chọn                        | Mặc định    | Ý nghĩa                                                             |
| ------------------------------- | ----------- | ------------------------------------------------------------------- |
| `DOAS_AUTH_PROVIDER`            | `PAM`       | `PAM` hoặc `SHADOW` (thay cho `configure --with-pam/--with-shadow`) |
| `DOAS_PERSIST`                  | `OFF`       | Bật timestamp / `persist` (thay cho `configure --with-timestamp`)   |
| `DOAS_PERSIST_DIR`              | `/run/doas` | Thư mục chứa file timestamp                                         |
| `DOAS_UID_MAX` / `DOAS_GID_MAX` | `65535`     | Trần uid/gid chấp nhận trong `doas.conf`                            |
| `DOAS_SYSCONFDIR`               | `/etc`      | Thư mục chứa `doas.conf`                                            |
| `DOAS_PAM_DIR`                  | `/etc/pam.d` | Nơi cài file dịch vụ PAM `doas` (chỉ khi provider là `PAM`)        |
| `DOAS_INSTALL_SETUID` | `ON` | Cài binary kèm bit setuid |
| `DOAS_TIDY_JOBS` | *(rỗng = số CPU)* | Số job song song cho target `tidy` |
| `DOAS_SANITIZE` | *(rỗng)* | Danh sách sanitizer cho build debug |

Code biên dịch với `-fno-exceptions -fno-rtti`; runtime C++ link tĩnh
(`-static-libstdc++ -static-libgcc`), còn libc/libpam/libcrypt link động. Kèm
`-ffunction-sections -fdata-sections` và `-Wl,--gc-sections` để bỏ hết phần
không dùng tới.

## Cài đặt

```sh
sudo cmake --install build      # /usr/bin/doas-common + symlink doas, doasedit, sudo
```

Rule cài đặt đặt mode `4755` (tắt bằng `-DDOAS_INSTALL_SETUID=OFF`) và tự tạo ba
symlink tương đối cạnh binary. Với provider `PAM`, file dịch vụ PAM cũng được cài
thành `${DOAS_PAM_DIR}/doas` (mặc định `/etc/pam.d/doas`), mode `0644`: đó là file
`pam.d__doas__linux` mà OpenDoas upstream còn giữ tới `v6.8.2` trước khi xoá, nội
dung dùng `include system-auth` nên cần distro có `system-auth` (Arch, Fedora);
trên Debian/Ubuntu phải thay bằng `common-auth`/`common-account`/`common-session`.
Provider `SHADOW` không cài file này. Biến thể PAM cho Darwin của upstream không
được dùng vì bản port chỉ chạy trên Linux/glibc.

`doas.conf` nằm ở `/etc/doas.conf` (xem `man 5 doas.conf`), phải thuộc root và
không được group/other ghi, nếu không applet từ chối chạy.

## Dùng

### doas

Không khác bản OpenDoas: các cờ `-C config`, `-L`, `-n`, `-s`, `-u user`, cùng
cú pháp `doas.conf` và cùng thông điệp. Xem `man 1 doas`, `man 5 doas.conf`.

```sh
doas whoami
doas -u nobody id
doas -C /etc/doas.conf -n reboot        # chỉ kiểm tra cấu hình, in permit/deny
```

### doasedit

```sh
doasedit /etc/hosts /etc/fstab
DOAS_EDITOR=nano doasedit /etc/doas.conf
```

Editor lấy theo thứ tự `DOAS_EDITOR` → `VISUAL` → `EDITOR` → `vi`. File thuộc
user, file nằm trong thư mục thuộc user, và thư mục user ghi được đều bị từ chối.
Mật khẩu được hỏi **một lần, trước khi editor mở** (nếu rule không `nopass`), rồi
lưu không hỏi lại; các bước đọc/ghi file chạy trực tiếp trong tiến trình con đã
nâng euid, không gọi `cat`/`dd`/`test`/`stat` bên ngoài, và `doas.conf` được kiểm
tra cú pháp bằng chính parser trong binary trước khi ghi lại. Xem `man 1 doasedit`.

### sudo

```sh
sudo whoami
sudo -n reboot
```

`sudo` in ra hai dòng trace rồi chạy applet doas **trong cùng tiến trình**:

```
$ sudo whoami
sudo <<< sudo whoami
sudo >>> /usr/bin/doas whoami
root
```

Cờ không dịch được thì bị bỏ qua và để `doas.conf` quyết định (bảng dịch đầy đủ
trong `man 1 sudo`). `SUDO_USER`, `SUDO_UID`, `SUDO_GID` được export.

## Kiểm thử

`ctest` chạy unit test cộng golden corpus: mỗi cấu hình trong
`tests/fixtures/configs` được applet parse, rồi stdout/stderr/exit code so
**từng byte** với output của bản C tham chiếu (`tests/fixtures/golden`, 99 file).
Sinh lại golden bằng:

```sh
tests/tools/regen_golden.sh /path/to/reference/doas
```

với binary tham chiếu dựng từ commit trước khi port:

```sh
git worktree add /tmp/doas-ref b96106b
(cd /tmp/doas-ref && ./configure && make)
```

Thêm: `cmake --build build --target tidy` (clang-tidy **song song** qua
`run-clang-tidy`, đổi số job bằng `-DDOAS_TIDY_JOBS=N`; cần
`CMAKE_EXPORT_COMPILE_COMMANDS=ON`, preset `default` đã bật sẵn), `--target
format` (áp `.clang-format`), `--target format-check` (chỉ kiểm tra). Chi tiết
kiến trúc và các tầng kiểm chứng ở [`architecture.md`](architecture.md).

## Giấy phép

- `src/doas/*`, `src/sys/*`: ISC/BSD, lấy từ OpenDoas/OpenBSD, giữ nguyên thông
  báo bản quyền trong từng file.
- `src/sudo/*`: BSD-2-Clause, copyright 2023 Paco Pascal.
- `src/doasedit/*`: MIT, copyright 2025 Nguyen Hoang Ky.
