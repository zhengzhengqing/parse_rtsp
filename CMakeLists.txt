cmake_minimum_required(VERSION 3.0.2)
project(parse_rtsp)
# 设置 C++17 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")

## Compile as C++11, supported in ROS Kinetic and newer
# add_compile_options(-std=c++11)

## Find catkin macros and libraries
## if COMPONENTS list like find_package(catkin REQUIRED COMPONENTS xyz)
## is used, also find other catkin packages
find_package(catkin REQUIRED COMPONENTS
  roscpp
  rospy
  std_msgs
)

#
catkin_package(
  INCLUDE_DIRS include
#  LIBRARIES parse_rtsp
#  CATKIN_DEPENDS roscpp rospy std_msgs
#  DEPENDS system_lib
)

## Specify additional locations of header files
## Your package locations should be listed before other locations
include_directories(
 include
  ${catkin_INCLUDE_DIRS}
)


# 添加包含目录
find_package(PkgConfig)
find_package(SDL2 REQUIRED)

include_directories(${SDL2_INCLUDE_DIRS})

add_executable(parse_rtsp src/main.cc)
add_executable(test_rtsp src/test.cc)

target_link_libraries(parse_rtsp
        ${SDL2_LIBRARIES}
        avformat
        avcodec
        avutil
        swresample
        -pthread
        ${catkin_LIBRARIES}
        )

target_link_libraries(test_rtsp
        ${catkin_LIBRARIES}
        )