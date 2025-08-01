// Copyright 2023 ros2_control Development Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "r6bot_controller/r6bot_controller.hpp"

#include <stddef.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "rclcpp/qos.hpp"
#include "rclcpp/time.hpp"
#include "rclcpp_lifecycle/node_interfaces/lifecycle_node_interface.hpp"
#include "rclcpp_lifecycle/state.hpp"

using config_type = controller_interface::interface_configuration_type;

namespace r6bot_controller
{
  RobotController::RobotController() : controller_interface::ControllerInterface() {}

  controller_interface::CallbackReturn RobotController::on_init()
  {
    // should have error handling
    joint_names_ = auto_declare<std::vector<std::string>>("joints", joint_names_);
    command_interface_types_ =
        auto_declare<std::vector<std::string>>("command_interfaces", command_interface_types_);
    state_interface_types_ =
        auto_declare<std::vector<std::string>>("state_interfaces", state_interface_types_);

    point_interp_.positions.assign(joint_names_.size(), 0);
    point_interp_.velocities.assign(joint_names_.size(), 0);

    return CallbackReturn::SUCCESS;
  }

  controller_interface::InterfaceConfiguration RobotController::command_interface_configuration()
      const
  {
    controller_interface::InterfaceConfiguration conf = {config_type::INDIVIDUAL, {}};

    conf.names.reserve(joint_names_.size() * command_interface_types_.size());
    for (const auto &joint_name : joint_names_)
    {
      for (const auto &interface_type : command_interface_types_)
      {
        conf.names.push_back(joint_name + "/" + interface_type);
      }
    }

    return conf;
  }

  controller_interface::InterfaceConfiguration RobotController::state_interface_configuration() const
  {
    controller_interface::InterfaceConfiguration conf = {config_type::INDIVIDUAL, {}};

    conf.names.reserve(joint_names_.size() * state_interface_types_.size());
    for (const auto &joint_name : joint_names_)
    {
      for (const auto &interface_type : state_interface_types_)
      {
        conf.names.push_back(joint_name + "/" + interface_type);
      }
    }

    return conf;
  }

  controller_interface::CallbackReturn RobotController::on_configure(const rclcpp_lifecycle::State &)
  {
    auto callback = [this](const trajectory_msgs::msg::JointTrajectory traj_msg) -> void
    {
      RCLCPP_INFO(get_node()->get_logger(), "Received new trajectory.");
      traj_msg_external_.set(traj_msg);
      new_msg_ = true;
    };

    joint_command_subscriber_ =
        get_node()->create_subscription<trajectory_msgs::msg::JointTrajectory>(
            "~/joint_trajectory", rclcpp::SystemDefaultsQoS(), callback);

    return CallbackReturn::SUCCESS;
  }

  controller_interface::CallbackReturn RobotController::on_activate(const rclcpp_lifecycle::State &)
  {
    // clear out vectors in case of restart
    joint_position_command_interface_.clear();
    joint_velocity_command_interface_.clear();
    joint_position_state_interface_.clear();
    joint_velocity_state_interface_.clear();

    // assign command interfaces
    for (auto &interface : command_interfaces_)
    {
      command_interface_map_[interface.get_interface_name()]->push_back(interface);
    }

    // assign state interfaces
    for (auto &interface : state_interfaces_)
    {
      state_interface_map_[interface.get_interface_name()]->push_back(interface);
    }

    return CallbackReturn::SUCCESS;
  }

  void interpolate_point(
      const trajectory_msgs::msg::JointTrajectoryPoint &point_1,
      const trajectory_msgs::msg::JointTrajectoryPoint &point_2,
      trajectory_msgs::msg::JointTrajectoryPoint &point_interp, double delta)
  {
    for (size_t i = 0; i < point_1.positions.size(); i++)
    {
      point_interp.positions[i] = delta * point_2.positions[i] + (1.0 - delta) * point_1.positions[i];
    }
    for (size_t i = 0; i < point_1.positions.size(); i++)
    {
      point_interp.velocities[i] =
          delta * point_2.velocities[i] + (1.0 - delta) * point_1.velocities[i];
    }
  }

  void interpolate_trajectory_point(
      const trajectory_msgs::msg::JointTrajectory &traj_msg, const rclcpp::Duration &cur_time,
      trajectory_msgs::msg::JointTrajectoryPoint &point_interp, bool &reached_end)
  {
    double traj_len = static_cast<double>(traj_msg.points.size());
    auto last_time = traj_msg.points.back().time_from_start;
    double total_time = last_time.sec + last_time.nanosec * 1E-9;
    double cur_time_sec = cur_time.seconds();
    reached_end = (cur_time_sec >= total_time);

    // If we reached the end of the trajectory, set the velocities to zero.
    if (reached_end)
    {
      point_interp.positions = traj_msg.points.back().positions;
      std::fill(point_interp.velocities.begin(), point_interp.velocities.end(), 0.0);
      return;
    }

    size_t ind =
        static_cast<size_t>(cur_time_sec * (traj_len / total_time)); // Assumes evenly spaced points.
    ind = std::min(ind, static_cast<size_t>(traj_len) - 2);
    double delta = std::min(cur_time_sec - static_cast<double>(ind) * (total_time / traj_len), 1.0);
    interpolate_point(traj_msg.points[ind], traj_msg.points[ind + 1], point_interp, delta);
  }

  controller_interface::return_type RobotController::update(
      const rclcpp::Time &time, const rclcpp::Duration & /*period*/)
  {
    if (new_msg_)
    {
      auto trajectory_msg_op = traj_msg_external_.try_get();
      if (trajectory_msg_op.has_value())
      {
        trajectory_msg_ = trajectory_msg_op.value();
        start_time_ = time;
        new_msg_ = false;
      }
    }

    if (!trajectory_msg_.points.empty())
    {
      bool reached_end;
      interpolate_trajectory_point(trajectory_msg_, time - start_time_, point_interp_, reached_end);

      // If we have reached the end of the trajectory, reset it..
      if (reached_end)
      {
        RCLCPP_INFO(get_node()->get_logger(), "Trajectory execution complete.");
        trajectory_msg_.points.clear();
      }

      for (size_t i = 0; i < joint_position_command_interface_.size(); i++)
      {
        if (!joint_position_command_interface_[i].get().set_value(point_interp_.positions[i]))
        {
          RCLCPP_ERROR(get_node()->get_logger(), "Failed to set position value for index %ld", i);
        }
      }
      for (size_t i = 0; i < joint_velocity_command_interface_.size(); i++)
      {
        if (!joint_velocity_command_interface_[i].get().set_value(point_interp_.velocities[i]))
        {
          RCLCPP_ERROR(get_node()->get_logger(), "Failed to set velocity value for index %ld", i);
        }
      }
    }

    return controller_interface::return_type::OK;
  }

  controller_interface::CallbackReturn RobotController::on_deactivate(const rclcpp_lifecycle::State &)
  {
    release_interfaces();

    return CallbackReturn::SUCCESS;
  }

} // namespace r6bot_controller

#include "pluginlib/class_list_macros.hpp"

PLUGINLIB_EXPORT_CLASS(
    r6bot_controller::RobotController, controller_interface::ControllerInterface)
