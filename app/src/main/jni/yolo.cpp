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
#include <cstdio>
#include "yolo.h"
#include <iostream>
#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <jni.h>
#include <android/log.h>
#include "cpu.h"
#include <jni.h>
#include "sqlite3.h"
#include <android/log.h>
JavaVM* javaVM = nullptr;
jobject activity;
static float fast_exp(float x)
{
    union {
        uint32_t i;
        float f;
    } v{};
    v.i = (1 << 23) * (1.4426950409 * x + 126.93490512f);
    return v.f;
}

static float sigmoid(float x)
{
    return 1.0f / (1.0f + fast_exp(-x));
}
static float intersection_area(const Object& a, const Object& b)
{
    cv::Rect_<float> inter = a.rect & b.rect;
    return inter.area();
}

static void qsort_descent_inplace(std::vector<Object>& faceobjects, int left, int right)
{
    int i = left;
    int j = right;
    float p = faceobjects[(left + right) / 2].prob;

    while (i <= j)
    {
        while (faceobjects[i].prob > p)
            i++;

        while (faceobjects[j].prob < p)
            j--;

        if (i <= j)
        {
            // swap
            std::swap(faceobjects[i], faceobjects[j]);

            i++;
            j--;
        }
    }

    //     #pragma omp parallel sections
    {
        //         #pragma omp section
        {
            if (left < j) qsort_descent_inplace(faceobjects, left, j);
        }
        //         #pragma omp section
        {
            if (i < right) qsort_descent_inplace(faceobjects, i, right);
        }
    }
}

static void qsort_descent_inplace(std::vector<Object>& faceobjects)
{
    if (faceobjects.empty())
        return;

    qsort_descent_inplace(faceobjects, 0, faceobjects.size() - 1);
}

static void nms_sorted_bboxes(const std::vector<Object>& faceobjects, std::vector<int>& picked, float nms_threshold)
{
    picked.clear();

    const int n = faceobjects.size();

    std::vector<float> areas(n);
    for (int i = 0; i < n; i++)
    {
        areas[i] = faceobjects[i].rect.width * faceobjects[i].rect.height;
    }

    for (int i = 0; i < n; i++)
    {
        const Object& a = faceobjects[i];

        int keep = 1;
        for (int j = 0; j < (int)picked.size(); j++)
        {
            const Object& b = faceobjects[picked[j]];

            // intersection over union
            float inter_area = intersection_area(a, b);
            float union_area = areas[i] + areas[picked[j]] - inter_area;
            // float IoU = inter_area / union_area
            if (inter_area / union_area > nms_threshold)
                keep = 0;
        }

        if (keep)
            picked.push_back(i);
    }
}
static void generate_grids_and_stride(const int target_w, const int target_h, std::vector<int>& strides, std::vector<GridAndStride>& grid_strides)
{
    for (int i = 0; i < (int)strides.size(); i++)
    {
        int stride = strides[i];
        int num_grid_w = target_w / stride;
        int num_grid_h = target_h / stride;
        for (int g1 = 0; g1 < num_grid_h; g1++)
        {
            for (int g0 = 0; g0 < num_grid_w; g0++)
            {
                GridAndStride gs;
                gs.grid0 = g0;
                gs.grid1 = g1;
                gs.stride = stride;
                grid_strides.push_back(gs);
            }
        }
    }
}
static void generate_proposals(std::vector<GridAndStride> grid_strides, const ncnn::Mat& pred, float prob_threshold, std::vector<Object>& objects) {
    const int num_points = grid_strides.size();
    const int num_class = 5;
    const int reg_max_1 = 16;

    for (int i = 0; i < num_points; i++) {
        const float *scores = pred.row(i) + 4 * reg_max_1;

        // find label with max score
        int label = -1;
        float score = -FLT_MAX;
        for (int k = 0; k < num_class; k++) {
            float confidence = scores[k];
            if (confidence > score) {
                label = k;
                score = confidence;
            }
        }
        float box_prob = sigmoid(score);
        if (box_prob >= prob_threshold) {
            ncnn::Mat bbox_pred(reg_max_1, 4, (void *) pred.row(i));
            {
                ncnn::Layer *softmax = ncnn::create_layer("Softmax");

                ncnn::ParamDict pd;
                pd.set(0, 1); // axis
                pd.set(1, 1);
                softmax->load_param(pd);

                ncnn::Option opt;
                opt.num_threads = 1;
                opt.use_packing_layout = false;

                softmax->create_pipeline(opt);

                softmax->forward_inplace(bbox_pred, opt);

                softmax->destroy_pipeline(opt);

                delete softmax;
            }

            float pred_ltrb[4];
            for (int k = 0; k < 4; k++) {
                float dis = 0.f;
                const float *dis_after_sm = bbox_pred.row(k);
                for (int l = 0; l < reg_max_1; l++) {
                    dis += l * dis_after_sm[l];
                }

                pred_ltrb[k] = dis * grid_strides[i].stride;
            }

            float pb_cx = (grid_strides[i].grid0 + 0.5f) * grid_strides[i].stride;
            float pb_cy = (grid_strides[i].grid1 + 0.5f) * grid_strides[i].stride;

            float x0 = pb_cx - pred_ltrb[0];
            float y0 = pb_cy - pred_ltrb[1];
            float x1 = pb_cx + pred_ltrb[2];
            float y1 = pb_cy + pred_ltrb[3];
            //这里由绝对像素坐标
            Object obj;
            obj.rect.x = x0;
            obj.rect.y = y0;
            obj.rect.width = x1 - x0;
            obj.rect.height = y1 - y0;
            obj.label = label;
            obj.prob = box_prob;

// 计算物体在图像上的宽度
            float width_in_image = obj.rect.width;

// 计算距离
            float focal_length = 1700;  // 焦距
            float real_width = 3.35;  // 物体的实际宽度
            obj.distance = focal_length * real_width / width_in_image;
            objects.push_back(obj);


        }
    }
}
int clearDatabase() {
    sqlite3* db;
    char* errMsg = 0;
    int rc;

    rc = sqlite3_open("/storage/emulated/0/blind/objects.db", &db);
    if (rc) {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    const char* sql_delete_all = "DELETE FROM OBJECTS;";
    rc = sqlite3_exec(db, sql_delete_all, 0, 0, &errMsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to delete all records: %s\n", errMsg);
        sqlite3_free(errMsg);
        sqlite3_close(db);
        return -1;
    }

    sqlite3_close(db);
    return 0;
}
Yolo::Yolo()
{
    blob_pool_allocator.set_size_compare_ratio(0.f);
    workspace_pool_allocator.set_size_compare_ratio(0.f);
    clearDatabase();
}


int Yolo::load(AAssetManager* mgr, const char* modeltype, int _target_size, const float* _mean_vals, const float* _norm_vals, bool use_gpu)
{
    yolo.clear();
    blob_pool_allocator.clear();
    workspace_pool_allocator.clear();

    ncnn::set_cpu_powersave(2);
    ncnn::set_omp_num_threads(ncnn::get_big_cpu_count());

    yolo.opt = ncnn::Option();

#if NCNN_VULKAN
    yolo.opt.use_vulkan_compute = use_gpu;
#endif

    yolo.opt.num_threads = ncnn::get_big_cpu_count();
    yolo.opt.blob_allocator = &blob_pool_allocator;
    yolo.opt.workspace_allocator = &workspace_pool_allocator;

    char parampath[256];
    char modelpath[256];
    sprintf(parampath, "yolov8%s.param", modeltype);
    sprintf(modelpath, "yolov8%s.bin", modeltype);

    yolo.load_param(mgr, parampath);
    yolo.load_model(mgr, modelpath);

    target_size = _target_size;
    mean_vals[0] = _mean_vals[0];
    mean_vals[1] = _mean_vals[1];
    mean_vals[2] = _mean_vals[2];
    norm_vals[0] = _norm_vals[0];
    norm_vals[1] = _norm_vals[1];
    norm_vals[2] = _norm_vals[2];

    return 0;
}

int Yolo::detect(const cv::Mat& rgb, std::vector<Object>& objects, float prob_threshold, float nms_threshold)
{
    int width = rgb.cols;
    int height = rgb.rows;

    // pad to multiple of 32
    int w = width;
    int h = height;
    float scale = 1.f;
    if (w > h)
    {
        scale = (float)target_size / w;
        w = target_size;
        h = h * scale;
    }
    else
    {
        scale = (float)target_size / h;
        h = target_size;
        w = w * scale;
    }

    ncnn::Mat in = ncnn::Mat::from_pixels_resize(rgb.data, ncnn::Mat::PIXEL_RGB2BGR, width, height, w, h);

    // pad to target_size rectangle
    int wpad = (w + 31) / 32 * 32 - w;
    int hpad = (h + 31) / 32 * 32 - h;
    ncnn::Mat in_pad;
    ncnn::copy_make_border(in, in_pad, hpad / 2, hpad - hpad / 2, wpad / 2, wpad - wpad / 2, ncnn::BORDER_CONSTANT, 0.f);

    in_pad.substract_mean_normalize(0, norm_vals);

    ncnn::Extractor ex = yolo.create_extractor();

    ex.input("images", in_pad);

    std::vector<Object> proposals;

    ncnn::Mat out;
    ex.extract("output0", out);

    std::vector<int> strides = {8, 16, 32}; // might have stride=64
    std::vector<GridAndStride> grid_strides;
    generate_grids_and_stride(in_pad.w, in_pad.h, strides, grid_strides);
    generate_proposals(grid_strides, out, prob_threshold, proposals);
    //generate_proposals(grid_strides, out, prob_threshold, objects,  activity);

    // sort all proposals by score from highest to lowest
    qsort_descent_inplace(proposals);

    // apply nms with nms_threshold
    std::vector<int> picked;
    nms_sorted_bboxes(proposals, picked, nms_threshold);

    int count = picked.size();

    objects.resize(count);
    for (int i = 0; i < count; i++)
    {
        objects[i] = proposals[picked[i]];

        // adjust offset to original unpadded
        float x0 = (objects[i].rect.x - (wpad / 2)) / scale;
        float y0 = (objects[i].rect.y - (hpad / 2)) / scale;
        float x1 = (objects[i].rect.x + objects[i].rect.width - (wpad / 2)) / scale;
        float y1 = (objects[i].rect.y + objects[i].rect.height - (hpad / 2)) / scale;

        // clip
        x0 = std::max(std::min(x0, (float)(width - 1)), 0.f);
        y0 = std::max(std::min(y0, (float)(height - 1)), 0.f);
        x1 = std::max(std::min(x1, (float)(width - 1)), 0.f);
        y1 = std::max(std::min(y1, (float)(height - 1)), 0.f);

        objects[i].rect.x = x0;
        objects[i].rect.y = y0;
        objects[i].rect.width = x1 - x0;
        objects[i].rect.height = y1 - y0;
    }

    // sort objects by area
    struct
    {
        bool operator()(const Object& a, const Object& b) const
        {
            return a.rect.area() > b.rect.area();
        }
    } objects_area_greater;
    std::sort(objects.begin(), objects.end(), objects_area_greater);

    return 0;
}

int Yolo::detect_mat(const ncnn::Mat& mat, std::vector<Object>& objects, float prob_threshold, float nms_threshold) {
    /*
    ncnn::Mat转cv::Mat注意：

    1） ncnn中的数据是float类型.
    2）rgb的类型是CV_8UC3, mat.data指定的类型是char *型, 故rgb可以用下标[]直接索引.
    3）ncnn中数据的排列格式为(channel, h, w), cv::Mat中数据的排列格式为(h, w, channel).
    4） cv::Mat中颜色顺序为BGR, ncnn::Mat格式为BGR.
   */

    cv::Mat rgb(mat.h, mat.w, CV_8UC3);
    for (int c = 0; c < 3; c++) {
        for (int i = 0; i < mat.h; i++) {
            for (int j = 0; j < mat.w; j++) {
                float t = ((float *) mat.data)[j + i * mat.w + c * mat.h * mat.w];
                rgb.data[(2 - c) + j * 3 + i * mat.w * 3] = t;
            }
        }
    }

    int img_w = rgb.cols;
    int img_h = rgb.rows;

    // letterbox pad to multiple of 32
    int w = img_w;
    int h = img_h;
    float scale = 1.f;
    if (w > h) {
        scale = (float) target_size / w;
        w = target_size;
        h = h * scale;
    } else {
        scale = (float) target_size / h;
        h = target_size;
        w = w * scale;
    }

    ncnn::Mat in = ncnn::Mat::from_pixels_resize(rgb.data, ncnn::Mat::PIXEL_RGB, img_w, img_h, w,
                                                 h);

    // pad to target_size rectangle
    // yolov5/utils/datasets.py letterbox
    int wpad = target_size - w;//(w + 31) / 32 * 32 - w;
    int hpad = target_size - h;//(h + 31) / 32 * 32 - h;
    ncnn::Mat in_pad;
    ncnn::copy_make_border(in, in_pad, 0, hpad, 0, wpad, ncnn::BORDER_CONSTANT, 114.f);

    // so for 0-255 input image, rgb_mean should multiply 255 and norm should div by std.
    in_pad.substract_mean_normalize(0, norm_vals);
    //in_pad.substract_mean_normalize(mean_vals, norm_vals);

    ncnn::Extractor ex = yolo.create_extractor();

    ex.input("images", in_pad);

    std::vector<Object> proposals;

    {
        ncnn::Mat out;
        ex.extract("output0", out);

        std::vector<int> strides = {8, 16, 32}; // might have stride=64
        std::vector<GridAndStride> grid_strides;
        generate_grids_and_stride(target_size, target_size, strides, grid_strides);
        generate_proposals(grid_strides, out, prob_threshold, proposals);
        //generate_proposals(grid_strides, out, prob_threshold, objects, activity);
    }

    // sort all proposals by score from highest to lowest
    qsort_descent_inplace(proposals);

    // apply nms with nms_threshold
    std::vector<int> picked;
    nms_sorted_bboxes(proposals, picked, nms_threshold);

    int count = picked.size();


    objects.resize(count);
    for (int i = 0; i < count; i++) {
        objects[i] = proposals[picked[i]];

        // adjust offset to original unpadded
        float x0 = (objects[i].rect.x) / scale;
        float y0 = (objects[i].rect.y) / scale;
        float x1 = (objects[i].rect.x + objects[i].rect.width) / scale;
        float y1 = (objects[i].rect.y + objects[i].rect.height) / scale;

        // clip
        x0 = std::max(std::min(x0, (float) (img_w - 1)), 0.f);
        y0 = std::max(std::min(y0, (float) (img_h - 1)), 0.f);
        x1 = std::max(std::min(x1, (float) (img_w - 1)), 0.f);
        y1 = std::max(std::min(y1, (float) (img_h - 1)), 0.f);

        objects[i].rect.x = x0;
        objects[i].rect.y = y0;
        objects[i].rect.width = x1 - x0;
        objects[i].rect.height = y1 - y0;

    }
    return 0;}
//
// Function to log object information
void logObjectInfo(size_t i, int label, float distance) {
    __android_log_print(ANDROID_LOG_DEBUG, "YourTag", "Object %zu: label=%d, distance=%.5f", i, label, distance);
}
void Yolo::setDetectionCallback(DetectionCallback* callback) {
    mCallback = callback;
}

int Yolo::draw(cv::Mat& rgb, const std::vector<Object>& objects)
{
    static const char* class_names[] = {
        "cart", "person", "warning sign","skim milk", "largeskim milk"
    };

    static const unsigned char colors[20][3] = {
            { 54,  67, 244},
            { 99,  30, 233},
            {176,  39, 156},
            {183,  58, 103},
            {120, 200, 100},  // 新添加的颜色
    };

    int color_index = 0;
    // SQLite database
    sqlite3* db;
    char* errMsg = 0;
    int rc;

    // Open database
    rc = sqlite3_open("/storage/emulated/0/blind/objects.db", &db);

    if (rc) {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    // Create table
    std::string sql = "CREATE TABLE IF NOT EXISTS OBJECTS("
                      "ID INTEGER PRIMARY KEY AUTOINCREMENT,"
                      "LABEL TEXT NOT NULL,"
                      "DISTANCE REAL NOT NULL);";

    rc = sqlite3_exec(db, sql.c_str(), 0, 0, &errMsg);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", errMsg);
        sqlite3_free(errMsg);
    }
    const char* beginTransaction = "BEGIN";
    rc = sqlite3_exec(db, beginTransaction, 0, 0, &errMsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to begin transaction: %s\n", errMsg);
        sqlite3_free(errMsg);
        // 处理错误情况
        // 可能需要回滚事务，然后关闭数据库并返回错误
        sqlite3_close(db);
        return -1;
    }
    for (size_t i = 0; i < objects.size(); i++)
    {

        const Object& obj = objects[i];
        logObjectInfo(i, obj.label, obj.distance);

//         fprintf(stderr, "%d = %.5f at %.2f %.2f %.2f x %.2f\n", obj.label, obj.prob,
//                 obj.rect.x, obj.rect.y, obj.rect.width, obj.rect.height);

        const unsigned char* color = colors[color_index % 19];
        color_index++;

        cv::Scalar cc(color[0], color[1], color[2]);

        cv::rectangle(rgb, obj.rect, cc, 2);


        char text[256];
//sprintf(text, "%s %.1f%%", class_names[obj.label], obj.prob * 100);
        sprintf(text, "%s %.1f%% Dist: %.5f cm", class_names[obj.label], obj.prob * 100, obj.distance*0.254);//shuchu
        int baseLine = 0;
        cv::Size label_size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseLine);

        int x = obj.rect.x;
        int y = obj.rect.y - label_size.height - baseLine;
        if (y < 0)
            y = 0;
        if (x + label_size.width > rgb.cols)
            x = rgb.cols - label_size.width;

        cv::rectangle(rgb, cv::Rect(cv::Point(x, y), cv::Size(label_size.width, label_size.height + baseLine)), cc, -1);

        cv::Scalar textcc = (color[0] + color[1] + color[2] >= 381) ? cv::Scalar(0, 0, 0) : cv::Scalar(255, 255, 255);

        cv::putText(rgb, text, cv::Point(x, y + label_size.height), cv::FONT_HERSHEY_SIMPLEX, 0.5, textcc, 1);

        if (rc != SQLITE_OK) {
            fprintf(stderr, "SQL error: %s\n", errMsg);
            sqlite3_free(errMsg);
            // 处理错误情况
            // 可能需要回滚事务，然后关闭数据库并返回错误
            sqlite3_exec(db, "ROLLBACK", 0, 0, 0); // 回滚事务
            sqlite3_close(db);
            return -1;
        }
        const char* sql_query = "SELECT COUNT(*) FROM OBJECTS;";
        sqlite3_stmt* stmt;
        rc = sqlite3_prepare_v2(db, sql_query, -1, &stmt, 0);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "Failed to execute query: %s\n", errMsg);
            sqlite3_free(errMsg);
            sqlite3_close(db);
            return -1;
        }
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) {
            fprintf(stderr, "Failed to fetch data: %s\n", errMsg);
            sqlite3_free(errMsg);
            sqlite3_close(db);
            return -1;
        }
        int count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);  // 记得在每次查询后释放stmt

        // 如果记录的数量已经达到10条，那么删除最早的一条记录
        if (count >= 20) {
            const char* sql_delete = "DELETE FROM OBJECTS WHERE ID = (SELECT MIN(ID) FROM OBJECTS);";
            rc = sqlite3_exec(db, sql_delete, 0, 0, &errMsg);
            if (rc != SQLITE_OK) {
                fprintf(stderr, "Failed to delete record: %s\n", errMsg);
                sqlite3_free(errMsg);
                sqlite3_close(db);
                return -1;
            }
        }
        // Insert objects
        std::cout << "Object: label=" << obj.label << ", distance=" << obj.distance*0.254 << std::endl;
        std::stringstream ss;
        ss << "INSERT INTO OBJECTS (LABEL, DISTANCE) "
                << "VALUES ('" << obj.label << "', " << static_cast<int>(obj.distance*0.254 + 0.5) << ");";
        std::string sql_insert = ss.str();

        rc = sqlite3_exec(db, sql_insert.c_str(), 0, 0, &errMsg);

        const char* commitTransaction = "COMMIT";
        rc = sqlite3_exec(db, commitTransaction, 0, 0, &errMsg);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "Failed to commit transaction: %s\n", errMsg);
            sqlite3_free(errMsg);
            // 处理错误情况
            // 可能需要回滚事务，然后关闭数据库并返回错误
            sqlite3_exec(db, "ROLLBACK", 0, 0, 0); // 回滚事务
            sqlite3_close(db);
            return -1;
        }



    }// Close database
    sqlite3_close(db);
    return 0;
}

