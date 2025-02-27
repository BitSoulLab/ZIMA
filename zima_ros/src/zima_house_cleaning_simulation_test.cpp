/*
 * This file is part of Project Zima.
 * Copyright © 2022-2025 Austin Liu.
 * Released under the [MIT] License.
 */

#include "zima/auto_cleaning/auto_cleaning.h"
#include "zima/simulator/simulator.h"
#include "zima_ros/zima_node.h"

namespace zima_ros {

using namespace zima;

const std::string kInputMapDir = "/tmp/zima_map/";
const std::string kSensorMapFileName = "test_room_sensor_map.pb.txt";
const std::string kRoomMapFileName = "test_room_room_map.pb.txt";

void ZimaNode::Run(const ZimaThreadWrapper::ThreadParam &param) {
  ZINFO << "Thread \"" << param.thread_name_ << "\" start.";
  auto thread_manager = ZimaThreadManager::Instance();
  if (param.bind_cpu_id_ >= 0) {
    thread_manager->BindCPUCore(param.thread_name_, param.bind_cpu_id_);
  }

  Simulator simulator;
  auto &tf = *TransformManager::Instance();

  auto map = NavMap::SPtr(new NavMap());

  {
    auto sensor_map_file_name = kSensorMapFileName;
    CharGridMap2DLoader loader(kInputMapDir, sensor_map_file_name);
    {
      auto sensor_map = map->GetSensorLayer();
      if (!loader.LoadMap(sensor_map)) {
        ZERROR;
        return;
      }

      auto print_map = map->GetPrintLayer();
      if (!loader.LoadMap(print_map)) {
        ZERROR;
        return;
      }
    }
  }

  map->GetPrintLayer()->Print(__FILE__, __FUNCTION__, __LINE__);

  auto nav_map = operation_data_->GetNavMapRef();
  if (false) {
    auto room_map_file_name = kRoomMapFileName;
    // "/tmp/zima_map/"
    // "test_room_room_map.pb.txt";
    CharGridMap2DLoader loader(kInputMapDir, room_map_file_name);
    {
      auto room_map = nav_map->GetRoomLayer();
      if (!loader.LoadMap(room_map)) {
        ZERROR;
        return;
      }
    }
  }

  nav_map->GetRoomLayer()->Print(__FILE__, __FUNCTION__, __LINE__);

  // auto x_offset = (map->GetPrintLayer()->GetDataBound().GetMax().X() -
  //                  map->GetPrintLayer()->GetDataBound().GetMin().X()) *
  //                     map->GetPrintLayer()->GetResolution() +
  //                 0.1;
  PublishMap(map->GetPrintLayer(), kVirtualObsMap);
  // while (!shutdown_request_) {
  //   PublishMap(map->GetPrintLayer(), kVirtualObsMap);
  //   Time::SleepMSec(200);
  // }
  Time::SleepMSec(200);

  MapPoint current_pose(-40 * NavMap::GetResolution(),
                        42 * NavMap::GetResolution());
  Transform new_chassis_tf(tf.kOdomFrame_, tf.kRobotFrame_, current_pose.X(),
                           current_pose.Y(), current_pose.Degree());
  operation_data_->SetStartPoint(current_pose);
  tf.UpdateTransform(new_chassis_tf);

  Steps all_steps;
  ZINFO;
  RoomsInfo rooms_info;
  for (auto room = NavMap::kRoomA_; room <= NavMap::kRoomD_; room++) {
    rooms_info[room] = std::make_shared<RoomInfo>(
        room, DynamicMapCellBound(INT_MIN, INT_MAX, INT_MIN, INT_MAX),
        MapCell(0, 0), 40, 40);
  }
  operation_data_->SetRoomsInfo(rooms_info);
  AutoCleaning auto_cleaning(operation_data_);

  ChassisController::SPtr chassis_controller(new ChassisController(chassis_));
  chassis_controller->RunThread();

  auto publish_time = Time::Now();

  StepsRecorder steps_recorder;
  while (!shutdown_request_ &&
         auto_cleaning.GetState() != AutoCleaning::State::kFinished) {
    thread_manager->UpdateThreadCycle(param.thread_name_);
    if (Time::Now() - publish_time > 3) {
      PublishMap(nav_map->GetRoomLayer(), kRoomMap);
      Time::SleepMSec(50);
      PublishMap(nav_map->GetFootStepLayer(), kPrintMap);
      Time::SleepMSec(50);
      PublishMap(map->GetPrintLayer(), kVirtualObsMap);
      Time::SleepMSec(50);
      publish_time = Time::Now();
      all_steps = steps_recorder.GetPath();
      PublishPath(map->GetPrintLayer(), all_steps);
    }

    const float duration = 0.02;
    Transform robot_tf("", "");
    tf.GetTransform(tf.kOdomFrame_, tf.kRobotFrame_, robot_tf);
    MapPoint world_pose(robot_tf.X(), robot_tf.Y(), robot_tf.Degree());
    const auto odom_pose = world_pose;
    steps_recorder.AddPathPoint(StepPoint(world_pose));
    PublishRobot(map->GetPrintLayer(), world_pose);
    PublishWallSensor(world_pose);
    auto_cleaning.ChassisSupervise(chassis_, chassis_controller);

    simulator.Run(chassis_, tf, duration, map);
    // auto left_speed =
    //     chassis->GetWheel(chassis->kLeftWheel_)->TargetSpeed();
    // auto right_speed =
    //     chassis->GetWheel(chassis->kRightWheel_)->TargetSpeed();

    // ZINFO << "Left: " << left_speed << ", right: " << right_speed;

    float speed_times;
    // if (count > 42) {
    //   speed_times = 1;
    // } else {
    //   speed_times = 150;
    // }
    speed_times = 30;
    chassis_controller->SetSpeedTimes(speed_times);

    Time::SleepSec(duration / speed_times);
  }

  chassis_controller->StopThread();

  PublishMap(nav_map->GetPrintLayer(), kPrintMap);
  all_steps = steps_recorder.GetPath();
  PublishPath(map->GetPrintLayer(), all_steps);
  Time::SleepSec(1);

  ros::shutdown();
  ZINFO << "Finish cleaning.";
  ZINFO << "Thread \"" << param.thread_name_ << "\" exiting.";
  thread_manager->MarkThreadExited(param.thread_name_, __FILE__, __FUNCTION__,
                                   __LINE__);
  ZINFO << "Thread \"" << param.thread_name_ << "\" exit.";
}

}  // namespace zima_ros

int main(int argc, char **argv) {
  FLAGS_alsologtostderr = true;
  FLAGS_colorlogtostderr = true;
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();

  ros::init(argc, argv, "zima_house_cleaning_simulation_test");
  zima_ros::ZimaNode test(true);
  test.InitForChassis();
  test.InitForAutoCleaning();
  test.InitForRos();

  auto thread_manager = zima::ZimaThreadManager::Instance();
  zima::ZimaThreadWrapper::ThreadParam core_thread_param(
      "Core thread", zima::ZimaThreadManager::kCoreThreadIndex_, 100, 0.2, 1);
  thread_manager->RegisterThread(
      std::thread(&zima_ros::ZimaNode::Run, &test, core_thread_param),
      core_thread_param);

  // Start running.
  while (ros::ok()) {
    zima::Time::SleepMSec(50);
  }
  test.Shutdown();

  // Exiting.
  const double wait_for_exit_timeout_s = 1;
  thread_manager->WaitForThreadExit(core_thread_param.thread_name_,
                                    wait_for_exit_timeout_s);

  return 0;
}
