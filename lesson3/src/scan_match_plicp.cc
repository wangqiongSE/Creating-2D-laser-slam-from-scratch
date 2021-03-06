/*
 * Copyright 2020 The Project Author: lixiang
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "lesson3/scan_match_plicp.h"

ScanMatchPLICP::ScanMatchPLICP()
{
    // \033[1;32m，\033[0m 终端显示成绿色
    ROS_INFO_STREAM("\033[1;32m----> Scan Match with PLICP started.\033[0m");

    laser_scan_subscriber_ = node_handle_.subscribe(
        "laser_scan", 1, &ScanMatchPLICP::ScanCallback, this);

    initialized_ = false;
}

ScanMatchPLICP::~ScanMatchPLICP()
{
}

/*
 * 回调函数 进行数据处理
 */
void ScanMatchPLICP::ScanCallback(const sensor_msgs::LaserScan::ConstPtr &scan_msg)
{
    // 如果是第一帧数据，首先进行初始化
    if (!initialized_)
    {
        // 将雷达各个角度的sin与cos值保存下来，以节约计算量
        CreateCache(scan_msg);

        // 将 prev_ldp_scan_,last_icp_time_ 初始化
        LaserScanToLDP(scan_msg, prev_ldp_scan_);
        last_icp_time_ = scan_msg->header.stamp;
        initialized_ = true;
    }

    // step1 进行数据类型转换
    start_time_ = std::chrono::steady_clock::now();

    LDP curr_ldp_scan;
    LaserScanToLDP(scan_msg, curr_ldp_scan);

    end_time_ = std::chrono::steady_clock::now();
    time_used_ = std::chrono::duration_cast<std::chrono::duration<double>>(end_time_ - start_time_);
    std::cout << "\n转换数据格式用时: " << time_used_.count() << " 秒。" << std::endl;

    // step2 使用PLICP计算雷达前后两帧间的坐标变换
    start_time_ = std::chrono::steady_clock::now();

    ScanMatchWithPLICP(curr_ldp_scan, scan_msg->header.stamp);

    end_time_ = std::chrono::steady_clock::now();
    time_used_ = std::chrono::duration_cast<std::chrono::duration<double>>(end_time_ - start_time_);
    std::cout << "PLICP计算用时: " << time_used_.count() << " 秒。" << std::endl;
}

/**
 * 雷达数据间的角度是固定的，因此可以将对应角度的cos与sin值缓存下来，不用每次都计算
 */
void ScanMatchPLICP::CreateCache(const sensor_msgs::LaserScan::ConstPtr &scan_msg)
{
    a_cos_.clear();
    a_sin_.clear();
    double angle;

    for (unsigned int i = 0; i < scan_msg->ranges.size(); i++)
    {
        angle = scan_msg->angle_min + i * scan_msg->angle_increment;
        a_cos_.push_back(cos(angle));
        a_sin_.push_back(sin(angle));
    }

    input_.min_reading = scan_msg->range_min;
    input_.max_reading = scan_msg->range_max;
}

/**
 * 将雷达的数据格式转成 csm 需要的格式
 */
void ScanMatchPLICP::LaserScanToLDP(const sensor_msgs::LaserScan::ConstPtr &scan_msg, LDP &ldp)
{
    unsigned int n = scan_msg->ranges.size();
    // 调用csm里的函数进行申请空间
    ldp = ld_alloc_new(n);

    for (unsigned int i = 0; i < n; i++)
    {
        // calculate position in laser frame
        double r = scan_msg->ranges[i];

        if (r > scan_msg->range_min && r < scan_msg->range_max)
        {
            // 填充雷达数据
            ldp->valid[i] = 1;
            ldp->readings[i] = r;
        }
        else
        {
            ldp->valid[i] = 0;
            ldp->readings[i] = -1; // for invalid range
        }

        ldp->theta[i] = scan_msg->angle_min + i * scan_msg->angle_increment;
        ldp->cluster[i] = -1;
    }

    ldp->min_theta = ldp->theta[0];
    ldp->max_theta = ldp->theta[n - 1];

    ldp->odometry[0] = 0.0;
    ldp->odometry[1] = 0.0;
    ldp->odometry[2] = 0.0;

    ldp->true_pose[0] = 0.0;
    ldp->true_pose[1] = 0.0;
    ldp->true_pose[2] = 0.0;
}

/**
 * 使用PLICP进行帧间位姿的计算
 */
void ScanMatchPLICP::ScanMatchWithPLICP(LDP &curr_ldp_scan, const ros::Time &time)
{
    // CSM is used in the following way:
    // The scans are always in the laser frame
    // The reference scan (prevLDPcan_) has a pose of [0, 0, 0]
    // The new scan (currLDPScan) has a pose equal to the movement
    // of the laser in the laser frame since the last scan
    // The computed correction is then propagated using the tf machinery

    prev_ldp_scan_->odometry[0] = 0.0;
    prev_ldp_scan_->odometry[1] = 0.0;
    prev_ldp_scan_->odometry[2] = 0.0;

    prev_ldp_scan_->estimate[0] = 0.0;
    prev_ldp_scan_->estimate[1] = 0.0;
    prev_ldp_scan_->estimate[2] = 0.0;

    prev_ldp_scan_->true_pose[0] = 0.0;
    prev_ldp_scan_->true_pose[1] = 0.0;
    prev_ldp_scan_->true_pose[2] = 0.0;

    input_.laser_ref = prev_ldp_scan_;
    input_.laser_sens = curr_ldp_scan;

    // 位姿的预测值为0，就是不进行预测
    input_.first_guess[0] = 0;
    input_.first_guess[1] = 0;
    input_.first_guess[2] = 0;

    // 调用csm里的函数进行plicp计算帧间的匹配，输出结果保存在output里
    sm_icp(&input_, &output_);

    if (output_.valid)
    {
        std::cout << "transfrom: (" << output_.x[0] << ", " << output_.x[1] << ", " 
            << output_.x[2] * 180 / M_PI << ")" << std::endl;
    }
    else
    {
        std::cout << "not Converged" << std::endl;
    }

    // 删除prev_ldp_scan_，用curr_ldp_scan进行替代
    ld_free(prev_ldp_scan_);
    prev_ldp_scan_ = curr_ldp_scan;
    last_icp_time_ = time;
}


int main(int argc, char **argv)
{
    ros::init(argc, argv, "lesson3_scan_match_plicp_node"); // 节点的名字
    ScanMatchPLICP scan_match_plicp;

    ros::spin(); 
    return 0;
}