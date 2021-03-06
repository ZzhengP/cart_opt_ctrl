#include "cart_opt_ctrl/compute_traj_comp.hpp"

using namespace RTT;

KDLTrajCompute::KDLTrajCompute(const std::string& name) : RTT::TaskContext(name)
{ 
  this->addPort("TrajectoryPointPosOut",port_pnt_pos_out_);
  this->addPort("TrajectoryPointVelOut",port_pnt_vel_out_);
  this->addPort("TrajectoryPointAccOut",port_pnt_acc_out_);
  this->addPort("PathROSOut",port_path_out_);
  this->addPort("PathPosesROSOut",port_pose_array_out_);
  this->addPort("ButtonPressed",port_button_pressed_in_);
  this->addOperation("updateWaypoints",&KDLTrajCompute::updateWaypoints,this,RTT::ClientThread);
  
  this->addProperty("base_frame",base_frame_).doc("Max cartesian velocity");
  this->addProperty("vel_max",vel_max_).doc("Max cartesian velocity");
  this->addProperty("acc_max",acc_max_).doc("Max cartesian acceleration");
  this->addProperty("radius",radius_).doc("Radius for path roundness");
  this->addProperty("eqradius",eqradius_).doc("Equivalent radius for path roundness");
  
  // Default params
  base_frame_ = "base_link";
  vel_max_ = 0.1;
  acc_max_ = 2.0;
  radius_ = 0.01;
  eqradius_ = 0.05;
  
  // Match all properties (defined in the constructor) 
  // with the rosparams in the namespace : 
  // nameOfThisComponent/nameOftheProperty
  // Equivalent to ros::param::get("CartOptCtrl/p_gains_");
  rtt_ros_kdl_tools::getAllPropertiesFromROSParam(this);
  
  interpolator_ = new KDL::RotationalInterpolation_SingleAxis();
  tf_ = new tf::TransformListener();
  
  button_pressed_ = false;
}

bool KDLTrajCompute::updateWaypoints(cart_opt_ctrl::UpdateWaypoints::Request& req, cart_opt_ctrl::UpdateWaypoints::Response& resp){  
  
  // Transform the waypoints to the base_frame
  geometry_msgs::PoseStamped tmp_pose_stmp;
  tmp_pose_stmp.header = req.waypoints.header;
  waypoints_in_.header = req.waypoints.header;
  waypoints_in_.poses.clear();
  waypoints_in_.poses.resize(req.waypoints.poses.size());
  try{
    for(int i=0; i<req.waypoints.poses.size(); i++){
      tmp_pose_stmp.pose = req.waypoints.poses[i];
      tf_->transformPose(base_frame_, tmp_pose_stmp, tmp_pose_stmp);
      waypoints_in_.poses[i] = tmp_pose_stmp.pose;
    }
  }catch(tf::TransformException ex){
    ROS_ERROR("%s",ex.what());
    return false;
  }
  waypoints_in_.header.frame_id = base_frame_;
  
  bool success = computeTrajectory();
  current_traj_time_ = 0.0;
  traj_computed_ = success;
  resp.success = success;
  
  while(traj_computed_){
    // Read button press port
    if(this->port_button_pressed_in_.read(button_pressed_) != RTT::NoData){
      // If gravity compensation activated, return failure on service
      if (button_pressed_){
        current_traj_time_ = 0.0;
        traj_computed_ = false;
        resp.success = false;
        return false;
      }
    }
    usleep(1e05);
  }

  return true;
}


bool KDLTrajCompute::configureHook(){ 
  current_traj_time_ = 0.0;
  traj_computed_ = false;
  return true;
}

bool KDLTrajCompute::startHook(){ 
  return true;
}

void KDLTrajCompute::updateHook(){ 
  if (traj_computed_){
    if (current_traj_time_ < ctraject_->Duration()){
      // Get trajectory point
      current_pos_ = ctraject_->Pos(current_traj_time_);
      current_vel_ = ctraject_->Vel(current_traj_time_);
      current_acc_ = ctraject_->Acc(current_traj_time_);
      
      // Send point via ports
      port_pnt_pos_out_.write(current_pos_);
      port_pnt_vel_out_.write(current_vel_);
      port_pnt_acc_out_.write(current_acc_);
      
      // Increase timer
      current_traj_time_ += this->getPeriod();
    }
    else{
      traj_computed_ = false;
      current_traj_time_ = 0.0;
    }
  }
}

bool KDLTrajCompute::computeTrajectory(){  
  try {
    // Initialize path with roundness between waypoints
    path_ = new KDL::Path_RoundedComposite(radius_,eqradius_,interpolator_);

    // Add all the waypoints to the path
    KDL::Frame frame, previous_frame;
    std::vector<KDL::Frame> waypoints;
    for(int i=0; i<waypoints_in_.poses.size(); i++){
      tf::poseMsgToKDL(waypoints_in_.poses[i], frame);
      // If the previous points is too similar dont add it
      if(i>0){
        KDL::Twist err =  diff(frame, previous_frame);
        if((std::abs(err(0))<0.01) && (std::abs(err(1))<0.01) && (std::abs(err(2))<0.01)){
          ROS_WARN_STREAM("Skipping point #"<<i<<" of the path");
          continue;
        }
      }
      waypoints.push_back(frame);
      previous_frame = frame;
    }

    ctraject_ = new KDL::Trajectory_Composite();
    // Add a path only if there is at least 2 points
    if(waypoints.size() > 1){
      for(int i=0; i< waypoints.size(); i++)
        path_->Add(waypoints[i]);
      path_->Finish();
      
      // Set velocity profile of the trajectory
      vel_profile_ = new KDL::VelocityProfile_Trap(vel_max_,acc_max_);
      vel_profile_->SetProfile(0,path_->PathLength());
      
      traject_ = new KDL::Trajectory_Segment(path_, vel_profile_);
      ctraject_->Add(traject_);
    }
    else{
      vel_profile_ = new KDL::VelocityProfile_Trap(vel_max_,acc_max_);
      ctraject_->Add(new KDL::Trajectory_Segment(new KDL::Path_Point(frame), vel_profile_));
    }
    
    // Wait 0.5s at the end of the trajectory
    ctraject_->Add(new KDL::Trajectory_Stationary(0.5,frame));
    
    // Publish a displayable path to ROS
    publishTrajectory();
  
    } catch(KDL::Error& error) {
      std::cout <<"Tried planning with the following waypoints : \n" << waypoints_in_ << endlog();
      std::cout <<"I encountered this error : " << error.Description() << endlog();
      std::cout << "with the following type " << error.GetType() << endlog();
      return false;
    }
  return true;
}

void KDLTrajCompute::publishTrajectory(){
  nav_msgs::Path path_ros;
  path_ros.header.frame_id = waypoints_in_.header.frame_id;
  path_ros.header.stamp = ros::Time::now();

  geometry_msgs::PoseArray pose_array;
  pose_array.header = path_ros.header;
  
  KDL::Frame current_pose;
  KDL::Twist current_vel,current_acc;
  geometry_msgs::Pose pose;
  geometry_msgs::PoseStamped pose_st;
  pose_st.header = path_ros.header;
  for (double t=0.0; t <= ctraject_->Duration(); t+= 0.1) {    
    current_pose = ctraject_->Pos(t);
    current_vel = ctraject_->Vel(t);
    current_acc = ctraject_->Acc(t);
                
    tf::poseKDLToMsg(current_pose,pose);
    pose_array.poses.push_back(pose);
    pose_st.pose = pose;
    path_ros.poses.push_back(pose_st);

  }
  port_path_out_.write(path_ros);
  port_pose_array_out_.write(pose_array);
}

void KDLTrajCompute::stopHook(){}
