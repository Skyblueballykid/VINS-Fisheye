/*******************************************************
 * Copyright (C) 2019, Aerial Robotics Group, Hong Kong University of Science and Technology
 * 
 * This file is part of VINS.
 * 
 * Licensed under the GNU General Public License v3.0;
 * you may not use this file except in compliance with the License.
 *
 * Author: Qin Tong (qintonguav@gmail.com)
 *******************************************************/

#pragma once

#include <cstdio>
#include <iostream>
#include <queue>
#include <execinfo.h>
#include <csignal>
#include <opencv2/opencv.hpp>
#include <eigen3/Eigen/Dense>

#include "../utility/opencv_cuda.h"

#include "camodocal/camera_models/CameraFactory.h"
#include "camodocal/camera_models/CataCamera.h"
#include "camodocal/camera_models/PinholeCamera.h"
#include "../estimator/parameters.h"
#include "../utility/tic_toc.h"

#ifdef WITH_VWORKS
#include "vworks_feature_tracker.hpp"
#endif

#define PYR_LEVEL 3
#define WIN_SIZE cv::Size(21, 21)

using namespace std;
using namespace camodocal;
using namespace Eigen;


typedef Eigen::Matrix<double, 8, 1> TrackFeatureNoId;
typedef pair<int, TrackFeatureNoId> TrackFeature;
typedef vector<TrackFeature> FeatureFramenoId;
typedef map<int, FeatureFramenoId> FeatureFrame;

class Estimator;
class FisheyeUndist;

namespace FeatureTracker {


class BaseFeatureTracker {
public:
    BaseFeatureTracker(Estimator * _estimator):
        estimator(_estimator)
    {
    }
    void setPrediction(map<int, Eigen::Vector3d> &predictPts);
    virtual FeatureFrame trackImage(double _cur_time, cv::InputArray _img, 
        cv::InputArray _img1 = cv::noArray()) = 0;
    virtual void removeOutliers(set<int> &removePtsIds) = 0;

protected:
    bool hasPrediction = false;
    int n_id = 0;

    double cur_time;
    double prev_time;
    int height, width;

    Estimator * estimator = nullptr;
    virtual void readIntrinsicParameter(const vector<string> &calib_file) = 0;
    
    void setup_feature_frame(FeatureFrame & ff, vector<int> ids, vector<cv::Point2f> cur_pts, vector<cv::Point3f> cur_un_pts, vector<cv::Point3f> cur_pts_vel, int camera_id);
    FeatureFrame setup_feature_frame();

    void drawTrackImage(cv::Mat & img, vector<cv::Point2f> pts, vector<int> ids, map<int, cv::Point2f> prev_pts);


    virtual void detectPoints(cv::InputArray img, cv::InputArray mask, vector<cv::Point2f> & n_pts, 
        vector<cv::Point2f> & cur_pts, int require_pts) = 0;
    
    map<int, int> pts_status;
    set<int> removed_pts;

    void setFeatureStatus(int feature_id, int status) {
        this->pts_status[feature_id] = status;
        if (status < 0) {
            removed_pts.insert(feature_id);
        }
    }

    bool stereo_cam = false;

};
map<int, cv::Point2f> pts_map(vector<int> ids, vector<cv::Point2f> cur_pts);
void reduceVector(vector<cv::Point2f> &v, vector<uchar> status);
void reduceVector(vector<int> &v, vector<uchar> status);
double distance(cv::Point2f &pt1, cv::Point2f &pt2);

#ifdef USE_CUDA

vector<cv::Point2f> opticalflow_track(cv::cuda::GpuMat & cur_img, 
                    std::vector<cv::cuda::GpuMat> & prev_pyr, vector<cv::Point2f> & prev_pts, 
                    vector<int> & ids, vector<int> & track_cnt, std::set<int> removed_pts,
                    bool is_lr_track, vector<cv::Point2f> prediction_points = vector<cv::Point2f>());
#endif
    
vector<cv::Point2f> opticalflow_track(vector<cv::Mat> * cur_pyr, 
                    vector<cv::Mat> * prev_pyr, vector<cv::Point2f> & prev_pts, 
                    vector<int> & ids, vector<int> & track_cnt, std::set<int> removed_pts, vector<cv::Point2f> prediction_points = vector<cv::Point2f>());

vector<cv::Point2f> opticalflow_track(cv::Mat & cur_img, vector<cv::Mat> * cur_pyr, 
                    cv::Mat & prev_img, vector<cv::Mat> * prev_pyr, vector<cv::Point2f> & prev_pts, 
                    vector<int> & ids, vector<int> & track_cnt, std::set<int> removed_pts, vector<cv::Point2f> prediction_points = vector<cv::Point2f>());

std::vector<cv::Point2f> detect_orb_by_region(cv::InputArray _img, cv::InputArray _mask, int features, int cols = 4, int rows = 4);

std::vector<cv::cuda::GpuMat> buildImagePyramid(const cv::cuda::GpuMat& prevImg, int maxLevel_ = 3);

};

