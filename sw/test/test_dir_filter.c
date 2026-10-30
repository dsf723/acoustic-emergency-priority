/*
 * PC 端测试：读入"块序号 方位 权重"序列，逐条加入方向滤波器并输出滤波结果
 * 用法：test_dir_filter <输入文本>
 */
#include <stdio.h>
#include "dir_filter.h"

int main(int argc, char **argv)
{
    FILE *fp;
    unsigned blk;
    float az, w;

    if (argc < 2 || !(fp = fopen(argv[1], "r")))
        return 1;
    dir_filter_reset();
    while (fscanf(fp, "%u %f %f", &blk, &az, &w) == 3) {
        dir_filter_add(az, w, blk);
        printf("%u %.2f\n", blk, dir_filter_get(blk));
    }
    fclose(fp);
    return 0;
}
