// Copyright (c) 2017 Franka Emika GmbH
// Use of this source code is governed by the Apache-2.0 license, see LICENSE
#include <franka_human_friendly_controllers/cartesian_variable_impedance_controller.h>

#include <cmath>

#include <controller_interface/controller_base.h>
#include <franka_human_friendly_controllers/franka_model.h>
#include <franka/robot_state.h>
#include <pluginlib/class_list_macros.h>
#include <ros/ros.h>
#include <franka_human_friendly_controllers/pseudo_inversion.h>
namespace franka_human_friendly_controllers {

bool CartesianVariableImpedanceController::init(hardware_interface::RobotHW* robot_hw,
                                               ros::NodeHandle& node_handle) {
  std::vector<double> cartesian_stiffness_vector;
  std::vector<double> cartesian_damping_vector;

  sub_equilibrium_pose_ = node_handle.subscribe(
      "/equilibrium_pose", 20, &CartesianVariableImpedanceController::equilibriumPoseCallback, this,
      ros::TransportHints().reliable().tcpNoDelay());
  sub_equilibrium_config_ = node_handle.subscribe(
      "/equilibrium_configuration", 20, &CartesianVariableImpedanceController::equilibriumConfigurationCallback, this,
      ros::TransportHints().reliable().tcpNoDelay());
  // We want to add the subscriber to the note for reading the desired stiffness in the different directions
  sub_vibration_ = node_handle.subscribe(
      "/vibration", 20, &CartesianVariableImpedanceController::equilibriumVibrationCallback, this,
      ros::TransportHints().reliable().tcpNoDelay());

  pub_cartesian_pose_= node_handle.advertise<geometry_msgs::PoseStamped>("/cartesian_pose",1);

  pub_force_torque_= node_handle.advertise<geometry_msgs::WrenchStamped>("/force_torque_ext",1);

  std::string arm_id;
  if (!node_handle.getParam("arm_id", arm_id)) {
    ROS_ERROR_STREAM("CartesianVariableImpedanceController: Could not read parameter arm_id");
    return false;
  }
  std::vector<std::string> joint_names;
  if (!node_handle.getParam("joint_names", joint_names) || joint_names.size() != 7) {
    ROS_ERROR(
        "CartesianVariableImpedanceController: Invalid or no joint_names parameters provided, "
        "aborting controller init!");
    return false;
  }

  franka_hw::FrankaModelInterface* model_interface =
      robot_hw->get<franka_hw::FrankaModelInterface>();
  if (model_interface == nullptr) {
    ROS_ERROR_STREAM(
        "CartesianVariableImpedanceController: Error getting model interface from hardware");
    return false;
  }
  try {
    model_handle_.reset(
        new franka_hw::FrankaModelHandle(model_interface->getHandle(arm_id + "_model")));
  } catch (hardware_interface::HardwareInterfaceException& ex) {
    ROS_ERROR_STREAM(
        "CartesianVariableImpedanceController: Exception getting model handle from interface: "
        << ex.what());
    return false;
  }

  franka_hw::FrankaStateInterface* state_interface =
      robot_hw->get<franka_hw::FrankaStateInterface>();
  if (state_interface == nullptr) {
    ROS_ERROR_STREAM(
        "CartesianVariableImpedanceController: Error getting state interface from hardware");
    return false;
  }
  try {
    state_handle_.reset(
        new franka_hw::FrankaStateHandle(state_interface->getHandle(arm_id + "_robot")));
  } catch (hardware_interface::HardwareInterfaceException& ex) {
    ROS_ERROR_STREAM(
        "CartesianVariableImpedanceController: Exception getting state handle from interface: "
        << ex.what());
    return false;
  }

  hardware_interface::EffortJointInterface* effort_joint_interface =
      robot_hw->get<hardware_interface::EffortJointInterface>();
  if (effort_joint_interface == nullptr) {
    ROS_ERROR_STREAM(
        "CartesianVariableImpedanceController: Error getting effort joint interface from hardware");
    return false;
  }
  for (size_t i = 0; i < 7; ++i) {
    try {
      joint_handles_.push_back(effort_joint_interface->getHandle(joint_names[i]));
    } catch (const hardware_interface::HardwareInterfaceException& ex) {
      ROS_ERROR_STREAM(
          "CartesianVariableImpedanceController: Exception getting joint handles: " << ex.what());
      return false;
    }
  }

  dynamic_reconfigure_compliance_param_node_ =
      ros::NodeHandle("dynamic_reconfigure_compliance_param_node");

  dynamic_server_compliance_param_.reset(
      new dynamic_reconfigure::Server<franka_human_friendly_controllers::compliance_paramConfig>(
          dynamic_reconfigure_compliance_param_node_));
  dynamic_server_compliance_param_->setCallback(
      boost::bind(&CartesianVariableImpedanceController::complianceParamCallback, this, _1, _2));

  position_d_.setZero();
  orientation_d_.coeffs() << 0.0, 0.0, 0.0, 1.0;
  //cartesian_stiffness_.setZero();
  //cartesian_damping_.setZero();

  stiff_.setZero();

  return true;
}

void CartesianVariableImpedanceController::starting(const ros::Time& /*time*/) {
  // compute initial velocity with jacobian and set x_attractor and q_d_nullspace
  // to initial configuration
  franka::RobotState initial_state = state_handle_->getRobotState();
  // get jacobian
  std::array<double, 42> jacobian_array =
      model_handle_->getZeroJacobian(franka::Frame::kEndEffector);
  // convert to eigen
  Eigen::Map<Eigen::Matrix<double, 6, 7> > jacobian(jacobian_array.data());
  Eigen::Map<Eigen::Matrix<double, 7, 1> > dq_initial(initial_state.dq.data());
  Eigen::Map<Eigen::Matrix<double, 7, 1> > q_initial(initial_state.q.data());
  Eigen::Affine3d initial_transform(Eigen::Matrix4d::Map(initial_state.O_T_EE.data()));
  // set equilibrium point to current state
  position_d_ = initial_transform.translation(); // this allows the robot to start on the starting configuration
  orientation_d_ = Eigen::Quaterniond(initial_transform.linear()); // this allows the robot to start on the starting configuration
  //position_d_target_ = initial_transform.translation();
  //orientation_d_target_ = Eigen::Quaterniond(initial_transform.linear());
  // set nullspace equilibrium configuration to initial q
  q_d_nullspace_ = q_initial;
  force_torque_old.setZero();
  double time_old=ros::Time::now().toSec();
  count_vibration=1000;
}

void CartesianVariableImpedanceController::update(const ros::Time& /*time*/,
                                                 const ros::Duration& /*period*/) {
  // get state variables
  franka::RobotState robot_state = state_handle_->getRobotState();
  std::array<double, 7> coriolis_array = model_handle_->getCoriolis();
  std::array<double, 49> mass_array = model_handle_->getMass();
  Eigen::Map<Eigen::Matrix<double, 7, 7> > mass(mass_array.data());
  std::array<double, 42> jacobian_array =
      model_handle_->getZeroJacobian(franka::Frame::kEndEffector);

  // convert to Eigen
  Eigen::Map<Eigen::Matrix<double, 7, 1> > coriolis(coriolis_array.data());
  Eigen::Map<Eigen::Matrix<double, 6, 7> > jacobian(jacobian_array.data());
  Eigen::Map<Eigen::Matrix<double, 7, 1> > q(robot_state.q.data());
  Eigen::Map<Eigen::Matrix<double, 7, 1> > dq(robot_state.dq.data());
  double time_=ros::Time::now().toSec();

  Eigen::Map<Eigen::Matrix<double, 7, 1> > tau_J_d(  // NOLINT (readability-identifier-naming)
      robot_state.tau_J_d.data());
  Eigen::Map<Eigen::Matrix<double, 7, 1> > tau_ext(robot_state.tau_ext_hat_filtered.data());
  std::array<double, 7> gravity = model_handle_->getGravity();
  Eigen::Affine3d transform(Eigen::Matrix4d::Map(robot_state.O_T_EE.data()));
  Eigen::Vector3d position(transform.translation());
  Eigen::Quaterniond orientation(transform.linear());
  Eigen::Matrix<double, 7, 1>  tau_f;
  Eigen::MatrixXd jacobian_transpose_pinv;
  Eigen::MatrixXd Null_mat;
  pseudoInverse(jacobian.transpose(), jacobian_transpose_pinv);
  // Compute the value of the friction
  tau_f(0) =  FI_11/(1+exp(-FI_21*(dq(0)+FI_31))) - TAU_F_CONST_1;
  tau_f(1) =  FI_12/(1+exp(-FI_22*(dq(1)+FI_32))) - TAU_F_CONST_2;
  tau_f(2) =  FI_13/(1+exp(-FI_23*(dq(2)+FI_33))) - TAU_F_CONST_3;
  tau_f(3) =  FI_14/(1+exp(-FI_24*(dq(3)+FI_34))) - TAU_F_CONST_4;
  tau_f(4) =  FI_15/(1+exp(-FI_25*(dq(4)+FI_35))) - TAU_F_CONST_5;
  tau_f(5) =  FI_16/(1+exp(-FI_26*(dq(5)+FI_36))) - TAU_F_CONST_6;
  tau_f(6) =  FI_17/(1+exp(-FI_27*(dq(6)+FI_37))) - TAU_F_CONST_7;

   //Low pass filter for the external force estimation
  float iCutOffFrequency=10.0;
  force_torque+=(-jacobian_transpose_pinv*(tau_ext-tau_f)-force_torque)*(1-exp(-0.001 * 2.0 * M_PI * iCutOffFrequency));
  geometry_msgs::WrenchStamped force_torque_msg;
  force_torque_msg.wrench.force.x=force_torque[0];
  force_torque_msg.wrench.force.y=force_torque[1];
  force_torque_msg.wrench.force.z=force_torque[2];
  force_torque_msg.wrench.torque.x=force_torque[3];
  force_torque_msg.wrench.torque.y=force_torque[4];
  force_torque_msg.wrench.torque.z=force_torque[5];
  pub_force_torque_.publish(force_torque_msg);
  
  geometry_msgs::PoseStamped pose_msg;
  pose_msg.pose.position.x=position[0];
  pose_msg.pose.position.y=position[1];
  pose_msg.pose.position.z=position[2];
  pose_msg.pose.orientation.x=orientation.x();
  pose_msg.pose.orientation.y=orientation.y();
  pose_msg.pose.orientation.z=orientation.z();
  pose_msg.pose.orientation.w=orientation.w();
  pub_cartesian_pose_.publish(pose_msg);
  // compute error to desired pose
  // position error
  Eigen::Matrix<double, 6, 1> error;
  error.head(3) << position - position_d_;

  // orientation error
  if (orientation_d_.coeffs().dot(orientation.coeffs()) < 0.0) {
    orientation.coeffs() << -orientation.coeffs();
  }
  // "difference" quaternion
  Eigen::Quaterniond error_quaternion(orientation * orientation_d_.inverse());
  // convert to axis angle
  Eigen::AngleAxisd error_quaternion_angle_axis(error_quaternion);
  // compute "orientation error"
  error.tail(3) << error_quaternion_angle_axis.axis() * error_quaternion_angle_axis.angle();

  // compute control
  // allocate variables
  Eigen::VectorXd tau_task(7), tau_nullspace(7), tau_d(7), null_vect(7), tau_joint_limit(7);

  // pseudoinverse for nullspace handling
  // kinematic pseuoinverse

  Null_mat=(Eigen::MatrixXd::Identity(7, 7) -jacobian.transpose() * jacobian_transpose_pinv);
  null_vect.setZero();
  null_vect(0)=(q_d_nullspace_(0) - q(0));
  null_vect(1)=(q_d_nullspace_(1) - q(1));
  null_vect(2)=(q_d_nullspace_(2) - q(2));
  null_vect(3)=(q_d_nullspace_(3) - q(3));
  null_vect(4)=(q_d_nullspace_(4) - q(4));
  null_vect(5)=(q_d_nullspace_(5) - q(5));
  null_vect(6)=(q_d_nullspace_(6) - q(6));
  // Cartesian PD control with damping ratio = 1
  tau_task << jacobian.transpose() *
                  (-cartesian_stiffness_ * error -  cartesian_damping_ * (jacobian * dq)); //double critic damping
  // nullspace PD control with damping ratio = 1
  tau_nullspace << (Eigen::MatrixXd::Identity(7, 7) -
                    jacobian.transpose() * jacobian_transpose_pinv) *
                       (nullspace_stiffness_ * null_vect -
                        1*(2.0 * sqrt(nullspace_stiffness_)) * dq); //double critic damping
  tau_joint_limit.setZero();
  if (q(0)>2.85)     { tau_joint_limit(0)=-10; }
  if (q(0)<-2.85)    { tau_joint_limit(0)=+10; }
  if (q(1)>1.7)      { tau_joint_limit(1)=-10; }
  if (q(1)<-1.7)     { tau_joint_limit(1)=+10; }
  if (q(2)>2.85)     { tau_joint_limit(2)=-10; }
  if (q(2)<-2.85)    { tau_joint_limit(2)=+10; }
  if (q(3)>-0.1)     { tau_joint_limit(3)=-10; }
  if (q(3)<-3.0)     { tau_joint_limit(3)=+10; }
  if (q(4)>2.85)     { tau_joint_limit(4)=-10; }
  if (q(4)<-2.85)    { tau_joint_limit(4)=+10; }
  if (q(5)>3.7)      { tau_joint_limit(5)=-10; }
  if (q(5)<-0.1)     { tau_joint_limit(5)=+10; }
  if (q(6)>2.8)      { tau_joint_limit(6)=-10; }
  if (q(6)<-2.8)     { tau_joint_limit(6)=+10; }
  // Desired torque
  tau_d << tau_task + tau_nullspace + coriolis+ tau_joint_limit;

  // Saturate torque rate to avoid discontinuities
  tau_d << saturateTorqueRate(tau_d, tau_J_d);
  if (count_vibration<1000.0*duration_vibration){tau_d(6)=tau_d(6)+5.0*sin(100.0/1000.0*2.0*3.14*count_vibration);
  count_vibration=count_vibration+1;
  //ROS_INFO_STREAM("count_vibration" << count_vibration << "tau" << tau_d);
}

  for (size_t i = 0; i < 7; ++i) {
    joint_handles_[i].setCommand(tau_d(i));
  }
}

Eigen::Matrix<double, 7, 1> CartesianVariableImpedanceController::saturateTorqueRate(
    const Eigen::Matrix<double, 7, 1>& tau_d_calculated,
    const Eigen::Matrix<double, 7, 1>& tau_J_d) {  // NOLINT (readability-identifier-naming)
  Eigen::Matrix<double, 7, 1> tau_d_saturated{};
  for (size_t i = 0; i < 7; i++) {
    double difference = tau_d_calculated[i] - tau_J_d[i];
    tau_d_saturated[i] =
        tau_J_d[i] + std::max(std::min(difference, delta_tau_max_), -delta_tau_max_);
  }
  return tau_d_saturated;
}

void CartesianVariableImpedanceController::complianceParamCallback(
    franka_human_friendly_controllers::compliance_paramConfig& config,
    uint32_t /*level*/) {
  cartesian_stiffness_.setIdentity();
  cartesian_stiffness_(0,0)=config.translational_stiffness_X;
  cartesian_stiffness_(1,1)=config.translational_stiffness_Y;
  cartesian_stiffness_(2,2)=config.translational_stiffness_Z;
  cartesian_stiffness_(3,3)=config.rotational_stiffness_X;
  cartesian_stiffness_(4,4)=config.rotational_stiffness_Y;
  cartesian_stiffness_(5,5)=config.rotational_stiffness_Z;

  cartesian_damping_(0,0)=2.0 * sqrt(config.translational_stiffness_X);
  cartesian_damping_(1,1)=2.0 * sqrt(config.translational_stiffness_Y);
  cartesian_damping_(2,2)=2.0 * sqrt(config.translational_stiffness_Z);
  cartesian_damping_(3,3)=2.0 * sqrt(config.rotational_stiffness_X);
  cartesian_damping_(4,4)=2.0 * sqrt(config.rotational_stiffness_Y);
  cartesian_damping_(5,5)=2.0 * sqrt(config.rotational_stiffness_Z);
  nullspace_stiffness_ = config.nullspace_stiffness;
}



void CartesianVariableImpedanceController::equilibriumPoseCallback(
    const geometry_msgs::PoseStampedConstPtr& msg) {
  position_d_ << msg->pose.position.x, msg->pose.position.y, msg->pose.position.z;
  Eigen::Quaterniond last_orientation_d_(orientation_d_);
  orientation_d_.coeffs() << msg->pose.orientation.x, msg->pose.orientation.y,
      msg->pose.orientation.z, msg->pose.orientation.w;
  if (last_orientation_d_.coeffs().dot(orientation_d_.coeffs()) < 0.0) {
    orientation_d_.coeffs() << -orientation_d_.coeffs();
}
}

void CartesianVariableImpedanceController::equilibriumConfigurationCallback( const sensor_msgs::JointState::ConstPtr& joint) {
  int i = 0;
  Eigen::Matrix<double, 7, 1> q_d_nullspace_;

  for(int i=0; i<7; ++i)
  {
    q_d_nullspace_[i] = joint->position[i];
  }
}
void CartesianVariableImpedanceController::equilibriumVibrationCallback( const std_msgs::Float32::ConstPtr& vibration_msg) {
  count_vibration = 0;
  duration_vibration = vibration_msg->data;

}


}  // namespace franka_human_friendly_controllers

PLUGINLIB_EXPORT_CLASS(franka_human_friendly_controllers::CartesianVariableImpedanceController,
                       controller_interface::ControllerBase)
