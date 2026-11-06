#include "ThreadPool.h"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace std;
namespace fs = std::filesystem;

const int THREAD_COUNT = 4;
const size_t BLOCK_COUNT = 512 * 1024;
const size_t TASK_BATCH = 32;

struct BlockInfo {
    fs::path filename;
    streamoff offset;
    size_t count;
};

bool readNumber(ifstream &in, int64_t &value) {
    in >> ws;
    if (in.bad()) throw runtime_error("读取文件失败");
    if (in.eof()) return false;
    if (!(in >> value)) throw runtime_error("数据不是合法的64位整数");
    return true;
}

void closeOutput(ofstream &out) {
    out.close();
    if (!out) throw runtime_error("写入失败，请检查磁盘空间");
}

fs::path getRunPath(const fs::path &temp, int round, uint64_t id) {
    string name = to_string(round) + "_" + to_string(id) + ".txt";
    return temp / name;
}

void waitTasks(vector<future<int>> &results) {
    for (size_t i = 0; i < results.size(); ++i) {
        results[i].get();
    }
    results.clear();
}

// 合并两个有序区间，区间采用左闭右开形式。
void mergeRanges(vector<int64_t> &data, vector<int64_t> &buffer,
                 size_t left, size_t middle, size_t right) {
    size_t i = left;
    size_t j = middle;
    size_t k = left;

    while (i < middle && j < right) {
        if (data[i] <= data[j]) {
            buffer[k] = data[i];
            ++i;
        } else {
            buffer[k] = data[j];
            ++j;
        }
        ++k;
    }
    while (i < middle) {
        buffer[k] = data[i];
        ++k;
        ++i;
    }
    while (j < right) {
        buffer[k] = data[j];
        ++k;
        ++j;
    }
    for (size_t p = left; p < right; ++p) {
        data[p] = buffer[p];
    }
}

// 辅助数组只申请一次，各层递归重复使用。
void mergeSort(vector<int64_t> &data, vector<int64_t> &buffer,
               size_t left, size_t right) {
    if (right - left <= 1) return;
    size_t middle = left + (right - left) / 2;
    mergeSort(data, buffer, left, middle);
    mergeSort(data, buffer, middle, right);
    mergeRanges(data, buffer, left, middle, right);
}

// 扫描文件，将每块的路径、字节偏移和整数个数写到清单中。
uint64_t scanFileBlocks(const fs::path &filename, ofstream &list) {
    ifstream in(filename, ios::binary);
    if (!in) throw runtime_error("无法打开输入文件");
    uint64_t blocks = 0;

    while (true) {
        streamoff offset = in.tellg();
        size_t count = 0;
        int64_t value = 0;
        while (count < BLOCK_COUNT && readNumber(in, value)) {
            ++count;
        }
        if (count == 0) break;
        if (offset < 0) throw runtime_error("无法取得文件偏移");
        list << quoted(filename.u8string()) << ' ' << offset << ' ' << count << '\n';
        if (!list) throw runtime_error("写入分块清单失败");
        ++blocks;
        if (count < BLOCK_COUNT) break;
    }
    return blocks;
}

uint64_t collectFileBlocks(const fs::path &input, const fs::path &listPath) {
    ofstream list(listPath);
    if (!list) throw runtime_error("无法创建分块清单");
    uint64_t total = 0;
    for (const fs::directory_entry &entry : fs::directory_iterator(input)) {
        if (fs::is_regular_file(entry.symlink_status())) {
            total += scanFileBlocks(entry.path(), list);
        }
    }
    closeOutput(list);
    return total;
}

bool readBlockInfo(ifstream &list, BlockInfo &info) {
    string line;
    if (!getline(list, line)) {
        if (!list.eof() || list.bad()) throw runtime_error("读取清单失败");
        return false;
    }
    istringstream record(line);
    string filename;
    if (!(record >> quoted(filename) >> info.offset >> info.count)) {
        throw runtime_error("分块清单格式错误");
    }
    info.filename = fs::u8path(filename);
    if (info.offset < 0 || info.count == 0 || info.count > BLOCK_COUNT) {
        throw runtime_error("分块信息超出范围");
    }
    return true;
}

int sortBlock(BlockInfo info, fs::path output) {
    ifstream in(info.filename, ios::binary);
    if (!in) throw runtime_error("无法打开数据块所在文件");
    in.seekg(info.offset);
    if (!in) throw runtime_error("文件定位失败");

    vector<int64_t> data(info.count);
    vector<int64_t> buffer(info.count);
    for (size_t i = 0; i < info.count; ++i) {
        if (!readNumber(in, data[i])) throw runtime_error("输入文件发生变化");
    }
    mergeSort(data, buffer, 0, data.size());

    ofstream out(output);
    if (!out) throw runtime_error("无法创建排序结果");
    for (size_t i = 0; i < data.size(); ++i) {
        out << data[i] << '\n';
    }
    closeOutput(out);
    return 0;
}

void processFileBlocks(ThreadPool &pool, const fs::path &listPath,
                       const fs::path &temp) {
    ifstream list(listPath);
    if (!list) throw runtime_error("无法打开分块清单");
    vector<future<int>> results;
    BlockInfo info;
    uint64_t id = 0;

    while (readBlockInfo(list, info)) {
        fs::path output = getRunPath(temp, 0, id);
        // 方括号里的变量是本任务保存的参数副本。
        future<int> result = pool.submit<int>([info, output]() {
            return sortBlock(info, output);
        });
        results.push_back(std::move(result));
        ++id;
        // 只保留32个future，队列中只有描述信息，没有整数数组。
        if (results.size() == TASK_BATCH) waitTasks(results);
    }
    waitTasks(results);
}

// 每次最多合并64个文件，用手写最小堆选出当前最小值。
const uint64_t MERGE_COUNT = 64;

struct HeapNode {
    int64_t value;
    size_t file;
};

void pushHeap(vector<HeapNode> &heap, HeapNode node) {
    heap.push_back(node);
    size_t child = heap.size() - 1;
    while (child > 0) {
        size_t parent = (child - 1) / 2;
        if (heap[parent].value <= heap[child].value) break;
        swap(heap[parent], heap[child]);
        child = parent;
    }
}

HeapNode popHeap(vector<HeapNode> &heap) {
    HeapNode result = heap[0];
    heap[0] = heap.back();
    heap.pop_back();
    size_t parent = 0;
    while (2 * parent + 1 < heap.size()) {
        size_t child = 2 * parent + 1;
        if (child + 1 < heap.size() && heap[child + 1].value < heap[child].value) {
            ++child;
        }
        if (heap[parent].value <= heap[child].value) break;
        swap(heap[parent], heap[child]);
        parent = child;
    }
    return result;
}

int mergeFiles(fs::path temp, int round, uint64_t first,
               uint64_t number, fs::path output) {
    {
        vector<ifstream> inputs(number);
        vector<HeapNode> heap;
        ofstream out(output);
        if (!out) throw runtime_error("无法创建归并结果");
        for (size_t i = 0; i < number; ++i) {
            inputs[i].open(getRunPath(temp, round, first + i));
            if (!inputs[i]) throw runtime_error("无法打开归并文件");
            int64_t value = 0;
            if (readNumber(inputs[i], value)) {
                HeapNode node = {value, i};
                pushHeap(heap, node);
            }
        }
        while (!heap.empty()) {
            HeapNode node = popHeap(heap);
            out << node.value << '\n';
            if (!out) throw runtime_error("归并写入失败");
            int64_t value = 0;
            if (readNumber(inputs[node.file], value)) {
                node.value = value;
                pushHeap(heap, node);
            }
        }
        closeOutput(out);
    } // 所有输入流关闭后再删除旧段。
    for (uint64_t i = 0; i < number; ++i) {
        fs::remove(getRunPath(temp, round, first + i));
    }
    return 0;
}

int mergeAllFiles(ThreadPool &pool, const fs::path &temp, uint64_t count) {
    int round = 0;
    while (count > 1) {
        vector<future<int>> results;
        uint64_t nextCount = count / MERGE_COUNT;
        if (count % MERGE_COUNT != 0) ++nextCount;
        for (uint64_t i = 0; i < nextCount; ++i) {
            uint64_t first = i * MERGE_COUNT;
            uint64_t number = count - first;
            if (number > MERGE_COUNT) number = MERGE_COUNT;
            fs::path output = getRunPath(temp, round + 1, i);
            if (number == 1) {
                fs::rename(getRunPath(temp, round, first), output);
            } else {
                future<int> result = pool.submit<int>([temp, round, first, number, output]() {
                    return mergeFiles(temp, round, first, number, output);
                });
                results.push_back(std::move(result));
                if (results.size() == TASK_BATCH) waitTasks(results);
            }
        }
        waitTasks(results);
        ++round;
        count = nextCount;
        cout << "第" << round << "轮完成，剩余" << count << "段\n";
    }
    return round;
}
void saveResult(const fs::path &temp, int round, const fs::path &output) {
    if (fs::exists(output)) throw runtime_error("输出文件已存在");
    fs::rename(getRunPath(temp, round, 0), output);
}

void clearTempFiles(const fs::path &temp) {
    fs::remove_all(temp); // 只清理本次成功创建的目录。
}

void checkPaths(const fs::path &input, const fs::path &output) {
    if (!fs::is_directory(input)) throw runtime_error("输入必须是目录");
    if (fs::exists(output)) throw runtime_error("输出文件已存在");
    fs::path parent = fs::canonical(output.parent_path());
    while (true) {
        if (parent == input) throw runtime_error("输出须放在输入目录外");
        if (parent == parent.parent_path()) break;
        parent = parent.parent_path();
    }
}

void runSort(const fs::path &input, const fs::path &output, const fs::path &temp) {
    fs::path listPath = temp / "blocks.txt";
    cout << "1. 扫描文件，生成分块清单\n";
    uint64_t count = collectFileBlocks(input, listPath);
    cout << "共" << count << "个数据块\n";

    ThreadPool pool(THREAD_COUNT);
    pool.start();
    cout << "2. 使用线程池进行分块排序\n";
    processFileBlocks(pool, listPath, temp);
    if (count == 0) {
        ofstream empty(getRunPath(temp, 0, 0));
        closeOutput(empty);
        count = 1;
    }

    cout << "3. 64路归并排序结果\n";
    int round = mergeAllFiles(pool, temp, count);
    pool.shutdown();
    cout << "4. 保存最终结果\n";
    saveResult(temp, round, output);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        cerr << "用法：sort 输入目录 输出文件\n";
        return 1;
    }
    fs::path temp;
    bool created = false;
    try {
        // Linux终端通常使用UTF-8，清单和命令行路径采用同一编码。
        fs::path input = fs::canonical(fs::u8path(argv[1]));
        fs::path output = fs::absolute(fs::u8path(argv[2]));
        checkPaths(input, output);
        temp = output;
        temp += ".tmp";
        if (!fs::create_directory(temp)) throw runtime_error("临时目录已存在");
        created = true;
        runSort(input, output, temp);
        clearTempFiles(temp);
        cout << "排序完成\n";
        return 0;
    } catch (const exception &e) {
        // runSort退出时，线程池析构先等待工作线程结束。
        if (created) {
            error_code error;
            fs::remove_all(temp, error);
        }
        cerr << "错误：" << e.what() << '\n';
        return 1;
    }
}

