#ifndef DNB_ZM_H
#define DNB_ZM_H
/* DNB11xx 阻抗换算 (移植自原厂 DNB11xx_ArrayDef.c) */

/* 由设置频率的 Mantissa/Exponent 反查 CoeffMapArr 频率索引; 失败返回 -1 */
int dnb_zm_index(unsigned char mant, unsigned char exp);

/* 把 DNB 原始 Zreal/Zimag(M|E<<12) + VZM 原始 + 采样电阻档 + 频率索引
 * 换算成阻抗实部/虚部 (主机侧 /100000 = μΩ), 输出 64 位有符号。 */
void dnb_zm_convert(unsigned short re_raw, unsigned short im_raw, unsigned short vzm_raw,
                    unsigned char samp_sel, int idx, long long *zr, long long *zi);

#endif
