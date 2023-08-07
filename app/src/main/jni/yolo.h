// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2021 THL A29 Limited, a Tencent company. All rights reserved.
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

#define YOLO_H
#define YOLO_H

#include <opencv2/core/core.hpp>

#include <net.h>

struct Object
{
    cv::Rect_<float> rect;
    int label;
    float prob;
    float distance;
};
struct GridAndStride
{
    int grid0;
    int grid1;
    int stride;
};
// 声明一个新的回调接口，震动
class DetectionCallback {
public:
    virtual void onObjectDetected() = 0;
};

class Yolo
{
public:
    Yolo();

    int load(const char* modeltype, int target_size, const float* mean_vals, const float* norm_vals, bool use_gpu = false);

    int load(AAssetManager* mgr, const char* modeltype, int target_size, const float* mean_vals, const float* norm_vals, bool use_gpu = false);

    int detect(const cv::Mat& rgb, std::vector<Object>& objects, float prob_threshold = 0.45f, float nms_threshold = 0.65);

    //int detect_mat(const ncnn::Mat& rgb, std::vector<Object>& objects, float prob_threshold = 0.45f, float nms_threshold = 0.65f);
    int detect_mat(const ncnn::Mat& rgb, std::vector<Object>& objects, float prob_threshold = 0.45f, float nms_threshold = 0.65f);

    int write_counter;

    int draw(cv::Mat& rgb, const std::vector<Object>& objects);

// 添加一个新方法来设置检测回调
    void setDetectionCallback(DetectionCallback* callback);
//说出距离
    void speakDistance(float distance);
private:
    ncnn::Net yolo;
    int target_size;
    float mean_vals[3];
    float norm_vals[3];

    ncnn::UnlockedPoolAllocator blob_pool_allocator;
    ncnn::PoolAllocator workspace_pool_allocator;
    // 添加一个新的成员变量来保存回调接口的引用
    DetectionCallback* mCallback;
};
//震动


