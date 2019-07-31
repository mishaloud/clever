/*
 * Simplified copter control in OFFBOARD mode
 * Copyright (C) 2019 Copter Express Technologies
 *
 * Author: Oleg Kalachev <okalachev@gmail.com>
 *
 * Distributed under MIT License (available at https://opensource.org/licenses/MIT).
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 */

#include <algorithm>
#include <string>
#include <cmath>
#include <boost/format.hpp>
#include <stdexcept>
#include <GeographicLib/Geodesic.hpp>
#include <ros/ros.h>
#include <tf/transform_datatypes.h>
#include <tf2/utils.h>
#include <tf2_ros/transform_listener.h>
#include <tf2_ros/transform_broadcaster.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#include <std_srvs/Trigger.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/TwistStamped.h>
#include <geometry_msgs/Vector3Stamped.h>
#include <geometry_msgs/QuaternionStamped.h>
#include <sensor_msgs/NavSatFix.h>
#include <sensor_msgs/BatteryState.h>
#include <mavros_msgs/CommandBool.h>
#include <mavros_msgs/SetMode.h>
#include <mavros_msgs/PositionTarget.h>
#include <mavros_msgs/AttitudeTarget.h>
#include <mavros_msgs/Thrust.h>
#include <mavros_msgs/State.h>
#include <mavros_msgs/StatusText.h>

#include <clever/GetTelemetry.h>
#include <clever/Navigate.h>
#include <clever/NavigateGlobal.h>
#include <clever/SetPosition.h>
#include <clever/SetVelocity.h>
#include <clever/SetAttitude.h>
#include <clever/SetRates.h>

using std::string;
using namespace geometry_msgs;
using namespace sensor_msgs;
using namespace clever;
using mavros_msgs::PositionTarget;
using mavros_msgs::AttitudeTarget;
using mavros_msgs::Thrust;

// tf2
tf2_ros::Buffer tf_buffer;
std::shared_ptr<tf2_ros::TransformBroadcaster> transform_broadcaster;

// Parameters
string local_frame;
string fcu_frame;
ros::Duration transform_timeout;
ros::Duration telemetry_transform_timeout;
ros::Duration offboard_timeout;
ros::Duration land_timeout;
ros::Duration arming_timeout;
ros::Duration local_position_timeout;
ros::Duration state_timeout;
ros::Duration velocity_timeout;
ros::Duration global_position_timeout;
ros::Duration battery_timeout;
float default_speed;
bool auto_release;
bool land_only_in_offboard;
std::map<string, string> reference_frames;

// Publishers
ros::Publisher attitude_pub, attitude_raw_pub, position_pub, position_raw_pub, rates_pub, thrust_pub;

// Service clients
ros::ServiceClient arming, set_mode;

// Containers
ros::Timer setpoint_timer;
tf::Quaternion tq;
PoseStamped position_msg;
PositionTarget position_raw_msg;
AttitudeTarget att_raw_msg;
Thrust thrust_msg;
TwistStamped rates_msg;
TransformStamped target;
geometry_msgs::TransformStamped body;

// State
PoseStamped nav_start;
PoseStamped setpoint_position, setpoint_position_transformed;
Vector3Stamped setpoint_velocity, setpoint_velocity_transformed;
QuaternionStamped setpoint_attitude, setpoint_attitude_transformed;
float setpoint_yaw_rate;
float nav_speed;
bool busy = false;
bool wait_armed = false;

enum setpoint_type_t {
	NONE,
	NAVIGATE,
	NAVIGATE_GLOBAL,
	POSITION,
	VELOCITY,
	ATTITUDE,
	RATES
};

enum setpoint_type_t setpoint_type = NONE;

enum { YAW, YAW_RATE, TOWARDS } setpoint_yaw_type;

// Last received telemetry messages
mavros_msgs::State state;
mavros_msgs::StatusText statustext;
PoseStamped local_position;
TwistStamped velocity;
NavSatFix global_position;
BatteryState battery;

// Common subscriber callback template that stores message to the variable
template<typename T, T& STORAGE>
void handleMessage(const T& msg)
{
	STORAGE = msg;
}

inline void publishBodyFrame()
{
	if (body.child_frame_id.empty()) return;

	tf::Quaternion q;
	q.setRPY(0, 0, tf::getYaw(local_position.pose.orientation));
	tf::quaternionTFToMsg(q, body.transform.rotation);

	body.transform.translation.x = local_position.pose.position.x;
	body.transform.translation.y = local_position.pose.position.y;
	body.transform.translation.z = local_position.pose.position.z;
	body.header.frame_id = local_position.header.frame_id;
	body.header.stamp = local_position.header.stamp;
	transform_broadcaster->sendTransform(body);
}

void handleLocalPosition(const PoseStamped& pose)
{
	local_position = pose;
	publishBodyFrame();
	// TODO: terrain?, home?
}

// wait for transform without interrupting publishing setpoints
inline bool waitTransform(const string& target, const string& source,
                          const ros::Time& stamp, const ros::Duration& timeout) // editorconfig-checker-disable-line
{
	ros::Rate r(10);
	auto start = ros::Time::now();
	while (ros::ok()) {
		if (ros::Time::now() - start > timeout) return false;
		if (tf_buffer.canTransform(target, source, stamp)) return true;
		ros::spinOnce();
		r.sleep();
	}
}

#define TIMEOUT(msg, timeout) (ros::Time::now() - msg.header.stamp > timeout)

bool getTelemetry(GetTelemetry::Request& req, GetTelemetry::Response& res)
{
	ros::Time stamp = ros::Time::now();

	if (req.frame_id.empty())
		req.frame_id = local_frame;

	res.frame_id = req.frame_id;
	res.x = NAN;
	res.y = NAN;
	res.z = NAN;
	res.lat = NAN;
	res.lon = NAN;
	res.alt = NAN;
	res.vx = NAN;
	res.vy = NAN;
	res.vz = NAN;
	res.pitch = NAN;
	res.roll = NAN;
	res.yaw = NAN;
	res.pitch_rate = NAN;
	res.roll_rate = NAN;
	res.yaw_rate = NAN;
	res.voltage = NAN;
	res.cell_voltage = NAN;

	if (!TIMEOUT(state, state_timeout)) {
		res.connected = state.connected;
		res.armed = state.armed;
		res.mode = state.mode;
	}

	waitTransform(local_frame, req.frame_id, stamp, telemetry_transform_timeout);

	if (!TIMEOUT(local_position, local_position_timeout)) {
		try {
			// transform pose
			PoseStamped pose;
			tf_buffer.transform(local_position, pose, req.frame_id);
			res.x = pose.pose.position.x;
			res.y = pose.pose.position.y;
			res.z = pose.pose.position.z;

			// Tait-Bryan angles, order z-y-x
			double yaw, pitch, roll;
			tf2::getEulerYPR(pose.pose.orientation, yaw, pitch, roll);
			res.yaw = yaw;
			res.pitch = pitch;
			res.roll = roll;
		} catch (const tf2::TransformException& e) {}
	}

	if (!TIMEOUT(velocity, velocity_timeout)) {
		try {
			// transform velocity
			Vector3Stamped vec, vec_out;
			vec.header = velocity.header;
			vec.vector = velocity.twist.linear;
			tf_buffer.transform(vec, vec_out, req.frame_id);

			res.vx = vec_out.vector.x;
			res.vy = vec_out.vector.y;
			res.vz = vec_out.vector.z;
		} catch (const tf2::TransformException& e) {}

		// use angular velocities as they are
		res.yaw_rate = velocity.twist.angular.z;
		res.pitch_rate = velocity.twist.angular.y;
		res.roll_rate = velocity.twist.angular.x;
	}

	if (!TIMEOUT(global_position, global_position_timeout)) {
		res.lat = global_position.latitude;
		res.lon = global_position.longitude;
		res.alt = global_position.altitude;
	}

	if (!TIMEOUT(battery, battery_timeout)) {
		res.voltage = battery.voltage;
		if (!battery.cell_voltage.empty()) {
			res.cell_voltage = battery.cell_voltage[0];
		}
	}

	return true;
}

// throws std::runtime_error
void offboardAndArm()
{
	ros::Rate r(10);

	if (state.mode != "OFFBOARD") {
		auto start = ros::Time::now();
		ROS_INFO("simple_offboard: switch to OFFBOARD");
		static mavros_msgs::SetMode sm;
		sm.request.custom_mode = "OFFBOARD";

		if (!set_mode.call(sm))
			throw std::runtime_error("Error calling set_mode service");

		// wait for OFFBOARD mode
		while (ros::ok()) {
			ros::spinOnce();
			if (state.mode == "OFFBOARD") {
				break;
			} else if (ros::Time::now() - start > offboard_timeout) {
				string report = "OFFBOARD timed out";
				if (statustext.header.stamp > start)
					report += ": " + statustext.text;
				throw std::runtime_error(report);
			}
			ros::spinOnce();
			r.sleep();
		}
	}

	if (!state.armed) {
		ros::Time start = ros::Time::now();
		ROS_INFO("simple_offboard: arming");
		mavros_msgs::CommandBool srv;
		srv.request.value = true;
		if (!arming.call(srv)) {
			throw std::runtime_error("Error calling arming service");
		}

		// wait until armed
		while (ros::ok()) {
			ros::spinOnce();
			if (state.armed) {
				break;
			} else if (ros::Time::now() - start > arming_timeout) {
				string report = "Arming timed out";
				if (statustext.header.stamp > start)
					report += ": " + statustext.text;
				throw std::runtime_error(report);
			}
			ros::spinOnce();
			r.sleep();
		}
	}
}

inline double hypot(double x, double y, double z)
{
	return std::sqrt(x * x + y * y + z * z);
}

inline float getDistance(const Point& from, const Point& to)
{
	return hypot(from.x - to.x, from.y - to.y, from.z - to.z);
}

void getNavigateSetpoint(const ros::Time& stamp, float speed, Point& nav_setpoint)
{
	if (wait_armed) {
		// don't start navigating if we're waiting arming
		nav_start.header.stamp = stamp;
	}

	float distance = getDistance(nav_start.pose.position, setpoint_position_transformed.pose.position);
	float time = distance / speed;
	float passed = std::min((stamp - nav_start.header.stamp).toSec() / time, 1.0);

	nav_setpoint.x = nav_start.pose.position.x + (setpoint_position_transformed.pose.position.x - nav_start.pose.position.x) * passed;
	nav_setpoint.y = nav_start.pose.position.y + (setpoint_position_transformed.pose.position.y - nav_start.pose.position.y) * passed;
	nav_setpoint.z = nav_start.pose.position.z + (setpoint_position_transformed.pose.position.z - nav_start.pose.position.z) * passed;
}

PoseStamped globalToLocal(double lat, double lon)
{
	auto earth = GeographicLib::Geodesic::WGS84();

	// Determine azimuth and distance between current and destination point
	double _, distance, azimuth;
	earth.Inverse(global_position.latitude, global_position.longitude, lat, lon, distance, _, azimuth);

	double x_offset, y_offset;
	double azimuth_radians = azimuth * M_PI / 180;
	x_offset = distance * sin(azimuth_radians);
	y_offset = distance * cos(azimuth_radians);

	auto local = tf_buffer.lookupTransform(local_frame, fcu_frame, global_position.header.stamp);

	PoseStamped pose;
	pose.header.stamp = global_position.header.stamp; // TODO: ?
	pose.header.frame_id = local_frame;
	pose.pose.position.x = local.transform.translation.x + x_offset;
	pose.pose.position.y = local.transform.translation.y + y_offset;
	pose.pose.orientation.w = 1;
	return pose;
}

void publish(const ros::Time stamp)
{
	if (setpoint_type == NONE) return;

	position_raw_msg.header.stamp = stamp;
	thrust_msg.header.stamp = stamp;
	rates_msg.header.stamp = stamp;

	try {
		// transform position and/or yaw
		if (setpoint_type == NAVIGATE || setpoint_type == NAVIGATE_GLOBAL || setpoint_type == POSITION || setpoint_type == VELOCITY || setpoint_type == ATTITUDE) {
			setpoint_position.header.stamp = stamp;
			tf_buffer.transform(setpoint_position, setpoint_position_transformed, local_frame, ros::Duration(0.05));
		}

		// transform velocity
		if (setpoint_type == VELOCITY) {
			setpoint_velocity.header.stamp = stamp;
			tf_buffer.transform(setpoint_velocity, setpoint_velocity_transformed, local_frame, ros::Duration(0.05));
		}

	} catch (const tf2::TransformException& e) {
		ROS_WARN_THROTTLE(10, "simple_offboard: can't transform");
	}

	if (!target.child_frame_id.empty()) {
		if (setpoint_type == NAVIGATE || setpoint_type == NAVIGATE_GLOBAL || setpoint_type == POSITION) {
			target.header = setpoint_position_transformed.header;
			target.transform.translation.x = setpoint_position_transformed.pose.position.x;
			target.transform.translation.y = setpoint_position_transformed.pose.position.y;
			target.transform.translation.z = setpoint_position_transformed.pose.position.z;
			target.transform.rotation = setpoint_position_transformed.pose.orientation;
			transform_broadcaster->sendTransform(target);
		}
	}

	if (setpoint_type == NAVIGATE || setpoint_type == NAVIGATE_GLOBAL) {
		position_msg.pose.orientation = setpoint_position_transformed.pose.orientation; // copy yaw
		getNavigateSetpoint(stamp, nav_speed, position_msg.pose.position);

		if (setpoint_yaw_type == TOWARDS) {
			double yaw_towards = atan2(position_msg.pose.position.y - nav_start.pose.position.y,
			                           position_msg.pose.position.x - nav_start.pose.position.x);
			position_msg.pose.orientation = tf::createQuaternionMsgFromRollPitchYaw(0, 0, yaw_towards);
		}
	}

	if (setpoint_type == POSITION) {
		position_msg = setpoint_position_transformed;
	}

	if (setpoint_type == POSITION || setpoint_type == NAVIGATE || setpoint_type == NAVIGATE_GLOBAL) {
		if (setpoint_yaw_type == YAW || setpoint_yaw_type == TOWARDS) {
			position_msg.header.stamp = stamp;
			position_pub.publish(position_msg);

		} else {
			position_raw_msg.type_mask = PositionTarget::IGNORE_VX +
			                             PositionTarget::IGNORE_VY +
			                             PositionTarget::IGNORE_VZ +
			                             PositionTarget::IGNORE_AFX +
			                             PositionTarget::IGNORE_AFY +
			                             PositionTarget::IGNORE_AFZ +
			                             PositionTarget::IGNORE_YAW;
			position_raw_msg.yaw_rate = setpoint_yaw_rate;
			position_raw_msg.position = position_msg.pose.position;
			position_raw_pub.publish(position_raw_msg);
		}
	}

	if (setpoint_type == VELOCITY) {
		position_raw_msg.type_mask = PositionTarget::IGNORE_PX +
		                             PositionTarget::IGNORE_PY +
		                             PositionTarget::IGNORE_PZ +
		                             PositionTarget::IGNORE_AFX +
		                             PositionTarget::IGNORE_AFY +
		                             PositionTarget::IGNORE_AFZ;
		position_raw_msg.type_mask += setpoint_yaw_type == YAW ? PositionTarget::IGNORE_YAW_RATE : PositionTarget::IGNORE_YAW;
		position_raw_msg.velocity = setpoint_velocity_transformed.vector;
		position_raw_msg.yaw = tf2::getYaw(setpoint_position_transformed.pose.orientation);
		position_raw_msg.yaw_rate = setpoint_yaw_rate;
		position_raw_pub.publish(position_raw_msg);
	}

	if (setpoint_type == ATTITUDE) {
		attitude_pub.publish(setpoint_position_transformed);
		thrust_pub.publish(thrust_msg);
	}

	if (setpoint_type == RATES) {
		// rates_pub.publish(rates_msg);
		// thrust_pub.publish(thrust_msg);
		// mavros rates topics waits for rates in local frame
		// use rates in body frame for simplicity
		att_raw_msg.header.stamp = stamp;
		att_raw_msg.header.frame_id = fcu_frame;
		att_raw_msg.type_mask = AttitudeTarget::IGNORE_ATTITUDE;
		att_raw_msg.body_rate = rates_msg.twist.angular;
		att_raw_msg.thrust = thrust_msg.thrust;
		attitude_raw_pub.publish(att_raw_msg);
	}
}

void publishSetpoint(const ros::TimerEvent& event)
{
	publish(event.current_real);
}

inline void checkState()
{
	if (TIMEOUT(state, state_timeout))
		throw std::runtime_error("State timeout, check mavros settings");

	if (!state.connected)
		throw std::runtime_error("No connection to FCU, https://clever.copterexpress.com/connection.html");
}

bool serve(enum setpoint_type_t sp_type, float x, float y, float z, float vx, float vy, float vz,
           float pitch, float roll, float yaw, float pitch_rate, float roll_rate, float yaw_rate, // editorconfig-checker-disable-line
           float lat, float lon, float thrust, float speed, string frame_id, bool auto_arm, // editorconfig-checker-disable-line
           uint8_t& success, string& message) // editorconfig-checker-disable-line
{
	auto stamp = ros::Time::now();

	try {
		if (busy)
			throw std::runtime_error("Busy");

		busy = true;

		// Checks
		checkState();

		if (sp_type == NAVIGATE || sp_type == NAVIGATE_GLOBAL) {
			if (TIMEOUT(local_position, local_position_timeout))
				throw std::runtime_error("No local position, check settings");

			if (speed < 0)
				throw std::runtime_error("Navigate speed must be positive, " + std::to_string(speed) + " passed");

			if (speed == 0)
				speed = default_speed;
		}

		if (sp_type == NAVIGATE || sp_type == NAVIGATE_GLOBAL || sp_type == POSITION || sp_type == VELOCITY) {
			if (yaw_rate != 0 && !std::isnan(yaw))
				throw std::runtime_error("Yaw value should be NaN for setting yaw rate");

			if (std::isnan(yaw_rate) && std::isnan(yaw))
				throw std::runtime_error("Both yaw and yaw_rate cannot be NaN");
		}

		if (sp_type == NAVIGATE_GLOBAL) {
			if (TIMEOUT(global_position, global_position_timeout))
				throw std::runtime_error("No global position");
		}

		// default frame is local frame
		if (frame_id.empty())
			frame_id = local_frame;

		// look up for reference frame
		auto search = reference_frames.find(frame_id);
		const string& reference_frame = search == reference_frames.end() ? frame_id : search->second;

		if (sp_type == NAVIGATE || sp_type == NAVIGATE_GLOBAL || sp_type == POSITION || sp_type == VELOCITY || sp_type == ATTITUDE) {
			// make sure transform from frame_id to reference frame available
			if (!waitTransform(reference_frame, frame_id, stamp, transform_timeout))
				throw std::runtime_error("Can't transform from " + frame_id + " to " + reference_frame);

			// make sure transform from reference frame to local frame available
			if (!waitTransform(local_frame, reference_frame, stamp, transform_timeout))
				throw std::runtime_error("Can't transform from " + reference_frame + " to " + local_frame);
		}

		if (sp_type == NAVIGATE_GLOBAL) {
			// Calculate x and from lat and lot in request's frame
			auto xy_in_req_frame = tf_buffer.transform(globalToLocal(lat, lon), frame_id);
			x = xy_in_req_frame.pose.position.x;
			y = xy_in_req_frame.pose.position.y;
		}

		// Everything fine - switch setpoint type
		setpoint_type = sp_type;

		if (sp_type == NAVIGATE || sp_type == NAVIGATE_GLOBAL) {
			// starting point
			nav_start = local_position;
			nav_speed = speed;
		}

		// if (sp_type == NAVIGATE || sp_type == NAVIGATE_GLOBAL || sp_type == POSITION || sp_type == VELOCITY) {
		// 	if (std::isnan(yaw) && yaw_rate == 0) {
		// 		// keep yaw unchanged
		//		// TODO: this is incorrect, because we need yaw in desired frame
		// 		yaw = tf2::getYaw(local_position.pose.orientation);
		// 	}
		// }

		if (sp_type == POSITION || sp_type == NAVIGATE || sp_type == NAVIGATE_GLOBAL || sp_type == VELOCITY || sp_type == ATTITUDE) {
			// destination point and/or yaw
			static PoseStamped ps;
			ps.header.frame_id = frame_id;
			ps.header.stamp = stamp;
			ps.pose.position.x = x;
			ps.pose.position.y = y;
			ps.pose.position.z = z;

			if (std::isnan(yaw)) {
				setpoint_yaw_type = YAW_RATE;
				setpoint_yaw_rate = yaw_rate;
			} else if (std::isinf(yaw) && yaw > 0) {
				// yaw towards
				setpoint_yaw_type = TOWARDS;
				yaw = 0;
				setpoint_yaw_rate = 0;
			} else {
				setpoint_yaw_type = YAW;
				setpoint_yaw_rate = 0;
				ps.pose.orientation = tf::createQuaternionMsgFromRollPitchYaw(roll, pitch, yaw);
			}

			tf_buffer.transform(ps, setpoint_position, reference_frame);
		}

		if (sp_type == VELOCITY) {
			static Vector3Stamped vel;
			vel.header.frame_id = frame_id;
			vel.header.stamp = stamp;
			vel.vector.x = vx;
			vel.vector.y = vy;
			vel.vector.z = vz;
			tf_buffer.transform(vel, setpoint_velocity, reference_frame);
		}

		if (sp_type == ATTITUDE || sp_type == RATES) {
			thrust_msg.thrust = thrust;
		}

		if (sp_type == RATES) {
			rates_msg.twist.angular.x = roll_rate;
			rates_msg.twist.angular.y = pitch_rate;
			rates_msg.twist.angular.z = yaw_rate;
		}

		wait_armed = auto_arm;

		publish(stamp); // calculate initial transformed messages first
		setpoint_timer.start();

		if (auto_arm) {
			offboardAndArm();
			wait_armed = false;
		} else if (state.mode != "OFFBOARD") {
			setpoint_timer.stop();
			throw std::runtime_error("Copter is not in OFFBOARD mode, use auto_arm?");
		} else if (!state.armed) {
			setpoint_timer.stop();
			throw std::runtime_error("Copter is not armed, use auto_arm?");
		}

	} catch (const std::exception& e) {
		message = e.what();
		ROS_INFO("simple_offboard: %s", message.c_str());
		busy = false;
		return true;
	}

	success = true;
	busy = false;
	return true;
}

bool navigate(Navigate::Request& req, Navigate::Response& res) {
	return serve(NAVIGATE, req.x, req.y, req.z, 0, 0, 0, 0, 0, req.yaw, 0, 0, req.yaw_rate, 0, 0, 0, req.speed, req.frame_id, req.auto_arm, res.success, res.message);
}

bool navigateGlobal(NavigateGlobal::Request& req, NavigateGlobal::Response& res) {
	return serve(NAVIGATE_GLOBAL, 0, 0, req.z, 0, 0, 0, 0, 0, req.yaw, 0, 0, req.yaw_rate, req.lat, req.lon, 0, req.speed, req.frame_id, req.auto_arm, res.success, res.message);
}

bool setPosition(SetPosition::Request& req, SetPosition::Response& res) {
	return serve(POSITION, req.x, req.y, req.z, 0, 0, 0, 0, 0, req.yaw, 0, 0, req.yaw_rate, 0, 0, 0, 0, req.frame_id, req.auto_arm, res.success, res.message);
}

bool setVelocity(SetVelocity::Request& req, SetVelocity::Response& res) {
	return serve(VELOCITY, 0, 0, 0, req.vx, req.vy, req.vz, 0, 0, req.yaw, 0, 0, req.yaw_rate, 0, 0, 0, 0, req.frame_id, req.auto_arm, res.success, res.message);
}

bool setAttitude(SetAttitude::Request& req, SetAttitude::Response& res) {
	return serve(ATTITUDE, 0, 0, 0, 0, 0, 0, req.pitch, req.roll, req.yaw, 0, 0, 0, 0, 0, req.thrust, 0, req.frame_id, req.auto_arm, res.success, res.message);
}

bool setRates(SetRates::Request& req, SetRates::Response& res) {
	return serve(RATES, 0, 0, 0, 0, 0, 0, 0, 0, 0, req.pitch_rate, req.roll_rate, req.yaw_rate, 0, 0, req.thrust, 0, "", req.auto_arm, res.success, res.message);
}

bool land(std_srvs::Trigger::Request& req, std_srvs::Trigger::Response& res)
{
	try {
		if (busy)
			throw std::runtime_error("Busy");

		busy = true;

		checkState();

		if (land_only_in_offboard) {
			if (state.mode != "OFFBOARD") {
				throw std::runtime_error("Copter is not in OFFBOARD mode");
			}
		}

		static mavros_msgs::SetMode sm;
		sm.request.custom_mode = "AUTO.LAND";

		if (!set_mode.call(sm))
			throw std::runtime_error("Can't call set_mode service");

		if (!sm.response.mode_sent)
			throw std::runtime_error("Can't send set_mode request");

		static ros::Rate r(10);
		auto start = ros::Time::now();
		while (ros::ok()) {
			if (state.mode == "AUTO.LAND") {
				res.success = true;
				busy = false;
				return true;
			}
			if (ros::Time::now() - start > land_timeout)
				throw std::runtime_error("Land request timed out");

			ros::spinOnce();
			r.sleep();
		}

	} catch (const std::exception& e) {
		res.message = e.what();
		ROS_INFO("simple_offboard: %s", e.what());
		busy = false;
		return true;
	}
}

int main(int argc, char **argv)
{
	ros::init(argc, argv, "simple_offboard");
	ros::NodeHandle nh, nh_priv("~");

	tf2_ros::TransformListener tf_listener(tf_buffer);
	transform_broadcaster = std::make_shared<tf2_ros::TransformBroadcaster>();

	// Params
	nh.param<string>("mavros/local_position/tf/frame_id", local_frame, "map");
	nh.param<string>("mavros/local_position/tf/child_frame_id", fcu_frame, "base_link");
	nh_priv.param("target_frame", target.child_frame_id, string("navigate_target"));
	nh_priv.param("auto_release", auto_release, true);
	nh_priv.param("land_only_in_offboard", land_only_in_offboard, true);
	nh_priv.param("default_speed", default_speed, 0.5f);
	nh_priv.param<string>("body_frame", body.child_frame_id, "body");
	nh_priv.getParam("reference_frames", reference_frames);

	state_timeout = ros::Duration(nh_priv.param("state_timeout", 3.0));
	local_position_timeout = ros::Duration(nh_priv.param("local_position_timeout", 2.0));
	velocity_timeout = ros::Duration(nh_priv.param("velocity_timeout", 2.0));
	global_position_timeout = ros::Duration(nh_priv.param("global_position_timeout", 10.0));
	battery_timeout = ros::Duration(nh_priv.param("battery_timeout", 2.0));

	transform_timeout = ros::Duration(nh_priv.param("transform_timeout", 0.5));
	telemetry_transform_timeout = ros::Duration(nh_priv.param("telemetry_transform_timeout", 0.5));
	offboard_timeout = ros::Duration(nh_priv.param("offboard_timeout", 3.0));
	land_timeout = ros::Duration(nh_priv.param("land_timeout", 3.0));
	arming_timeout = ros::Duration(nh_priv.param("arming_timeout", 4.0));

	// Service clients
	arming = nh.serviceClient<mavros_msgs::CommandBool>("mavros/cmd/arming");
	set_mode = nh.serviceClient<mavros_msgs::SetMode>("mavros/set_mode");

	// Telemetry subscribers
	auto state_sub = nh.subscribe("mavros/state", 1, &handleMessage<mavros_msgs::State, state>);
	auto velocity_sub = nh.subscribe("mavros/local_position/velocity", 1, &handleMessage<TwistStamped, velocity>);
	auto global_position_sub = nh.subscribe("mavros/global_position/global", 1, &handleMessage<NavSatFix, global_position>);
	auto battery_sub = nh.subscribe("mavros/battery", 1, &handleMessage<BatteryState, battery>);
	auto statustext_sub = nh.subscribe("mavros/statustext/recv", 1, &handleMessage<mavros_msgs::StatusText, statustext>);
	auto local_position_sub = nh.subscribe("mavros/local_position/pose", 1, &handleLocalPosition);

	// Setpoint publishers
	position_pub = nh.advertise<PoseStamped>("mavros/setpoint_position/local", 1);
	position_raw_pub = nh.advertise<PositionTarget>("mavros/setpoint_raw/local", 1);
	attitude_pub = nh.advertise<PoseStamped>("mavros/setpoint_attitude/attitude", 1);
	attitude_raw_pub = nh.advertise<AttitudeTarget>("mavros/setpoint_raw/attitude", 1);
	rates_pub = nh.advertise<TwistStamped>("mavros/setpoint_attitude/cmd_vel", 1);
	thrust_pub = nh.advertise<Thrust>("mavros/setpoint_attitude/thrust", 1);

	 // Service servers
	auto gt_serv = nh.advertiseService("get_telemetry", &getTelemetry);
	auto na_serv = nh.advertiseService("navigate", &navigate);
	auto ng_serv = nh.advertiseService("navigate_global", &navigateGlobal);
	auto sp_serv = nh.advertiseService("set_position", &setPosition);
	auto sv_serv = nh.advertiseService("set_velocity", &setVelocity);
	auto sa_serv = nh.advertiseService("set_attitude", &setAttitude);
	auto sr_serv = nh.advertiseService("set_rates", &setRates);
	auto ld_serv = nh.advertiseService("land", &land);

	// Setpoint timer
	setpoint_timer = nh.createTimer(ros::Duration(1 / nh_priv.param("setpoint_rate", 30.0)), &publishSetpoint, false, false);

	position_msg.header.frame_id = local_frame;
	position_raw_msg.header.frame_id = local_frame;
	position_raw_msg.coordinate_frame = PositionTarget::FRAME_LOCAL_NED;
	rates_msg.header.frame_id = fcu_frame;

	ROS_INFO("simple_offboard: ready");
	ros::spin();
}
