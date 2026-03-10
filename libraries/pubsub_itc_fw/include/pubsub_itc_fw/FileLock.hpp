class FileLock {
public:
    explicit FileLock(const std::string& path)
        : file_path_(path)
    {
        fd_ = ::open(file_path_.c_str(), O_CREAT | O_RDWR, 0644);
        if (fd_ < 0) {
            throw std::runtime_error(
                "Failed to open lock file '" + file_path_ +
                "': " + std::strerror(errno));
        }

        if (flock(fd_, LOCK_EX) != 0) {
            ::close(fd_);
            throw std::runtime_error(
                "Failed to acquire lock on '" + file_path_ +
                "': " + std::strerror(errno));
        }
    }

    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

    FileLock(FileLock&& other)
        : fd_(other.fd_), file_path_(std::move(other.file_path_))
    {
        other.fd_ = -1;
    }

    FileLock& operator=(FileLock&& other) {
        if (this != &other) {
            release();
            fd_ = other.fd_;
            file_path_ = std::move(other.file_path_);
            other.fd_ = -1;
        }
        return *this;
    }

    ~FileLock() {
        release();
    }

private:
    void release() {
        if (fd_ >= 0) {
            flock(fd_, LOCK_UN);
            ::close(fd_);
            fd_ = -1;
        }
    }

    int fd_{-1};
    std::string file_path_;
};
