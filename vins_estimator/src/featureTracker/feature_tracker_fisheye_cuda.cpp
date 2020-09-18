#include "feature_tracker.h"
#include "../estimator/estimator.h"
#include "fisheye_undist.hpp"
#include "feature_tracker_fisheye.hpp"
namespace FeatureTracker {
#ifdef USE_CUDA

void FisheyeFeatureTrackerCuda::drawTrackFisheye(const cv::Mat & img_up,
    const cv::Mat & img_down,
    cv::cuda::GpuMat imUpTop,
    cv::cuda::GpuMat imDownTop,
    cv::cuda::GpuMat imUpSide_cuda, 
    cv::cuda::GpuMat imDownSide_cuda) {
    cv::Mat a, b, c, d;
    imUpTop.download(a);
    imDownTop.download(b);
    imUpSide_cuda.download(c);
    imDownSide_cuda.download(d);
    drawTrackFisheye(img_up, img_down, a, b, c, d);
}

cv::cuda::GpuMat concat_side(const std::vector<cv::cuda::GpuMat> & arr) {
    int cols = arr[1].cols;
    int rows = arr[1].rows;
    if (enable_rear_side) {
        cv::cuda::GpuMat NewImg(rows, cols*4, arr[1].type()); 
        for (int i = 1; i < 5; i ++) {
            arr[i].copyTo(NewImg(cv::Rect(cols * (i-1), 0, cols, rows)));
        }
        return NewImg;
    } else {
        cv::cuda::GpuMat NewImg(rows, cols*3, arr[1].type()); 
        for (int i = 1; i < 4; i ++) {
            arr[i].copyTo(NewImg(cv::Rect(cols * (i-1), 0, cols, rows)));
        }
        return NewImg;
    }
}



std::vector<cv::Mat> convertCPUMat(const std::vector<cv::cuda::GpuMat> & arr) {
    std::vector<cv::Mat> ret;
    for (const auto & mat:arr) {
        cv::Mat matcpu;
        mat.download(matcpu);
        cv::cvtColor(matcpu, matcpu, cv::COLOR_GRAY2BGR);
        ret.push_back(matcpu);
    }

    return ret;
}


void FisheyeFeatureTrackerCuda::detectPoints(const cv::cuda::GpuMat & img, vector<cv::Point2f> & n_pts, 
        vector<cv::Point2f> & cur_pts, int require_pts) {
    int lack_up_top_pts = require_pts - static_cast<int>(cur_pts.size());

    TicToc tic;
    

    if (lack_up_top_pts > require_pts/4) {

        // ROS_INFO("Lack %d pts; Require %d will detect %d", lack_up_top_pts, require_pts, lack_up_top_pts > require_pts/4);
        cv::Ptr<cv::cuda::CornersDetector> detector = cv::cuda::createGoodFeaturesToTrackDetector(
            img.type(), lack_up_top_pts, 0.01, MIN_DIST);
        cv::cuda::GpuMat d_prevPts;
        detector->detect(img, d_prevPts);


        std::vector<cv::Point2f> n_pts_tmp;

        // std::cout << "d_prevPts size: "<< d_prevPts.size()<<std::endl;
        if(!d_prevPts.empty()) {
            n_pts_tmp = cv::Mat_<cv::Point2f>(cv::Mat(d_prevPts));
        }
        else {
            n_pts_tmp.clear();
        }

        n_pts.clear();

        if (cur_pts.size() > 0) {
            cv::flann::KDTreeIndexParams indexParams;
            // std::cout << cv::Mat(cur_pts).reshape(1) << std::endl;
            cv::flann::Index kdtree(cv::Mat(cur_pts).reshape(1), indexParams);

            for (auto & pt : n_pts_tmp) {
                std::vector<float> query;
                query.push_back(pt.x); //Insert the 2D point we need to find neighbours to the query
                query.push_back(pt.y); //Insert the 2D point we need to find neighbours to the query

                vector<int> indices;
                vector<float> dists;
                auto ret = kdtree.radiusSearch(query, indices, dists, MIN_DIST, 1);

                if (ret && indices.size() > 0) {
                    // printf("Ret %ld Found pt %d dis %f ", ret, indices[0], dists[0]);
                    // printf("New PT %f %f foundpt %f %f Skipping...\n", pt.x, pt.y, cur_pts[indices[0]].x, cur_pts[indices[0]].y);
                } else {
                    // printf("No nearest neighbors found\n");
                    n_pts.push_back(pt);
                }
            }
        } else {
            n_pts = n_pts_tmp;
            }
        }
    else {
        n_pts.clear();
    }
#ifdef PERF_OUTPUT
    ROS_INFO("Detected %ld npts %fms", n_pts.size(), tic.toc());
#endif

 }


std::vector<cv::cuda::GpuMat> buildImagePyramid(const cv::cuda::GpuMat& prevImg, int maxLevel_ = 3) {
    std::vector<cv::cuda::GpuMat> prevPyr;
    prevPyr.resize(maxLevel_ + 1);

    int cn = prevImg.channels();

    CV_Assert(cn == 1 || cn == 3 || cn == 4);

    prevPyr[0] = prevImg;
    for (int level = 1; level <= maxLevel_; ++level) {
        cv::cuda::pyrDown(prevPyr[level - 1], prevPyr[level]);
    }

    return prevPyr;
}


FeatureFrame FisheyeFeatureTrackerCuda::trackImage(double _cur_time,   
    cv::InputArray img1, cv::InputArray img2) {
    cur_time = _cur_time;
    static double detected_time_sum = 0;
    static double ft_time_sum = 0;
    static double count = 0;
    
    if (!is_blank_init) {
        count += 1;
    }
    CvCudaImages fisheye_imgs_up, fisheye_imgs_down;
    img1.getGpuMatVector(fisheye_imgs_up);
    img2.getGpuMatVector(fisheye_imgs_down);
    TicToc t_r;
    cv::cuda::GpuMat up_side_img = concat_side(fisheye_imgs_up);
    cv::cuda::GpuMat down_side_img = concat_side(fisheye_imgs_down);
    cv::cuda::GpuMat up_top_img = fisheye_imgs_up[0];
    cv::cuda::GpuMat down_top_img = fisheye_imgs_down[1];
    double concat_cost = t_r.toc();
    TicToc t_ft;
    top_size = up_top_img.size();
    side_size = up_side_img.size();

    //Clear All current pts
    cur_up_top_pts.clear();
    cur_up_side_pts.clear();
    cur_down_top_pts.clear();
    cur_down_side_pts.clear();

    cur_up_top_un_pts.clear();
    cur_up_side_un_pts.clear();
    cur_down_top_un_pts.clear();
    cur_down_side_un_pts.clear();

    if (!up_top_img.empty() && up_top_img.channels() == 3) {
        cv::cuda::cvtColor(up_top_img, up_top_img, cv::COLOR_BGR2GRAY);
    }

    if (!down_top_img.empty() && down_top_img.channels() == 3) {
        cv::cuda::cvtColor(down_top_img, down_top_img, cv::COLOR_BGR2GRAY);
    }

    if (!up_side_img.empty() && up_side_img.channels() == 3) {
        cv::cuda::cvtColor(up_side_img, up_side_img, cv::COLOR_BGR2GRAY);
    }

    if (!down_side_img.empty() && down_side_img.channels() == 3) {
        cv::cuda::cvtColor(down_side_img, down_side_img, cv::COLOR_BGR2GRAY);
    }

    if (enable_up_top) {
        // ROS_INFO("Tracking top");
        cur_up_top_pts = opticalflow_track(up_top_img, prev_up_top_pyr, prev_up_top_pts, 
            ids_up_top, track_up_top_cnt, removed_pts, false);
    }
    if (enable_up_side) {
        cur_up_side_pts = opticalflow_track(up_side_img, prev_up_side_pyr, prev_up_side_pts, 
            ids_up_side, track_up_side_cnt, removed_pts, false);
    }

    if (enable_down_top) {
        cur_down_top_pts = opticalflow_track(down_top_img, prev_down_top_pyr, prev_down_top_pts, 
            ids_down_top, track_down_top_cnt, removed_pts, false);
    }
    
    ft_time_sum += t_ft.toc();
    // setMaskFisheye();

    if (ENABLE_PERF_OUTPUT) {
        ROS_INFO("Optical flow 1 %fms", t_ft.toc());
    }
    
    TicToc t_d;
    if (enable_up_top) {
        detectPoints(up_top_img, n_pts_up_top, cur_up_top_pts, TOP_PTS_CNT);
    }
    if (enable_down_top) {
        detectPoints(down_top_img, n_pts_down_top, cur_down_top_pts, TOP_PTS_CNT);
    }

    if (enable_up_side) {
        detectPoints(up_side_img, n_pts_up_side, cur_up_side_pts, SIDE_PTS_CNT);
    }


    if (ENABLE_PERF_OUTPUT) {
        ROS_INFO("DetectPoints %fms", t_d.toc());
    }
    detected_time_sum = detected_time_sum + t_d.toc();

    addPointsFisheye();

    TicToc tic2;
    if (enable_down_side) {
        ids_down_side = ids_up_side;
        std::vector<cv::Point2f> down_side_init_pts = cur_up_side_pts;
        cur_down_side_pts = opticalflow_track(down_side_img, prev_up_side_pyr, down_side_init_pts, ids_down_side, 
            track_down_side_cnt, removed_pts, true);
        ft_time_sum += tic2.toc();
        if (ENABLE_PERF_OUTPUT) {
            ROS_INFO("Optical flow 2 %fms", tic2.toc());
        }
    }

    if (is_blank_init) {
        detected_time_sum = 0;
        ft_time_sum = 0;
        count = 0;
        auto ff = setup_feature_frame();
        return ff;
    }

    //Undist points
    cur_up_top_un_pts = undistortedPtsTop(cur_up_top_pts, fisheys_undists[0]);
    cur_down_top_un_pts = undistortedPtsTop(cur_down_top_pts, fisheys_undists[1]);

    cur_up_side_un_pts = undistortedPtsSide(cur_up_side_pts, fisheys_undists[0], false);
    cur_down_side_un_pts = undistortedPtsSide(cur_down_side_pts, fisheys_undists[1], true);

    //Calculate Velocitys
    up_top_vel = ptsVelocity3D(ids_up_top, cur_up_top_un_pts, cur_up_top_un_pts_map, prev_up_top_un_pts_map);
    down_top_vel = ptsVelocity3D(ids_down_top, cur_down_top_un_pts, cur_down_top_un_pts_map, prev_down_top_un_pts_map);

    up_side_vel = ptsVelocity3D(ids_up_side, cur_up_side_un_pts, cur_up_side_un_pts_map, prev_up_side_un_pts_map);
    down_side_vel = ptsVelocity3D(ids_down_side, cur_down_side_un_pts, cur_down_side_un_pts_map, prev_down_side_un_pts_map);

    // ROS_INFO("Up top VEL %ld", up_top_vel.size());
    double tcost_all = t_r.toc();
    if (SHOW_TRACK) {
        drawTrackFisheye(cv::Mat(), cv::Mat(), up_top_img, down_top_img, up_side_img, down_side_img);
    }
        
    prev_up_top_pts = cur_up_top_pts;
    prev_down_top_pts = cur_down_top_pts;
    prev_up_side_pts = cur_up_side_pts;
    prev_down_side_pts = cur_down_side_pts;

    prev_up_top_un_pts = cur_up_top_un_pts;
    prev_down_top_un_pts = cur_down_top_un_pts;
    prev_up_side_un_pts = cur_up_side_un_pts;
    prev_down_side_un_pts = cur_down_side_un_pts;

    prev_up_top_un_pts_map = cur_up_top_un_pts_map;
    prev_down_top_un_pts_map = cur_down_top_un_pts_map;
    prev_up_side_un_pts_map = cur_up_side_un_pts_map;
    prev_down_side_un_pts_map = cur_up_side_un_pts_map;
    prev_time = cur_time;

    up_top_prevLeftPtsMap = pts_map(ids_up_top, cur_up_top_pts);
    down_top_prevLeftPtsMap = pts_map(ids_down_top, cur_down_top_pts);
    up_side_prevLeftPtsMap = pts_map(ids_up_side, cur_up_side_pts);
    down_side_prevLeftPtsMap = pts_map(ids_down_side, cur_down_side_pts);

    // hasPrediction = false;
    auto ff = setup_feature_frame();

    printf("FT Whole %3.1fms; PTS %ld, STEREO %ld; Detect AVG %3.1fms OpticalFlow %3.1fms concat %3.1fms\n", 
        t_r.toc(), 
        cur_up_top_un_pts.size() + cur_up_side_un_pts.size(),
        cur_down_side_un_pts.size(),
        detected_time_sum/count, 
        ft_time_sum/count,
        concat_cost);
    return ff;
}
#endif
};