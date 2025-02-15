/// -*- tab-width: 4; Mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
/*
   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <AP_Common.h>
#include <AP_Progmem.h>
#include <AP_Param.h>
#include <StorageManager.h>
#include <fenv.h>
#include <AP_Math.h>
#include <AP_HAL.h>
#include <AP_HAL_AVR.h>
#include <AP_HAL_SITL.h>
#include <AP_HAL_Linux.h>
#include <AP_HAL_Empty.h>
#include <AP_ADC.h>
#include <AP_Declination.h>
#include <AP_ADC_AnalogSource.h>
#include <Filter.h>
#include <AP_Buffer.h>
#include <AP_Airspeed.h>
#include <AP_Vehicle.h>
#include <AP_Notify.h>
#include <DataFlash.h>
#include <GCS_MAVLink.h>
#include <AP_GPS.h>
#include <AP_AHRS.h>
#include <SITL.h>
#include <AP_Compass.h>
#include <AP_Baro.h>
#include <AP_InertialSensor.h>
#include <AP_InertialNav.h>
#include <AP_NavEKF.h>
#include <AP_Mission.h>
#include <AP_Rally.h>
#include <AP_BattMonitor.h>
#include <AP_Terrain.h>
#include <AP_OpticalFlow.h>
#include <AP_SerialManager.h>
#include <RC_Channel.h>
#include <AP_RangeFinder.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <utility/getopt_cpp.h>
#include "Parameters.h"
#include "VehicleType.h"
#include "MsgHandler.h"

#ifndef INT16_MIN
#define INT16_MIN -32768
#define INT16_MAX 32767
#endif

#include "LogReader.h"
#include "DataFlashFileReader.h"

#define streq(x, y) (!strcmp(x, y))

const AP_HAL::HAL& hal = AP_HAL_BOARD_DRIVER;

class ReplayVehicle {
public:
    void setup();
    void load_parameters(void);

    AP_InertialSensor ins;
    AP_Baro barometer;
    AP_GPS gps;
    Compass compass;
    RangeFinder rng;
    NavEKF EKF{&ahrs, barometer, rng};
    AP_AHRS_NavEKF ahrs {ins, barometer, gps, rng, EKF};
    AP_InertialNav_NavEKF inertial_nav{ahrs};
    AP_Vehicle::FixedWing aparm;
    AP_Airspeed airspeed{aparm};
    DataFlash_Class dataflash;

private:
    Parameters g;

    // setup the var_info table
    AP_Param param_loader{var_info};

    static const AP_Param::Info var_info[];
};

ReplayVehicle replayvehicle;

#define GSCALAR(v, name, def) { replayvehicle.g.v.vtype, name, Parameters::k_param_ ## v, &replayvehicle.g.v, {def_value : def} }
#define GOBJECT(v, name, class) { AP_PARAM_GROUP, name, Parameters::k_param_ ## v, &replayvehicle.v, {group_info : class::var_info} }
#define GOBJECTN(v, pname, name, class) { AP_PARAM_GROUP, name, Parameters::k_param_ ## pname, &replayvehicle.v, {group_info : class::var_info} }

const AP_Param::Info ReplayVehicle::var_info[] PROGMEM = {
    GSCALAR(dummy,         "_DUMMY", 0),

    // barometer ground calibration. The GND_ prefix is chosen for
    // compatibility with previous releases of ArduPlane
    // @Group: GND_
    // @Path: ../libraries/AP_Baro/AP_Baro.cpp
    GOBJECT(barometer, "GND_", AP_Baro),

    // @Group: INS_
    // @Path: ../libraries/AP_InertialSensor/AP_InertialSensor.cpp
    GOBJECT(ins,                    "INS_", AP_InertialSensor),

    // @Group: AHRS_
    // @Path: ../libraries/AP_AHRS/AP_AHRS.cpp
    GOBJECT(ahrs,                   "AHRS_",    AP_AHRS),

    // @Group: ARSPD_
    // @Path: ../libraries/AP_Airspeed/AP_Airspeed.cpp
    GOBJECT(airspeed,                               "ARSPD_",   AP_Airspeed),

    // @Group: EKF_
    // @Path: ../libraries/AP_NavEKF/AP_NavEKF.cpp
    GOBJECTN(EKF, NavEKF, "EKF_", NavEKF),

    // @Group: COMPASS_
    // @Path: ../libraries/AP_Compass/AP_Compass.cpp
    GOBJECT(compass, "COMPASS_", Compass),

    AP_VAREND
};


void ReplayVehicle::load_parameters(void)
{
    if (!AP_Param::check_var_info()) {
        hal.scheduler->panic(PSTR("Bad parameter table"));
    }
}

/*
  Replay specific log structures
 */
struct PACKED log_Chek {
    LOG_PACKET_HEADER;
    uint64_t time_us;
    int16_t roll;
    int16_t pitch;
    uint16_t yaw;
    int32_t lat;
    int32_t lng;
    float alt;
    float vnorth;
    float veast;
    float vdown;
};


enum {
    LOG_CHEK_MSG=100
};

static const struct LogStructure log_structure[] PROGMEM = {
    LOG_COMMON_STRUCTURES,
    { LOG_CHEK_MSG, sizeof(log_Chek),
      "CHEK", "QccCLLffff",  "TimeUS,Roll,Pitch,Yaw,Lat,Lng,Alt,VN,VE,VD" }
};

void ReplayVehicle::setup(void) 
{
    // we pass zero log structures, as we will be outputting the log
    // structures we need manually, to prevent FMT duplicates
    dataflash.Init(log_structure, 0);
    dataflash.StartNewLog();

    ahrs.set_compass(&compass);
    ahrs.set_fly_forward(true);
    ahrs.set_wind_estimation(true);
    ahrs.set_correct_centrifugal(true);
    ahrs.set_ekf_use(true);

    printf("Starting disarmed\n");
    hal.util->set_soft_armed(false);

    barometer.init();
    barometer.setHIL(0);
    barometer.update();
    compass.init();
    ins.set_hil_mode();
}

class Replay {
public:
    void setup();
    void loop();

    Replay(ReplayVehicle &vehicle) :
        filename("log.bin"),
        _vehicle(vehicle) { }

    void flush_dataflash(void);

    bool check_solution = false;
    const char *log_filename = NULL;

    /*
      information about a log from find_log_info
     */
    struct log_information {
        uint16_t update_rate;
        bool have_imu2;
    } log_info {};

private:
    const char *filename;
    ReplayVehicle &_vehicle;

#if CONFIG_HAL_BOARD == HAL_BOARD_SITL
    SITL sitl;
#endif

    LogReader logreader{_vehicle.ahrs, _vehicle.ins, _vehicle.barometer, _vehicle.compass, _vehicle.gps, _vehicle.airspeed, _vehicle.dataflash, log_structure, ARRAY_SIZE(log_structure)};

    FILE *plotf;
    FILE *plotf2;
    FILE *ekf1f;
    FILE *ekf2f;
    FILE *ekf3f;
    FILE *ekf4f;

    bool done_parameters;
    bool done_baro_init;
    bool done_home_init;
    int32_t arm_time_ms = -1;
    bool ahrs_healthy;
    bool have_imt = false;
    bool have_imt2 = false;
    bool have_fram = false;
    bool use_imt = true;
    bool check_generate = false;
    float tolerance_euler = 3;
    float tolerance_pos = 2;
    float tolerance_vel = 2;

    struct {
        float max_roll_error;
        float max_pitch_error;
        float max_yaw_error;
        float max_pos_error;
        float max_alt_error;
        float max_vel_error;
    } check_result {};

    void _parse_command_line(uint8_t argc, char * const argv[]);

    uint8_t num_user_parameters;
    struct {
        char name[17];
        float value;
    } user_parameters[100];

    void set_ins_update_rate(uint16_t update_rate);

    void usage(void);
    void set_user_parameters(void);
    void read_sensors(const char *type);
    void log_check_generate();
    void log_check_solution();
    bool show_error(const char *text, float max_error, float tolerance);
    void report_checks();
    bool find_log_info(struct log_information &info);
};

Replay replay(replayvehicle);

void Replay::usage(void)
{
    ::printf("Options:\n");
    ::printf("\t--parm NAME=VALUE  set parameter NAME to VALUE\n");
    ::printf("\t--accel-mask MASK  set accel mask (1=accel1 only, 2=accel2 only, 3=both)\n");
    ::printf("\t--gyro-mask MASK   set gyro mask (1=gyro1 only, 2=gyro2 only, 3=both)\n");
    ::printf("\t--arm-time time    arm at time (milliseconds)\n");
    ::printf("\t--no-imt           don't use IMT data\n");
    ::printf("\t--check-generate   generate CHEK messages in output\n");
    ::printf("\t--check            check solution against CHEK messages\n");
    ::printf("\t--tolerance-euler  tolerance for euler angles in degrees\n");
    ::printf("\t--tolerance-pos    tolerance for position in meters\n");
    ::printf("\t--tolerance-vel    tolerance for velocity in meters/second\n");
}


enum {
    OPT_CHECK = 128,
    OPT_CHECK_GENERATE,
    OPT_TOLERANCE_EULER,
    OPT_TOLERANCE_POS,
    OPT_TOLERANCE_VEL
};

void Replay::flush_dataflash(void) {
    _vehicle.dataflash.flush();
}

void Replay::_parse_command_line(uint8_t argc, char * const argv[])
{
    const struct GetOptLong::option options[] = {
        {"parm",            true,   0, 'p'},
        {"param",           true,   0, 'p'},
        {"help",            false,  0, 'h'},
        {"accel-mask",      true,   0, 'a'},
        {"gyro-mask",       true,   0, 'g'},
        {"arm-time",        true,   0, 'A'},
        {"no-imt",          false,  0, 'n'},
        {"check-generate",  false,  0, OPT_CHECK_GENERATE},
        {"check",           false,  0, OPT_CHECK},
        {"tolerance-euler", true,   0, OPT_TOLERANCE_EULER},
        {"tolerance-pos",   true,   0, OPT_TOLERANCE_POS},
        {"tolerance-vel",   true,   0, OPT_TOLERANCE_VEL},
        {0, false, 0, 0}
    };

    GetOptLong gopt(argc, argv, "r:p:ha:g:A:", options);

    int opt;
    while ((opt = gopt.getoption()) != -1) {
		switch (opt) {
        case 'g':
            logreader.set_gyro_mask(strtol(gopt.optarg, NULL, 0));
            break;

        case 'a':
            logreader.set_accel_mask(strtol(gopt.optarg, NULL, 0));
            break;

        case 'A':
            arm_time_ms = strtol(gopt.optarg, NULL, 0);
            break;

        case 'n':
            use_imt = false;
            logreader.set_use_imt(use_imt);
            break;

        case 'p': {
            const char *eq = strchr(gopt.optarg, '=');
            if (eq == NULL) {
                ::printf("Usage: -p NAME=VALUE\n");
                exit(1);
            }
            memset(user_parameters[num_user_parameters].name, '\0', 16);
            strncpy(user_parameters[num_user_parameters].name, gopt.optarg, eq-gopt.optarg);
            user_parameters[num_user_parameters].value = atof(eq+1);
            num_user_parameters++;
            if (num_user_parameters >= ARRAY_SIZE(user_parameters)) {
                ::printf("Too many user parameters\n");
                exit(1);
            }
            break;
        }

        case OPT_CHECK_GENERATE:
            check_generate = true;
            break;

        case OPT_CHECK:
            check_solution = true;
            break;

        case OPT_TOLERANCE_EULER:
            tolerance_euler = atof(gopt.optarg);
            break;

        case OPT_TOLERANCE_POS:
            tolerance_pos = atof(gopt.optarg);
            break;

        case OPT_TOLERANCE_VEL:
            tolerance_vel = atof(gopt.optarg);
            break;

        case 'h':
        default:
            usage();
            exit(0);
        }
    }

	argv += gopt.optind;
	argc -= gopt.optind;

    if (argc > 0) {
        filename = argv[0];
    }
}

class IMUCounter : public DataFlashFileReader {
public:
    IMUCounter() {}
    bool handle_log_format_msg(const struct log_Format &f);
    bool handle_msg(const struct log_Format &f, uint8_t *msg);

    uint64_t last_imu_timestamp;
private:
    MsgHandler *handler;
};

bool IMUCounter::handle_log_format_msg(const struct log_Format &f) {
    if (!strncmp(f.name,"IMU",4)) {
        // an IMU message
        handler = new MsgHandler(f);
    }

    return true;
};

bool IMUCounter::handle_msg(const struct log_Format &f, uint8_t *msg) {
    if (strncmp(f.name,"IMU",4)) {
        // not an IMU message
        return true;
    }

    if (handler->field_value(msg, "TimeUS", last_imu_timestamp)) {
    } else if (handler->field_value(msg, "TimeMS", last_imu_timestamp)) {
        last_imu_timestamp *= 1000;
    } else {
        ::printf("Unable to find timestamp in IMU message");
    }
    return true;
}

/*
  find information about the log
 */
bool Replay::find_log_info(struct log_information &info) 
{
    IMUCounter reader;
    if (!reader.open_log(filename)) {
        perror(filename);
        exit(1);
    }
    int samplecount = 0;
    uint64_t prev = 0;
    uint64_t smallest_delta = 0;
    prev = 0;
    while (samplecount < 1000) {
        char type[5];
        if (!reader.update(type)) {
            break;
        }
        if (streq(type, "IMU")) {
            if (prev == 0) {
                prev = reader.last_imu_timestamp;
            } else {
                uint64_t delta = reader.last_imu_timestamp - prev;
                if (smallest_delta == 0 || delta < smallest_delta) {
                    smallest_delta = delta;
                }
                samplecount++;
            }
        }
        if (streq(type, "IMU2") && !info.have_imu2) {
            info.have_imu2 = true;
        }
    }
    if (smallest_delta == 0) {
        ::printf("Unable to determine log rate - insufficient IMU messages?!");
        return false;
    }

    float rate = 1.0e6f/smallest_delta;
    if (rate < 100) {
        info.update_rate = 50;
    } else {
        info.update_rate = 400;
    }
    return true;
}

// catch floating point exceptions
static void _replay_sig_fpe(int signum)
{
    fprintf(stderr, "ERROR: Floating point exception - flushing dataflash...\n");
    replay.flush_dataflash();
    fprintf(stderr, "ERROR: ... and aborting.\n");
    if (replay.check_solution) {
        FILE *f = fopen("replay_results.txt","a");
        fprintf(f, "%s\tFPE\tFPE\tFPE\tFPE\tFPE\n",
                replay.log_filename);
        fclose(f);
    }
    abort();
}

void Replay::setup()
{
    ::printf("Starting\n");

    uint8_t argc;
    char * const *argv;

    hal.util->commandline_arguments(argc, argv);

    _parse_command_line(argc, argv);

    // _parse_command_line sets up an FPE handler.  We can do better:
    signal(SIGFPE, _replay_sig_fpe);

    hal.console->printf("Processing log %s\n", filename);

    // remember filename for reporting
    log_filename = filename;

    if (!find_log_info(log_info)) {
        printf("Update to get log information\n");
        exit(1);
    }

    hal.console->printf("Using an update rate of %u Hz\n", log_info.update_rate);

    if (!logreader.open_log(filename)) {
        perror(filename);
        exit(1);
    }

    _vehicle.setup();
    set_ins_update_rate(log_info.update_rate);

    feenableexcept(FE_INVALID | FE_OVERFLOW);


    plotf = fopen("plot.dat", "w");
    plotf2 = fopen("plot2.dat", "w");
    ekf1f = fopen("EKF1.dat", "w");
    ekf2f = fopen("EKF2.dat", "w");
    ekf3f = fopen("EKF3.dat", "w");
    ekf4f = fopen("EKF4.dat", "w");

    fprintf(plotf, "time SIM.Roll SIM.Pitch SIM.Yaw BAR.Alt FLIGHT.Roll FLIGHT.Pitch FLIGHT.Yaw FLIGHT.dN FLIGHT.dE FLIGHT.Alt AHR2.Roll AHR2.Pitch AHR2.Yaw DCM.Roll DCM.Pitch DCM.Yaw EKF.Roll EKF.Pitch EKF.Yaw INAV.dN INAV.dE INAV.Alt EKF.dN EKF.dE EKF.Alt\n");
    fprintf(plotf2, "time E1 E2 E3 VN VE VD PN PE PD GX GY GZ WN WE MN ME MD MX MY MZ E1ref E2ref E3ref\n");
    fprintf(ekf1f, "timestamp TimeMS Roll Pitch Yaw VN VE VD PN PE PD GX GY GZ\n");
    fprintf(ekf2f, "timestamp TimeMS AX AY AZ VWN VWE MN ME MD MX MY MZ\n");
    fprintf(ekf3f, "timestamp TimeMS IVN IVE IVD IPN IPE IPD IMX IMY IMZ IVT\n");
    fprintf(ekf4f, "timestamp TimeMS SV SP SH SMX SMY SMZ SVT OFN EFE FS DS\n");
}

void Replay::set_ins_update_rate(uint16_t _update_rate) {
    switch (_update_rate) {
    case 50:
        _vehicle.ins.init(AP_InertialSensor::WARM_START, AP_InertialSensor::RATE_50HZ);
        break;
    case 100:
        _vehicle.ins.init(AP_InertialSensor::WARM_START, AP_InertialSensor::RATE_100HZ);
        break;
    case 200:
        _vehicle.ins.init(AP_InertialSensor::WARM_START, AP_InertialSensor::RATE_200HZ);
        break;
    case 400:
        _vehicle.ins.init(AP_InertialSensor::WARM_START, AP_InertialSensor::RATE_400HZ);
        break;
    default:
        printf("Invalid update rate (%d); use 50, 100, 200 or 400\n", _update_rate);
        exit(1);
    }
}


/*
  setup user -p parameters
 */
void Replay::set_user_parameters(void)
{
    for (uint8_t i=0; i<num_user_parameters; i++) {
        if (!logreader.set_parameter(user_parameters[i].name, user_parameters[i].value)) {
            ::printf("Failed to set parameter %s to %f\n", user_parameters[i].name, user_parameters[i].value);
            exit(1);
        }
    }
}

void Replay::read_sensors(const char *type)
{
    if (!done_parameters && !streq(type,"FMT") && !streq(type,"PARM")) {
        done_parameters = true;
        set_user_parameters();
    }
    if (use_imt && streq(type,"IMT")) {
        have_imt = true;
    }
    if (use_imt && streq(type,"IMT2")) {
        have_imt2 = true;
    }

    if (!done_home_init) {
        if (streq(type, "GPS") &&
            (_vehicle.gps.status() >= AP_GPS::GPS_OK_FIX_3D) && done_baro_init) {
            const Location &loc = _vehicle.gps.location();
            ::printf("GPS Lock at %.7f %.7f %.2fm time=%.1f seconds\n", 
                     loc.lat * 1.0e-7f, 
                     loc.lng * 1.0e-7f,
                     loc.alt * 0.01f,
                     hal.scheduler->millis()*0.001f);
            _vehicle.ahrs.set_home(loc);
            _vehicle.compass.set_initial_location(loc.lat, loc.lng);
            done_home_init = true;
        }
    }

    if (streq(type,"GPS")) {
        _vehicle.gps.update();
        if (_vehicle.gps.status() >= AP_GPS::GPS_OK_FIX_3D) {
            _vehicle.ahrs.estimate_wind();
        }
    } else if (streq(type,"MAG")) {
        _vehicle.compass.read();
    } else if (streq(type,"ARSP")) {
        _vehicle.ahrs.set_airspeed(&_vehicle.airspeed);
    } else if (streq(type,"BARO")) {
        _vehicle.barometer.update();
        if (!done_baro_init) {
            done_baro_init = true;
            ::printf("Barometer initialised\n");
            _vehicle.barometer.update_calibration();
        }
    } 

    bool run_ahrs = false;
    if (streq(type,"FRAM")) {
        if (!have_fram) {
            have_fram = true;
            printf("Have FRAM framing\n");
        }
        run_ahrs = true;
    }

    if (have_imt) {
        if ((streq(type,"IMT") && !have_imt2) ||
            (streq(type,"IMT2") && have_imt2)) {
            run_ahrs = true;
        }
    }

    // special handling of IMU messages as these trigger an ahrs.update()
    if (!have_fram && 
        !have_imt &&
        ((streq(type,"IMU") && !log_info.have_imu2) || (streq(type, "IMU2") && log_info.have_imu2))) {
        run_ahrs = true;
    }

    /*
      always run AHRS on CHECK messages when checking the solution
     */
    if (check_solution) {
        run_ahrs = streq(type, "CHEK");
    }
    
    if (run_ahrs) {
        _vehicle.ahrs.update();
        if (_vehicle.ahrs.get_home().lat != 0) {
            _vehicle.inertial_nav.update(_vehicle.ins.get_delta_time());
        }
        _vehicle.dataflash.Log_Write_EKF(_vehicle.ahrs,false);
        _vehicle.dataflash.Log_Write_AHRS2(_vehicle.ahrs);
        _vehicle.dataflash.Log_Write_POS(_vehicle.ahrs);
        if (_vehicle.ahrs.healthy() != ahrs_healthy) {
            ahrs_healthy = _vehicle.ahrs.healthy();
            printf("AHRS health: %u at %lu\n", 
                   (unsigned)ahrs_healthy,
                   (unsigned long)hal.scheduler->millis());
        }
        if (check_generate) {
            log_check_generate();
        } else if (check_solution) {
            log_check_solution();
        }
    }
}


/*
  copy current data to CHEK message
 */
void Replay::log_check_generate(void)
{
    Vector3f euler;
    Vector3f velocity;
    Location loc {};

    _vehicle.EKF.getEulerAngles(euler);
    _vehicle.EKF.getVelNED(velocity);
    _vehicle.EKF.getLLH(loc);

    struct log_Chek packet = {
        LOG_PACKET_HEADER_INIT(LOG_CHEK_MSG),
        time_us : hal.scheduler->micros64(),
        roll    : (int16_t)(100*degrees(euler.x)), // roll angle (centi-deg, displayed as deg due to format string)
        pitch   : (int16_t)(100*degrees(euler.y)), // pitch angle (centi-deg, displayed as deg due to format string)
        yaw     : (uint16_t)wrap_360_cd(100*degrees(euler.z)), // yaw angle (centi-deg, displayed as deg due to format string)
        lat     : loc.lat,
        lng     : loc.lng,
        alt     : loc.alt*0.01f,
        vnorth  : velocity.x,
        veast   : velocity.y,
        vdown   : velocity.z
    };

    _vehicle.dataflash.WriteBlock(&packet, sizeof(packet));
}


/*
  check current solution against CHEK message
 */
void Replay::log_check_solution(void)
{
    const LR_MsgHandler::CheckState &check_state = logreader.get_check_state();
    Vector3f euler;
    Vector3f velocity;
    Location loc {};

    _vehicle.EKF.getEulerAngles(euler);
    _vehicle.EKF.getVelNED(velocity);
    _vehicle.EKF.getLLH(loc);

    float roll_error  = degrees(fabsf(euler.x - check_state.euler.x));
    float pitch_error = degrees(fabsf(euler.y - check_state.euler.y));
    float yaw_error = wrap_180_cd_float(100*degrees(fabsf(euler.z - check_state.euler.z)))*0.01f;
    float vel_error = (velocity - check_state.velocity).length();
    float pos_error = get_distance(check_state.pos, loc);

    check_result.max_roll_error  = max(check_result.max_roll_error,  roll_error);
    check_result.max_pitch_error = max(check_result.max_pitch_error, pitch_error);
    check_result.max_yaw_error   = max(check_result.max_yaw_error,   yaw_error);
    check_result.max_vel_error   = max(check_result.max_vel_error,   vel_error);
    check_result.max_pos_error   = max(check_result.max_pos_error,   pos_error);
}


void Replay::loop()
{
    while (true) {
        char type[5];

        if (arm_time_ms >= 0 && hal.scheduler->millis() > (uint32_t)arm_time_ms) {
            if (!hal.util->get_soft_armed()) {
                hal.util->set_soft_armed(true);
                ::printf("Arming at %u ms\n", (unsigned)hal.scheduler->millis());
            }
        }

        if (!logreader.update(type)) {
            ::printf("End of log at %.1f seconds\n", hal.scheduler->millis()*0.001f);
            fclose(plotf);
            break;
        }
        read_sensors(type);

        if (streq(type,"ATT")) {
            Vector3f ekf_euler;
            Vector3f velNED;
            Vector3f posNED;
            Vector3f gyroBias;
            float accelWeighting;
            float accelZBias1;
            float accelZBias2;
            Vector3f windVel;
            Vector3f magNED;
            Vector3f magXYZ;
            Vector3f DCM_attitude;
            Vector3f ekf_relpos;
            Vector3f velInnov;
            Vector3f posInnov;
            Vector3f magInnov;
            float    tasInnov;
            float velVar;
            float posVar;
            float hgtVar;
            Vector3f magVar;
            float tasVar;
            Vector2f offset;
            uint8_t faultStatus;

            const Matrix3f &dcm_matrix = _vehicle.ahrs.AP_AHRS_DCM::get_dcm_matrix();
            dcm_matrix.to_euler(&DCM_attitude.x, &DCM_attitude.y, &DCM_attitude.z);
            _vehicle.EKF.getEulerAngles(ekf_euler);
            _vehicle.EKF.getVelNED(velNED);
            _vehicle.EKF.getPosNED(posNED);
            _vehicle.EKF.getGyroBias(gyroBias);
            _vehicle.EKF.getIMU1Weighting(accelWeighting);
            _vehicle.EKF.getAccelZBias(accelZBias1, accelZBias2);
            _vehicle.EKF.getWind(windVel);
            _vehicle.EKF.getMagNED(magNED);
            _vehicle.EKF.getMagXYZ(magXYZ);
            _vehicle.EKF.getInnovations(velInnov, posInnov, magInnov, tasInnov);
            _vehicle.EKF.getVariances(velVar, posVar, hgtVar, magVar, tasVar, offset);
            _vehicle.EKF.getFilterFaults(faultStatus);
            _vehicle.EKF.getPosNED(ekf_relpos);
            Vector3f inav_pos = _vehicle.inertial_nav.get_position() * 0.01f;
            float temp = degrees(ekf_euler.z);

            if (temp < 0.0f) temp = temp + 360.0f;
            fprintf(plotf, "%.3f %.1f %.1f %.1f %.2f %.1f %.1f %.1f %.2f %.2f %.2f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.2f %.2f %.2f %.2f %.2f %.2f\n",
                    hal.scheduler->millis() * 0.001f,
                    logreader.get_sim_attitude().x,
                    logreader.get_sim_attitude().y,
                    logreader.get_sim_attitude().z,
                    _vehicle.barometer.get_altitude(),
                    logreader.get_attitude().x,
                    logreader.get_attitude().y,
                    wrap_180_cd(logreader.get_attitude().z*100)*0.01f,
                    logreader.get_inavpos().x,
                    logreader.get_inavpos().y,
                    logreader.get_relalt(),
                    logreader.get_ahr2_attitude().x,
                    logreader.get_ahr2_attitude().y,
                    wrap_180_cd(logreader.get_ahr2_attitude().z*100)*0.01f,
                    degrees(DCM_attitude.x),
                    degrees(DCM_attitude.y),
                    degrees(DCM_attitude.z),
                    degrees(ekf_euler.x),
                    degrees(ekf_euler.y),
                    degrees(ekf_euler.z),
                    inav_pos.x,
                    inav_pos.y,
                    inav_pos.z,
                    ekf_relpos.x,
                    ekf_relpos.y,
                    -ekf_relpos.z);
            fprintf(plotf2, "%.3f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f\n",
                    hal.scheduler->millis() * 0.001f,
                    degrees(ekf_euler.x),
                    degrees(ekf_euler.y),
                    temp,
                    velNED.x, 
                    velNED.y, 
                    velNED.z, 
                    posNED.x, 
                    posNED.y, 
                    posNED.z, 
                    60*degrees(gyroBias.x), 
                    60*degrees(gyroBias.y), 
                    60*degrees(gyroBias.z), 
                    windVel.x, 
                    windVel.y, 
                    magNED.x, 
                    magNED.y, 
                    magNED.z, 
                    magXYZ.x, 
                    magXYZ.y, 
                    magXYZ.z,
                    logreader.get_attitude().x,
                    logreader.get_attitude().y,
                    logreader.get_attitude().z);

            // define messages for EKF1 data packet
            int16_t     roll  = (int16_t)(100*degrees(ekf_euler.x)); // roll angle (centi-deg)
            int16_t     pitch = (int16_t)(100*degrees(ekf_euler.y)); // pitch angle (centi-deg)
            uint16_t    yaw   = (uint16_t)wrap_360_cd(100*degrees(ekf_euler.z)); // yaw angle (centi-deg)
            float       velN  = (float)(velNED.x); // velocity North (m/s)
            float       velE  = (float)(velNED.y); // velocity East (m/s)
            float       velD  = (float)(velNED.z); // velocity Down (m/s)
            float       posN  = (float)(posNED.x); // metres North
            float       posE  = (float)(posNED.y); // metres East
            float       posD  = (float)(posNED.z); // metres Down
            float       gyrX  = (float)(6000*degrees(gyroBias.x)); // centi-deg/min
            float       gyrY  = (float)(6000*degrees(gyroBias.y)); // centi-deg/min
            float       gyrZ  = (float)(6000*degrees(gyroBias.z)); // centi-deg/min

            // print EKF1 data packet
            fprintf(ekf1f, "%.3f %u %d %d %u %.2f %.2f %.2f %.2f %.2f %.2f %.0f %.0f %.0f\n",
                    hal.scheduler->millis() * 0.001f,
                    hal.scheduler->millis(),
                    roll, 
                    pitch, 
                    yaw, 
                    velN, 
                    velE, 
                    velD, 
                    posN, 
                    posE, 
                    posD, 
                    gyrX,
                    gyrY,
                    gyrZ);

            // define messages for EKF2 data packet
            int8_t  accWeight  = (int8_t)(100*accelWeighting);
            int8_t  acc1  = (int8_t)(100*accelZBias1);
            int8_t  acc2  = (int8_t)(100*accelZBias2);
            int16_t windN = (int16_t)(100*windVel.x);
            int16_t windE = (int16_t)(100*windVel.y);
            int16_t magN  = (int16_t)(magNED.x);
            int16_t magE  = (int16_t)(magNED.y);
            int16_t magD  = (int16_t)(magNED.z);
            int16_t magX  = (int16_t)(magXYZ.x);
            int16_t magY  = (int16_t)(magXYZ.y);
            int16_t magZ  = (int16_t)(magXYZ.z);

            // print EKF2 data packet
            fprintf(ekf2f, "%.3f %d %d %d %d %d %d %d %d %d %d %d %d\n",
                    hal.scheduler->millis() * 0.001f,
                    hal.scheduler->millis(),
                    accWeight, 
                    acc1, 
                    acc2, 
                    windN, 
                    windE, 
                    magN, 
                    magE, 
                    magD, 
                    magX,
                    magY,
                    magZ);

            // define messages for EKF3 data packet
            int16_t innovVN = (int16_t)(100*velInnov.x);
            int16_t innovVE = (int16_t)(100*velInnov.y);
            int16_t innovVD = (int16_t)(100*velInnov.z);
            int16_t innovPN = (int16_t)(100*posInnov.x);
            int16_t innovPE = (int16_t)(100*posInnov.y);
            int16_t innovPD = (int16_t)(100*posInnov.z);
            int16_t innovMX = (int16_t)(magInnov.x);
            int16_t innovMY = (int16_t)(magInnov.y);
            int16_t innovMZ = (int16_t)(magInnov.z);
            int16_t innovVT = (int16_t)(100*tasInnov);

            // print EKF3 data packet
            fprintf(ekf3f, "%.3f %d %d %d %d %d %d %d %d %d %d %d\n",
                    hal.scheduler->millis() * 0.001f,
                    hal.scheduler->millis(),
                    innovVN, 
                    innovVE, 
                    innovVD, 
                    innovPN, 
                    innovPE, 
                    innovPD, 
                    innovMX, 
                    innovMY, 
                    innovMZ, 
                    innovVT);

            // define messages for EKF4 data packet
            int16_t sqrtvarV = (int16_t)(constrain_float(100*velVar,INT16_MIN,INT16_MAX));
            int16_t sqrtvarP = (int16_t)(constrain_float(100*posVar,INT16_MIN,INT16_MAX));
            int16_t sqrtvarH = (int16_t)(constrain_float(100*hgtVar,INT16_MIN,INT16_MAX));
            int16_t sqrtvarMX = (int16_t)(constrain_float(100*magVar.x,INT16_MIN,INT16_MAX));
            int16_t sqrtvarMY = (int16_t)(constrain_float(100*magVar.y,INT16_MIN,INT16_MAX));
            int16_t sqrtvarMZ = (int16_t)(constrain_float(100*magVar.z,INT16_MIN,INT16_MAX));
            int16_t sqrtvarVT = (int16_t)(constrain_float(100*tasVar,INT16_MIN,INT16_MAX));
            int16_t offsetNorth = (int8_t)(constrain_float(offset.x,INT16_MIN,INT16_MAX));
            int16_t offsetEast = (int8_t)(constrain_float(offset.y,INT16_MIN,INT16_MAX));

            // print EKF4 data packet
            fprintf(ekf4f, "%.3f %u %d %d %d %d %d %d %d %d %d %d\n",
                    hal.scheduler->millis() * 0.001f,
                    (unsigned)hal.scheduler->millis(),
                    (int)sqrtvarV,
                    (int)sqrtvarP,
                    (int)sqrtvarH,
                    (int)sqrtvarMX, 
                    (int)sqrtvarMY, 
                    (int)sqrtvarMZ,
                    (int)sqrtvarVT,
                    (int)offsetNorth,
                    (int)offsetEast,
                    (int)faultStatus);
        }
    }

    flush_dataflash();

    if (check_solution) {
        report_checks();
    }
    exit(0);
}


bool Replay::show_error(const char *text, float max_error, float tolerance)
{
    bool failed = max_error > tolerance;
    printf("%s:\t%.2f %c %.2f\n", 
           text,
           max_error,
           failed?'>':'<',
           tolerance);
    return failed;
}

/*
  report results of --check
 */
void Replay::report_checks(void)
{
    bool failed = false;
    if (tolerance_euler < 0.01f) {
        tolerance_euler = 0.01f;
    }
    FILE *f = fopen("replay_results.txt","a");
    if (f != NULL) {
        fprintf(f, "%s\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\n",
                log_filename, 
                check_result.max_roll_error,
                check_result.max_pitch_error,
                check_result.max_yaw_error,
                check_result.max_pos_error,
                check_result.max_vel_error);
        fclose(f);
    }
    failed |= show_error("Roll error", check_result.max_roll_error, tolerance_euler);
    failed |= show_error("Pitch error", check_result.max_pitch_error, tolerance_euler);
    failed |= show_error("Yaw error", check_result.max_yaw_error, tolerance_euler);
    failed |= show_error("Position error", check_result.max_pos_error, tolerance_pos);
    failed |= show_error("Velocity error", check_result.max_vel_error, tolerance_vel);
    if (failed) {
        printf("Checks failed\n");
        exit(1);
    } else {
        printf("Checks passed\n");
    }
}

/*
  compatibility with old pde style build
 */
void setup(void);
void loop(void);

void setup(void)
{
    replay.setup();
}
void loop(void)
{
    replay.loop();
}

AP_HAL_MAIN();
