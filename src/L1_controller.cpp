/*
Copyright (c) 2017, ChanYuan KUO, YoRu LU,
All rights reserved. (Hypha ROS Workshop)

This file is part of hypha_racecar package.

hypha_racecar is free software: you can redistribute it and/or modify
it under the terms of the GNU LESSER GENERAL PUBLIC LICENSE as published
by the Free Software Foundation, either version 3 of the License, or
any later version.

hypha_racecar is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU LESSER GENERAL PUBLIC LICENSE for more details.

You should have received a copy of the GNU LESSER GENERAL PUBLIC LICENSE
along with hypha_racecar.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "ros/ros.h"
#include <time.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/Twist.h>
#include <tf/transform_listener.h>
#include <tf/transform_datatypes.h>
#include "nav_msgs/Path.h"
#include <nav_msgs/Odometry.h>
#include <visualization_msgs/Marker.h>

 
#define PI 3.14159265

/*ANGLE of the forward point*/

class L1Controller
{
public:
    L1Controller()
    {
        //Read Parameters
        //----------------
        ros::NodeHandle pn("~");

        /*Car's parameter*/
        pn.param("L", L, 0.26);
        pn.param("Lfw", Lfw, 0.15);
        pn.param("Lrv", Lrv, 10.0);
        pn.param("Vcmd", Vcmd, 1.0);
        pn.param("lfw", lfw, 0.13);
        pn.param("lrv", lrv, 10.0);

        /*Controller's parameter*/
        pn.param("controller_freq", controller_freq, 20);
        pn.param("AngleGain", Kp, -1.0);
        pn.param("GasGain", Gas_gain, 1.0);
        pn.param("baseSpeed", baseSpeed, 1470);
        pn.param("baseAngle", baseAngle, 90.0);
        pn.param("Ki", Ki, 0.0);

        ROS_INFO("[param] baseSpeed: %d", baseSpeed);
        ROS_INFO("[param] baseAngle: %f", baseAngle);
        ROS_INFO("[param] AngleGain: %f", Kp);

        //Publishers and Subscribers
        //--------------------------
        sub_ = n_.subscribe("/odometry/filtered", 1, &L1Controller::update_odom, this);
        waypoint_sub = n_.subscribe("move_base/TrajectoryPlannerROS/global_plan", 5, &L1Controller::update_globalPath, this);
        goal_sub = n_.subscribe("/move_base_simple/goal", 1, &L1Controller::update_goal, this);
        marker_pub = n_.advertise<visualization_msgs::Marker>("car_path", 10);
        pub_ = n_.advertise<geometry_msgs::Twist>("car/cmd_vel", 1);

        //Timer
        //--------------------------
        timer1 = n_.createTimer(ros::Duration((1.0)/controller_freq), &L1Controller::control_loop, this); // Duration(0.05) -> 20Hz
        timer2 = n_.createTimer(ros::Duration((0.5)/controller_freq), &L1Controller::goalReachingCheck, this); // Duration(0.05) -> 20Hz

        //Init variables
        //--------------------------
        L1 = getL1Distance(Vcmd);
        Lfw = L1;
        goalRadius = L1;
        dt = (1.0)/controller_freq;
        cur_err = 0;
        int_err = 0;
        foundForwardPt = false;
        goal_received = false;
        goal_reached = false;
        cmd_vel.linear.x = 1500;
        cmd_vel.angular.z = 85;

        //Visualization Marker Settings
        initMarker();
    }

    void initMarker()
    {
        points.header.frame_id = line_strip.header.frame_id = goal_circle.header.frame_id = "odom";
        points.ns = line_strip.ns = goal_circle.ns = "Markers";
        points.action = line_strip.action = goal_circle.action = visualization_msgs::Marker::ADD;
        points.pose.orientation.w = line_strip.pose.orientation.w = goal_circle.pose.orientation.w = 1.0;
        points.id = 0;
        line_strip.id = 1;
        goal_circle.id = 2;

        points.type = visualization_msgs::Marker::POINTS;
        line_strip.type = visualization_msgs::Marker::LINE_STRIP;
        goal_circle.type = visualization_msgs::Marker::CYLINDER;
        // POINTS markers use x and y scale for width/height respectively
        points.scale.x = 0.2;
        points.scale.y = 0.2;

        //LINE_STRIP markers use only the x component of scale, for the line width
        line_strip.scale.x = 0.1;

        goal_circle.scale.x = goalRadius;
        goal_circle.scale.y = goalRadius;
        goal_circle.scale.z = 0.1;

        // Points are green
        points.color.g = 1.0f;
        points.color.a = 1.0;

        // Line strip is blue
        line_strip.color.b = 1.0;
        line_strip.color.a = 1.0;

        //goal_circle is yellow
        goal_circle.color.r = 1.0;
        goal_circle.color.g = 1.0;
        goal_circle.color.b = 0.0;
        goal_circle.color.a = 0.5;
    }


    /* @update_odom: Update the current odom with new received odom data from robot_localization
     */
    void update_odom(const nav_msgs::Odometry::ConstPtr& _odom)
    {
        odom = *_odom;
    }

    /* @update_globalPath: Update the current global path with new received global path data
     *                     and transfrom all the waypoints on the path from map frame into odom frame
     */
    void update_globalPath(const nav_msgs::Path::ConstPtr& _path)
    {
        map_path = *_path;
    }


    void update_goal(const geometry_msgs::PoseStamped::ConstPtr& _goal)
    {
        try
        {
            ros::Time now = ros::Time(0);
            geometry_msgs::PoseStamped odom_goal;
            tf_listener.transformPose("odom", now , *_goal, "map" ,odom_goal);
            odom_goal_pos = odom_goal.pose.position;
            goal_received = true;

            /*Draw Goal on RVIZ*/
            goal_circle.pose = odom_goal.pose;
            marker_pub.publish(goal_circle);
        }
        catch(tf::TransformException &ex)
        {
            ROS_ERROR("%s",ex.what());
            ros::Duration(1.0).sleep();
        }
    }

    double getYawFromPose(const geometry_msgs::Pose& carPose)
    {
            float x = carPose.orientation.x;
            float y = carPose.orientation.y;
            float z = carPose.orientation.z;
            float w = carPose.orientation.w;

            double tmp,yaw;
            tf::Quaternion q(x,y,z,w);
            tf::Matrix3x3 quaternion(q);
            quaternion.getRPY(tmp,tmp, yaw);

            return yaw;
    }

    bool isForwardWayPt(const geometry_msgs::Point& wayPt, const geometry_msgs::Pose& carPose)
    {
        float car2wayPt_x = wayPt.x - carPose.position.x;
        float car2wayPt_y = wayPt.y - carPose.position.y;
        double car_theta = getYawFromPose(carPose);

        float car_car2wayPt_x = cos(car_theta)*car2wayPt_x + sin(car_theta)*car2wayPt_y;
        float car_car2wayPt_y = -sin(car_theta)*car2wayPt_x + cos(car_theta)*car2wayPt_y;

        if(car_car2wayPt_x >0) /*is Forward WayPt*/
            return true;
        else
            return false;
    }

    bool isWayPtAwayFromLfwDist(const geometry_msgs::Point& wayPt, const geometry_msgs::Point& car_pos)
    {
        double dx = wayPt.x - car_pos.x;
        double dy = wayPt.y - car_pos.y;
        double dist = sqrt(dx*dx + dy*dy);

        if(dist < Lfw)
            return false;
        else if(dist >= Lfw)
            return true;
    }

    geometry_msgs::Point get_odom_car2WayPtVec(const geometry_msgs::Pose& carPose)
    {
        geometry_msgs::Point carPose_pos = carPose.position;
        double carPose_yaw = getYawFromPose(carPose);
        geometry_msgs::Point forwardPt;
        geometry_msgs::Point odom_car2WayPtVec;
        foundForwardPt = false;

        if(!goal_reached){
            for(int i =0; i< map_path.poses.size(); i++)
            {
                geometry_msgs::PoseStamped map_path_pose = map_path.poses[i];
                geometry_msgs::PoseStamped odom_path_pose;

                try
                {
                    ros::Time now = ros::Time(0);
                    tf_listener.transformPose("odom", now , map_path_pose, "map" ,odom_path_pose);
                    geometry_msgs::Point odom_path_wayPt = odom_path_pose.pose.position;
                    bool _isForwardWayPt = isForwardWayPt(odom_path_wayPt,carPose);

                    if(_isForwardWayPt)
                    {
                        bool _isWayPtAwayFromLfwDist = isWayPtAwayFromLfwDist(odom_path_wayPt,carPose_pos);
                        if(_isWayPtAwayFromLfwDist)
                        {
                            forwardPt = odom_path_wayPt;
                            foundForwardPt = true;
                            break;
                        }
                    }
                }
                catch(tf::TransformException &ex)
                {
                    ROS_ERROR("%s",ex.what());
                    ros::Duration(1.0).sleep();
                }
            }
            
        }
        else if(goal_reached)
        {
            forwardPt = odom_goal_pos;
            foundForwardPt = true;
            //ROS_INFO("goal REACHED!");
        }

        /*Visualized Target Point on RVIZ*/
        /*Clear former target point Marker*/
        points.points.clear();
        line_strip.points.clear();
        
        if(foundForwardPt && !goal_reached){
            points.points.push_back(carPose_pos);
            points.points.push_back(forwardPt);
            line_strip.points.push_back(carPose_pos);
            line_strip.points.push_back(forwardPt);
        }

        marker_pub.publish(points);
        marker_pub.publish(line_strip);
        
        odom_car2WayPtVec.x = cos(carPose_yaw)*(forwardPt.x - carPose_pos.x) + sin(carPose_yaw)*(forwardPt.y - carPose_pos.y);
        odom_car2WayPtVec.y = -sin(carPose_yaw)*(forwardPt.x - carPose_pos.x) + cos(carPose_yaw)*(forwardPt.y - carPose_pos.y);
        return odom_car2WayPtVec;


    }

    double getEta(const geometry_msgs::Pose& carPose)
    {
        geometry_msgs::Point odom_car2WayPtVec = get_odom_car2WayPtVec(carPose);

        double eta = atan(odom_car2WayPtVec.y/odom_car2WayPtVec.x);
        return eta;
    }

    double getCar2GoalDist()
    {
        geometry_msgs::Point car_pose = odom.pose.pose.position;
        double car2goal_x = odom_goal_pos.x - car_pose.x;
        double car2goal_y = odom_goal_pos.y - car_pose.y;

        double dist2goal = sqrt(car2goal_x*car2goal_x + car2goal_y*car2goal_y);

        return dist2goal;
    }

    void goalReachingCheck(const ros::TimerEvent&){

        if(goal_received)
        {
            double car2goal_dist = getCar2GoalDist();
            if(car2goal_dist < goalRadius)
            {
                    goal_reached = true;
                    Vcmd = -5;	//Force car to stop.
                    ROS_INFO("STOP!");
            }
        }
    }


    /* @getL1Distance: Calculate the corresponding L1 distance with the velocity comand(Vcmd).
     */
    double getL1Distance(const double& _Vcmd){
            double L1 = 0;

            if(_Vcmd < 1.34){
                L1 = 3 / 3.0;
            }else if(_Vcmd > 1.34 && _Vcmd < 5.36){
                L1 = _Vcmd*2.24 / 3.0;
            }else{
                L1 = 12 / 3.0;
            }

            return L1;
    }

    double getSteeringAngle(double eta){

            double steeringAnge = -atan((L*sin(eta))/(Lfw/2+lfw*cos(eta)))*(180.0/PI);
            //ROS_INFO("Steering Angle = %.2f", steeringAnge);
            return steeringAnge;

    }

    double getGasInput(const float& current_v){

            double u = (Vcmd - current_v)*Gas_gain;
            //ROS_INFO("velocity = %.2f\tu = %.2f",current_v, u);
            return u;

    }

    void control_loop(const ros::TimerEvent&){

        geometry_msgs::Pose carPose = odom.pose.pose;
        geometry_msgs::Twist car_vel = odom.twist.twist;

        if(goal_received){
        
                /*Estimate Steering Angle*/
                double eta = getEta(carPose);  
                if(foundForwardPt)
                {
                    double steeringAngle = getSteeringAngle(eta);
                    cur_err = steeringAngle;
                    int_err += cur_err*dt;
                    if(int_err >= 40){
                        int_err = 40;
                    }else if(int_err <= -40){
                        int_err = -40;
                    }

                    cmd_vel.angular.z = baseAngle + cur_err*Kp + int_err*Ki;

                    /*Estimate Gas Input*/
                    if(Vcmd >=0)
                    {
                            //double u = getGasInput(car_vel.linear.x);
                            //cmd_vel.linear.x = baseSpeed - u;
                            cmd_vel.linear.x = baseSpeed;
                    }
                    else	/*The car is closed enough to the goal, therefore STOP!*/
                            cmd_vel.linear.x = 1500;
                }
                else
                {
                    cmd_vel.linear.x = 1500;
                    cmd_vel.angular.z = baseAngle;
                }
                ROS_INFO("\nGas = %.2f\nSteering angle = %.2f",cmd_vel.linear.x,cmd_vel.angular.z);

        }

        pub_.publish(cmd_vel);

    }

private:
        ros::NodeHandle n_;

        ros::Subscriber sub_;
        ros::Subscriber waypoint_sub;
        ros::Subscriber goal_sub;
        ros::Publisher pub_;
        ros::Publisher marker_pub;
        ros::Timer timer1;
        ros::Timer timer2;

        visualization_msgs::Marker points, line_strip, goal_circle;

        nav_msgs::Odometry odom;

        nav_msgs::Path map_path;
        nav_msgs::Path odom_path;

        double L;
        double Lfw;
        double Lrv;
        double Vcmd;
        double lfw;
        double lrv;
        double steering;
        double u;
        double v;

        int controller_freq;
        double Angle_gain;
        double Gas_gain;
        int baseSpeed;
        double baseAngle;
        geometry_msgs::Twist cmd_vel;

        double Kp;
        double Ki;
        double cur_err;
        double int_err;
        double dt;

        tf::TransformListener tf_listener;

        double L1;
        bool foundForwardPt;        
        double goalRadius;

        geometry_msgs::Point odom_goal_pos;
        bool goal_received;
        bool goal_reached;

};

int main(int argc, char **argv)
{
        //Initiate ROS
        ros::init(argc, argv, "L1Controller");

        //Create an object of class SubscribeAndPublish that will take care of everything
        L1Controller controller;

        ros::spin();

        return 0;
}

