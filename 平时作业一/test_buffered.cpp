#include "BufferedFile.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstdlib>
#include <stdexcept>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
using namespace std;

// 记录并显示每次检查的结果。
int passed = 0;
void check(bool condition, const string& purpose) {
    if (!condition) {
        cerr <<purpose << '\n';
        throw runtime_error(purpose);
    }
    ++passed;
    cout << "pass" <<endl;
}

void runTests(const char* path) {

    // 创建缓存为4字节的文件类，使少量数据就能触发缓存边界。
    BufferedFile f(4);
    char data[32] {};

    // 验证未打开和重复打开等非法操作能正确报错。
    check(f.read(data, 1) == -1 && errno == EBADF, "未打开文件时读取失败");
    check(f.open(path, O_RDWR | O_TRUNC) == 0, "以读写方式成功打开并清空文件");
    check(f.open(path, O_RDWR) == -1 && errno == EBUSY, "重复打开失败");

    // 测试写缓存、显式刷新以及超过缓存容量的写入。
    check(f.write("abc", 3) == 3, "成功接收3字节 abc 到缓存");
    struct stat st {}; check(::stat(path, &st) == 0 && st.st_size == 0, "缓存未满时文件大小仍为0，测试延迟写入通过");
    check(f.flush() == 0, "显式刷新成功");
    check(::stat(path, &st) == 0 && st.st_size == 3, "刷新后文件大小为3，测试缓存数据已经写出");
    check(f.write("defghi", 6) == 6, "成功写入6字节 defghi，覆盖跨缓存写入");
    check(f.lseek(0, SEEK_SET) == 0, "SEEK_SET 定位到文件开头");

    // 测试预读、读写切换、原位覆盖及相对定位。
    check(f.read(data, 2) == 2 && string(data, 2) == "ab", "读出前2字节 ab，剩余预读内容留在缓存");
    check(f.lseek(0, SEEK_CUR) == 2, "SEEK_CUR 返回逻辑位置2，而非预读后的内核位置"); 
    check(f.read(data, 1) == 1 && data[0] == 'c', "继续读到 c，验证预读后的定位正确");
    check(f.write("X", 1) == 1, "在逻辑位置3写入 X，验证读后写的偏移同步"); 
    check(f.read(data, 2) == 2 && string(data, 2) == "ef", "写后读得到 ef，写缓存刷新与位置正确");
    check(f.lseek(-1, SEEK_CUR) == 5, "SEEK_CUR 后退1字节，新位置为5");
    check(f.read(data, sizeof(data)) == 4 && string(data, 4) == "fghi", "跨缓存读取剩余4字节 fghi");
    check(f.read(data, 1) == 0, "到达文件末尾后读取返回0");
    check(f.lseek(0, SEEK_SET) == 0, "重新定位到文件开头");
    check(f.read(data, 9) == 9 && string(data, 9) == "abcXefghi", "完整内容为 abcXefghi，预期位置已被覆盖");

    // 测试末尾定位、非法位置及越过文件末尾后的补零。
    check(f.lseek(-1, SEEK_END) == 8, "SEEK_END 定位到末尾前1字节");
    check(f.read(data, 1) == 1 && data[0] == 'i', "末尾前1字节为 i");
    check(f.lseek(-1, SEEK_SET) == -1, "负数绝对位置被拒绝");
    check(f.lseek(12, SEEK_SET) == 12, "允许定位到文件末尾之外的位置12");
    check(f.write("Z", 1) == 1, "在位置12写入 Z");
    check(f.close() == 0, "关闭时自动刷新末尾的 Z");

    // 测试访问模式限制，同时检查刚才写入数据的稀疏区。
    check(f.open(path, O_RDONLY) == 0, "以只读方式重新打开文件");
    check(f.write("x", 1) == -1 && errno == EBADF, "只读文件拒绝写入");
    check(f.lseek(9, SEEK_SET) == 9, "定位到稀疏区起点9");
    check(f.read(data, 4) == 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 'Z', "稀疏区读出3个零字节，随后读到 Z");
    check(f.close() == 0, "关闭只读文件成功");

    // 测试 O_APPEND 追加方式。
    check(f.open(path, O_WRONLY | O_APPEND) == 0, "以只写和追加方式打开文件");

    // 测试未打开和重复打开等非法操作。
    check(f.read(data, 1) == -1 && errno == EBADF, "只写文件拒绝读取");
    check(f.lseek(0, SEEK_SET) == 0, "追加模式下先将偏移设为0");
    check(f.write("!", 1) == 1 && f.close() == 0, "追加写入感叹号并成功关闭");
    check(::stat(path, &st) == 0 && st.st_size == 14, "文件长度变为14，证明追加没有覆盖文件开头");
    {
        BufferedFile automatic(4);
        check(automatic.open(path, O_WRONLY | O_APPEND) == 0, "局部对象以追加方式打开文件");
        check(automatic.write("?", 1) == 1, "局部对象接收问号到缓存");
    }
    check(::stat(path, &st) == 0 && st.st_size == 15, "离开作用域后长度变为15，验证析构刷新");

    // 写入并读回10003字节，测试跨多块缓存的数据完整性。
    check(f.open(path, O_RDWR | O_TRUNC) == 0, "重新打开并清空文件");
    const string large_data(10003, 'q');
    check(f.write(large_data.data(), large_data.size()) == static_cast<ssize_t>(large_data.size()), "成功接收10003字节，覆盖多轮缓存刷新");
    check(f.lseek(0, SEEK_SET) == 0, "定位到文件开头");
    string readback(large_data.size(), '\0');
    check(f.read(&readback[0], readback.size()) == static_cast<ssize_t>(large_data.size()), "成功读回10003字节");
    check(readback == large_data, "读回内容逐字节一致，没有丢失或重复数据");

    // 测试零长度请求、正常和重复关闭。
    check(f.read(nullptr, 0) == 0 && f.write(nullptr, 0) == 0, "零长度读写允许空指针并返回0");
    check(f.close() == 0, "最终关闭文件成功");
    check(f.close() == -1 && errno == EBADF, "重复关闭失败");

}

int main() {
    char path[] = "/tmp/buffered-test";
    int seed = ::mkstemp(path);
    if (seed < 0) {
        cerr << "创建临时文件失败" << '\n';
        return 1;
    }
    if (::close(seed) < 0) {
        cerr << "关闭临时文件描述符失败\n";
        ::unlink(path);
        return 1;
    }
    int result = 0;
    try {
        runTests(path);
        cout << "文件测试完成：" << passed << " 项检查全部通过。\n";
    } catch (const exception& error) {
        cerr << "文件测试失败，已通过 " << passed
             << "项\n";
        result = 1;
    }
    // 清理测试创建的临时文件。
    if (::unlink(path) < 0) {
        cerr << "清理临时文件失败" <<'\n';
        result = 1;
    }
    return result;
}
