// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2020 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

static void conv3x3s1_winograd64_transform_kernel_pack8_fp16sa_neon(const Mat& kernel, Mat& kernel_tm_pack8, int inch, int outch)
{
    // winograd63 transform kernel
    Mat kernel_tm;
    kernel_tm.create(8 * 8, inch, outch);

    const float ktm[8][3] = {
        {1.0f, 0.0f, 0.0f},
        {-2.0f / 9, -2.0f / 9, -2.0f / 9},
        {-2.0f / 9, 2.0f / 9, -2.0f / 9},
        {1.0f / 90, 1.0f / 45, 2.0f / 45},
        {1.0f / 90, -1.0f / 45, 2.0f / 45},
        {1.0f / 45, 1.0f / 90, 1.0f / 180},
        {1.0f / 45, -1.0f / 90, 1.0f / 180},
        {0.0f, 0.0f, 1.0f}
    };

    #pragma omp parallel for
    for (int p = 0; p < outch; p++)
    {
        for (int q = 0; q < inch; q++)
        {
            const float* kernel0 = (const float*)kernel + p * inch * 9 + q * 9;
            float* kernel_tm0 = kernel_tm.channel(p).row(q);

            // transform kernel, transposed
            const float* k0 = kernel0;
            const float* k1 = kernel0 + 3;
            const float* k2 = kernel0 + 6;

            // h
            float tmp[8][3];
            for (int i = 0; i < 8; i++)
            {
                tmp[i][0] = k0[0] * ktm[i][0] + k0[1] * ktm[i][1] + k0[2] * ktm[i][2];
                tmp[i][1] = k1[0] * ktm[i][0] + k1[1] * ktm[i][1] + k1[2] * ktm[i][2];
                tmp[i][2] = k2[0] * ktm[i][0] + k2[1] * ktm[i][1] + k2[2] * ktm[i][2];
            }

            // v
            for (int j = 0; j < 8; j++)
            {
                float* tmpp = &tmp[j][0];

                for (int i = 0; i < 8; i++)
                {
                    kernel_tm0[j * 8 + i] = tmpp[0] * ktm[i][0] + tmpp[1] * ktm[i][1] + tmpp[2] * ktm[i][2];
                }
            }
        }
    }

    // interleave
    // src = 64-inch-outch
    // dst = 4b-4a-inch/4a-64-outch/4b;
    kernel_tm_pack8.create(inch / 8, 64, outch / 8, (size_t)2u * 64, 64);

    int q = 0;
    for (; q + 7 < outch; q += 8)
    {
        const Mat k0 = kernel_tm.channel(q);
        const Mat k1 = kernel_tm.channel(q + 1);
        const Mat k2 = kernel_tm.channel(q + 2);
        const Mat k3 = kernel_tm.channel(q + 3);
        const Mat k4 = kernel_tm.channel(q + 4);
        const Mat k5 = kernel_tm.channel(q + 5);
        const Mat k6 = kernel_tm.channel(q + 6);
        const Mat k7 = kernel_tm.channel(q + 7);

        Mat g0 = kernel_tm_pack8.channel(q / 8);

        for (int k = 0; k < 64; k++)
        {
            __fp16* g00 = g0.row<__fp16>(k);

            for (int p = 0; p + 7 < inch; p += 8)
            {
                for (int i = 0; i < 8; i++)
                {
                    const float* k00 = k0.row(p + i);
                    const float* k10 = k1.row(p + i);
                    const float* k20 = k2.row(p + i);
                    const float* k30 = k3.row(p + i);
                    const float* k40 = k4.row(p + i);
                    const float* k50 = k5.row(p + i);
                    const float* k60 = k6.row(p + i);
                    const float* k70 = k7.row(p + i);

                    g00[0] = (__fp16)k00[k];
                    g00[1] = (__fp16)k10[k];
                    g00[2] = (__fp16)k20[k];
                    g00[3] = (__fp16)k30[k];
                    g00[4] = (__fp16)k40[k];
                    g00[5] = (__fp16)k50[k];
                    g00[6] = (__fp16)k60[k];
                    g00[7] = (__fp16)k70[k];

                    g00 += 8;
                }
            }
        }
    }
}

static void conv3x3s1_winograd64_pack8_fp16sa_neon(const Mat& bottom_blob, Mat& top_blob, const Mat& kernel_tm, const Mat& _bias, const Option& opt)
{
    int w = bottom_blob.w;
    int h = bottom_blob.h;
    int inch = bottom_blob.c;
    size_t elemsize = bottom_blob.elemsize;
    int elempack = bottom_blob.elempack;

    int outw = top_blob.w;
    int outh = top_blob.h;
    int outch = top_blob.c;

    // pad to 6n+2
    Mat bottom_blob_bordered = bottom_blob;

    outw = (outw + 5) / 6 * 6;
    outh = (outh + 5) / 6 * 6;

    w = outw + 2;
    h = outh + 2;
    copy_make_border(bottom_blob, bottom_blob_bordered, 0, h - bottom_blob.h, 0, w - bottom_blob.w, BORDER_CONSTANT, 0.f, opt);

    const __fp16* bias = _bias;

    // BEGIN transform input
    Mat bottom_blob_tm;
    {
        int w_tm = outw / 6 * 8;
        int h_tm = outh / 6 * 8;

        const int tiles = w_tm / 8 * h_tm / 8;

        //         bottom_blob_tm.create(tiles, 64, inch, elemsize, elempack, opt.workspace_allocator);
        bottom_blob_tm.create(tiles, 64, inch, 2u * elempack, elempack, opt.workspace_allocator);

        //         const float itm[8][8] = {
        //             {1.0f,  0.0f, -5.25f,  0.00f,  5.25f,  0.00f, -1.0f, 0.0f},
        //
        //             {0.0f,  1.0f,  1.00f, -4.25f, -4.25f,  1.00f,  1.0f, 0.0f},
        //             {0.0f, -1.0f,  1.00f,  4.25f, -4.25f, -1.00f,  1.0f, 0.0f},
        //
        //             {0.0f,  0.5f,  0.25f, -2.50f, -1.25f,  2.00f,  1.0f, 0.0f},
        //             {0.0f, -0.5f,  0.25f,  2.50f, -1.25f, -2.00f,  1.0f, 0.0f},
        //
        //             {0.0f,  2.0f,  4.00f, -2.50f, -5.00f,  0.50f,  1.0f, 0.0f},
        //             {0.0f, -2.0f,  4.00f,  2.50f, -5.00f, -0.50f,  1.0f, 0.0f},
        //
        //             {0.0f, -1.0f,  0.00f,  5.25f,  0.00f, -5.25f,  0.0f, 1.0f}
        //         };

        // 0 = r00 - r06 + (r04 - r02) * 5.25
        // 7 = r07 - r01 + (r03 - r05) * 5.25

        // 1 = (r02 + r06 - r04 * 4.25) + (r01 - r03 * 4.25 + r05)
        // 2 = (r02 + r06 - r04 * 4.25) - (r01 - r03 * 4.25 + r05)

        // 3 = (r06 + r02 * 0.25 - r04 * 1.25) + (r01 * 0.5 - r03 * 2.5 + r05 * 2)
        // 4 = (r06 + r02 * 0.25 - r04 * 1.25) - (r01 * 0.5 - r03 * 2.5 + r05 * 2)

        // reuse r04 * 1.25
        // reuse r03 * 2.5
        // 5 = (r06 + (r02 - r04 * 1.25) * 4) + (r01 * 2 - r03 * 2.5 + r05 * 0.5)
        // 6 = (r06 + (r02 - r04 * 1.25) * 4) - (r01 * 2 - r03 * 2.5 + r05 * 0.5)

        #pragma omp parallel for num_threads(opt.num_threads)
        for (int q = 0; q < inch; q++)
        {
            const Mat img0 = bottom_blob_bordered.channel(q);
            Mat img0_tm = bottom_blob_tm.channel(q);

            __fp16 tmp[8][8][8];

            // tile
            for (int i = 0; i < h_tm / 8; i++)
            {
                for (int j = 0; j < w_tm / 8; j++)
                {
                    const __fp16* r0 = img0.row<const __fp16>(i * 6) + (j * 6) * 8;

                    for (int m = 0; m < 8; m++)
                    {
                        float16x8_t _r00 = vld1q_f16(r0);
                        float16x8_t _r01 = vld1q_f16(r0 + 8);
                        float16x8_t _r02 = vld1q_f16(r0 + 16);
                        float16x8_t _r03 = vld1q_f16(r0 + 24);
                        float16x8_t _r04 = vld1q_f16(r0 + 32);
                        float16x8_t _r05 = vld1q_f16(r0 + 40);
                        float16x8_t _r06 = vld1q_f16(r0 + 48);
                        float16x8_t _r07 = vld1q_f16(r0 + 56);

                        float16x8_t _tmp0m = vfmaq_n_f16(vsubq_f16(_r00, _r06), vsubq_f16(_r04, _r02), 5.25f);
                        float16x8_t _tmp7m = vfmaq_n_f16(vsubq_f16(_r07, _r01), vsubq_f16(_r03, _r05), 5.25f);
                        vst1q_f16(tmp[0][m], _tmp0m);
                        vst1q_f16(tmp[7][m], _tmp7m);

                        //                         tmp[0][m] = r0[0] - r0[6] + (r0[4] - r0[2]) * 5.25;
                        //                         tmp[7][m] = r0[7] - r0[1] + (r0[3] - r0[5]) * 5.25;

                        float16x8_t _tmp12a = vfmsq_n_f16(vaddq_f16(_r02, _r06), _r04, 4.25f);
                        float16x8_t _tmp12b = vfmsq_n_f16(vaddq_f16(_r01, _r05), _r03, 4.25f);

                        //                         float tmp12a = (r0[2] + r0[6] - r0[4] * 4.25);
                        //                         float tmp12b = (r0[1] + r0[5] - r0[3] * 4.25);

                        float16x8_t _tmp1m = vaddq_f16(_tmp12a, _tmp12b);
                        float16x8_t _tmp2m = vsubq_f16(_tmp12a, _tmp12b);
                        vst1q_f16(tmp[1][m], _tmp1m);
                        vst1q_f16(tmp[2][m], _tmp2m);

                        //                         tmp[1][m] = tmp12a + tmp12b;
                        //                         tmp[2][m] = tmp12a - tmp12b;

                        float16x8_t _tmp34a = vfmsq_n_f16(vfmaq_n_f16(_r06, _r02, 0.25f), _r04, 1.25f);
                        float16x8_t _tmp34b = vfmaq_n_f16(vfmsq_n_f16(vmulq_n_f16(_r01, 0.5f), _r03, 2.5f), _r05, 2.f);

                        //                         float tmp34a = (r0[6] + r0[2] * 0.25 - r0[4] * 1.25);
                        //                         float tmp34b = (r0[1] * 0.5 - r0[3] * 2.5 + r0[5] * 2);

                        float16x8_t _tmp3m = vaddq_f16(_tmp34a, _tmp34b);
                        float16x8_t _tmp4m = vsubq_f16(_tmp34a, _tmp34b);
                        vst1q_f16(tmp[3][m], _tmp3m);
                        vst1q_f16(tmp[4][m], _tmp4m);

                        //                         tmp[3][m] = tmp34a + tmp34b;
                        //                         tmp[4][m] = tmp34a - tmp34b;

                        float16x8_t _tmp56a = vfmaq_n_f16(_r06, vfmsq_n_f16(_r02, _r04, 1.25f), 4.f);
                        float16x8_t _tmp56b = vfmaq_n_f16(vfmsq_n_f16(vmulq_n_f16(_r01, 2.f), _r03, 2.5f), _r05, 0.5f);

                        //                         float tmp56a = (r0[6] + (r0[2] - r0[4] * 1.25) * 4);
                        //                         float tmp56b = (r0[1] * 2 - r0[3] * 2.5 + r0[5] * 0.5);

                        float16x8_t _tmp5m = vaddq_f16(_tmp56a, _tmp56b);
                        float16x8_t _tmp6m = vsubq_f16(_tmp56a, _tmp56b);
                        vst1q_f16(tmp[5][m], _tmp5m);
                        vst1q_f16(tmp[6][m], _tmp6m);

                        //                         tmp[5][m] = tmp56a + tmp56b;
                        //                         tmp[6][m] = tmp56a - tmp56b;

                        r0 += w * 8;
                    }

                    __fp16* r0_tm_0 = (__fp16*)img0_tm + (i * w_tm / 8 + j) * 8;
                    __fp16* r0_tm_1 = r0_tm_0 + tiles * 8;
                    __fp16* r0_tm_2 = r0_tm_0 + tiles * 16;
                    __fp16* r0_tm_3 = r0_tm_0 + tiles * 24;
                    __fp16* r0_tm_4 = r0_tm_0 + tiles * 32;
                    __fp16* r0_tm_5 = r0_tm_0 + tiles * 40;
                    __fp16* r0_tm_6 = r0_tm_0 + tiles * 48;
                    __fp16* r0_tm_7 = r0_tm_0 + tiles * 56;

                    for (int m = 0; m < 8; m++)
                    {
                        float16x8_t _tmp00 = vld1q_f16(tmp[m][0]);
                        float16x8_t _tmp01 = vld1q_f16(tmp[m][1]);
                        float16x8_t _tmp02 = vld1q_f16(tmp[m][2]);
                        float16x8_t _tmp03 = vld1q_f16(tmp[m][3]);
                        float16x8_t _tmp04 = vld1q_f16(tmp[m][4]);
                        float16x8_t _tmp05 = vld1q_f16(tmp[m][5]);
                        float16x8_t _tmp06 = vld1q_f16(tmp[m][6]);
                        float16x8_t _tmp07 = vld1q_f16(tmp[m][7]);

                        float16x8_t _r0tm0 = vfmaq_n_f16(vsubq_f16(_tmp00, _tmp06), vsubq_f16(_tmp04, _tmp02), 5.25f);
                        float16x8_t _r0tm7 = vfmaq_n_f16(vsubq_f16(_tmp07, _tmp01), vsubq_f16(_tmp03, _tmp05), 5.25f);

                        //                         r0_tm[0] = tmp0[0] - tmp0[6] + (tmp0[4] - tmp0[2]) * 5.25;
                        //                         r0_tm[7] = tmp0[7] - tmp0[1] + (tmp0[3] - tmp0[5]) * 5.25;

                        float16x8_t _tmp12a = vfmsq_n_f16(vaddq_f16(_tmp02, _tmp06), _tmp04, 4.25f);
                        float16x8_t _tmp12b = vfmsq_n_f16(vaddq_f16(_tmp01, _tmp05), _tmp03, 4.25f);

                        //                         float tmp12a = (tmp0[2] + tmp0[6] - tmp0[4] * 4.25);
                        //                         float tmp12b = (tmp0[1] + tmp0[5] - tmp0[3] * 4.25);

                        float16x8_t _r0tm1 = vaddq_f16(_tmp12a, _tmp12b);
                        float16x8_t _r0tm2 = vsubq_f16(_tmp12a, _tmp12b);

                        //                         r0_tm[1] = tmp12a + tmp12b;
                        //                         r0_tm[2] = tmp12a - tmp12b;

                        float16x8_t _tmp34a = vfmsq_n_f16(vfmaq_n_f16(_tmp06, _tmp02, 0.25f), _tmp04, 1.25f);
                        float16x8_t _tmp34b = vfmaq_n_f16(vfmsq_n_f16(vmulq_n_f16(_tmp01, 0.5f), _tmp03, 2.5f), _tmp05, 2.f);

                        //                         float tmp34a = (tmp0[6] + tmp0[2] * 0.25 - tmp0[4] * 1.25);
                        //                         float tmp34b = (tmp0[1] * 0.5 - tmp0[3] * 2.5 + tmp0[5] * 2);

                        float16x8_t _r0tm3 = vaddq_f16(_tmp34a, _tmp34b);
                        float16x8_t _r0tm4 = vsubq_f16(_tmp34a, _tmp34b);

                        //                         r0_tm[3] = tmp34a + tmp34b;
                        //                         r0_tm[4] = tmp34a - tmp34b;

                        float16x8_t _tmp56a = vfmaq_n_f16(_tmp06, vfmsq_n_f16(_tmp02, _tmp04, 1.25f), 4.f);
                        float16x8_t _tmp56b = vfmaq_n_f16(vfmsq_n_f16(vmulq_n_f16(_tmp01, 2.f), _tmp03, 2.5f), _tmp05, 0.5f);

                        //                         float tmp56a = (tmp0[6] + (tmp0[2] - tmp0[4] * 1.25) * 4);
                        //                         float tmp56b = (tmp0[1] * 2 - tmp0[3] * 2.5 + tmp0[5] * 0.5);

                        float16x8_t _r0tm5 = vaddq_f16(_tmp56a, _tmp56b);
                        float16x8_t _r0tm6 = vsubq_f16(_tmp56a, _tmp56b);

                        //                         r0_tm[5] = tmp56a + tmp56b;
                        //                         r0_tm[6] = tmp56a - tmp56b;

                        vst1q_f16(r0_tm_0, _r0tm0);
                        vst1q_f16(r0_tm_1, _r0tm1);
                        vst1q_f16(r0_tm_2, _r0tm2);
                        vst1q_f16(r0_tm_3, _r0tm3);
                        vst1q_f16(r0_tm_4, _r0tm4);
                        vst1q_f16(r0_tm_5, _r0tm5);
                        vst1q_f16(r0_tm_6, _r0tm6);
                        vst1q_f16(r0_tm_7, _r0tm7);

                        r0_tm_0 += tiles * 64;
                        r0_tm_1 += tiles * 64;
                        r0_tm_2 += tiles * 64;
                        r0_tm_3 += tiles * 64;
                        r0_tm_4 += tiles * 64;
                        r0_tm_5 += tiles * 64;
                        r0_tm_6 += tiles * 64;
                        r0_tm_7 += tiles * 64;
                    }
                }
            }
        }
    }
    bottom_blob_bordered = Mat();
    // END transform input

    // BEGIN dot
    Mat top_blob_tm;
    {
        int w_tm = outw / 6 * 8;
        int h_tm = outh / 6 * 8;

        const int tiles = h_tm / 8 * w_tm / 8;

        // permute
        //         bottom_blob_tm.create(tiles, 64, inch, elemsize, elempack, opt.workspace_allocator);
        Mat bottom_blob_tm2;
        if (tiles >= 8)
            bottom_blob_tm2.create(8 * inch, tiles / 8 + (tiles % 8) / 4 + (tiles % 4) / 2 + tiles % 2, 64, 2u * elempack, elempack, opt.workspace_allocator);
        else if (tiles >= 4)
            bottom_blob_tm2.create(4 * inch, tiles / 4 + (tiles % 4) / 2 + tiles % 2, 64, 2u * elempack, elempack, opt.workspace_allocator);
        else if (tiles >= 2)
            bottom_blob_tm2.create(2 * inch, tiles / 2 + tiles % 2, 64, 2u * elempack, elempack, opt.workspace_allocator);
        else // if (tiles >= 1)
            bottom_blob_tm2.create(1 * inch, tiles, 64, 2u * elempack, elempack, opt.workspace_allocator);

        #pragma omp parallel for num_threads(opt.num_threads)
        for (int r = 0; r < 64; r++)
        {
            Mat tm2 = bottom_blob_tm2.channel(r);

            // tile
            int i = 0;
            for (; i + 7 < tiles; i += 8)
            {
                __fp16* tmpptr = tm2.row<__fp16>(i / 8);

                const __fp16* r0 = bottom_blob_tm;

                r0 += (r * tiles + i) * 8;

                for (int q = 0; q < inch; q++)
                {
                    float16x8_t _v0 = vld1q_f16(r0);
                    float16x8_t _v1 = vld1q_f16(r0 + 8);
                    float16x8_t _v2 = vld1q_f16(r0 + 16);
                    float16x8_t _v3 = vld1q_f16(r0 + 24);
                    float16x8_t _v4 = vld1q_f16(r0 + 32);
                    float16x8_t _v5 = vld1q_f16(r0 + 40);
                    float16x8_t _v6 = vld1q_f16(r0 + 48);
                    float16x8_t _v7 = vld1q_f16(r0 + 56);
                    vst1q_f16(tmpptr, _v0);
                    vst1q_f16(tmpptr + 8, _v1);
                    vst1q_f16(tmpptr + 16, _v2);
                    vst1q_f16(tmpptr + 24, _v3);
                    vst1q_f16(tmpptr + 32, _v4);
                    vst1q_f16(tmpptr + 40, _v5);
                    vst1q_f16(tmpptr + 48, _v6);
                    vst1q_f16(tmpptr + 56, _v7);

                    tmpptr += 64;
                    r0 += bottom_blob_tm.cstep * 8;
                }
            }
            for (; i + 3 < tiles; i += 4)
            {
                __fp16* tmpptr = tm2.row<__fp16>(i / 8 + (i % 8) / 4);

                const __fp16* r0 = bottom_blob_tm;

                r0 += (r * tiles + i) * 8;

                for (int q = 0; q < inch; q++)
                {
                    float16x8_t _v0 = vld1q_f16(r0);
                    float16x8_t _v1 = vld1q_f16(r0 + 8);
                    float16x8_t _v2 = vld1q_f16(r0 + 16);
                    float16x8_t _v3 = vld1q_f16(r0 + 24);
                    vst1q_f16(tmpptr, _v0);
                    vst1q_f16(tmpptr + 8, _v1);
                    vst1q_f16(tmpptr + 16, _v2);
                    vst1q_f16(tmpptr + 24, _v3);

                    tmpptr += 32;
                    r0 += bottom_blob_tm.cstep * 8;
                }
            }
            for (; i + 1 < tiles; i += 2)
            {
                __fp16* tmpptr = tm2.row<__fp16>(i / 8 + (i % 8) / 4 + (i % 4) / 2);

                const __fp16* r0 = bottom_blob_tm;

                r0 += (r * tiles + i) * 8;

                for (int q = 0; q < inch; q++)
                {
                    float16x8_t _v0 = vld1q_f16(r0);
                    float16x8_t _v1 = vld1q_f16(r0 + 8);
                    vst1q_f16(tmpptr, _v0);
                    vst1q_f16(tmpptr + 8, _v1);

                    tmpptr += 16;
                    r0 += bottom_blob_tm.cstep * 8;
                }
            }
            for (; i < tiles; i++)
            {
                __fp16* tmpptr = tm2.row<__fp16>(i / 8 + (i % 8) / 4 + (i % 4) / 2 + i % 2);

                const __fp16* r0 = bottom_blob_tm;

                r0 += (r * tiles + i) * 8;

                for (int q = 0; q < inch; q++)
                {
                    float16x8_t _v = vld1q_f16(r0);
                    vst1q_f16(tmpptr, _v);

                    tmpptr += 8;
                    r0 += bottom_blob_tm.cstep * 8;
                }
            }
        }

        bottom_blob_tm = Mat();
        // permute end

        top_blob_tm.create(tiles, 64, outch, 2u * elempack, elempack, opt.workspace_allocator);

        #pragma omp parallel for num_threads(opt.num_threads)
        for (int p = 0; p < outch; p++)
        {
            __fp16* output0_tm = top_blob_tm.channel(p);

            const Mat kernel0_tm = kernel_tm.channel(p);

            for (int r = 0; r < 64; r++)
            {
                const Mat bb2 = bottom_blob_tm2.channel(r);

                int i = 0;
                for (; i + 7 < tiles; i += 8)
                {
                    const __fp16* r0 = bb2.row<const __fp16>(i / 8);
                    const __fp16* k0 = kernel0_tm.row<const __fp16>(r);

                    float16x8_t _sum0 = vdupq_n_f16((__fp16)0.f);
                    float16x8_t _sum1 = vdupq_n_f16((__fp16)0.f);
                    float16x8_t _sum2 = vdupq_n_f16((__fp16)0.f);
                    float16x8_t _sum3 = vdupq_n_f16((__fp16)0.f);
                    float16x8_t _sum4 = vdupq_n_f16((__fp16)0.f);
                    float16x8_t _sum5 = vdupq_n_f16((__fp16)0.f);
                    float16x8_t _sum6 = vdupq_n_f16((__fp16)0.f);
                    float16x8_t _sum7 = vdupq_n_f16((__fp16)0.f);

                    for (int q=0; q<inch; q++)
                    {
                        float16x8_t _v0 = vld1q_f16(r0);
                        float16x8_t _v1 = vld1q_f16(r0 + 8);
                        float16x8_t _v2 = vld1q_f16(r0 + 16);
                        float16x8_t _v3 = vld1q_f16(r0 + 24);
                        float16x8_t _v4 = vld1q_f16(r0 + 32);
                        float16x8_t _v5 = vld1q_f16(r0 + 40);
                        float16x8_t _v6 = vld1q_f16(r0 + 48);
                        float16x8_t _v7 = vld1q_f16(r0 + 56);

                        float16x8_t _k0 = vld1q_f16(k0);
                        float16x8_t _k1 = vld1q_f16(k0 + 8);
                        float16x8_t _k2 = vld1q_f16(k0 + 16);
                        float16x8_t _k3 = vld1q_f16(k0 + 24);
                        float16x8_t _k4 = vld1q_f16(k0 + 32);
                        float16x8_t _k5 = vld1q_f16(k0 + 40);
                        float16x8_t _k6 = vld1q_f16(k0 + 48);
                        float16x8_t _k7 = vld1q_f16(k0 + 56);

                        _sum0 = vfmaq_laneq_f16(_sum0, _k0, _v0, 0);
                        _sum0 = vfmaq_laneq_f16(_sum0, _k1, _v0, 1);
                        _sum0 = vfmaq_laneq_f16(_sum0, _k2, _v0, 2);
                        _sum0 = vfmaq_laneq_f16(_sum0, _k3, _v0, 3);
                        _sum0 = vfmaq_laneq_f16(_sum0, _k4, _v0, 4);
                        _sum0 = vfmaq_laneq_f16(_sum0, _k5, _v0, 5);
                        _sum0 = vfmaq_laneq_f16(_sum0, _k6, _v0, 6);
                        _sum0 = vfmaq_laneq_f16(_sum0, _k7, _v0, 7);

                        _sum1 = vfmaq_laneq_f16(_sum1, _k0, _v1, 0);
                        _sum1 = vfmaq_laneq_f16(_sum1, _k1, _v1, 1);
                        _sum1 = vfmaq_laneq_f16(_sum1, _k2, _v1, 2);
                        _sum1 = vfmaq_laneq_f16(_sum1, _k3, _v1, 3);
                        _sum1 = vfmaq_laneq_f16(_sum1, _k4, _v1, 4);
                        _sum1 = vfmaq_laneq_f16(_sum1, _k5, _v1, 5);
                        _sum1 = vfmaq_laneq_f16(_sum1, _k6, _v1, 6);
                        _sum1 = vfmaq_laneq_f16(_sum1, _k7, _v1, 7);

                        _sum2 = vfmaq_laneq_f16(_sum2, _k0, _v2, 0);
                        _sum2 = vfmaq_laneq_f16(_sum2, _k1, _v2, 1);
                        _sum2 = vfmaq_laneq_f16(_sum2, _k2, _v2, 2);
                        _sum2 = vfmaq_laneq_f16(_sum2, _k3, _v2, 3);
                        _sum2 = vfmaq_laneq_f16(_sum2, _k4, _v2, 4);
                        _sum2 = vfmaq_laneq_f16(_sum2, _k5, _v2, 5);
                        _sum2 = vfmaq_laneq_f16(_sum2, _k6, _v2, 6);
                        _sum2 = vfmaq_laneq_f16(_sum2, _k7, _v2, 7);

                        _sum3 = vfmaq_laneq_f16(_sum3, _k0, _v3, 0);
                        _sum3 = vfmaq_laneq_f16(_sum3, _k1, _v3, 1);
                        _sum3 = vfmaq_laneq_f16(_sum3, _k2, _v3, 2);
                        _sum3 = vfmaq_laneq_f16(_sum3, _k3, _v3, 3);
                        _sum3 = vfmaq_laneq_f16(_sum3, _k4, _v3, 4);
                        _sum3 = vfmaq_laneq_f16(_sum3, _k5, _v3, 5);
                        _sum3 = vfmaq_laneq_f16(_sum3, _k6, _v3, 6);
                        _sum3 = vfmaq_laneq_f16(_sum3, _k7, _v3, 7);

                        _sum4 = vfmaq_laneq_f16(_sum4, _k0, _v4, 0);
                        _sum4 = vfmaq_laneq_f16(_sum4, _k1, _v4, 1);
                        _sum4 = vfmaq_laneq_f16(_sum4, _k2, _v4, 2);
                        _sum4 = vfmaq_laneq_f16(_sum4, _k3, _v4, 3);
                        _sum4 = vfmaq_laneq_f16(_sum4, _k4, _v4, 4);
                        _sum4 = vfmaq_laneq_f16(_sum4, _k5, _v4, 5);
                        _sum4 = vfmaq_laneq_f16(_sum4, _k6, _v4, 6);
                        _sum4 = vfmaq_laneq_f16(_sum4, _k7, _v4, 7);

                        _sum5 = vfmaq_laneq_f16(_sum5, _k0, _v5, 0);
                        _sum5 = vfmaq_laneq_f16(_sum5, _k1, _v5, 1);
                        _sum5 = vfmaq_laneq_f16(_sum5, _k2, _v5, 2);
                        _sum5 = vfmaq_laneq_f16(_sum5, _k3, _v5, 3);
                        _sum5 = vfmaq_laneq_f16(_sum5, _k4, _v5, 4);
                        _sum5 = vfmaq_laneq_f16(_sum5, _k5, _v5, 5);
                        _sum5 = vfmaq_laneq_f16(_sum5, _k6, _v5, 6);
                        _sum5 = vfmaq_laneq_f16(_sum5, _k7, _v5, 7);

                        _sum6 = vfmaq_laneq_f16(_sum6, _k0, _v6, 0);
                        _sum6 = vfmaq_laneq_f16(_sum6, _k1, _v6, 1);
                        _sum6 = vfmaq_laneq_f16(_sum6, _k2, _v6, 2);
                        _sum6 = vfmaq_laneq_f16(_sum6, _k3, _v6, 3);
                        _sum6 = vfmaq_laneq_f16(_sum6, _k4, _v6, 4);
                        _sum6 = vfmaq_laneq_f16(_sum6, _k5, _v6, 5);
                        _sum6 = vfmaq_laneq_f16(_sum6, _k6, _v6, 6);
                        _sum6 = vfmaq_laneq_f16(_sum6, _k7, _v6, 7);

                        _sum7 = vfmaq_laneq_f16(_sum7, _k0, _v7, 0);
                        _sum7 = vfmaq_laneq_f16(_sum7, _k1, _v7, 1);
                        _sum7 = vfmaq_laneq_f16(_sum7, _k2, _v7, 2);
                        _sum7 = vfmaq_laneq_f16(_sum7, _k3, _v7, 3);
                        _sum7 = vfmaq_laneq_f16(_sum7, _k4, _v7, 4);
                        _sum7 = vfmaq_laneq_f16(_sum7, _k5, _v7, 5);
                        _sum7 = vfmaq_laneq_f16(_sum7, _k6, _v7, 6);
                        _sum7 = vfmaq_laneq_f16(_sum7, _k7, _v7, 7);

                        r0 += 64;
                        k0 += 64;
                    }

                    vst1q_f16(output0_tm, _sum0);
                    vst1q_f16(output0_tm + 8, _sum1);
                    vst1q_f16(output0_tm + 16, _sum2);
                    vst1q_f16(output0_tm + 24, _sum3);
                    vst1q_f16(output0_tm + 32, _sum4);
                    vst1q_f16(output0_tm + 40, _sum5);
                    vst1q_f16(output0_tm + 48, _sum6);
                    vst1q_f16(output0_tm + 56, _sum7);

                    output0_tm += 64;
                }
                for (; i + 3 < tiles; i += 4)
                {
                    const __fp16* r0 = bb2.row<const __fp16>(i / 8 + (i % 8) / 4);
                    const __fp16* k0 = kernel0_tm.row<const __fp16>(r);

                    float16x8_t _sum0 = vdupq_n_f16((__fp16)0.f);
                    float16x8_t _sum1 = vdupq_n_f16((__fp16)0.f);
                    float16x8_t _sum2 = vdupq_n_f16((__fp16)0.f);
                    float16x8_t _sum3 = vdupq_n_f16((__fp16)0.f);

                    for (int q=0; q<inch; q++)
                    {
                        float16x8_t _v0 = vld1q_f16(r0);
                        float16x8_t _v1 = vld1q_f16(r0 + 8);
                        float16x8_t _v2 = vld1q_f16(r0 + 16);
                        float16x8_t _v3 = vld1q_f16(r0 + 24);

                        float16x8_t _k0 = vld1q_f16(k0);
                        float16x8_t _k1 = vld1q_f16(k0 + 8);
                        float16x8_t _k2 = vld1q_f16(k0 + 16);
                        float16x8_t _k3 = vld1q_f16(k0 + 24);
                        float16x8_t _k4 = vld1q_f16(k0 + 32);
                        float16x8_t _k5 = vld1q_f16(k0 + 40);
                        float16x8_t _k6 = vld1q_f16(k0 + 48);
                        float16x8_t _k7 = vld1q_f16(k0 + 56);

                        _sum0 = vfmaq_laneq_f16(_sum0, _k0, _v0, 0);
                        _sum0 = vfmaq_laneq_f16(_sum0, _k1, _v0, 1);
                        _sum0 = vfmaq_laneq_f16(_sum0, _k2, _v0, 2);
                        _sum0 = vfmaq_laneq_f16(_sum0, _k3, _v0, 3);
                        _sum0 = vfmaq_laneq_f16(_sum0, _k4, _v0, 4);
                        _sum0 = vfmaq_laneq_f16(_sum0, _k5, _v0, 5);
                        _sum0 = vfmaq_laneq_f16(_sum0, _k6, _v0, 6);
                        _sum0 = vfmaq_laneq_f16(_sum0, _k7, _v0, 7);

                        _sum1 = vfmaq_laneq_f16(_sum1, _k0, _v1, 0);
                        _sum1 = vfmaq_laneq_f16(_sum1, _k1, _v1, 1);
                        _sum1 = vfmaq_laneq_f16(_sum1, _k2, _v1, 2);
                        _sum1 = vfmaq_laneq_f16(_sum1, _k3, _v1, 3);
                        _sum1 = vfmaq_laneq_f16(_sum1, _k4, _v1, 4);
                        _sum1 = vfmaq_laneq_f16(_sum1, _k5, _v1, 5);
                        _sum1 = vfmaq_laneq_f16(_sum1, _k6, _v1, 6);
                        _sum1 = vfmaq_laneq_f16(_sum1, _k7, _v1, 7);

                        _sum2 = vfmaq_laneq_f16(_sum2, _k0, _v2, 0);
                        _sum2 = vfmaq_laneq_f16(_sum2, _k1, _v2, 1);
                        _sum2 = vfmaq_laneq_f16(_sum2, _k2, _v2, 2);
                        _sum2 = vfmaq_laneq_f16(_sum2, _k3, _v2, 3);
                        _sum2 = vfmaq_laneq_f16(_sum2, _k4, _v2, 4);
                        _sum2 = vfmaq_laneq_f16(_sum2, _k5, _v2, 5);
                        _sum2 = vfmaq_laneq_f16(_sum2, _k6, _v2, 6);
                        _sum2 = vfmaq_laneq_f16(_sum2, _k7, _v2, 7);

                        _sum3 = vfmaq_laneq_f16(_sum3, _k0, _v3, 0);
                        _sum3 = vfmaq_laneq_f16(_sum3, _k1, _v3, 1);
                        _sum3 = vfmaq_laneq_f16(_sum3, _k2, _v3, 2);
                        _sum3 = vfmaq_laneq_f16(_sum3, _k3, _v3, 3);
                        _sum3 = vfmaq_laneq_f16(_sum3, _k4, _v3, 4);
                        _sum3 = vfmaq_laneq_f16(_sum3, _k5, _v3, 5);
                        _sum3 = vfmaq_laneq_f16(_sum3, _k6, _v3, 6);
                        _sum3 = vfmaq_laneq_f16(_sum3, _k7, _v3, 7);

                        r0 += 32;
                        k0 += 64;
                    }

                    vst1q_f16(output0_tm, _sum0);
                    vst1q_f16(output0_tm + 8, _sum1);
                    vst1q_f16(output0_tm + 16, _sum2);
                    vst1q_f16(output0_tm + 24, _sum3);

                    output0_tm += 32;
                }
                for (; i + 1 < tiles; i += 2)
                {
                    const __fp16* r0 = bb2.row<const __fp16>(i / 8 + (i % 8) / 4 + (i % 4) / 2);
                    const __fp16* k0 = kernel0_tm.row<const __fp16>(r);

                    float16x8_t _sum0 = vdupq_n_f16((__fp16)0.f);
                    float16x8_t _sum1 = vdupq_n_f16((__fp16)0.f);

                    for (int q=0; q<inch; q++)
                    {
                        float16x8_t _v0 = vld1q_f16(r0);
                        float16x8_t _v1 = vld1q_f16(r0 + 8);

                        float16x8_t _k0 = vld1q_f16(k0);
                        float16x8_t _k1 = vld1q_f16(k0 + 8);
                        float16x8_t _k2 = vld1q_f16(k0 + 16);
                        float16x8_t _k3 = vld1q_f16(k0 + 24);
                        float16x8_t _k4 = vld1q_f16(k0 + 32);
                        float16x8_t _k5 = vld1q_f16(k0 + 40);
                        float16x8_t _k6 = vld1q_f16(k0 + 48);
                        float16x8_t _k7 = vld1q_f16(k0 + 56);

                        _sum0 = vfmaq_laneq_f16(_sum0, _k0, _v0, 0);
                        _sum0 = vfmaq_laneq_f16(_sum0, _k1, _v0, 1);
                        _sum0 = vfmaq_laneq_f16(_sum0, _k2, _v0, 2);
                        _sum0 = vfmaq_laneq_f16(_sum0, _k3, _v0, 3);
                        _sum0 = vfmaq_laneq_f16(_sum0, _k4, _v0, 4);
                        _sum0 = vfmaq_laneq_f16(_sum0, _k5, _v0, 5);
                        _sum0 = vfmaq_laneq_f16(_sum0, _k6, _v0, 6);
                        _sum0 = vfmaq_laneq_f16(_sum0, _k7, _v0, 7);

                        _sum1 = vfmaq_laneq_f16(_sum1, _k0, _v1, 0);
                        _sum1 = vfmaq_laneq_f16(_sum1, _k1, _v1, 1);
                        _sum1 = vfmaq_laneq_f16(_sum1, _k2, _v1, 2);
                        _sum1 = vfmaq_laneq_f16(_sum1, _k3, _v1, 3);
                        _sum1 = vfmaq_laneq_f16(_sum1, _k4, _v1, 4);
                        _sum1 = vfmaq_laneq_f16(_sum1, _k5, _v1, 5);
                        _sum1 = vfmaq_laneq_f16(_sum1, _k6, _v1, 6);
                        _sum1 = vfmaq_laneq_f16(_sum1, _k7, _v1, 7);

                        r0 += 16;
                        k0 += 64;
                    }

                    vst1q_f16(output0_tm, _sum0);
                    vst1q_f16(output0_tm + 8, _sum1);

                    output0_tm += 16;
                }
                for (; i < tiles; i++)
                {
                    const __fp16* r0 = bb2.row<const __fp16>(i / 8 + (i % 8) / 4 + (i % 4) / 2 + i % 2);
                    const __fp16* k0 = kernel0_tm.row<const __fp16>(r);

                    float16x8_t _sum = vdupq_n_f16((__fp16)0.f);

                    for (int q=0; q<inch; q++)
                    {
                        float16x8_t _v = vld1q_f16(r0);

                        float16x8_t _k0 = vld1q_f16(k0);
                        float16x8_t _k1 = vld1q_f16(k0 + 8);
                        float16x8_t _k2 = vld1q_f16(k0 + 16);
                        float16x8_t _k3 = vld1q_f16(k0 + 24);
                        float16x8_t _k4 = vld1q_f16(k0 + 32);
                        float16x8_t _k5 = vld1q_f16(k0 + 40);
                        float16x8_t _k6 = vld1q_f16(k0 + 48);
                        float16x8_t _k7 = vld1q_f16(k0 + 56);

                        _sum = vfmaq_laneq_f16(_sum, _k0, _v, 0);
                        _sum = vfmaq_laneq_f16(_sum, _k1, _v, 1);
                        _sum = vfmaq_laneq_f16(_sum, _k2, _v, 2);
                        _sum = vfmaq_laneq_f16(_sum, _k3, _v, 3);
                        _sum = vfmaq_laneq_f16(_sum, _k4, _v, 4);
                        _sum = vfmaq_laneq_f16(_sum, _k5, _v, 5);
                        _sum = vfmaq_laneq_f16(_sum, _k6, _v, 6);
                        _sum = vfmaq_laneq_f16(_sum, _k7, _v, 7);

                        r0 += 8;
                        k0 += 64;
                    }

                    vst1q_f16(output0_tm, _sum);

                    output0_tm += 8;
                }
            }
        }
    }
    bottom_blob_tm = Mat();
    // END dot

    // BEGIN transform output
    Mat top_blob_bordered;
    if (outw == top_blob.w && outh == top_blob.h)
    {
        top_blob_bordered = top_blob;
    }
    else
    {
        top_blob_bordered.create(outw, outh, outch, elemsize, elempack, opt.workspace_allocator);
    }
    {
        //         const float otm[6][8] = {
        //             {1.0f,  1.0f,   1.0f,   1.0f,   1.0f,  32.0f, 32.0f, 0.0f},
        //             {0.0f,  1.0f,  -1.0f,   2.0f,  -2.0f,  16.0f,-16.0f, 0.0f},
        //             {0.0f,  1.0f,   1.0f,   4.0f,   4.0f,   8.0f,  8.0f, 0.0f},
        //             {0.0f,  1.0f,  -1.0f,   8.0f,  -8.0f,   4.0f, -4.0f, 0.0f},
        //             {0.0f,  1.0f,   1.0f,  16.0f,  16.0f,   2.0f,  2.0f, 0.0f},
        //             {0.0f,  1.0f,  -1.0f,  32.0f, -32.0f,   1.0f, -1.0f, 1.0f}
        //         };

        // 0 = r0 + (r1 + r2) + (r3 + r4)     + (r5 + r6) * 32
        // 1 =      (r1 - r2) + (r3 - r4) * 2 + (r5 - r6) * 16
        // 2 =      (r1 + r2) + (r3 + r4) * 4 + (r5 + r6) * 8
        // 3 =      (r1 - r2) + (r3 - r4) * 8 + (r5 - r6) * 4
        // 4 =      (r1 + r2) + (r3 + r4) * 16+ (r5 + r6) * 2
        // 5 = r7 + (r1 - r2) + (r3 - r4) * 32+ (r5 - r6)

        int w_tm = outw / 6 * 8;
        int h_tm = outh / 6 * 8;
        const int tiles = w_tm / 8 * h_tm / 8;

        #pragma omp parallel for num_threads(opt.num_threads)
        for (int p = 0; p < outch; p++)
        {
            const Mat out0_tm = top_blob_tm.channel(p);
            Mat out0 = top_blob_bordered.channel(p);

            //             const float bias0 = bias ? bias[p] : 0.f;
            float16x8_t _bias0 = bias ? vld1q_f16((const __fp16*)bias + p * 8) : vdupq_n_f16(0.f);

            __fp16 tmp[6][8][8];

            // tile
            for (int i = 0; i < outh / 6; i++)
            {
                for (int j = 0; j < outw / 6; j++)
                {
                    //                     top_blob_tm.create(tiles, 64, outch, elemsize, elempack);

                    const __fp16* output0_tm_0 = (const __fp16*)out0_tm + (i * w_tm / 8 + j) * 8;
                    const __fp16* output0_tm_1 = output0_tm_0 + tiles * 8;
                    const __fp16* output0_tm_2 = output0_tm_0 + tiles * 16;
                    const __fp16* output0_tm_3 = output0_tm_0 + tiles * 24;
                    const __fp16* output0_tm_4 = output0_tm_0 + tiles * 32;
                    const __fp16* output0_tm_5 = output0_tm_0 + tiles * 40;
                    const __fp16* output0_tm_6 = output0_tm_0 + tiles * 48;
                    const __fp16* output0_tm_7 = output0_tm_0 + tiles * 56;

                    __fp16* output0 = out0.row<__fp16>(i * 6) + (j * 6) * 8;

                    // TODO neon optimize
                    for (int m = 0; m < 8; m++)
                    {
                        float16x8_t _out0tm0 = vld1q_f16(output0_tm_0);
                        float16x8_t _out0tm1 = vld1q_f16(output0_tm_1);
                        float16x8_t _out0tm2 = vld1q_f16(output0_tm_2);
                        float16x8_t _out0tm3 = vld1q_f16(output0_tm_3);
                        float16x8_t _out0tm4 = vld1q_f16(output0_tm_4);
                        float16x8_t _out0tm5 = vld1q_f16(output0_tm_5);
                        float16x8_t _out0tm6 = vld1q_f16(output0_tm_6);
                        float16x8_t _out0tm7 = vld1q_f16(output0_tm_7);

                        float16x8_t _tmp024a = vaddq_f16(_out0tm1, _out0tm2);
                        float16x8_t _tmp135a = vsubq_f16(_out0tm1, _out0tm2);

                        //                         float tmp024a = output0_tm[1] + output0_tm[2];
                        //                         float tmp135a = output0_tm[1] - output0_tm[2];

                        float16x8_t _tmp024b = vaddq_f16(_out0tm3, _out0tm4);
                        float16x8_t _tmp135b = vsubq_f16(_out0tm3, _out0tm4);

                        //                         float tmp024b = output0_tm[3] + output0_tm[4];
                        //                         float tmp135b = output0_tm[3] - output0_tm[4];

                        float16x8_t _tmp024c = vaddq_f16(_out0tm5, _out0tm6);
                        float16x8_t _tmp135c = vsubq_f16(_out0tm5, _out0tm6);

                        //                         float tmp024c = output0_tm[5] + output0_tm[6];
                        //                         float tmp135c = output0_tm[5] - output0_tm[6];

                        float16x8_t _tmp0m = vaddq_f16(vaddq_f16(_out0tm0, _tmp024a), vfmaq_n_f16(_tmp024b, _tmp024c, 32.f));
                        float16x8_t _tmp2m = vfmaq_n_f16(vfmaq_n_f16(_tmp024a, _tmp024b, 4.f), _tmp024c, 8.f);
                        float16x8_t _tmp4m = vfmaq_n_f16(vfmaq_n_f16(_tmp024a, _tmp024b, 16.f), _tmp024c, 2.f);
                        vst1q_f16(tmp[0][m], _tmp0m);
                        vst1q_f16(tmp[2][m], _tmp2m);
                        vst1q_f16(tmp[4][m], _tmp4m);

                        //                         tmp[0][m] = output0_tm[0] + tmp024a + tmp024b + tmp024c * 32;
                        //                         tmp[2][m] = tmp024a + tmp024b * 4 + tmp024c * 8;
                        //                         tmp[4][m] = tmp024a + tmp024b * 16 + tmp024c + tmp024c;

                        float16x8_t _tmp1m = vfmaq_n_f16(vfmaq_n_f16(_tmp135a, _tmp135b, 2.f), _tmp135c, 16.f);
                        float16x8_t _tmp3m = vfmaq_n_f16(vfmaq_n_f16(_tmp135a, _tmp135b, 8.f), _tmp135c, 4.f);
                        float16x8_t _tmp5m = vaddq_f16(vaddq_f16(_out0tm7, _tmp135a), vfmaq_n_f16(_tmp135c, _tmp135b, 32.f));
                        vst1q_f16(tmp[1][m], _tmp1m);
                        vst1q_f16(tmp[3][m], _tmp3m);
                        vst1q_f16(tmp[5][m], _tmp5m);

                        //                         tmp[1][m] = tmp135a + tmp135b + tmp135b + tmp135c * 16;
                        //                         tmp[3][m] = tmp135a + tmp135b * 8 + tmp135c * 4;
                        //                         tmp[5][m] = output0_tm[7] + tmp135a + tmp135b * 32 + tmp135c;

                        output0_tm_0 += tiles * 64;
                        output0_tm_1 += tiles * 64;
                        output0_tm_2 += tiles * 64;
                        output0_tm_3 += tiles * 64;
                        output0_tm_4 += tiles * 64;
                        output0_tm_5 += tiles * 64;
                        output0_tm_6 += tiles * 64;
                        output0_tm_7 += tiles * 64;
                    }

                    for (int m = 0; m < 6; m++)
                    {
                        float16x8_t _tmp00 = vld1q_f16(tmp[m][0]);
                        float16x8_t _tmp01 = vld1q_f16(tmp[m][1]);
                        float16x8_t _tmp02 = vld1q_f16(tmp[m][2]);
                        float16x8_t _tmp03 = vld1q_f16(tmp[m][3]);
                        float16x8_t _tmp04 = vld1q_f16(tmp[m][4]);
                        float16x8_t _tmp05 = vld1q_f16(tmp[m][5]);
                        float16x8_t _tmp06 = vld1q_f16(tmp[m][6]);
                        float16x8_t _tmp07 = vld1q_f16(tmp[m][7]);

                        float16x8_t _tmp024a = vaddq_f16(_tmp01, _tmp02);
                        float16x8_t _tmp135a = vsubq_f16(_tmp01, _tmp02);

                        //                         float tmp024a = tmp0[1] + tmp0[2];
                        //                         float tmp135a = tmp0[1] - tmp0[2];

                        float16x8_t _tmp024b = vaddq_f16(_tmp03, _tmp04);
                        float16x8_t _tmp135b = vsubq_f16(_tmp03, _tmp04);

                        //                         float tmp024b = tmp0[3] + tmp0[4];
                        //                         float tmp135b = tmp0[3] - tmp0[4];

                        float16x8_t _tmp024c = vaddq_f16(_tmp05, _tmp06);
                        float16x8_t _tmp135c = vsubq_f16(_tmp05, _tmp06);

                        //                         float tmp024c = tmp0[5] + tmp0[6];
                        //                         float tmp135c = tmp0[5] - tmp0[6];

                        float16x8_t _out00 = vaddq_f16(_bias0, vaddq_f16(vaddq_f16(_tmp00, _tmp024a), vfmaq_n_f16(_tmp024b, _tmp024c, 32.f)));
                        float16x8_t _out02 = vaddq_f16(_bias0, vfmaq_n_f16(vfmaq_n_f16(_tmp024a, _tmp024b, 4.f), _tmp024c, 8.f));
                        float16x8_t _out04 = vaddq_f16(_bias0, vfmaq_n_f16(vfmaq_n_f16(_tmp024a, _tmp024b, 16.f), _tmp024c, 2.f));
                        vst1q_f16(output0, _out00);
                        vst1q_f16(output0 + 16, _out02);
                        vst1q_f16(output0 + 32, _out04);

                        //                         output0[0] = bias0 + tmp0[0] + tmp024a + tmp024b + tmp024c * 32;
                        //                         output0[2] = bias0 + tmp024a + tmp024b * 4 + tmp024c * 8;
                        //                         output0[4] = bias0 + tmp024a + tmp024b * 16 + tmp024c + tmp024c;

                        float16x8_t _out01 = vaddq_f16(_bias0, vfmaq_n_f16(vfmaq_n_f16(_tmp135a, _tmp135b, 2.f), _tmp135c, 16.f));
                        float16x8_t _out03 = vaddq_f16(_bias0, vfmaq_n_f16(vfmaq_n_f16(_tmp135a, _tmp135b, 8.f), _tmp135c, 4.f));
                        float16x8_t _out05 = vaddq_f16(_bias0, vaddq_f16(vaddq_f16(_tmp07, _tmp135a), vfmaq_n_f16(_tmp135c, _tmp135b, 32.f)));
                        vst1q_f16(output0 + 8, _out01);
                        vst1q_f16(output0 + 24, _out03);
                        vst1q_f16(output0 + 40, _out05);

                        //                         output0[1] = bias0 + tmp135a + tmp135b + tmp135b + tmp135c * 16;
                        //                         output0[3] = bias0 + tmp135a + tmp135b * 8 + tmp135c * 4;
                        //                         output0[5] = bias0 + tmp0[7] + tmp135a + tmp135b * 32 + tmp135c;

                        output0 += outw * 8;
                    }
                }
            }
        }
    }
    // END transform output

    // cut result pad
    copy_cut_border(top_blob_bordered, top_blob, 0, top_blob_bordered.h - top_blob.h, 0, top_blob_bordered.w - top_blob.w, opt);
}
