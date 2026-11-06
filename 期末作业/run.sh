#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C

PROJECT="${1:-$HOME/langjingzhang/project_final}"
BASE="${2:-/mnt/e/sort_test_128G_20261004}"
COUNT="${3:-20000}"
[[ "$COUNT" =~ ^[1-9][0-9]*$ ]] || { echo 'Invalid file count'; exit 1; }
echo '正在启动排序实验，检查项目文件……'
cd "$PROJECT" || { echo "错误：无法进入项目目录 $PROJECT"; exit 1; }
for tool in g++ sort cmp sha256sum xargs sed tr awk stat; do
    command -v "$tool" >/dev/null || { echo "错误：缺少命令 $tool"; exit 1; }
done
[[ -x /usr/bin/time ]] || { echo '错误：缺少 /usr/bin/time，请先运行 sudo apt install time'; exit 1; }
[[ -f ThreadPool.h ]] || { echo "错误：$PROJECT 中缺少 ThreadPool.h"; exit 1; }
SOURCE_CPP=''
for candidate in 多路归并.cpp mult_sort.cpp multi_sort.cpp; do
    if [[ -f "$candidate" ]]; then
        SOURCE_CPP="$candidate"
        break
    fi
done
[[ -n "$SOURCE_CPP" ]] || {
    echo "错误：$PROJECT 中未找到多路归并源码（多路归并.cpp、mult_sort.cpp 或 multi_sort.cpp）"
    exit 1
}
echo "使用源码：$PROJECT/$SOURCE_CPP"

if [[ -f "$BASE/input/data_0.txt" && -f "$BASE/data_0.txt" ]]; then
    echo 'Ambiguous data directory: pass the actual input directory as argument 2.'
    exit 1
elif [[ -f "$BASE/input/data_0.txt" ]]; then
    SOURCE="$BASE/input"
elif [[ -f "$BASE/data_0.txt" ]]; then
    SOURCE="$BASE"
else
    echo "Cannot find data_0.txt under $BASE"
    exit 1
fi
SOURCE="$(realpath "$SOURCE")"
ROOT="$(mktemp -d /mnt/e/sort_verify_XXXXXXXX)"
INPUT="$ROOT/input"
OUTPUT="$ROOT/result.txt"
mkdir "$INPUT" "$ROOT/verify_tmp"
printf '%s\n' "$ROOT" > "$PROJECT/last_sort_verify_run.txt"
exec > >(tee "$ROOT/console.log") 2>&1
trap 'rc=$?; printf "FAILED exit=%s line=%s\n" "$rc" "$LINENO" > "$ROOT/status.txt"; echo "FAILED: see $ROOT"; exit "$rc"' ERR
stage() {
    printf '%s\n' "$1" > "$ROOT/status.txt"
    local message
    case "$1" in
        PREPARE) message='开始准备：检查数据并编译程序' ;;
        HASH_ORIGINAL_INPUT) message='正在检查全部输入文件并准备实验目录' ;;
        SORT) message='排序开始，请等待' ;;
        BUILD_FULL_REFERENCE) message='校验开始：生成全部输入的参考排序结果' ;;
        COMPARE_ALL_VALUES) message='参考结果已生成，正在完整比较所有整数' ;;
        CHECK_INPUT_UNCHANGED) message='排序结果一致，正在确认输入文件未改变' ;;
        PASS) message='校验通过：全局顺序、全部整数及重复次数均正确' ;;
        *) message="$1" ;;
    esac
    printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$message"
}
fail() { echo "$*"; return 1; }

echo "Results: $ROOT"
stage PREPARE
for ((i=0; i<COUNT; i++)); do
    name="data_$i.txt"
    [[ -f "$SOURCE/$name" && ! -L "$SOURCE/$name" ]] || fail "Missing regular file: $SOURCE/$name"
    printf '%s\0' "$name"
done > "$ROOT/files.nul"

INPUT_BYTES="$(cd "$SOURCE"; xargs -0 stat -c %s < "$ROOT/files.nul" | awk '{s+=$1} END{printf "%.0f",s}')"
FREE_BYTES="$(df -B1 --output=avail "$ROOT" | tail -n 1 | tr -d ' ')"
# 为临时文件和参考结果预留磁盘空间。
REQUIRED=$((INPUT_BYTES * 5 + 1073741824))
(( FREE_BYTES >= REQUIRED )) || fail "Insufficient free space: reserve $REQUIRED bytes; available $FREE_BYTES"
ulimit -n 1024
{ date -Is; uname -a; cat /etc/os-release; g++ --version; sort --version; free -h; } > "$ROOT/environment.txt"
cp -- "$SOURCE_CPP" ThreadPool.h "$ROOT/"
g++ -std=c++17 -O2 -Wall -Wextra -pthread "$ROOT/$SOURCE_CPP" -o "$ROOT/sort_multi"
sha256sum "$ROOT/sort_multi" > "$ROOT/executable.sha256"

stage HASH_ORIGINAL_INPUT
(cd "$SOURCE"; xargs -0 sha256sum < "$ROOT/files.nul") > "$ROOT/source_before.sha256"
for ((i=0; i<COUNT; i++)); do
    name="data_$i.txt"
    # 优先用硬链接，不支持时再复制。
    ln -- "$SOURCE/$name" "$INPUT/$name" 2>/dev/null || cp -- "$SOURCE/$name" "$INPUT/$name"
done
(cd "$INPUT"; sha256sum -c "$ROOT/source_before.sha256") > "$ROOT/input_copy_check.log"

stage SORT
/usr/bin/time -f 'sort_seconds=%e\npeak_rss_KiB=%M\nexit_code=%x' \
    -o "$ROOT/sort_metrics.txt" \
    "$ROOT/sort_multi" "$INPUT" "$OUTPUT" > "$ROOT/sort.log" 2>&1
printf '[%s] 排序结束，用时 %s 秒；详细过程已保存到 sort.log\n' \
    "$(date '+%H:%M:%S')" "$(sed -n 's/^sort_seconds=//p' "$ROOT/sort_metrics.txt")"

stage BUILD_FULL_REFERENCE
VERIFY_START=$SECONDS
# 统一数字写法后生成参考结果，避免浮点转换造成精度损失。
{
    for ((i=0; i<COUNT; i++)); do
        cat -- "$SOURCE/data_$i.txt"
        printf '\n'
    done
} | tr -s '[:space:]' '\n' \
  | sed -E '/^$/d; s/^\+//; s/^(-?)0+([0-9])/\1\2/; s/^-0$/0/' \
  | sort -n --parallel=2 -S 64M -T "$ROOT/verify_tmp" > "$ROOT/reference.txt"

stage COMPARE_ALL_VALUES
# 先检查顺序，再比较全部数据。
sort -n -c "$OUTPUT" 2> "$ROOT/order_check.log"
if ! cmp -- "$ROOT/reference.txt" "$OUTPUT" > "$ROOT/compare.log" 2>&1; then
    fail "Full comparison FAILED; see compare.log. All outputs are preserved."
fi
VERIFY_SECONDS=$((SECONDS-VERIFY_START))

stage CHECK_INPUT_UNCHANGED
(cd "$SOURCE"; sha256sum -c "$ROOT/source_before.sha256") > "$ROOT/source_unchanged.log"
(cd "$INPUT"; sha256sum -c "$ROOT/source_before.sha256") > "$ROOT/staged_unchanged.log"

N="$(wc -l < "$OUTPUT" | tr -d ' ')"
BYTES="$(stat -c %s "$OUTPUT")"
{
    echo 'status=PASS'
    echo "source=$SOURCE"
    echo "selected_files=$COUNT"
    echo "selection=data_0.txt through data_$((COUNT-1)).txt"
    echo "input_bytes=$INPUT_BYTES"
    echo "output_bytes=$BYTES"
    echo "integer_count=$N"
    cat "$ROOT/sort_metrics.txt"
    echo "reference_and_comparison_seconds=$VERIFY_SECONDS"
    echo 'global_ascending=PASS'
    echo 'all_values_and_frequencies=PASS'
    echo 'original_and_staged_inputs_unchanged=PASS'
} > "$ROOT/summary.txt"
cat > "$ROOT/report_results.txt" <<EOF
本次实验选取 data_0.txt 至 data_$((COUNT-1)).txt，共 $COUNT 个文件，
输入总量 $INPUT_BYTES 字节。排序结果包含 $N 个整数，共 $BYTES 字节。
独立校验重新读取全部选定文件，以本机 sort -n 生成完整参考序列，
并通过 cmp 对整个输出逐字节比较，未采用抽样验证。
全局非递减检查及完整序列比较均通过，说明整数值与重复次数一致。
实验前后对全部原始文件及实际排序输入执行 SHA256 一致性检查，均通过。
排序耗时与排序进程峰值 RSS 见 summary.txt；参考排序和校验不计入排序耗时。
本次仅验证上述选定文件，不表示128GiB全量数据已经验证，也不代表多次运行平均值。
EOF
stage PASS
echo "排序结果：$OUTPUT"
echo "实验统计：$ROOT/summary.txt"
echo "报告补充：$ROOT/report_results.txt"
