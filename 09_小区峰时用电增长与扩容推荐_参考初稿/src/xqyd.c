
#include <stdio.h>   /* printf、fgets、fopen 等输入输出函数 */
#include <string.h>  /* strcmp、strcpy：处理户号和小区名称 */
#include <ctype.h>   /* isspace：判断空格、制表符等空白字符 */
#include <limits.h>  /* INT_MIN、INT_MAX：int 的取值范围 */
#include <math.h>    /* isfinite：拒绝无穷大和 NaN */

/* 模块一：统一保存上限和单价，后续修改时不用四处寻找常量。 */
#define MAX_RECORDS 10000
#define MAX_AREAS 100
#define PEAK_PRICE 0.60
#define VALLEY_PRICE 0.30

/* 一条记录只代表“某个小区的某户在某年某月”的用电情况。
 * 户号只在小区内部唯一，不能只凭户号在所有小区里查找。
 * 电量采用 double，既能保存整数，也允许带小数的模拟读数。
 */
typedef struct {
    int areaId;
    char areaName[64];
    char household[32];
    int year, month;
    double peak, valley;
} Record;

/* 月报和推荐共用的汇总结构。
 * total 是峰谷合计，growth 是两年同月“户均峰时电量”的差。
 * households 是该月的户数，不是文件中全部记录的条数。
 */
typedef struct {
    int areaId;
    char areaName[64];
    int households;
    double peak, total, fee;
    double avgTotal, avgFee, avgPeak;
    double growth;
} AreaSummary;

/* 数据只读入一次。全局数组避免初学者频繁传递大数组；后续可改为参数。
 * 两个年份从文件识别，不在查询算法中写死为 2024、2025。
 */
static Record records[MAX_RECORDS];
static int recordCount = 0;
static int firstYear, secondYear;

/* 模块二：输入辅助函数。
 * 每次读一整行，防止 scanf 读入字母后留在缓冲区导致死循环。
 * 返回 0 代表输入结束；菜单收到后逐层返回，最终正常退出。
 */
static int readLine(const char *prompt, char *buffer, int size)
{
    // prompt表示提示文字，如“请输入小区编号：”
    // size表示buffer的大小，buffer是用来存储输入的字符串
    int ch;
    printf("%s", prompt);
    fflush(stdout);
    if (fgets(buffer, size, stdin) == NULL) return 0; // 读取失败
    if (strchr(buffer, '\n') == NULL) {
        /* 行过长时清除剩余字符，不能让下一次输入读到上一行的尾部。 */
        while ((ch = getchar()) != '\n' && ch != EOF) { }
        printf("输入过长，请重新输入。\n");
        buffer[0] = '\0';
        return 1;
    }
    buffer[strcspn(buffer, "\r\n")] = '\0';
    return 1;
}

/* 数字后只允许空白，例如“7abc”不是合法整数。
 * 遍历剩余字符：遇到非空白就返回 0，走到末尾则返回 1。
 * 空字符串也返回 1，表示数字后没有其他内容。
 */
static int onlySpaces(const char *text)
{
    int i = 0;
    while (text[i] != '\0') {
        if (!isspace((unsigned char)text[i])) return 0;
        i++;
    }
    return 1;
}

/* 逐个检查数字字符，用“原数值 * 10 + 当前数字”转换整数。
 * 函数参数及返回值不变，角色、月份、小区编号等调用处继续共用它。
 */
static int readInt(const char *prompt, int low, int high, int *value)
{
    char line[128];

    /* 非法输入不结束函数，而是继续读取下一行。 */
    while (readLine(prompt, line, sizeof(line))) {
        int i = 0, sign = 1, hasDigit = 0, valid = 1;
        /* 本项目的 int 为 32 位；long long 暂存不带符号的大小，
         * 也能保存 INT_MIN 对应的大小 2147483648。
         */
        long long number = 0, limit;

        /* 允许数字前有空格或制表符。 */
        while (isspace((unsigned char)line[i])) i++;

        /* 开头最多出现一个正负号，符号后不能插入空格。 */
        if (line[i] == '+' || line[i] == '-') {
            if (line[i] == '-') sign = -1;
            i++;
        }

        /* 先转换为 long long 再取负，避免直接对 INT_MIN 取负。
         * 此处限制 int 的存储范围，最后再检查 low、high 业务范围。
         */
        if (sign == 1) limit = INT_MAX;
        else limit = -(long long)INT_MIN;

        while (line[i] >= '0' && line[i] <= '9') {
            int digit = line[i] - '0';
            hasDigit = 1;

            /* 乘法之前检查上限：
             * number * 10 + digit <= limit
             * 等价于 number <= (limit - digit) / 10。
             */
            if (number > (limit - digit) / 10) {
                valid = 0;
                break;
            }
            number = number * 10 + digit;
            i++;
        }

        /* 至少有一个数字，且数字后只能有空白。
         * 这会拒绝空行、单独的正负号、“7abc”、“1.5”和“1 2”。
         */
        if (valid && hasDigit && onlySpaces(line + i)) {
            number = number * sign;
            if (number >= low && number <= high) {
                *value = (int)number;
                return 1;
            }
        }
        printf("请输入 %d 至 %d 之间的整数。\n", low, high);
    }
    /* 输入结束时返回 0，不再反复提示。 */
    return 0;
}

/* 逐位读取普通小数，不再使用 strtod，也不支持科学计数法。
 * 例如 12.34：整数部分得到 12，小数部分为 3 * 0.1 + 4 * 0.01。
 * 接口不变，推荐模块仍通过输出参数 value 取得金额。
 */
static int readThreshold(double *value)
{
    char line[128];
    const double maxAmount = 1000000000000.0;

    while (readLine("当月最低总电费（元）：", line, sizeof(line))) {
        int i = 0, sign = 1, hasDigit = 0, hasPoint = 0;
        int fractionNonzero = 0, valid = 1;
        double whole = 0, fraction = 0, weight = 0.1, number;

        /* 跳过前面的空白，再识别可选的正负号。 */
        while (isspace((unsigned char)line[i])) i++;
        if (line[i] == '+' || line[i] == '-') {
            if (line[i] == '-') sign = -1;
            i++;
        }

        /* 连续处理数字和小数点，其他字符留到循环后检查。 */
        while (line[i] != '\0') {
            if (line[i] >= '0' && line[i] <= '9') {
                int digit = line[i] - '0';
                hasDigit = 1;
                if (!hasPoint) {
                    /* 小数点之前按整数方式累加，乘法前检查金额上限。 */
                    if (whole > (maxAmount - digit) / 10.0) {
                        valid = 0;
                        break;
                    }
                    whole = whole * 10 + digit;
                } else {
                    /* 小数点之后权重依次为 0.1、0.01、0.001…… */
                    fraction = fraction + digit * weight;
                    weight = weight / 10.0;
                    if (digit != 0) fractionNonzero = 1;
                }
            } else if (line[i] == '.') {
                /* 一个金额里最多只能有一个小数点。 */
                if (hasPoint) {
                    valid = 0;
                    break;
                }
                hasPoint = 1;
            } else {
                break;
            }
            i++;
        }

        /* 整数部分已到上限时，小数部分只能全为 0。
         * 单独记录非零数字，避免极小的小数因浮点舍入而被忽略。
         */
        if (whole == maxAmount && fractionNonzero) valid = 0;

        /* 至少包含一个数字；结尾允许空白，不允许字母等额外内容。
         * 合并整数、小数及符号后，再按非负金额范围判断。
         */
        if (valid && hasDigit && onlySpaces(line + i)) {
            number = (whole + fraction) * sign;
            if (number >= 0 && number <= maxAmount) {
                *value = number;
                return 1;
            }
        }
        printf("请输入 0 至 1000000000000 之间的金额。\n");
    }
    return 0;
}

/* 模块三：文件读取和基本检查。
 * 格式：小区编号 小区名称 户号 年 月 峰时电量 谷时电量
 * 以 # 开头的行作为说明跳过；年月分两列便于 C 语言初学者读取。
 * 出现错误就停止加载，避免只读了半份数据却继续输出统计结果。
 */
static int loadData(const char *path)
{
    FILE *file = fopen(path, "r");
    char line[512], extra;
    int lineNumber = 0, minYear = INT_MAX, maxYear = INT_MIN;
    if (file == NULL) {
        printf("无法打开数据文件：%s\n请在项目根目录运行，或通过命令行指定文件。\n", path);
        return 0;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        Record item;
        char *start = line;
        int fields;
        lineNumber++;
        while (isspace((unsigned char)*start)) start++;
        if (*start == '#' || *start == '\0') continue;
        /* 最后的 %c 用于检查是否存在多余列；正常情况下只读到 7 项。 */
        fields = sscanf(start, "%d %63s %31s %d %d %lf %lf %c",
                        &item.areaId, item.areaName, item.household,
                        &item.year, &item.month, &item.peak, &item.valley, &extra);
        if (fields != 7 || item.areaId <= 0 || item.year < 1900 ||
            item.year > 9998 || item.month < 1 || item.month > 12 ||
            !isfinite(item.peak) || !isfinite(item.valley) ||
            item.peak < 0 || item.valley < 0 || item.peak > 1e9 || item.valley > 1e9) {
            printf("数据第 %d 行格式或数值有误。\n", lineNumber);
            fclose(file);
            return 0;
        }
        if (recordCount >= MAX_RECORDS) {
            printf("记录超过上限 %d 条。\n", MAX_RECORDS);
            fclose(file);
            return 0;
        }
        /* 顺序检查重复记录及小区名称一致性，初稿先不使用复杂索引。 */
        for (int i = 0; i < recordCount; i++) {
            Record *old = &records[i];
            if (old->areaId != item.areaId) continue;
            if (strcmp(old->areaName, item.areaName) != 0 ||
                (strcmp(old->household, item.household) == 0 &&
                 old->year == item.year && old->month == item.month)) {
                printf("数据第 %d 行小区名称冲突或住户月份重复。\n", lineNumber);
                fclose(file);
                return 0;
            }
        }
        records[recordCount++] = item;
        if (item.year < minYear) minYear = item.year;
        if (item.year > maxYear) maxYear = item.year;
    }
    if (ferror(file)) {
        printf("读取数据文件时发生错误。\n");
        fclose(file);
        return 0;
    }
    fclose(file);
    if (recordCount == 0 || maxYear != minYear + 1) {
        printf("文件必须包含连续的两个年份。\n");
        return 0;
    }
    firstYear = minYear;
    secondYear = maxYear;
    return 1;
}

/* 检查每户恰有两个完整年度，同时统计不同住户、小区的数量。
 * 前面已排除重复月且限定月份范围，所以每个年份 12 条就代表全年齐全。
 * “01/H01”和“02/H01”在这里会算作两户。
 */
static int validateCoverage(void)
{
    int householdCount = 0, areaCount = 0;
    for (int i = 0; i < recordCount; i++) {
        int seenHousehold = 0, seenArea = 0, year1Count = 0, year2Count = 0;
        for (int j = 0; j < i; j++) {
            if (records[j].areaId == records[i].areaId) {
                seenArea = 1;
                if (strcmp(records[j].household, records[i].household) == 0)
                    seenHousehold = 1;
            }
        }
        if (!seenArea) areaCount++;
        if (seenHousehold) continue;
        householdCount++;
        for (int j = 0; j < recordCount; j++) {
            if (records[j].areaId == records[i].areaId &&
                strcmp(records[j].household, records[i].household) == 0) {
                if (records[j].year == firstYear) year1Count++;
                if (records[j].year == secondYear) year2Count++;
            }
        }
        if (year1Count != 12 || year2Count != 12) {
            printf("小区 %d 住户 %s 缺少月份，两年各需 12 条。\n",
                   records[i].areaId, records[i].household);
            return 0;
        }
    }
    if (areaCount < 2 || areaCount > MAX_AREAS || householdCount < 50) {
        printf("要求 2 至 %d 个小区、合计至少 50 户。\n", MAX_AREAS);
        return 0;
    }
    printf("已加载 %d 条记录，%d 个小区，%d 户，年份 %d—%d。\n",
           recordCount, areaCount, householdCount, firstYear, secondYear);
    return 1;
}

/* 模块四：共享月度汇总。
 * 第一次遇到某小区就建立汇总项；以后遇到该小区继续累加。
 * 一户一月一条且加载时已经去重，因此每处理一条当月记录就增加一户。
 * 使用输出参数 out 返回整个数组，返回值表示实际的小区数量。
 */
static int summarizeMonth(int year, int month, AreaSummary out[])
{
    int count = 0;
    for (int i = 0; i < recordCount; i++) {
        Record *r = &records[i];
        int pos = 0;
        if (r->year != year || r->month != month) continue;
        // pos 用于查找当前小区在汇总数组中的位置，未找到就自增
        while (pos < count && out[pos].areaId != r->areaId) pos++;
        // 若一直未找到，自增到 pos 等于 count————当前小区还没有汇总项，建新项
        if (pos == count) {
            // 把新小区的汇总项初始化为零，避免未初始化的垃圾值影响后续累加
            memset(&out[count], 0, sizeof(out[count]));
            out[count].areaId = r->areaId;
            strcpy(out[count].areaName, r->areaName);
            count++;
        }
        out[pos].households++;
        out[pos].peak += r->peak;
        out[pos].total += r->peak + r->valley;
        out[pos].fee += r->peak * PEAK_PRICE + r->valley * VALLEY_PRICE;
    }
    /* 累加完成后再做除法，避免把峰谷合计误当作推荐所用的峰时电量。 */
    for (int i = 0; i < count; i++) {
        out[i].avgTotal = out[i].total / out[i].households;
        out[i].avgFee = out[i].fee / out[i].households;
        out[i].avgPeak = out[i].peak / out[i].households;
    }
    return count;
}

/* 模块五：按户查询年账单。
 * 使用“小区编号 + 户号 + 年份”共同筛选，逐月显示，最后给出全年合计。
 * 用两个长度为 12 的数组暂存每月电量，保证原文件乱序时也按月份显示。
 */
static void queryAnnualBill(void)
{
    int areaId, year, found = 0;
    char household[32];
    double peak[12] = {0}, valley[12] = {0}, total = 0, fee = 0;
    if (!readInt("小区编号：", 1, INT_MAX, &areaId)) return;
    if (!readLine("户号（区分大小写）：", household, sizeof(household))) return;
    if (!readInt("年份：", firstYear, secondYear, &year)) return;
    for (int i = 0; i < recordCount; i++) {
        Record *r = &records[i];
        if (r->areaId == areaId && strcmp(r->household, household) == 0 && r->year == year) {
            peak[r->month - 1] = r->peak;
            valley[r->month - 1] = r->valley;
            found++;
        }
    }
    if (!found) {
        printf("未查询到该小区中的住户，请核对小区编号和户号。\n");
        return;
    }
    printf("\n%d 年年账单：小区 %d，户号 %s\n", year, areaId, household);
    printf("月份  峰时(kWh)  谷时(kWh)  总电量(kWh)  电费(元)\n");
    for (int m = 0; m < 12; m++) {
        double monthlyTotal = peak[m] + valley[m];
        double monthlyFee = peak[m] * PEAK_PRICE + valley[m] * VALLEY_PRICE;
        total += monthlyTotal;
        fee += monthlyFee;
        printf("%02d    %9.2f  %9.2f  %11.2f  %8.2f\n",
               m + 1, peak[m], valley[m], monthlyTotal, monthlyFee);
    }
    printf("全年总电量：%.2f kWh；全年总电费：%.2f 元。\n", total, fee);
}

/* 模块六：排序。
 * 选择排序每轮找出剩余元素中应排在最前面的一项，与当前项交换。
 * mode=1/2/3 对应月报三个指标；mode=4 对应推荐增量。
 * 比较使用原始 double 值，不先把数值四舍五入为显示的两位小数。
 */
static double sortValue(const AreaSummary *a, int mode)
{
    if (mode == 1) return a->peak;
    if (mode == 2) return a->fee;
    if (mode == 3) return a->avgTotal;
    // mode == 4（例外） 用于推荐模块，按户均峰时增量排序
    return a->growth;
}

static void sortSummaries(AreaSummary items[], int count, int mode)
{
    for (int i = 0; i < count - 1; i++) {
        int best = i;
        for (int j = i + 1; j < count; j++) {
            double a = sortValue(&items[j], mode), b = sortValue(&items[best], mode);
            if (a > b || (a == b && items[j].areaId < items[best].areaId)) best = j;
        }
        if (best != i) {
            AreaSummary temp = items[i];
            items[i] = items[best];
            items[best] = temp;
        }
    }
}

/* 模块七：生成小区月报。户均电量包含峰、谷两部分。 */
static void showMonthlyReport(void)
{
    AreaSummary summaries[MAX_AREAS];
    int year, month, mode, count;
    if (!readInt("年份：", firstYear, secondYear, &year)) return;
    if (!readInt("月份：", 1, 12, &month)) return;
    if (!readInt("排序：1 峰时电量 / 2 总电费 / 3 户均电量：", 1, 3, &mode)) return;
    count = summarizeMonth(year, month, summaries);
    sortSummaries(summaries, count, mode);
    printf("\n%d-%02d 小区月报（电量 kWh；金额 元；各列以 | 分隔）\n", year, month);
    printf("编号 | 名称 | 户数 | 峰时合计 | 总电量 | 总电费 | 户均电量 | 户均电费\n");
    for (int i = 0; i < count; i++) {
        AreaSummary *s = &summaries[i];
        printf("%02d | %s | %d | %.2f | %.2f | %.2f | %.2f | %.2f\n",
               s->areaId, s->areaName, s->households, s->peak, s->total,
               s->fee, s->avgTotal, s->avgFee);
        /* 留白 A：可在这里增加“峰时占比”一列。提示见 README，先不实现。 */
    }
}

/* 模块八：筛选并推荐。
 * 顺序不能颠倒：汇总两年同月 -> 用第二年总电费筛选 -> 算户均峰时差
 * -> 增量降序、同增量编号升序 -> 输出前 K 个。
 * 题目没有要求增长必须为正，因此不擅自排除零增长或负增长的小区。
 */
static void recommendAreas(void)
{
    AreaSummary previous[MAX_AREAS], current[MAX_AREAS], candidates[MAX_AREAS];
    int month, k, previousCount, currentCount, candidateCount = 0;
    double threshold;
    if (!readInt("第二年的月份（1—12）：", 1, 12, &month)) return;
    if (!readThreshold(&threshold)) return; // readThreshold 自带提示词“当月最低总电费（元）：”
    if (!readInt("推荐数 K（正整数）：", 1, INT_MAX, &k)) return;
    previousCount = summarizeMonth(firstYear, month, previous);
    currentCount = summarizeMonth(secondYear, month, current);
    for (int i = 0; i < currentCount; i++) {
        /* 仅对金额门槛加极小容差，避免 0.60 的二进制表示误差排除恰好达标项。
         * 该容差不用于增量排序。后续金额计算可统一改为整数“分”。 */
        if (current[i].fee + 1e-7 < threshold) continue;
        for (int j = 0; j < previousCount; j++) {
            if (current[i].areaId == previous[j].areaId) {
                current[i].growth = current[i].avgPeak - previous[j].avgPeak;
                candidates[candidateCount++] = current[i];
                break;
            }
        }
    }
    if (candidateCount == 0) {
        printf("没有符合条件的小区。\n");
        return;
    }
    sortSummaries(candidates, candidateCount, 4);
    if (k > candidateCount) k = candidateCount;
    printf("\n%d 与 %d 年 %02d 月比较，门槛 %.2f 元，实际推荐 %d 个。\n",
           firstYear, secondYear, month, threshold, k);
    printf("排名 | 编号 | 名称 | 当月总电费(元) | 户均峰时增量(kWh)\n");
    for (int i = 0; i < k; i++) {
        printf("%d | %02d | %s | %.2f | %.2f\n", i + 1, candidates[i].areaId,
               candidates[i].areaName, candidates[i].fee, candidates[i].growth);
        /* 留白 B：在此按增量的正、零、负输出一句解释，但不能改变筛选规则。 */
        /* 留白 C：在此调用你自己写的字符条形图函数，帮助比较增长大小。 */
    }
}

/* 模块九：两类人员的操作菜单。这里只做角色分流，不是密码登录系统。
 * 返回上层时结束当前循环，避免递归调用菜单而逐渐占用调用栈。
 */
static void statisticianMenu(void)
{
    int choice;
    for (;;) {
        printf("\n--- 统计员菜单 ---\n1. 按户查询年账单\n2. 小区月报\n0. 返回角色选择\n");
        if (!readInt("请选择：", 0, 2, &choice) || choice == 0) return;
        if (choice == 1) queryAnnualBill();
        else showMonthlyReport();
    }
}

static void expansionMenu(void)
{
    int choice;
    for (;;) {
        printf("\n--- 扩容工作人员菜单 ---\n1. 筛选并推荐小区\n0. 返回角色选择\n");
        if (!readInt("请选择：", 0, 1, &choice) || choice == 0) return;
        recommendAreas();
    }
}

/* 主函数只负责组织流程：确定路径 -> 加载并检查 -> 选择身份 -> 退出。
 * 默认从工作目录下的 src/data.txt 读取；命令行第一个参数可替换路径。
 */
int main(int argc, char *argv[])
{
    const char *path = argc > 1 ? argv[1] : "src/data.txt";
    int role;
    printf("小区峰时用电增长与扩容推荐（参考初稿）\n");
    if (!loadData(path) || !validateCoverage()) return 1;
    for (;;) {
        printf("\n1. 统计员\n2. 扩容工作人员\n0. 退出\n");
        if (!readInt("选择角色：", 0, 2, &role) || role == 0) break;
        if (role == 1) statisticianMenu();
        else expansionMenu();
    }
    printf("程序已退出。\n");
    return 0;
}
