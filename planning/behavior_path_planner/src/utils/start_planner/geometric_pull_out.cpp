// Copyright 2022 TIER IV, Inc.
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

#include "behavior_path_planner/utils/start_planner/geometric_pull_out.hpp"

#include "behavior_path_planner/utils/path_safety_checker/objects_filtering.hpp"
#include "behavior_path_planner/utils/start_planner/util.hpp"
#include "behavior_path_planner/utils/utils.hpp"

#include <lanelet2_extension/utility/utilities.hpp>

using lanelet::utils::getArcCoordinates;
using motion_utils::findNearestIndex;
using tier4_autoware_utils::calcDistance2d;
using tier4_autoware_utils::calcOffsetPose;
namespace behavior_path_planner
{
using start_planner_utils::combineReferencePath;
using start_planner_utils::getPullOutLanes;

GeometricPullOut::GeometricPullOut(rclcpp::Node & node, const StartPlannerParameters & parameters)
: PullOutPlannerBase{node, parameters},
  parallel_parking_parameters_{parameters.parallel_parking_parameters}
{
  planner_.setParameters(parallel_parking_parameters_);
}

boost::optional<PullOutPath> GeometricPullOut::plan(Pose start_pose, Pose goal_pose)
{
  PullOutPath output;

  // combine road lane and pull out lane
  const double backward_path_length =
    planner_data_->parameters.backward_path_length + parameters_.max_back_distance;
  const auto road_lanes = utils::getExtendedCurrentLanes(
    planner_data_, backward_path_length, std::numeric_limits<double>::max(),
    /*forward_only_in_route*/ true);
  const auto pull_out_lanes = getPullOutLanes(planner_data_, backward_path_length);
  auto lanes = road_lanes;
  for (const auto & pull_out_lane : pull_out_lanes) {
    auto it = std::find_if(
      lanes.begin(), lanes.end(), [&pull_out_lane](const lanelet::ConstLanelet & lane) {
        return lane.id() == pull_out_lane.id();
      });
    if (it == lanes.end()) {
      lanes.push_back(pull_out_lane);
    }
  }

  planner_.setTurningRadius(
    planner_data_->parameters, parallel_parking_parameters_.pull_out_max_steer_angle);
  planner_.setPlannerData(planner_data_);
  const bool found_valid_path =
    planner_.planPullOut(start_pose, goal_pose, road_lanes, pull_out_lanes);
  if (!found_valid_path) {
    return {};
  }

  // collision check with stop objects in pull out lanes
  const auto arc_path = planner_.getArcPath();
  const auto [pull_out_lane_objects, others] =
    utils::path_safety_checker::separateObjectsByLanelets(
      *(planner_data_->dynamic_object), pull_out_lanes);
  const auto pull_out_lane_stop_objects = utils::path_safety_checker::filterObjectsByVelocity(
    pull_out_lane_objects, parameters_.th_moving_object_velocity);

  if (utils::checkCollisionBetweenPathFootprintsAndObjects(
        vehicle_footprint_, arc_path, pull_out_lane_stop_objects,
        parameters_.collision_check_margin)) {
    return {};
  }
  const double velocity = parallel_parking_parameters_.forward_parking_velocity;

  if (parameters_.divide_pull_out_path) {
    output.partial_paths = planner_.getPaths();
    /*
    Calculate the acceleration required to reach the forward parking velocity at the center of
    the front path, assuming constant acceleration and deceleration.
    v                     v
    |                     |
    |    /\               |    /\
    |   /  \              |   /  \
    |  /    \             |  /    \
    | /      \            | /      \
    |/________\_____ x    |/________\______ t
    0  a_l/2  a_l         0    t_c 2*t_c
    Notes:
    a_l represents "arc_length_on_path_front"
    t_c represents "time_to_center"
    */
    // insert stop velocity to first arc path end
    output.partial_paths.front().points.back().point.longitudinal_velocity_mps = 0.0;
    const double arc_length_on_first_arc_path =
      motion_utils::calcArcLength(output.partial_paths.front().points);
    const double time_to_center = arc_length_on_first_arc_path / (2 * velocity);
    const double average_velocity = arc_length_on_first_arc_path / (time_to_center * 2);
    const double average_acceleration = average_velocity / (time_to_center * 2);
    output.pairs_terminal_velocity_and_accel.push_back(
      std::make_pair(average_velocity, average_acceleration));
    const double arc_length_on_second_arc_path =
      motion_utils::calcArcLength(planner_.getArcPaths().at(1).points);
    output.pairs_terminal_velocity_and_accel.push_back(
      std::make_pair(velocity, velocity * velocity / (2 * arc_length_on_second_arc_path)));
  } else {
    const auto partial_paths = planner_.getPaths();
    const auto combined_path = combineReferencePath(partial_paths.at(0), partial_paths.at(1));
    output.partial_paths.push_back(combined_path);

    // Calculate the acceleration required to reach the forward parking velocity at the center of
    // the path, assuming constant acceleration and deceleration.
    const double arc_length_on_path = motion_utils::calcArcLength(combined_path.points);
    output.pairs_terminal_velocity_and_accel.push_back(
      std::make_pair(velocity, velocity * velocity / 2 * arc_length_on_path));
  }
  output.start_pose = planner_.getArcPaths().at(0).points.front().point.pose;
  output.end_pose = planner_.getArcPaths().at(1).points.back().point.pose;

  return output;
}
}  // namespace behavior_path_planner