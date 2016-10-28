#include "cart_opt_ctrl/cart_opt_ctrl.hpp"
#include <eigen_conversions/eigen_kdl.h>
#include <kdl/frames_io.hpp>

using namespace RTT;
using namespace KDL;

CartOptCtrl::CartOptCtrl(const std::string& name):RTT::TaskContext(name)
{
    this->addPort("JointPosition",this->port_joint_position_in);
    this->addPort("JointVelocity",this->port_joint_velocity_in);
    this->addPort("JointTorqueCommand",this->port_joint_torque_out);
    this->addPort("TrajectoryPointIn",this->port_traj_in);
    
    this->addProperty("FrameOfInterest",this->ee_frame).doc("The robot frame to track the trajectory");
    this->addProperty("P_gain",this->P_gain).doc("Proportional gain");
    this->addProperty("D_gain",this->D_gain).doc("Derivative gain");
}

bool CartOptCtrl::configureHook()
{
    // Initialise the model, the internal solvers etc
    if( ! this->arm.init() )
    {
        log(RTT::Error) << "Could not init chain utils !" << endlog();
        return false;
    }
    // The number of joints
    const int dof  = this->arm.getNrOfJoints();
    
    // Not using Matrix<double,6,1> because of ops script limitations
    this->P_gain.resize(6);
    this->D_gain.resize(6);
    
    // Let's use the last segment to track by default
    this->ee_frame = arm.getSegmentName( arm.getNrOfSegments() - 1 );
    
    // Default gains, works but stiff
    this->P_gain << 1000,1000,1000,300,300,300;
    this->D_gain << 50,50,50,10,10,10;
    
    // Match all properties (defined in the constructor) 
    // with the rosparams in the namespace : 
    // nameOfThisComponent/nameOftheProperty
    // Equivalent to ros::param::get("CartOptCtrl/P_gain");
    rtt_ros_kdl_tools::getAllPropertiesFromROSParam(this);
    
    
    // For now we have 0 constraints for now
    int number_of_constraints = 0;
    this->qpoases_solver.reset(new qpOASES::SQProblem(dof,number_of_constraints));
    
    // Resize and set torque at zero
    this->joint_torque_out.setZero(dof);
    this->joint_position_in.setZero(dof);
    this->joint_velocity_in.setZero(dof);
    
    
    // QPOases options
    qpOASES::Options options;
    // This options enables regularisation (required) and disable
    // some checks to be very fast !
    //options.setToMPC();
    options.enableRegularisation = qpOASES::BT_TRUE;
    
    this->qpoases_solver->setOptions(options);
    // Remove verbosity
    this->qpoases_solver->setPrintLevel(qpOASES::PL_NONE/*qpOASES::PL_DEBUG_ITER*/);
    
    return true;
}

bool CartOptCtrl::startHook()
{
    this->has_first_command = false;
    return true;
}

void CartOptCtrl::updateHook()
{  
   
    // Read the current state of the robot
    RTT::FlowStatus fp = this->port_joint_position_in.read(this->joint_position_in);
    RTT::FlowStatus fv = this->port_joint_velocity_in.read(this->joint_velocity_in);
    
    // Return if not giving anything (might happend during startup)
    if(fp == RTT::NoData || fv == RTT::NoData)
    {
        return;
    }
    
    // Feed the internal model
    arm.setState(this->joint_position_in,this->joint_velocity_in);
    // Make some calculations
    arm.updateModel();
    
    // Get Current end effector Pose
    X_curr = arm.getSegmentPosition(this->ee_frame);
    Xd_curr = arm.getSegmentVelocity(this->ee_frame);

    // Initialize the desired velocity and acceleration to zero
    KDL::SetToZero(Xd_traj);
    KDL::SetToZero(Xdd_traj);  
    
    // If we get a new trajectory point to track
    if(this->port_traj_in.read(this->traj_pt_in) != RTT::NoData)
    {
        // Then overrride the desired
        X_traj = this->traj_pt_in.GetFrame();
        Xd_traj = this->traj_pt_in.GetTwist();
        Xdd_traj = this->traj_pt_in.GetAccTwist();

        has_first_command = true;
    }
    
    // First step, initialise the first X,Xd,Xdd desired
    if(!has_first_command)
    {
        // Stay at the same position
        X_traj = X_curr;
                
        has_first_command = true;
    }
    
    // Compute errors
    X_err = diff( X_curr , X_traj );
    Xd_err = diff( Xd_curr , Xd_traj);
    
    KDL::Twist kp_,kd_;
    
    tf::twistEigenToKDL(this->P_gain,kp_);
    tf::twistEigenToKDL(this->D_gain,kd_);
    
    Xdd_des.vel = Xdd_traj.vel + kp_.vel * ( X_err.vel ) + kd_.vel * ( Xd_err.vel );
    Xdd_des.rot = Xdd_traj.rot + kp_.rot * ( X_err.rot ) + kd_.rot * ( Xd_err.rot );
    
    Eigen::Matrix<double,6,1> xdd_des;
    tf::twistKDLToEigen(Xdd_des,xdd_des);
        
    KDL::Jacobian& J = arm.getSegmentJacobian(this->ee_frame);
    
    KDL::JntSpaceInertiaMatrix& M_inv = arm.getInertiaInverseMatrix();
    
    KDL::JntArray& coriolis = arm.getCoriolisTorque();
    
    KDL::JntArray& gravity = arm.getGravityTorque();
    
    KDL::Twist& Jdotqdot = arm.getSegmentJdotQdot(this->ee_frame);
    
    Eigen::Matrix<double,6,1> jdot_qdot;
    tf::twistKDLToEigen(Jdotqdot,jdot_qdot);
    
    // We put it in the form ax + b
    // M(q).qdd + B(qd) + G(q) = T
    // --> qdd = Minv.( T - B - G)
    // Xd = J.qd
    // --> Xdd = Jdot.qdot + J.qdd
    // --> Xdd = Jdot.qdot + J.Minv.( T - B - G)
    // --> Xdd = Jdot.qdot + J.Minv.T - J.Minv.( B + G )
    // And we have Xdd_des = Xdd_traj + P_gain.( X_des - X_curr) + D_gain.( Xd_des - Xd_curr)
    // ==> We want to compute min(T) || Xdd - Xdd_des ||²
    // If with replace, we can put it in the form ax + b
    // With a = J.Minv
    //      b = - J.Minv.( B + G ) + Jdot.qdot - Xdd_des

    Eigen::MatrixXd a;
    a.resize(6,arm.getNrOfJoints());
    
    Eigen::Matrix<double,6,1> b;
    
    a.noalias() = J.data * M_inv.data;
    b.noalias() = - a * ( coriolis.data + gravity.data ) + jdot_qdot - xdd_des;
    
    // Matrices for qpOASES
    // NOTE: We need RowMajor (see qpoases doc)
    Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> H;
    H.resize(arm.getNrOfJoints(),arm.getNrOfJoints());
    
    Eigen::VectorXd g;
    g.resize(arm.getNrOfJoints());
    
    Eigen::MatrixXd regularisation;
    regularisation.resize( arm.getNrOfJoints(), arm.getNrOfJoints() );
    regularisation.setIdentity();
    regularisation *= 1.0e-6;
    
    H = 2.0 * a.transpose() * a /*+ regularisation*/;
    g = 2.0 * a.transpose() * b;
    
    // TODO: get this from URDF
    Eigen::VectorXd torque_max;
    torque_max.resize(arm.getNrOfJoints());
    torque_max << 200,200,100,100,100,30,30; // N.m

    Eigen::VectorXd torque_min;
    torque_min.resize(arm.getNrOfJoints());
    torque_min = -torque_max; // N.m
    
    // Compute bounds
    Eigen::VectorXd lb,ub;
    lb.resize(arm.getNrOfJoints());
    ub.resize(arm.getNrOfJoints());
    
    //TODO : write constraints for q and qdot
    lb = torque_min;
    ub = torque_max;
    
    // number of allowed compute steps
    int nWSR = 1000; 
    
    // Let's compute !
    qpOASES::returnValue ret;
    static bool qpoases_initialized = false;
    
    if(!qpoases_initialized)
    {
        // Initialise the problem, once it has found a solution, we can hotstart
        ret = qpoases_solver->init(H.data(),g.data(),NULL,lb.data(),ub.data(),NULL,NULL,nWSR);
        
        // Keep init if it didn't work
        if(ret == qpOASES::SUCCESSFUL_RETURN)
        {
            qpoases_initialized = true;
        }
    }
    else
    {
        // Otherwise let's reuse the previous solution to find a solution faster
        ret = qpoases_solver->hotstart(H.data(),g.data(),NULL,lb.data(),ub.data(),NULL,NULL,nWSR);
    }
    
    // Zero grav if not found
    // TODO: find a better alternative
    this->joint_torque_out.setZero();
    
    if(ret == qpOASES::SUCCESSFUL_RETURN)
    {
        // Get the solution
        qpoases_solver->getPrimalSolution(this->joint_torque_out.data());
        // Remove gravity because Kuka already adds it
        this->joint_torque_out -= arm.getGravityTorque().data;
    }

    // Send torques to the robot
    this->port_joint_torque_out.write(this->joint_torque_out);
}


void CartOptCtrl::stopHook()
{
    this->has_first_command = false;
}

