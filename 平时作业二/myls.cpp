#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

// ----------------------------------------------------------
//模仿ls命令的实现，支持-a和-l选项。
// -a：显示所有文件，包括隐藏文件。
// -l：显示详细信息，包括权限、链接数、所有者、组、大小和修改时间。
// ----------------------------------------------------------   
class MyLs {
public:
    int run(int argc, char* argv[]);

private:
    bool showAll = false;    //-a
    bool longFormat = false; // -l
    bool list(const std::string& path);
    bool printFile(const std::string& path, const std::string& name);
    std::string permissions(mode_t mode);
    bool error(const std::string& path);
};

bool MyLs::error(const std::string& path) {
    std::cerr << "myls: " << path << ": " << std::strerror(errno) << '\n';
    return false;
}


std::string MyLs::permissions(mode_t mode) {
    std::string result = "----------";
    if (S_ISDIR(mode))       result[0] = 'd';
    else if (S_ISLNK(mode))  result[0] = 'l';
    else if (S_ISCHR(mode))  result[0] = 'c';
    else if (S_ISBLK(mode))  result[0] = 'b';
    else if (S_ISFIFO(mode)) result[0] = 'p';
    else if (S_ISSOCK(mode)) result[0] = 's';
    else if (!S_ISREG(mode)) result[0] = '?';

    const mode_t bits[] = {S_IRUSR, S_IWUSR, S_IXUSR,
                          S_IRGRP, S_IWGRP, S_IXGRP,
                          S_IROTH, S_IWOTH, S_IXOTH};
    const std::string letters = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i)
        if (mode & bits[i]) result[i + 1] = letters[i];

    if (mode & S_ISUID) result[3] = (mode & S_IXUSR) ? 's' : 'S';
    if (mode & S_ISGID) result[6] = (mode & S_IXGRP) ? 's' : 'S';
    if (mode & S_ISVTX) result[9] = (mode & S_IXOTH) ? 't' : 'T';
    return result;
}

bool MyLs::printFile(const std::string& path, const std::string& name) {
    if (!longFormat) {
        std::cout << name << "  ";
        return true;
    }

    struct stat info {};
    if (::lstat(path.c_str(), &info) < 0) return error(path);

    passwd* user = ::getpwuid(info.st_uid);
    std::string owner = user ? user->pw_name : std::to_string(info.st_uid);
    group* groupInfo = ::getgrgid(info.st_gid);
    std::string groupName = groupInfo ? groupInfo->gr_name : std::to_string(info.st_gid);

    char timeText[32] = "?";
    std::tm* local = std::localtime(&info.st_mtime);
    if (local) std::strftime(timeText, sizeof(timeText), "%b %e %H:%M", local);

    std::string target;
    if (S_ISLNK(info.st_mode)) 
    {
        //使用 readlink 获取软连接指向的文件
        std::vector<char> buffer(256);
        while (true) {
            ssize_t length = ::readlink(path.c_str(), buffer.data(), buffer.size());
            if (length < 0) return error(path);
            if (static_cast<std::size_t>(length) < buffer.size()) {
                target = " -> " + std::string(buffer.data(), length);
                break;
            }
            buffer.resize(buffer.size() * 2);
        }
    }

    std::cout << permissions(info.st_mode) << ' '
              << info.st_nlink << ' ' << owner << ' ' << groupName << ' '
              << std::setw(8) << info.st_size << ' ' << timeText << ' '
              << name << target << '\n';
    return true;
}

bool MyLs::list(const std::string& path) {
    struct stat info {};
    if (::lstat(path.c_str(), &info) < 0) return error(path);
    if (!S_ISDIR(info.st_mode)) {
        bool ok = printFile(path, path);
        if (!longFormat) std::cout << '\n';
        return ok;
    }

    DIR* directory = ::opendir(path.c_str());
    if (!directory) return error(path);

    // 先收集名称，再排序，最后按所选格式输出。
    std::vector<std::string> names;
    bool ok = true;
    while (true) {
        errno = 0;
        dirent* entry = ::readdir(directory);
        if (!entry) {
            if (errno != 0) ok = error(path);
            break;
        }
        if (showAll || entry->d_name[0] != '.') names.push_back(entry->d_name);
    }
    if (::closedir(directory) < 0) ok = error(path);

    std::sort(names.begin(), names.end());
    for (const std::string& name : names)
        if (!printFile(path + "/" + name, name)) ok = false;
    if (!longFormat) std::cout << '\n';
    return ok;
}

int MyLs::run(int argc, char* argv[]) {
    showAll = false;
    longFormat = false;
    bool parseOptions = true;
    std::vector<std::string> paths;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (parseOptions && arg == "--") {
            parseOptions = false;
        } else if (parseOptions && arg.size() > 1 && arg[0] == '-') {
            for (std::size_t j = 1; j < arg.size(); ++j) {
                if (arg[j] == 'a') showAll = true;
                else if (arg[j] == 'l') longFormat = true;
                else {
                    std::cerr << "usage: myls [-a] [-l] [--] [path ...]\n";
                    return 2;
                }
            }
        } else {
            paths.push_back(arg);
        }
    }

    if (paths.empty()) paths.push_back(".");
    bool ok = true;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (paths.size() > 1) std::cout << (i ? "\n" : "") << paths[i] << ":\n";
        if (!list(paths[i])) ok = false;
    }
    return ok ? 0 : 1;
}

int main(int argc, char* argv[]) {
    MyLs myls;
    return myls.run(argc, argv);
}
