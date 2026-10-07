#include "BufferedFile.h"
#include <fcntl.h>
#include <cstdio>
#include <cstring>
#include <iostream>

using namespace std;

// 测试创建文件、缓存写入、定位、读取和关闭。
int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "buffered_demo.txt";

    //创建一个 缓存容量为16字节的文件操作类。
    BufferedFile file(16);

    // 创建文件，并以读写方式打开。
    // 0644 表示所有者可读写，其他用户只读，实际权限还会受 umask 影响。
    if (file.open(path, O_RDWR | O_CREAT | O_EXCL, 0644) < 0) {
        perror("创建文件失败");
        return 1;
    }
    cout << "文件创建成功：" << path << '\n';

    // 写入文本
    // strlen 不包含字符串末尾的 '\0'；只将实际文本写入文件。
    const char text[] = "Hello, buffered file!\n";
    const size_t length = strlen(text);
    ssize_t num_wiritten = file.write(text, length);
    if (num_wiritten < 0) {
        perror("写入失败");
        return 1;
    }
    if (static_cast<size_t>(num_wiritten) != length) {
        cerr << “未完全写入\n";
        return 1;
    }
    cout << "已完全写入\n";

    // 从文件开头开始读取。
    // 本类的 lseek 会先刷新剩余写缓存，再把文件位置移到开头。
    if (file.lseek(0, SEEK_SET) < 0) {
        perror("刷新缓存或定位失败");
        return 1;
    }

    // 读取文件内容，最多读取128字节。
    // read 返回实际读取长度；返回0表示已经到达文件末尾。
    char result[128] = {};
    ssize_t n = file.read(result, sizeof(result));
    if (n < 0) {
        perror("读取失败");
        return 1;
    }
    cout << "实际读取 " << n << " 字节，内容如下：\n";
    cout.write(result, n);

    // 检查读回的长度和内容。
    if (static_cast<size_t>(n) != length || memcmp(result, text, length) != 0) {
        cerr << "读到内容与写入文本不一致。\n";
        return 1;
    }
    cout << "读到内容与写入文本一致。\n";

    // 关闭文件。
    if (file.close() < 0) {
        perror("关闭文件失败");
        return 1;
    }
    cout << "文件关闭成功，测试结束。\n";
    return 0;
}
