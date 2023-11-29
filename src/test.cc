#include "ros/ros.h"
#include "std_msgs/String.h"
#include <sstream>
#include <iostream>
using namespace std;
// rtsp://zlm.robot.genius-pros.com:18554/virtual-robot/900106-900106-573c887e-d19e-47e9-8040-3d1fad20edc8";
string start = "start_media_pull#1#rtsp://127.0.0.1:8554/test";
string stop = "stop_media_pull#1#rtsp://127.0.0.1:8554/test";

int main(int argc, char **argv)
{
  // 初始化ROS节点
  ros::init(argc, argv, "test");

  // 创建节点句柄
  ros::NodeHandle nh;

  // 创建一个发布者，发布的数据类型为std_msgs::String
  ros::Publisher string_pub = nh.advertise<std_msgs::String>("/cloud_command", 10);

  // 设置循环的频率为0.05 Hz，即每20秒发布一次
  ros::Rate loop_rate(0.1);
  

  int count = 0;
  while (ros::ok())
  {
    std_msgs::String msg;
    // 发布消息
    if(count %2 == 0)
    {
        msg.data = start;
        cout << "Hello ROS! Count: " << count <<" start"<<endl;
    }
    else
    {
        msg.data = stop;
        cout << "Hello ROS! Count: " << count <<" close"<<endl;
    }
    string_pub.publish(msg);

    // 输出发布的消息
    ROS_INFO("%s", msg.data.c_str());

    // 增加计数器
    count++;

    if(count > 400)
        break;

    // 循环等待
    ros::spinOnce();
    loop_rate.sleep();
  }

  cout <<"Over" <<endl;

  return 0;
}