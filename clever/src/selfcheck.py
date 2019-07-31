#!/usr/bin/env python

# Copyright (C) 2018 Copter Express Technologies
#
# Author: Oleg Kalachev <okalachev@gmail.com>
#
# Distributed under MIT License (available at https://opensource.org/licenses/MIT).
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.

import math
import subprocess
import re
import traceback
from threading import Event
import numpy
import rospy
from systemd import journal
import tf2_ros
import tf2_geometry_msgs
from pymavlink import mavutil
from std_srvs.srv import Trigger
from sensor_msgs.msg import Image, CameraInfo, NavSatFix, Imu, Range
from mavros_msgs.msg import State, OpticalFlowRad, Mavlink
from mavros_msgs.srv import ParamGet
from geometry_msgs.msg import PoseStamped, TwistStamped, PoseWithCovarianceStamped, Vector3Stamped
from visualization_msgs.msg import MarkerArray as VisualizationMarkerArray
import tf.transformations as t
from aruco_pose.msg import MarkerArray
from mavros import mavlink


# TODO: check attitude is present
# TODO: disk free space
# TODO: map, base_link, body
# TODO: rc service
# TODO: perform commander check, ekf2 status on PX4
# TODO: check if FCU params setter succeed
# TODO: selfcheck ROS service (with blacklists for checks)


rospy.init_node('selfcheck')


tf_buffer = tf2_ros.Buffer()
tf_listener = tf2_ros.TransformListener(tf_buffer)


failures = []
infos = []
current_check = None


def failure(text, *args):
    msg = text % args
    rospy.logwarn('%s: %s', current_check, msg)
    failures.append(msg)


def info(text, *args):
    msg = text % args
    rospy.loginfo('%s: %s', current_check, msg)
    infos.append(msg)


def check(name):
    def inner(fn):
        def wrapper(*args, **kwargs):
            failures[:] = []
            infos[:] = []
            global current_check
            current_check = name
            try:
                fn(*args, **kwargs)
            except Exception as e:
                traceback.print_exc()
                rospy.logerr('%s: exception occurred', name)
                return
            if not failures and not infos:
                rospy.loginfo('%s: OK', name)
        return wrapper
    return inner


param_get = rospy.ServiceProxy('mavros/param/get', ParamGet)


def get_param(name):
    try:
        res = param_get(param_id=name)
    except rospy.ServiceException as e:
        failure('%s: %s', name, str(e))
        return None

    if not res.success:
        failure('Unable to retrieve PX4 parameter %s', name)
    else:
        if res.value.integer != 0:
            return res.value.integer
        return res.value.real


recv_event = Event()
link = mavutil.mavlink.MAVLink('', 255, 1)
mavlink_pub = rospy.Publisher('mavlink/to', Mavlink, queue_size=1)
mavlink_recv = ''


def mavlink_message_handler(msg):
    global mavlink_recv
    if msg.msgid == 126:
        mav_bytes_msg = mavlink.convert_to_bytes(msg)
        mav_msg = link.decode(mav_bytes_msg)
        mavlink_recv += ''.join(chr(x) for x in mav_msg.data[:mav_msg.count])
        if 'nsh>' in mavlink_recv:
            # Remove the last line, including newline before prompt
            mavlink_recv = mavlink_recv[:mavlink_recv.find('nsh>') - 1]
            recv_event.set()


mavlink_sub = rospy.Subscriber('mavlink/from', Mavlink, mavlink_message_handler)
# FIXME: not sleeping here still breaks things
rospy.sleep(0.5)


def mavlink_exec(cmd, timeout=3.0):
    global mavlink_recv
    mavlink_recv = ''
    recv_event.clear()
    if not cmd.endswith('\n'):
        cmd += '\n'
    msg = mavutil.mavlink.MAVLink_serial_control_message(
        device=mavutil.mavlink.SERIAL_CONTROL_DEV_SHELL,
        flags=mavutil.mavlink.SERIAL_CONTROL_FLAG_RESPOND | mavutil.mavlink.SERIAL_CONTROL_FLAG_EXCLUSIVE |
              mavutil.mavlink.SERIAL_CONTROL_FLAG_MULTI,
        timeout=3,
        baudrate=0,
        count=len(cmd),
        data=map(ord, cmd.ljust(70, '\0')))
    msg.pack(link)
    ros_msg = mavlink.convert_to_rosmsg(msg)
    mavlink_pub.publish(ros_msg)
    recv_event.wait(timeout)
    return mavlink_recv


@check('FCU')
def check_fcu():
    try:
        state = rospy.wait_for_message('mavros/state', State, timeout=3)
        if not state.connected:
            failure('no connection to the FCU (check wiring)')
            return

        # Make sure the console is available to us
        mavlink_exec('\n')
        version_str = mavlink_exec('ver all')
        if version_str == '':
            info('no version data available from SITL')

        r = re.compile(r'^FW (git tag|version): (v?\d\.\d\.\d.*)$')
        is_clever_firmware = False
        for ver_line in version_str.split('\n'):
            match = r.search(ver_line)
            if match is not None:
                field, version = match.groups()
                info('firmware %s: %s' % (field, version))
                if 'clever' in version:
                    is_clever_firmware = True

        if not is_clever_firmware:
            failure('not running Clever PX4 firmware, check http://clever.copterexpress.com/firmware.html')

        est = get_param('SYS_MC_EST_GROUP')
        if est == 1:
            info('selected estimator: LPE')
            fuse = get_param('LPE_FUSION')
            if fuse & (1 << 4):
                info('LPE_FUSION: land detector fusion is enabled')
            else:
                info('LPE_FUSION: land detector fusion is disabled')
            if fuse & (1 << 7):
                info('LPE_FUSION: barometer fusion is enabled')
            else:
                info('LPE_FUSION: barometer fusion is disabled')

        elif est == 2:
            info('selected estimator: EKF2')
        else:
            failure('unknown selected estimator: %s', est)

    except rospy.ROSException:
        failure('no MAVROS state (check wiring)')


def describe_direction(v):
    if v.x > 0.9:
        return 'forward'
    elif v.x < - 0.9:
        return 'backward'
    elif v.y > 0.9:
        return 'left'
    elif v.y < -0.9:
        return 'right'
    elif v.z > 0.9:
        return 'upward'
    elif v.z < -0.9:
        return 'downward'
    else:
        return None


def check_camera(name):
    try:
        img = rospy.wait_for_message(name + '/image_raw', Image, timeout=1)
    except rospy.ROSException:
        failure('%s: no images (is the camera connected properly?)', name)
        return
    try:
        camera_info = rospy.wait_for_message(name + '/camera_info', CameraInfo, timeout=1)
    except rospy.ROSException:
        failure('%s: no calibration info', name)
        return

    if img.width != camera_info.width:
        failure('%s: calibration width doesn\'t match image width (%d != %d)', name, camera_info.width, img.width)
    if img.height != camera_info.height:
        failure('%s: calibration height doesn\'t match image height (%d != %d))', name, camera_info.height, img.height)

    try:
        optical = Vector3Stamped()
        optical.header.frame_id = img.header.frame_id
        optical.vector.z = 1
        cable = Vector3Stamped()
        cable.header.frame_id = img.header.frame_id
        cable.vector.y = 1

        optical = describe_direction(tf_buffer.transform(optical, 'base_link').vector)
        cable = describe_direction(tf_buffer.transform(cable, 'base_link').vector)
        if not optical or not cable:
            info('%s: custom camera orientation detected', name)
        else:
            info('camera is oriented %s, camera cable goes %s', optical, cable)

    except tf2_ros.TransformException:
        failure('cannot transform from base_link to camera frame')


@check('Main camera')
def check_main_camera():
    check_camera('main_camera')


def is_process_running(binary, exact=False, full=False):
    try:
        args = ['pgrep']
        if exact:
            args.append('-x')  # match exactly with the command name
        if full:
            args.append('-f')  # use full process name to match
        args.append(binary)
        subprocess.check_output(args)
        return True
    except subprocess.CalledProcessError:
        return False


@check('ArUco markers')
def check_aruco():
    if is_process_running('aruco_detect', full=True):
        info('aruco_detect/length = %g m', rospy.get_param('aruco_detect/length'))
        known_tilt = rospy.get_param('aruco_detect/known_tilt')
        if known_tilt == 'map':
            known_tilt += ' (ALL markers are on the floor)'
        elif known_tilt == 'map_flipped':
            known_tilt += ' (ALL markers are on the ceiling)'
        info('aruco_detector/known_tilt = %s', known_tilt)
        try:
            rospy.wait_for_message('aruco_detect/markers', MarkerArray, timeout=1)
        except rospy.ROSException:
            failure('no markers detection')
            return
    else:
        info('aruco_detect is not running')
        return

    if is_process_running('aruco_map', full=True):
        known_tilt = rospy.get_param('aruco_map/known_tilt')
        if known_tilt == 'map':
            known_tilt += ' (marker\'s map is on the floor)'
        elif known_tilt == 'map_flipped':
            known_tilt += ' (marker\'s map is on the ceiling)'
        info('aruco_map/known_tilt = %s', known_tilt)

        try:
            visualization = rospy.wait_for_message('aruco_map/visualization', VisualizationMarkerArray, timeout=1)
            info('map has %s markers', len(visualization.markers))
        except:
            failure('cannot read aruco_map/visualization topic')

        try:
            rospy.wait_for_message('aruco_map/pose', PoseWithCovarianceStamped, timeout=1)
        except rospy.ROSException:
            failure('no map detection')
    else:
        info('aruco_map is not running')


@check('Vision position estimate')
def check_vpe():
    vis = None
    try:
        vis = rospy.wait_for_message('mavros/vision_pose/pose', PoseStamped, timeout=1)
    except rospy.ROSException:
        try:
            vis = rospy.wait_for_message('mavros/mocap/pose', PoseStamped, timeout=1)
        except rospy.ROSException:
            failure('no VPE or MoCap messages')
            # check if vpe_publisher is running
            try:
                subprocess.check_output(['pgrep', '-x', 'vpe_publisher'])
            except subprocess.CalledProcessError:
                return  # it's not running, skip following checks

    # check PX4 settings
    est = get_param('SYS_MC_EST_GROUP')
    if est == 1:
        ext_yaw = get_param('ATT_EXT_HDG_M')
        if ext_yaw != 1:
            failure('vision yaw is disabled, change ATT_EXT_HDG_M parameter')
        vision_yaw_w = get_param('ATT_W_EXT_HDG')
        if vision_yaw_w == 0:
            failure('vision yaw weight is zero, change ATT_W_EXT_HDG parameter')
        else:
            info('Vision yaw weight: %.2f', vision_yaw_w)
        fuse = get_param('LPE_FUSION')
        if not fuse & (1 << 2):
            failure('vision position fusion is disabled, change LPE_FUSION parameter')
        delay = get_param('LPE_VIS_DELAY')
        if delay != 0:
            failure('LPE_VIS_DELAY parameter is %s, but it should be zero', delay)
        info('LPE_VIS_XY is %.2f m, LPE_VIS_Z is %.2f m', get_param('LPE_VIS_XY'), get_param('LPE_VIS_Z'))
    elif est == 2:
        fuse = get_param('EKF2_AID_MASK')
        if not fuse & (1 << 3):
            failure('vision position fusion is disabled, change EKF2_AID_MASK parameter')
        if not fuse & (1 << 4):
            failure('vision yaw fusion is disabled, change EKF2_AID_MASK parameter')
        delay = get_param('EKF2_EV_DELAY')
        if delay != 0:
            failure('EKF2_EV_DELAY is %.2f, but it should be zero', delay)
        info('EKF2_EVA_NOISE is %.3f, EKF2_EVP_NOISE is %.3f',
            get_param('EKF2_EVA_NOISE'),
            get_param('EKF2_EVP_NOISE'))

    if not vis:
        return

    # check vision pose and estimated pose inconsistency
    try:
        pose = rospy.wait_for_message('mavros/local_position/pose', PoseStamped, timeout=1)
    except:
        return
    horiz = math.hypot(vis.pose.position.x - pose.pose.position.x, vis.pose.position.y - pose.pose.position.y)
    if horiz > 0.5:
        failure('horizontal position inconsistency: %.2f m', horiz)
    vert = vis.pose.position.z - pose.pose.position.z
    if abs(vert) > 0.5:
        failure('vertical position inconsistency: %.2f m', vert)
    op = pose.pose.orientation
    ov = vis.pose.orientation
    yawp, _, _ = t.euler_from_quaternion((op.x, op.y, op.z, op.w), axes='rzyx')
    yawv, _, _ = t.euler_from_quaternion((ov.x, ov.y, ov.z, ov.w), axes='rzyx')
    yawdiff = yawp - yawv
    yawdiff = math.degrees((yawdiff + 180) % 360 - 180)
    if abs(yawdiff) > 8:
        failure('yaw inconsistency: %.2f deg', yawdiff)


@check('Simple offboard node')
def check_simpleoffboard():
    try:
        rospy.wait_for_service('navigate', timeout=3)
        rospy.wait_for_service('get_telemetry', timeout=3)
        rospy.wait_for_service('land', timeout=3)
    except rospy.ROSException:
        failure('no simple_offboard services')


@check('IMU')
def check_imu():
    try:
        rospy.wait_for_message('mavros/imu/data', Imu, timeout=1)
    except rospy.ROSException:
        failure('no IMU data (check flight controller calibration)')


@check('Local position')
def check_local_position():
    try:
        pose = rospy.wait_for_message('mavros/local_position/pose', PoseStamped, timeout=1)
        o = pose.pose.orientation
        _, pitch, roll = t.euler_from_quaternion((o.x, o.y, o.z, o.w), axes='rzyx')
        MAX_ANGLE = math.radians(2)
        if abs(pitch) > MAX_ANGLE:
            failure('pitch is %.2f deg; place copter horizontally or redo level horizon calib',
                    math.degrees(pitch))
        if abs(roll) > MAX_ANGLE:
            failure('roll is %.2f deg; place copter horizontally or redo level horizon calib',
                    math.degrees(roll))

    except rospy.ROSException:
        failure('no local position')


@check('Velocity estimation')
def check_velocity():
    try:
        velocity = rospy.wait_for_message('mavros/local_position/velocity', TwistStamped, timeout=1)
        horiz = math.hypot(velocity.twist.linear.x, velocity.twist.linear.y)
        vert = velocity.twist.linear.z
        if abs(horiz) > 0.1:
            failure('horizontal velocity estimation is %.2f m/s; is copter staying still?' % horiz)
        if abs(vert) > 0.1:
            failure('vertical velocity estimation is %.2f m/s; is copter staying still?' % vert)

        angular = velocity.twist.angular
        ANGULAR_VELOCITY_LIMIT = 0.1
        if abs(angular.x) > ANGULAR_VELOCITY_LIMIT:
            failure('pitch rate estimation is %.2f rad/s (%.2f deg/s); is copter staying still?',
                    angular.x, math.degrees(angular.x))
        if abs(angular.y) > ANGULAR_VELOCITY_LIMIT:
            failure('pitch rate estimation is %.2f rad/s (%.2f deg/s); is copter staying still?',
                    angular.y, math.degrees(angular.y))
        if abs(angular.z) > ANGULAR_VELOCITY_LIMIT:
            failure('pitch rate estimation is %.2f rad/s (%.2f deg/s); is copter staying still?',
                    angular.z, math.degrees(angular.z))
    except rospy.ROSException:
        failure('no velocity estimation')


@check('Global position (GPS)')
def check_global_position():
    try:
        rospy.wait_for_message('mavros/global_position/global', NavSatFix, timeout=1)
    except rospy.ROSException:
        failure('no global position')


@check('Optical flow')
def check_optical_flow():
    # TODO:check FPS!
    try:
        rospy.wait_for_message('mavros/px4flow/raw/send', OpticalFlowRad, timeout=0.5)

        # check PX4 settings
        rot = get_param('SENS_FLOW_ROT')
        if rot != 0:
            failure('SENS_FLOW_ROT parameter is %s, but it should be zero', rot)
        est = get_param('SYS_MC_EST_GROUP')
        if est == 1:
            fuse = get_param('LPE_FUSION')
            if not fuse & (1 << 1):
                failure('optical flow fusion is disabled, change LPE_FUSION parameter')
            if not fuse & (1 << 1):
                failure('flow gyro compensation is disabled, change LPE_FUSION parameter')
            scale = get_param('LPE_FLW_SCALE')
            if not numpy.isclose(scale, 1.0):
                failure('LPE_FLW_SCALE parameter is %.2f, but it should be 1.0', scale)

            info('LPE_FLW_QMIN is %s, LPE_FLW_R is %.4f, LPE_FLW_RR is %.4f, SENS_FLOW_MINHGT is %.3f, SENS_FLOW_MAXHGT is %.3f',
                          get_param('LPE_FLW_QMIN'),
                          get_param('LPE_FLW_R'),
                          get_param('LPE_FLW_RR'),
                          get_param('SENS_FLOW_MINHGT'),
                          get_param('SENS_FLOW_MAXHGT'))
        elif est == 2:
            fuse = get_param('EKF2_AID_MASK')
            if not fuse & (1 << 1):
                failure('optical flow fusion is disabled, change EKF2_AID_MASK parameter')
            delay = get_param('EKF2_OF_DELAY')
            if delay != 0:
                failure('EKF2_OF_DELAY is %.2f, but it should be zero', delay)
            info('EKF2_OF_QMIN is %s, EKF2_OF_N_MIN is %.4f, EKF2_OF_N_MAX is %.4f, SENS_FLOW_MINHGT is %.3f, SENS_FLOW_MAXHGT is %.3f',
                          get_param('EKF2_OF_QMIN'),
                          get_param('EKF2_OF_N_MIN'),
                          get_param('EKF2_OF_N_MAX'),
                          get_param('SENS_FLOW_MINHGT'),
                          get_param('SENS_FLOW_MAXHGT'))

    except rospy.ROSException:
        failure('no optical flow data (from Raspberry)')


@check('Rangefinder')
def check_rangefinder():
    # TODO: check FPS!
    rng = False
    try:
        rospy.wait_for_message('mavros/distance_sensor/rangefinder_sub', Range, timeout=4)
        rng = True
    except rospy.ROSException:
        failure('no rangefinder data from Raspberry')

    try:
        rospy.wait_for_message('mavros/distance_sensor/rangefinder', Range, timeout=4)
        rng = True
    except rospy.ROSException:
        failure('no rangefinder data from PX4')

    if not rng:
        return

    est = get_param('SYS_MC_EST_GROUP')
    if est == 1:
        fuse = get_param('LPE_FUSION')
        if not fuse & (1 << 5):
            info('"pub agl as lpos down" in LPE_FUSION is disabled, NOT operating over flat surface')
        else:
            info('"pub agl as lpos down" in LPE_FUSION is enabled, operating over flat surface')

    elif est == 2:
        hgt = get_param('EKF2_HGT_MODE')
        if hgt != 2:
            info('EKF2_HGT_MODE != Range sensor, NOT operating over flat surface')
        else:
            info('EKF2_HGT_MODE = Range sensor, operating over flat surface')
        aid = get_param('EKF2_RNG_AID')
        if aid != 1:
            info('EKF2_RNG_AID != 1, range sensor aiding disabled')
        else:
            info('EKF2_RNG_AID = 1, range sensor aiding enabled')


@check('Boot duration')
def check_boot_duration():
    output = subprocess.check_output('systemd-analyze')
    r = re.compile(r'([\d\.]+)s$')
    duration = float(r.search(output).groups()[0])
    if duration > 15:
        failure('long Raspbian boot duration: %ss (systemd-analyze for analyzing)', duration)


@check('CPU usage')
def check_cpu_usage():
    WHITELIST = 'nodelet',
    CMD = "top -n 1 -b -i | tail -n +8 | awk '{ printf(\"%-8s\\t%-8s\\t%-8s\\n\", $1, $9, $12); }'"
    output = subprocess.check_output(CMD, shell=True)
    processes = output.split('\n')
    for process in processes:
        if not process:
            continue
        pid, cpu, cmd = process.split('\t')

        if cmd.strip() not in WHITELIST and float(cpu) > 30:
            failure('high CPU usage (%s%%) detected: %s (PID %s)',
                    cpu.strip(), cmd.strip(), pid.strip())


@check('clever.service')
def check_clever_service():
    output = subprocess.check_output('systemctl show -p ActiveState --value clever.service'.split())
    if 'inactive' in output:
        failure('clever.service is not running, try sudo systemctl restart clever')
        return
    j = journal.Reader()
    j.this_boot()
    j.add_match(_SYSTEMD_UNIT='clever.service')
    j.add_disjunction()
    j.add_match(UNIT='clever.service')
    node_errors = []
    r = re.compile(r'^(.*)\[(FATAL|ERROR)\] \[\d+.\d+\]: (.*)$')
    for event in j:
        msg = event['MESSAGE']
        if ('Stopped Clever ROS package' in msg) or ('Started Clever ROS package' in msg):
            node_errors = []
        elif ('[ERROR]' in msg) or ('[FATAL]' in msg):
            msg = r.search(msg).groups()[2]
            if msg in node_errors:
                continue
            node_errors.append(msg)
    for error in node_errors:
        failure(error)


@check('Image')
def check_image():
    info('version: %s', open('/etc/clever_version').read().strip())


@check('Preflight status')
def check_preflight_status():
    # Make sure the console is available to us
    mavlink_exec('\n')
    cmdr_output = mavlink_exec('commander check')
    if cmdr_output == '':
        failure('No data from FCU')
        return
    cmdr_lines = cmdr_output.split('\n')
    r = re.compile(r'^(.*)(Preflight|Prearm) check: (.*)')
    for line in cmdr_lines:
        if 'WARN' in line:
            failure(line[line.find(']') + 2:])
            continue
        match = r.search(line)
        if match is not None:
            check_status = match.groups()[2]
            if check_status != 'OK':
                failure(' '.join([match.groups()[1], 'check:', check_status]))


def selfcheck():
    check_image()
    check_clever_service()
    check_fcu()
    check_imu()
    check_local_position()
    check_velocity()
    check_global_position()
    check_preflight_status()
    check_main_camera()
    check_aruco()
    check_simpleoffboard()
    check_optical_flow()
    check_vpe()
    check_rangefinder()
    check_cpu_usage()
    check_boot_duration()


if __name__ == '__main__':
    rospy.loginfo('Performing selfcheck...')
    selfcheck()
