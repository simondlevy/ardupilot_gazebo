/*
 * Copyright (C) 2016 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */
#include <functional>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
using raw_type = void;

#if defined(_MSC_VER)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif

#include <mutex>
#include <string>
#include <vector>
#include <sdf/sdf.hh>
#include <ignition/math/Filter.hh>
#include <gazebo/common/Assert.hh>
#include <gazebo/common/Plugin.hh>
#include <gazebo/msgs/msgs.hh>
#include <gazebo/sensors/sensors.hh>
#include <gazebo/transport/transport.hh>
#include "include/ArduPilotPlugin.hh"

#define MAX_MOTORS 255

using namespace gazebo;

GZ_REGISTER_MODEL_PLUGIN(ArduPilotPlugin)

    /// \brief A servo packet.
    struct ServoPacket
{
    /// \brief Motor speed data.
    /// should rename to servo_command here and in ArduPilot SIM_Gazebo.cpp
    float motorSpeed[MAX_MOTORS] = {0.0f};
};

/// \brief Flight Dynamics Model packet that is sent back to the ArduPilot
struct fdmPacket
{
    /// \brief packet timestamp
    double timestamp;

    /// \brief IMU angular velocity
    double imuAngularVelocityRPY[3];

    /// \brief IMU linear acceleration
    double imuLinearAccelerationXYZ[3];

    /// \brief IMU quaternion orientation
    double imuOrientationQuat[4];

    /// \brief Model velocity in NED frame
    double velocityXYZ[3];

    /// \brief Model position in NED frame
    double positionXYZ[3];
};

/// \brief Control class
class Control
{
    /// \brief Constructor
    public: Control()
            {
                // most of these coefficients are not used yet.
                this->rotorVelocitySlowdownSim = this->kDefaultRotorVelocitySlowdownSim;
                this->frequencyCutoff = this->kDefaultFrequencyCutoff;
                this->samplingRate = this->kDefaultSamplingRate;

                this->pid.Init(0.1, 0, 0, 0, 0, 1.0, -1.0);
            }

            /// \brief control id / channel
    public: int channel = 0;

            /// \brief Next command to be applied to the propeller
    public: double cmd = 0;

            /// \brief Velocity PID for motor control
    public: common::PID pid;

            /// \brief Control type. Can be:
            /// VELOCITY control velocity of joint
            /// POSITION control position of joint
            /// EFFORT control effort of joint
    public: std::string type;

            /// \brief use force controler
    public: bool useForce = true;

            /// \brief Control propeller joint.
    public: std::string jointName;

            /// \brief Control propeller joint.
    public: physics::JointPtr joint;

            /// \brief direction multiplier for this control
    public: double multiplier = 1;

            /// \brief input command offset
    public: double offset = 0;

            /// \brief unused coefficients
    public: double rotorVelocitySlowdownSim;
    public: double frequencyCutoff;
    public: double samplingRate;
    public: ignition::math::OnePole<double> filter;

    public: static double kDefaultRotorVelocitySlowdownSim;
    public: static double kDefaultFrequencyCutoff;
    public: static double kDefaultSamplingRate;
};

double Control::kDefaultRotorVelocitySlowdownSim = 10.0;
double Control::kDefaultFrequencyCutoff = 5.0;
double Control::kDefaultSamplingRate = 0.2;

// Private data class
class gazebo::ArduPilotSocketPrivate
{
    /// \brief constructor
    public: ArduPilotSocketPrivate()
            {
                // initialize socket udp socket
                this->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
                // Windows does not support FD_CLOEXEC
                fcntl(this->sockfd, F_SETFD, FD_CLOEXEC);
            }

            /// \brief destructor
    public: ~ArduPilotSocketPrivate()
            {
                if (this->sockfd != -1)
                {
                    ::close(this->sockfd);
                    this->sockfd = -1;
                }
            }

            /// \brief Bind to an adress and port
            /// \param[in] _address Address to bind to.
            /// \param[in] _port Port to bind to.
            /// \return True on success.
    public: bool Bind(const char *_address, const uint16_t _port)
            {
                struct sockaddr_in sockaddr;
                this->MakeSockAddr(_address, _port, sockaddr);

                if (bind(this->sockfd, (struct sockaddr *)&sockaddr, sizeof(sockaddr)) != 0)
                {
                    shutdown(this->sockfd, 0);
                    close(this->sockfd);
                    return false;
                }
                int one = 1;
                setsockopt(this->sockfd, SOL_SOCKET, SO_REUSEADDR,
                        reinterpret_cast<const char *>(&one), sizeof(one));

                fcntl(this->sockfd, F_SETFL,
                        fcntl(this->sockfd, F_GETFL, 0) | O_NONBLOCK);
                return true;
            }

            /// \brief Connect to an adress and port
            /// \param[in] _address Address to connect to.
            /// \param[in] _port Port to connect to.
            /// \return True on success.
    public : bool Connect(const char *_address, const uint16_t _port)
             {
                 struct sockaddr_in sockaddr;
                 this->MakeSockAddr(_address, _port, sockaddr);

                 if (connect(this->sockfd, (struct sockaddr *)&sockaddr, sizeof(sockaddr)) != 0)
                 {
                     shutdown(this->sockfd, 0);
                     close(this->sockfd);
                     return false;
                 }
                 int one = 1;
                 setsockopt(this->sockfd, SOL_SOCKET, SO_REUSEADDR,
                         reinterpret_cast<const char *>(&one), sizeof(one));

                 fcntl(this->sockfd, F_SETFL,
                         fcntl(this->sockfd, F_GETFL, 0) | O_NONBLOCK);
                 return true;
             }

             /// \brief Make a socket
             /// \param[in] _address Socket address.
             /// \param[in] _port Socket port
             /// \param[out] _sockaddr New socket address structure.
    public: void MakeSockAddr(const char *_address, const uint16_t _port,
                    struct sockaddr_in &_sockaddr)
            {
                memset(&_sockaddr, 0, sizeof(_sockaddr));
                _sockaddr.sin_port = htons(_port);
                _sockaddr.sin_family = AF_INET;
                _sockaddr.sin_addr.s_addr = inet_addr(_address);
            }

    public: ssize_t Send(const void *_buf, size_t _size)
            {
                return send(this->sockfd, _buf, _size, 0);
            }

            /// \brief Receive data
            /// \param[out] _buf Buffer that receives the data.
            /// \param[in] _size Size of the buffer.
            /// \param[in] _timeoutMS Milliseconds to wait for data.
    public: ssize_t Recv(void *_buf, const size_t _size, uint32_t _timeoutMs)
            {
                fd_set fds;
                struct timeval tv;

                FD_ZERO(&fds);
                FD_SET(this->sockfd, &fds);

                tv.tv_sec = _timeoutMs / 1000;
                tv.tv_usec = (_timeoutMs % 1000) * 1000UL;

                if (select(this->sockfd+1, &fds, NULL, NULL, &tv) != 1)
                {
                    return -1;
                }

                return recv(this->sockfd, _buf, _size, 0);
            }

            /// \brief Socket handle
    private: int sockfd;
};

// ====================================================================

// Private data class
class gazebo::ArduPilotPluginPrivate
{
    /// \brief Pointer to the update event connection.
    public: event::ConnectionPtr updateConnection;

            /// \brief Pointer to the model;
    public: physics::ModelPtr model;

            /// \brief String of the model name;
    public: std::string modelName;

            /// \brief array of propellers
    public: std::vector<Control> controls;

            /// \brief keep track of controller update sim-time.
    public: gazebo::common::Time lastControllerUpdateTime;

            /// \brief Controller update mutex.
    public: std::mutex mutex;

            /// \brief Ardupilot Socket for receive motor command on gazebo
    public: ArduPilotSocketPrivate socket_in;

            /// \brief Ardupilot Socket to send state to Ardupilot
    public: ArduPilotSocketPrivate socket_out;

            /// \brief Ardupilot address
    public: std::string fdm_addr;

            /// \brief The Ardupilot listen address
    public: std::string listen_addr;

            /// \brief Ardupilot port for receiver socket
    public: uint16_t fdm_port_in;

            /// \brief Ardupilot port for sender socket
    public: uint16_t fdm_port_out;

            /// \brief Pointer to an IMU sensor
    public: sensors::ImuSensorPtr imuSensor;

            /// \brief Pointer to an GPS sensor
    public: sensors::GpsSensorPtr gpsSensor;

            /// \brief Pointer to an Rangefinder sensor
    public: sensors::RaySensorPtr rangefinderSensor;

            /// \brief false before ardupilot controller is online
            /// to allow gazebo to continue without waiting
    public: bool arduPilotOnline;

            /// \brief number of times ArduCotper skips update
    public: int connectionTimeoutCount;

            /// \brief number of times ArduCotper skips update
            /// before marking ArduPilot offline
    public: int connectionTimeoutMaxCount;
};

///////////////////////////////////////////////// sdl
static bool spinning;
static double timestamp;
static double timestart;
static FILE * logfp;

static void report(const char * label, double * x, int n=3)
{
    fprintf(logfp, "%s: ", label);
    for (int k=0; k<n; ++k) {
        fprintf(logfp, "%+6.6f", x[k]);
        fprintf(logfp, "%s", (k==n-1) ? " | " : ",");
    }
}

/////////////////////////////////////////////////
ArduPilotPlugin::ArduPilotPlugin()
    : dataPtr(new ArduPilotPluginPrivate)
{
    this->dataPtr->arduPilotOnline = false;
    this->dataPtr->connectionTimeoutCount = 0;

    logfp = stderr;//fopen("gazebo.log", "w");
}

/////////////////////////////////////////////////
ArduPilotPlugin::~ArduPilotPlugin()
{
}

/////////////////////////////////////////////////
void ArduPilotPlugin::Load(physics::ModelPtr _model, sdf::ElementPtr _sdf)
{
    GZ_ASSERT(_model, "ArduPilotPlugin _model pointer is null");
    GZ_ASSERT(_sdf, "ArduPilotPlugin _sdf pointer is null");

    this->dataPtr->model = _model;
    this->dataPtr->modelName = this->dataPtr->model->GetName();

    // modelXYZToAirplaneXForwardZDown brings us from gazebo model frame:
    // x-forward, y-right, z-down
    // to the aerospace convention: x-forward, y-left, z-up
    this->modelXYZToAirplaneXForwardZDown =
        ignition::math::Pose3d(0, 0, 0, 0, 0, 0);
    if (_sdf->HasElement("modelXYZToAirplaneXForwardZDown"))
    {
        this->modelXYZToAirplaneXForwardZDown =
            _sdf->Get<ignition::math::Pose3d>("modelXYZToAirplaneXForwardZDown");
    }

    // gazeboXYZToNED: from gazebo model frame: x-forward, y-right, z-down
    // to the aerospace convention: x-forward, y-left, z-up
    this->gazeboXYZToNED = ignition::math::Pose3d(0, 0, 0, IGN_PI, 0, 0);
    if (_sdf->HasElement("gazeboXYZToNED"))
    {
        this->gazeboXYZToNED = _sdf->Get<ignition::math::Pose3d>("gazeboXYZToNED");
    }

    // per control channel
    sdf::ElementPtr controlSDF;
    if (_sdf->HasElement("control"))
    {
        controlSDF = _sdf->GetElement("control");
    }
    else if (_sdf->HasElement("rotor"))
    {
        gzwarn << "[" << this->dataPtr->modelName << "] "
            << "please deprecate <rotor> block, use <control> block instead.\n";
        controlSDF = _sdf->GetElement("rotor");
    }

    while (controlSDF)
    {
        Control control;

        if (controlSDF->HasAttribute("channel"))
        {
            control.channel =
                atoi(controlSDF->GetAttribute("channel")->GetAsString().c_str());
        }
        else if (controlSDF->HasAttribute("id"))
        {
            gzwarn << "[" << this->dataPtr->modelName << "] "
                <<  "please deprecate attribute id, use channel instead.\n";
            control.channel =
                atoi(controlSDF->GetAttribute("id")->GetAsString().c_str());
        }
        else
        {
            control.channel = this->dataPtr->controls.size();
            gzwarn << "[" << this->dataPtr->modelName << "] "
                <<  "id/channel attribute not specified, use order parsed ["
                << control.channel << "].\n";
        }

        if (controlSDF->HasElement("type"))
        {
            control.type = controlSDF->Get<std::string>("type");
        }
        else
        {
            gzerr << "[" << this->dataPtr->modelName << "] "
                <<  "Control type not specified,"
                << " using velocity control by default.\n";
            control.type = "VELOCITY";
        }

        if (control.type != "VELOCITY" &&
                control.type != "POSITION" &&
                control.type != "EFFORT")
        {
            gzwarn << "[" << this->dataPtr->modelName << "] "
                << "Control type [" << control.type
                << "] not recognized, must be one of VELOCITY, POSITION, EFFORT."
                << " default to VELOCITY.\n";
            control.type = "VELOCITY";
        }

        if (controlSDF->HasElement("useForce"))
        {
            control.useForce = controlSDF->Get<bool>("useForce");
        }

        if (controlSDF->HasElement("jointName"))
        {
            control.jointName = controlSDF->Get<std::string>("jointName");
        }
        else
        {
            gzerr << "[" << this->dataPtr->modelName << "] "
                << "Please specify a jointName,"
                << " where the control channel is attached.\n";
        }

        // Get the pointer to the joint.
        control.joint = _model->GetJoint(control.jointName);
        if (control.joint == nullptr)
        {
            gzerr << "[" << this->dataPtr->modelName << "] "
                << "Couldn't find specified joint ["
                << control.jointName << "]. This plugin will not run.\n";
            return;
        }

        if (controlSDF->HasElement("multiplier"))
        {
            // overwrite turningDirection, deprecated.
            control.multiplier = controlSDF->Get<double>("multiplier");
        }
        else if (controlSDF->HasElement("turningDirection"))
        {
            gzwarn << "[" << this->dataPtr->modelName << "] "
                << "<turningDirection> is deprecated. Please use"
                << " <multiplier>. Map 'cw' to '-1' and 'ccw' to '1'.\n";
            std::string turningDirection = controlSDF->Get<std::string>(
                    "turningDirection");
            // special cases mimic from controls_gazebo_plugins
            if (turningDirection == "cw")
            {
                control.multiplier = -1;
            }
            else if (turningDirection == "ccw")
            {
                control.multiplier = 1;
            }
            else
            {
                gzdbg << "[" << this->dataPtr->modelName << "] "
                    << "not string, check turningDirection as float\n";
                control.multiplier = controlSDF->Get<double>("turningDirection");
            }
        }
        else
        {
            gzdbg << "[" << this->dataPtr->modelName << "] "
                << "<multiplier> (or deprecated <turningDirection>) not specified,"
                << " Default 1 (or deprecated <turningDirection> 'ccw').\n";
            control.multiplier = 1;
        }

        if (controlSDF->HasElement("offset"))
        {
            control.offset = controlSDF->Get<double>("offset");
        }
        else
        {
            gzdbg << "[" << this->dataPtr->modelName << "] "
                << "<offset> not specified, default to 0.\n";
            control.offset = 0;
        }

        control.rotorVelocitySlowdownSim =
            controlSDF->Get("rotorVelocitySlowdownSim", 1).first;

        if (ignition::math::equal(control.rotorVelocitySlowdownSim, 0.0))
        {
            gzwarn << "[" << this->dataPtr->modelName << "] "
                << "control for joint [" << control.jointName
                << "] rotorVelocitySlowdownSim is zero,"
                << " assume no slowdown.\n";
            control.rotorVelocitySlowdownSim = 1.0;
        }

        control.frequencyCutoff =
            controlSDF->Get("frequencyCutoff", control.frequencyCutoff).first;
        control.samplingRate =
            controlSDF->Get("samplingRate", control.samplingRate).first;

        // use gazebo::math::Filter
        control.filter.Fc(control.frequencyCutoff, control.samplingRate);

        // initialize filter to zero value
        control.filter.Set(0.0);

        // note to use this filter, do
        // stateFiltered = filter.Process(stateRaw);

        // Overload the PID parameters if they are available.
        double param;
        // carry over from ArduCopter plugin
        param = controlSDF->Get("vel_p_gain", control.pid.GetPGain()).first;
        control.pid.SetPGain(param);

        param = controlSDF->Get("vel_i_gain", control.pid.GetIGain()).first;
        control.pid.SetIGain(param);

        param = controlSDF->Get("vel_d_gain", control.pid.GetDGain()).first;
        control.pid.SetDGain(param);

        param = controlSDF->Get("vel_i_max", control.pid.GetIMax()).first;
        control.pid.SetIMax(param);

        param = controlSDF->Get("vel_i_min", control.pid.GetIMin()).first;
        control.pid.SetIMin(param);

        param = controlSDF->Get("vel_cmd_max", control.pid.GetCmdMax()).first;
        control.pid.SetCmdMax(param);

        param = controlSDF->Get("vel_cmd_min", control.pid.GetCmdMin()).first;
        control.pid.SetCmdMin(param);

        // new params, overwrite old params if exist
        param = controlSDF->Get("p_gain", control.pid.GetPGain()).first;
        control.pid.SetPGain(param);

        param = controlSDF->Get("i_gain", control.pid.GetIGain()).first;
        control.pid.SetIGain(param);

        param = controlSDF->Get("d_gain", control.pid.GetDGain()).first;
        control.pid.SetDGain(param);

        param = controlSDF->Get("i_max", control.pid.GetIMax()).first;
        control.pid.SetIMax(param);

        param = controlSDF->Get("i_min", control.pid.GetIMin()).first;
        control.pid.SetIMin(param);

        param = controlSDF->Get("cmd_max", control.pid.GetCmdMax()).first;
        control.pid.SetCmdMax(param);

        param = controlSDF->Get("cmd_min", control.pid.GetCmdMin()).first;
        control.pid.SetCmdMin(param);

        // set pid initial command
        control.pid.SetCmd(0.0);

        this->dataPtr->controls.push_back(control);
        controlSDF = controlSDF->GetNextElement("control");
    }

    // Get sensors
    std::string imuName =
        _sdf->Get("imuName", static_cast<std::string>("imu_sensor")).first;
    std::vector<std::string> imuScopedName =
        this->dataPtr->model->SensorScopedName(imuName);

    if (imuScopedName.size() > 1)
    {
        gzwarn << "[" << this->dataPtr->modelName << "] "
            << "multiple names match [" << imuName << "] using first found"
            << " name.\n";
        for (unsigned k = 0; k < imuScopedName.size(); ++k)
        {
            gzwarn << "  sensor " << k << " [" << imuScopedName[k] << "].\n";
        }
    }

    if (imuScopedName.size() > 0)
    {
        this->dataPtr->imuSensor = std::dynamic_pointer_cast<sensors::ImuSensor>
            (sensors::SensorManager::Instance()->GetSensor(imuScopedName[0]));
    }

    if (!this->dataPtr->imuSensor)
    {
        if (imuScopedName.size() > 1)
        {
            gzwarn << "[" << this->dataPtr->modelName << "] "
                << "first imu_sensor scoped name [" << imuScopedName[0]
                << "] not found, trying the rest of the sensor names.\n";
            for (unsigned k = 1; k < imuScopedName.size(); ++k)
            {
                this->dataPtr->imuSensor = std::dynamic_pointer_cast<sensors::ImuSensor>
                    (sensors::SensorManager::Instance()->GetSensor(imuScopedName[k]));
                if (this->dataPtr->imuSensor)
                {
                    gzwarn << "found [" << imuScopedName[k] << "]\n";
                    break;
                }
            }
        }

        if (!this->dataPtr->imuSensor)
        {
            gzwarn << "[" << this->dataPtr->modelName << "] "
                << "imu_sensor scoped name [" << imuName
                << "] not found, trying unscoped name.\n" << "\n";
            // TODO: this fails for multi-nested models.
            // TODO: and transforms fail for rotated nested model,
            //       joints point the wrong way.
            this->dataPtr->imuSensor = std::dynamic_pointer_cast<sensors::ImuSensor>
                (sensors::SensorManager::Instance()->GetSensor(imuName));
        }

        if (!this->dataPtr->imuSensor)
        {
            gzerr << "[" << this->dataPtr->modelName << "] "
                << "imu_sensor [" << imuName
                << "] not found, abort ArduPilot plugin.\n" << "\n";
            return;
        }
    }

    // Controller time control.
    this->dataPtr->lastControllerUpdateTime = 0;

    // Initialise ardupilot sockets
    if (!InitArduPilotSockets(_sdf))
    {
        return;
    }

    // Missed update count before we declare arduPilotOnline status false
    this->dataPtr->connectionTimeoutMaxCount =
        _sdf->Get("connectionTimeoutMaxCount", 10).first;

    // Listen to the update event. This event is broadcast every simulation
    // iteration.
    this->dataPtr->updateConnection = event::Events::ConnectWorldUpdateBegin(
            std::bind(&ArduPilotPlugin::OnUpdate, this));

    gzlog << "[" << this->dataPtr->modelName << "] "
        << "ArduPilot ready to fly. The force will be with you" << std::endl;
}

/////////////////////////////////////////////////
void ArduPilotPlugin::OnUpdate()
{
    std::lock_guard<std::mutex> lock(this->dataPtr->mutex);

    const gazebo::common::Time curTime =
        this->dataPtr->model->GetWorld()->SimTime();

    // Update the control surfaces and publish the new state.
    if (curTime > this->dataPtr->lastControllerUpdateTime)
    {
        this->ReceiveMotorCommand();
        if (this->dataPtr->arduPilotOnline)
        {
            this->ApplyMotorForces((curTime - this->dataPtr->lastControllerUpdateTime).Double());
            this->SendState();
        }
    }

    this->dataPtr->lastControllerUpdateTime = curTime;
}

/////////////////////////////////////////////////
void ArduPilotPlugin::ResetPIDs()
{
    // Reset velocity PID for controls
    for (size_t i = 0; i < this->dataPtr->controls.size(); ++i)
    {
        this->dataPtr->controls[i].cmd = 0;
        // this->dataPtr->controls[i].pid.Reset();
    }
}

/////////////////////////////////////////////////
bool ArduPilotPlugin::InitArduPilotSockets(sdf::ElementPtr _sdf) const
{
    this->dataPtr->fdm_addr =
        _sdf->Get("fdm_addr", static_cast<std::string>("127.0.0.1")).first;
    this->dataPtr->listen_addr =
        _sdf->Get("listen_addr", static_cast<std::string>("127.0.0.1")).first;
    this->dataPtr->fdm_port_in =
        _sdf->Get("fdm_port_in", static_cast<uint16_t>(9002)).first;
    this->dataPtr->fdm_port_out =
        _sdf->Get("fdm_port_out", static_cast<uint16_t>(9003)).first;

    if (!this->dataPtr->socket_in.Bind(this->dataPtr->listen_addr.c_str(),
                this->dataPtr->fdm_port_in))
    {
        gzerr << "[" << this->dataPtr->modelName << "] "
            << "failed to bind with " << this->dataPtr->listen_addr
            << ":" << this->dataPtr->fdm_port_in << " aborting plugin.\n";
        return false;
    }

    if (!this->dataPtr->socket_out.Connect(this->dataPtr->fdm_addr.c_str(),
                this->dataPtr->fdm_port_out))
    {
        gzerr << "[" << this->dataPtr->modelName << "] "
            << "failed to bind with " << this->dataPtr->fdm_addr
            << ":" << this->dataPtr->fdm_port_out << " aborting plugin.\n";
        return false;
    }

    return true;
}

/////////////////////////////////////////////////
void ArduPilotPlugin::ApplyMotorForces(const double _dt)
{
    // update velocity PID for controls and apply force to joint
    for (size_t i = 0; i < this->dataPtr->controls.size(); ++i)
    {
        const double velTarget = this->dataPtr->controls[i].cmd;
        const double vel = this->dataPtr->controls[i].joint->GetVelocity(0);
        const double error = vel - velTarget;
        const double force = this->dataPtr->controls[i].pid.Update(error, _dt);
        this->dataPtr->controls[i].joint->SetForce(0, force);
    }
}

/////////////////////////////////////////////////
void ArduPilotPlugin::ReceiveMotorCommand()
{
    // Added detection for whether ArduPilot is online or not.
    // If ArduPilot is detected (receive of fdm packet from someone),
    // then socket receive wait time is increased from 1ms to 1 sec
    // to accomodate network jitter.
    // If ArduPilot is not detected, receive call blocks for 1ms
    // on each call.
    // Once ArduPilot presence is detected, it takes this many
    // missed receives before declaring the FCS offline.

    ServoPacket pkt;
    uint32_t waitMs;
    if (this->dataPtr->arduPilotOnline)
    {
        // increase timeout for receive once we detect a packet from
        // ArduPilot FCS.
        waitMs = 1000;
    }
    else
    {
        // Otherwise skip quickly and do not set control force.
        waitMs = 1;
    }
    ssize_t recvSize =
        this->dataPtr->socket_in.Recv(&pkt, sizeof(ServoPacket), waitMs);

    if (timestamp > 0) {
        float * m = pkt.motorSpeed;
        if (!spinning) {
            if (m[0] >= 0.10) {
                spinning = true;
                timestart = timestamp;
            }
        }
        if (spinning) {
            //fprintf(logfp, "t: %3.3f | m: %2.2f,%2.2f,%2.2f,%2.2f | ", 
            //        timestamp-timestart, m[0], m[1], m[2], m[3]);
        }
    }

    // Drain the socket in the case we're backed up
    int counter = 0;
    ServoPacket last_pkt;
    while (true)
    {
        // last_pkt = pkt;
        const ssize_t recvSize_last =
            this->dataPtr->socket_in.Recv(&last_pkt, sizeof(ServoPacket), 0ul);
        if (recvSize_last == -1)
        {
            break;
        }
        counter++;
        pkt = last_pkt;
        recvSize = recvSize_last;
    }
    if (counter > 0)
    {
        gzdbg << "[" << this->dataPtr->modelName << "] "
            << "Drained n packets: " << counter << std::endl;
    }

    if (recvSize == -1)
    {
        // didn't receive a packet
        // gzdbg << "no packet\n";
        gazebo::common::Time::NSleep(100);
        if (this->dataPtr->arduPilotOnline)
        {
            gzwarn << "[" << this->dataPtr->modelName << "] "
                << "Broken ArduPilot connection, count ["
                << this->dataPtr->connectionTimeoutCount
                << "/" << this->dataPtr->connectionTimeoutMaxCount
                << "]\n";
            if (++this->dataPtr->connectionTimeoutCount >
                    this->dataPtr->connectionTimeoutMaxCount)
            {
                this->dataPtr->connectionTimeoutCount = 0;
                this->dataPtr->arduPilotOnline = false;
                gzwarn << "[" << this->dataPtr->modelName << "] "
                    << "Broken ArduPilot connection, resetting motor control.\n";
                this->ResetPIDs();
            }
        }
    }
    else
    {
        const ssize_t expectedPktSize = sizeof(pkt.motorSpeed[0]) * this->dataPtr->controls.size();
        if (recvSize < expectedPktSize)
        {
            gzerr << "[" << this->dataPtr->modelName << "] "
                << "got less than model needs. Got: " << recvSize
                << "commands, expected size: " << expectedPktSize << "\n";
        }
        const ssize_t recvChannels = recvSize / sizeof(pkt.motorSpeed[0]);
        // for(unsigned int i = 0; i < recvChannels; ++i)
        // {
        //   gzdbg << "servo_command [" << i << "]: " << pkt.motorSpeed[i] << "\n";
        // }

        if (!this->dataPtr->arduPilotOnline)
        {
            gzdbg << "[" << this->dataPtr->modelName << "] "
                << "ArduPilot controller online detected.\n";
            // made connection, set some flags
            this->dataPtr->connectionTimeoutCount = 0;
            this->dataPtr->arduPilotOnline = true;
        }

        // compute command based on requested motorSpeed
        for (unsigned i = 0; i < this->dataPtr->controls.size(); ++i)
        {
            if (i < MAX_MOTORS)
            {
                if (this->dataPtr->controls[i].channel < recvChannels)
                {
                    // bound incoming cmd between 0 and 1
                    const double cmd = ignition::math::clamp(
                            pkt.motorSpeed[this->dataPtr->controls[i].channel],
                            -1.0f, 1.0f);
                    this->dataPtr->controls[i].cmd =
                        this->dataPtr->controls[i].multiplier *
                        (this->dataPtr->controls[i].offset + cmd);
                }
                else
                {
                    gzerr << "[" << this->dataPtr->modelName << "] "
                        << "control[" << i << "] channel ["
                        << this->dataPtr->controls[i].channel
                        << "] is greater than incoming commands size["
                        << recvChannels
                        << "], control not applied.\n";
                }
            }
            else
            {
                gzerr << "[" << this->dataPtr->modelName << "] "
                    << "too many motors, skipping [" << i
                    << " > " << MAX_MOTORS << "].\n";
            }
        }
    }
}

// sdl
static void quat2euler(double q[4], double euler[3])
{
    euler[0] = atan2(2.0f*(q[0]*q[1]+q[2]*q[3]),q[0]*q[0]-q[1]*q[1]-q[2]*q[2]+q[3]*q[3]);
    euler[1] =  asin(2.0f*(q[1]*q[3]-q[0]*q[2]));
    euler[2] = atan2(2.0f*(q[1]*q[2]+q[0]*q[3]),q[0]*q[0]+q[1]*q[1]-q[2]*q[2]-q[3]*q[3]);
}

/////////////////////////////////////////////////
void ArduPilotPlugin::SendState() const
{
    // send_fdm
    fdmPacket pkt;

    pkt.timestamp = this->dataPtr->model->GetWorld()->SimTime().Double();

    static uint32_t count;
    fprintf(stderr, "FPS: %d\r", (int)(++count/pkt.timestamp));

    // asssumed that the imu orientation is:
    //   x forward
    //   y right
    //   z down

    // get linear acceleration in body frame
    const ignition::math::Vector3d linearAccel =
        this->dataPtr->imuSensor->LinearAcceleration();

    //   from: model XYZ
    //   to: airplane x-forward, y-left, z-down
    const ignition::math::Pose3d gazeboXYZToModelXForwardZDown =
        this->modelXYZToAirplaneXForwardZDown +
        this->dataPtr->model->WorldPose();

    // get transform from world NED to Model frame
    const ignition::math::Pose3d NEDToModelXForwardZUp =
        gazeboXYZToModelXForwardZDown - this->gazeboXYZToNED;

    const ignition::math::Vector3d velGazeboWorldFrame =
        this->dataPtr->model->GetLink()->WorldLinearVel();
    const ignition::math::Vector3d velNEDFrame =
        this->gazeboXYZToNED.Rot().RotateVectorReverse(velGazeboWorldFrame);

    // get angular velocity in body frame
    const ignition::math::Vector3d angularVel =
        this->dataPtr->imuSensor->AngularVelocity();

    // sdl -------------------------------------------------------------

    pkt.imuAngularVelocityRPY[0] = angularVel.X();
    pkt.imuAngularVelocityRPY[1] = angularVel.Y();
    pkt.imuAngularVelocityRPY[2] = angularVel.Z();

    pkt.imuOrientationQuat[0] = NEDToModelXForwardZUp.Rot().W(); 
    pkt.imuOrientationQuat[1] = NEDToModelXForwardZUp.Rot().X();
    pkt.imuOrientationQuat[2] = NEDToModelXForwardZUp.Rot().Y();
    pkt.imuOrientationQuat[3] = NEDToModelXForwardZUp.Rot().Z();

    pkt.imuLinearAccelerationXYZ[0] = linearAccel.X(); 
    pkt.imuLinearAccelerationXYZ[1] = linearAccel.Y();
    pkt.imuLinearAccelerationXYZ[2] = linearAccel.Z();

    pkt.velocityXYZ[0] = velNEDFrame.X(); 
    pkt.velocityXYZ[1] = velNEDFrame.Y();
    pkt.velocityXYZ[2] = velNEDFrame.Z();

    pkt.positionXYZ[0] = NEDToModelXForwardZUp.Pos().X(); // N
    pkt.positionXYZ[1] = NEDToModelXForwardZUp.Pos().Y(); // E
    pkt.positionXYZ[2] = NEDToModelXForwardZUp.Pos().Z(); // D

    this->dataPtr->socket_out.Send(&pkt, sizeof(pkt));

    timestamp = pkt.timestamp;

    if (false /*spinning*/) {

        // Convert quaternion into Euler angles
        double euler[3] = {0, 0, 0};
        quat2euler(pkt.imuOrientationQuat, euler);

        report("g", pkt.imuAngularVelocityRPY);
        report("e", euler);
        report("a", pkt.imuLinearAccelerationXYZ);
        report("q", pkt.imuOrientationQuat, 4);
        report("p", pkt.positionXYZ);
        fprintf(logfp, "\n");
    }
}
