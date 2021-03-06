#include <tf/transform_datatypes.h>

#include "tuw_marker_slam_node.h"
#include "tuw_marker_slam/ekf_slam.h"

using namespace tuw;

int main ( int argc, char **argv ) {
    ros::init ( argc, argv, "slam" );
    ros::NodeHandle n;
    SLAMNode slam ( n );
    ros::Rate rate ( 10 );

    while ( ros::ok() ) {
        /// localization and mapping
        slam.cycle();

        /// publishes the estimated pose and mapping
        slam.publish();

        /// calls all callbacks waiting in the queue
        ros::spinOnce();

        /// sleep for the time remaining to let us hit our publish rate
        rate.sleep();
    }
    return 0;
}

/**
 * Constructor
 **/
SLAMNode::SLAMNode ( ros::NodeHandle & n )
    : SLAM (),
      n_ ( n ),
      n_param_ ( "~" ) {
    int mode;
    std::vector<double> beta;

    // read in common parameters
    n_param_.param<int> ( "mode", mode, 0 );
    n_param_.param<bool> ( "xzplane", xzplane_, false );
    n_param_.param<std::string> ( "frame_id_map", frame_id_map_, "map" );
    n_param_.param<std::string> ( "frame_id_odom", frame_id_odom_, "odom" );
    n_param_.param<std::string> ( "frame_id_base", frame_id_base_, "base_link" );
    tf::resolve ( n_.getNamespace(), frame_id_map_ );
    tf::resolve ( n_.getNamespace(), frame_id_odom_ );
    tf::resolve ( n_.getNamespace(), frame_id_base_ );

    switch ( mode ) {
    case SLAMTechnique::EKF:
        // read in EKF specific parameters
        beta =  std::vector<double> ( 18 ) ;

        n_param_.getParam ( "beta_1", beta[0] );
        n_param_.getParam ( "beta_2", beta[1] );
        n_param_.getParam ( "beta_3", beta[2] );
        n_param_.getParam ( "beta_4", beta[3] );
        n_param_.getParam ( "beta_5", beta[4] );
        n_param_.getParam ( "beta_6", beta[5] );
        n_param_.getParam ( "beta_7", beta[6] );
        n_param_.getParam ( "beta_8", beta[7] );
        n_param_.getParam ( "beta_9", beta[8] );
        n_param_.getParam ( "beta_10", beta[9] );
        n_param_.getParam ( "beta_11", beta[10] );
        n_param_.getParam ( "beta_12", beta[11] );
        n_param_.getParam ( "beta_13", beta[12] );
        n_param_.getParam ( "beta_14", beta[13] );
        n_param_.getParam ( "beta_15", beta[14] );
        n_param_.getParam ( "beta_16", beta[15] );
        n_param_.getParam ( "beta_17", beta[16] );
        n_param_.getParam ( "beta_18", beta[17] );

        zt_ = std::make_shared<tuw::MeasurementMarker>();
        slam_technique_ = std::make_shared<tuw::EKFSLAM> ( beta );
        break;
    default:
        ROS_ERROR ( "[%s] mode %i is not supported", ros::this_node::getName().c_str(), mode );
        return;
    }
    ROS_INFO ( "[%s] mode: %s (%i)", ros::this_node::getName().c_str(), slam_technique_->getTypeName().c_str(), ( int ) slam_technique_->getType() );

    /// subscribes to command values
    sub_cmd_ = n.subscribe ( "cmd", 1, &SLAMNode::callbackCmd, this );

    /// defines publishers for the resulting robot pose
    pub_xt_ = n_param_.advertise<geometry_msgs::PoseWithCovarianceStamped> ( "xt", 1 );
    xt_.header.frame_id = frame_id_map_;
    xt_.header.seq = 0;

    /// defines publishers for the resulting landmark poses
    pub_mt_ = n_param_.advertise<marker_msgs::MarkerWithCovarianceArray> ( "mt", 1 );
    mt_.header.frame_id = frame_id_map_;
    mt_.header.seq = 0;

    /// subscribes to transforamtions
    tf_listener_ = std::make_shared<tf::TransformListener>();

    /// start parameter server
    reconfigureFncSLAM_ = boost::bind ( &SLAMNode::callbackConfigSLAM, this,  _1, _2 );
    reconfigureServerSLAM_.setCallback ( reconfigureFncSLAM_ );

    switch ( slam_technique_->getType() ) {
    case SLAMTechnique::EKF:
        /// subscribes to marker detector
        sub_marker_ = n.subscribe ( "marker", 1, &SLAMNode::callbackMarker, this );

        /// start parameter server
        reconfigureServerEKFSLAM_ = std::make_shared< dynamic_reconfigure::Server<tuw_marker_slam::EKFSLAMConfig> > ( ros::NodeHandle ( "~/" + slam_technique_->getTypeName() ) );
        reconfigureFncEKFSLAM_ = boost::bind ( &SLAMNode::callbackConfigEKFSLAM, this,  _1, _2 );
        reconfigureServerEKFSLAM_->setCallback ( reconfigureFncEKFSLAM_ );
        break;
    default:
        assert ( 0 );
    }
}

void SLAMNode::cycle() {
    if ( config_.reset ) {
        slam_technique_->reset();
    }

    SLAM::cycle ();
}

/**
 * Publishes the estimated pose
 **/
void SLAMNode::publish () {
    if ( slam_technique_->time_last_update().is_not_a_date_time() ) return;

    // Broadast transformation map to odom
    tf::Transform base_to_map;
    tf::Stamped<tf::Pose> map_to_base;
    tf::Stamped<tf::Pose> odom_to_map;
    tf::StampedTransform map_to_odom;

    // subtracting base to odom from map to base (cp. http://wiki.ros.org/amcl)
    try {
        base_to_map = tf::Transform ( tf::createQuaternionFromYaw ( yt_[0].theta() ), tf::Point ( yt_[0].x(), yt_[0].y(), 0 ) );
        map_to_base = tf::Stamped<tf::Pose> ( base_to_map.inverse(), ros::Time::fromBoost ( slam_technique_->time_last_update() ), frame_id_base_ );
        tf_listener_->transformPose ( frame_id_odom_, map_to_base, odom_to_map );
        map_to_odom = tf::StampedTransform ( odom_to_map.inverse(), odom_to_map.stamp_, frame_id_map_, frame_id_odom_ );

        tf_broadcaster_.sendTransform ( map_to_odom );
    }
    catch (std::exception &ex) {
        ROS_ERROR ( "[%s publish] subtracting base-to-odom from map-to-base failed: %s", ros::this_node::getName().c_str(), ex.what() );
    }

    assert ( yt_.size() > 0 && C_Yt_.rows == 3*yt_.size() && C_Yt_.cols == 3*yt_.size() );

    // publish estimated robot pose and its variance
    xt_.header.stamp = ros::Time::fromBoost ( slam_technique_->time_last_update() );
    xt_.header.seq++;

    xt_.pose.pose.position.x = yt_[0].x();
    xt_.pose.pose.position.y = yt_[0].y();
    xt_.pose.pose.position.z = 0;
    xt_.pose.pose.orientation = tf::createQuaternionMsgFromYaw ( yt_[0].theta() );

    cv::Matx<double, 3, 3> C_X2 = cv::Mat_<double> ( C_Yt_, cv::Range ( 0, 3 ), cv::Range ( 0, 3 ) );
    xt_.pose.covariance[6*0 + 0] = C_X2 ( 0, 0 );
    xt_.pose.covariance[6*0 + 1] = C_X2 ( 0, 1 );
    xt_.pose.covariance[6*0 + 5] = C_X2 ( 0, 2 );
    xt_.pose.covariance[6*1 + 0] = C_X2 ( 1, 0 );
    xt_.pose.covariance[6*1 + 1] = C_X2 ( 1, 1 );
    xt_.pose.covariance[6*1 + 5] = C_X2 ( 1, 2 );
    xt_.pose.covariance[6*5 + 0] = C_X2 ( 2, 0 );
    xt_.pose.covariance[6*5 + 1] = C_X2 ( 2, 1 );
    xt_.pose.covariance[6*5 + 5] = C_X2 ( 2, 2 );

    pub_xt_.publish ( xt_ );

    // publish estimated landmark poses and their variance
    mt_.header.stamp = ros::Time::fromBoost ( slam_technique_->time_last_update() );
    mt_.header.seq++;

    mt_.markers.resize ( yt_.size() - 1 );
    for ( size_t i = 0; i < mt_.markers.size(); i++ ) {
        mt_.markers[i].marker.ids.resize(1);
        mt_.markers[i].marker.ids_confidence.resize(1);
        mt_.markers[i].marker.ids[0] = i + 1;
        mt_.markers[i].marker.ids_confidence[0] = 1.0;

        mt_.markers[i].marker.pose.position.x = yt_[i+1].x();
        mt_.markers[i].marker.pose.position.y = yt_[i+1].y();
        mt_.markers[i].marker.pose.position.z = 0;
        mt_.markers[i].marker.pose.orientation = tf::createQuaternionMsgFromYaw ( yt_[i+1].theta() );

        cv::Matx<double, 3, 3> C_Mi2 = cv::Mat_<double> ( C_Yt_, cv::Range ( 3* ( i+1 ), 3* ( i+1 ) + 3 ), cv::Range ( 3* ( i+1 ), 3* ( i+1 ) + 3 ) );
        mt_.markers[i].covariance[6*0 + 0] = C_Mi2 ( 0, 0 );
        mt_.markers[i].covariance[6*0 + 1] = C_Mi2 ( 0, 1 );
        mt_.markers[i].covariance[6*0 + 5] = C_Mi2 ( 0, 2 );
        mt_.markers[i].covariance[6*1 + 0] = C_Mi2 ( 1, 0 );
        mt_.markers[i].covariance[6*1 + 1] = C_Mi2 ( 1, 1 );
        mt_.markers[i].covariance[6*1 + 5] = C_Mi2 ( 1, 2 );
        mt_.markers[i].covariance[6*5 + 0] = C_Mi2 ( 2, 0 );
        mt_.markers[i].covariance[6*5 + 1] = C_Mi2 ( 2, 1 );
        mt_.markers[i].covariance[6*5 + 5] = C_Mi2 ( 2, 2 );
    }

    pub_mt_.publish ( mt_ );
}

/**
 * copies incoming robot command message
 * @param cmd
 **/
void SLAMNode::callbackCmd ( const geometry_msgs::Twist& cmd ) {
    ut_.v() = cmd.linear.x;
    ut_.w() = cmd.angular.z;
}

/**
 * copies incoming marker messages to the base class
 * @param marker
 **/
void SLAMNode::callbackMarker ( const marker_msgs::MarkerDetection &_marker ) {
    assert ( zt_->getType() == tuw::Measurement::Type::MARKER );
    MeasurementMarkerPtr zt = std::static_pointer_cast<MeasurementMarker> ( zt_ );

    try {
        tf::StampedTransform transform;
        tf_listener_->lookupTransform ( frame_id_base_, _marker.header.frame_id, ros::Time ( 0 ), transform );
        double yaw = tf::getYaw(transform.getRotation());
        zt->pose2d() = Pose2D ( transform.getOrigin().getX(),
                                transform.getOrigin().getY(),
                                ( xzplane_ ) ? yaw + M_PI/2 : yaw );
    } catch ( tf::TransformException &ex ) {
        ROS_ERROR ( "[%s callbackMarker] %s", ros::this_node::getName().c_str(), ex.what() );
        zt->pose2d() = Pose2D ( 0.225,  0, 0 );
    }

    if ( ( _marker.view_direction.x == 0 ) && ( _marker.view_direction.y == 0 ) && ( _marker.view_direction.z == 0 ) && ( _marker.view_direction.w == 1 ) ) {
        zt->angle_min() = -_marker.fov_horizontal/2.;
        zt->angle_max() = _marker.fov_horizontal/2.;
    } else {
        ROS_ERROR ( "[%s callbackMarker] %s", ros::this_node::getName().c_str(), "This node only deals with straight forward looking view directions" );
    }

    zt->range_min() = _marker.distance_min;
    zt->range_max() = _marker.distance_max;
    zt->range_max_id() = _marker.distance_max_id;
    zt->stamp() = _marker.header.stamp.toBoost();
    zt->resize ( 0 );

    for ( size_t i = 0; i < _marker.markers.size(); i++ ) {
        double x, y, z, roll, pitch, yaw, length, angle, theta;
        tf::Vector3 v;
        tf::Quaternion q;

        tf::pointMsgToTF ( _marker.markers[i].pose.position, v );
        tf::quaternionMsgToTF(_marker.markers[i].pose.orientation, q);
        tf::Matrix3x3(q).getRPY(roll, pitch, yaw);

        if ( xzplane_ ) {   // gazebo
            x = v.getZ();
            y = -v.getX();
            theta = angle_difference(M_PI, pitch);
        } else {            //stage
            x = v.getX();
            y = v.getY();
            theta = yaw;
        }

        length = sqrt( x*x + y*y );
        angle = atan2 ( y, x );

        if (length < zt->range_min() || length > zt->range_max() ||
            angle < zt->angle_min() || angle > zt->angle_max())
            continue;

        MeasurementMarker::Marker zt_i;
        zt_i.ids = _marker.markers[i].ids;
        zt_i.ids_confidence = _marker.markers[i].ids_confidence;
        zt_i.length = length;
        zt_i.angle = angle;
        zt_i.orientation = theta;
        zt_i.pose = Pose2D ( x, y, theta );
        zt->push_back( zt_i );
    }
}

void SLAMNode::callbackConfigSLAM ( tuw_marker_slam::SLAMConfig &config, uint32_t level ) {
    ROS_INFO ( "callbackConfigSLAM!" );
    config_ = config;
}

void SLAMNode::callbackConfigEKFSLAM ( tuw_marker_slam::EKFSLAMConfig &config, uint32_t level ) {
    ROS_INFO ( "callbackConfigEKFSLAM!" );
    slam_technique_->setConfig ( &config );
}
