#pragma once
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <sys/types.h>
#include <vector>
#include <cstddef>
using namespace  std;

//构造一个buffer的类
class BufferedFile {
public:
    explicit BufferedFile(size_t capacity = 4096):buffer(capacity)
    {   
        if (!capacity || capacity > static_cast<size_t>(numeric_limits<ssize_t>::max()))
        throw invalid_argument("invalid buffer capacity");
    }

    ~BufferedFile()
    {
        const int saved = errno;
        if (fd >= 0 && close() < 0 && fd >= 0) ::close(fd);
        errno = saved;
    }

    BufferedFile(const BufferedFile&) = delete;
    BufferedFile& operator=(const BufferedFile&) = delete;

    int open(const char* path, int flags, mode_t mode = 0666)
    {
        if (fd >= 0) { errno = EBUSY; return -1; }
        int allowed = O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_APPEND;
    #ifdef O_CLOEXEC
        allowed |= O_CLOEXEC;
    #endif
        if (!path || (flags & ~allowed) || (flags & O_ACCMODE) == O_ACCMODE) {
            errno = EINVAL; return -1;
        }
        int descriptor;
        do { descriptor = ::open(path, flags, mode); } while (descriptor < 0 && errno == EINTR);
        if (descriptor < 0) return -1;
        struct stat metadata {};
        if (::fstat(descriptor, &metadata) < 0) {
            int saved = errno; ::close(descriptor); errno = saved; return -1;
        }
        if (!S_ISREG(metadata.st_mode)) {
            ::close(descriptor); errno = EINVAL; return -1;
        }
        fd = descriptor;
        access = flags & O_ACCMODE;
        begin = end = 0;
        state = State::Empty;
        return 0;
    }
    
    ssize_t read(void* destination, size_t count)
    {
        if (fd < 0 || access == O_WRONLY) { errno = EBADF; return -1; }
        if (count > static_cast<size_t>(numeric_limits<ssize_t>::max())) { errno = EINVAL; return -1; }
        if (!count) return 0;
        if (!destination) { errno = EFAULT; return -1; }
        if (state == State::Writing && flush() < 0) return -1;
        size_t done = 0;
        auto* out = static_cast<char*>(destination);
        while (done < count) {
            if (begin == end) {
                ssize_t n;
                do { n = ::read(fd, buffer.data(), buffer.size()); } while (n < 0 && errno == EINTR);
                if (n < 0) return done ? static_cast<ssize_t>(done) : -1;
                if (n == 0) break;
                begin = 0; end = static_cast<size_t>(n);
                state = State::Reading;
            }
            const size_t n = std::min(count - done, end - begin);
            std::memcpy(out + done, buffer.data() + begin, n);
            begin += n; done += n;
        }
        return static_cast<ssize_t>(done);
    }

    ssize_t write(const void* source, size_t count)
    {
        if (fd < 0 || access == O_RDONLY) { errno = EBADF; return -1; }
        if (count > static_cast<size_t>(numeric_limits<ssize_t>::max())) { errno = EINVAL; return -1; }
        if (!count) return 0;
        if (!source) { errno = EFAULT; return -1; }
        if (state == State::Reading && synchronize() < 0) return -1;
        size_t done = 0;
        const auto* in = static_cast<const char*>(source);
        while (done < count) {
            if (end == buffer.size() && flush() < 0)
                return done ? static_cast<ssize_t>(done) : -1;
            state = State::Writing;
            const size_t n = std::min(count - done, buffer.size() - end);
            std::memcpy(buffer.data() + end, in + done, n);
            end += n; done += n;
            // 满缓存立即尝试刷新；失败时已接收的数据仍保留，可重试 flush。
            if (end == buffer.size() && flush() < 0) return static_cast<ssize_t>(done);
        }
        return static_cast<ssize_t>(done);
    }

    off_t lseek(off_t offset, int whence)
    {
    if (fd < 0) { errno = EBADF; return -1; }
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) { errno = EINVAL; return -1; }
    if (synchronize() < 0) return -1;
    return ::lseek(fd, offset, whence);
    }

    int flush()
    {
        if (fd < 0) { errno = EBADF; return -1; }
        if (state != State::Writing) return 0;
        // begin 记录已经写出的部分，发生短写或错误时不会重复写入。
        while (begin < end) {
            ssize_t n = ::write(fd, buffer.data() + begin, end - begin);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { if (n == 0) errno = EIO; return -1; }
            begin += static_cast<size_t>(n);
        }
        begin = end = 0;
        state = State::Empty;
        return 0;
    }

    int close()
    {
        if (fd < 0) { errno = EBADF; return -1; }
        if (flush() < 0) return -1; // 保留描述符与未刷出的数据，允许调用者重试。
        const int descriptor = fd;
        fd = -1; begin = end = 0; state = State::Empty;
        // Linux close 出错后不能盲目重试，描述符可能已被释放。
        return ::close(descriptor);
    }

private:
    int fd = -1;
    int access = 0;
    enum class State { Empty, Reading, Writing } state = State::Empty;
    vector<char> buffer;
    size_t begin = 0, end = 0;
    
    
    int synchronize()
    {
        if (state == State::Writing && flush() < 0) return -1;
        if (state == State::Reading) {
            // 内核偏移已越过预读内容，退回尚未交给调用者的字节数。
            off_t unread = static_cast<off_t>(end - begin);
            if (unread && ::lseek(fd, -unread, SEEK_CUR) == static_cast<off_t>(-1)) return -1;
            begin = end = 0;
            state = State::Empty;
        }
        return 0;
    }

};


