#include <DeviceProvider.h>
#include <windows.h>

#include <string>

#include "Communication/BTSerialCommunicationManager.h"
#include "Communication/SerialCommunicationManager.h"
#include "DeviceDriver/KnuckleDriver.h"
#include "DeviceDriver/LucidGloveDriver.h"
#include "DriverLog.h"
#include "Encode/AlphaEncodingManager.h"
#include "Encode/LegacyEncodingManager.h"
#include "Quaternion.h"

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

std::string GetDriverPath() {
  char path[MAX_PATH];
  HMODULE hm = NULL;

  if (GetModuleHandleEx(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          (LPCSTR)&__ImageBase, &hm) == 0) {
    int ret = GetLastError();
    fprintf(stderr, "GetModuleHandle failed, error = %d\n", ret);
    // Return or however you want to handle an error.
  }
  if (GetModuleFileName(hm, path, sizeof(path)) == 0) {
    int ret = GetLastError();
    fprintf(stderr, "GetModuleFileName failed, error = %d\n", ret);
    // Return or however you want to handle an error.
  }

  std::string::size_type pos = std::string(path).find_last_of("\\/");
  return std::string(path).substr(0, pos);
}

bool CreateBackgroundProcess() {
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  ZeroMemory(&pi, sizeof(pi));

  const std::string driverPath = GetDriverPath();
  DriverLog("Path to DLL: %s", driverPath.c_str());

  std::string path = driverPath + "\\openglove_overlay.exe";

  bool success = true;
  if (!CreateProcess(path.c_str(), NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) success = false;

  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  return success;
}

vr::EVRInitError DeviceProvider::Init(vr::IVRDriverContext* pDriverContext) {
  vr::EVRInitError initError = InitServerDriverContext(pDriverContext);
  if (initError != vr::EVRInitError::VRInitError_None) return initError;

  VR_INIT_SERVER_DRIVER_CONTEXT(pDriverContext);
  InitDriverLog(vr::VRDriverLog());
  DebugDriverLog("OpenGlove is running in DEBUG mode");

  if (!CreateBackgroundProcess()) {
    DriverLog("Could not create background process");
    return vr::VRInitError_Init_FileNotFound;
  }
  
  VRDeviceConfiguration_t leftConfiguration =
      GetDeviceConfiguration(vr::TrackedControllerRole_LeftHand);
  VRDeviceConfiguration_t rightConfiguration =
      GetDeviceConfiguration(vr::TrackedControllerRole_RightHand);

  if (leftConfiguration.enabled) {
    m_leftHand = InstantiateDeviceDriver(leftConfiguration);
    vr::VRServerDriverHost()->TrackedDeviceAdded(
        m_leftHand->GetSerialNumber().c_str(), vr::TrackedDeviceClass_Controller, m_leftHand.get());
  }
  if (rightConfiguration.enabled) {
    m_rightHand = InstantiateDeviceDriver(rightConfiguration);
    vr::VRServerDriverHost()->TrackedDeviceAdded(m_rightHand->GetSerialNumber().c_str(),
                                                 vr::TrackedDeviceClass_Controller,
                                                 m_rightHand.get());
  }

  return vr::VRInitError_None;
}

std::unique_ptr<IDeviceDriver> DeviceProvider::InstantiateDeviceDriver(
    VRDeviceConfiguration_t configuration) {
  std::unique_ptr<ICommunicationManager> communicationManager;
  std::unique_ptr<IEncodingManager> encodingManager;

  bool isRightHand = configuration.role == vr::TrackedControllerRole_RightHand;
  switch (configuration.encodingProtocol) {
    default:
      DriverLog("No encoding protocol set. Using legacy.");
    case VREncodingProtocol::LEGACY: {
      const int maxAnalogValue = vr::VRSettings()->GetInt32("encoding_legacy", "max_analog_value");
      encodingManager = std::make_unique<LegacyEncodingManager>(maxAnalogValue);
      break;
    }
    case VREncodingProtocol::ALPHA: {
      const int maxAnalogValue =
          vr::VRSettings()->GetInt32("encoding_alpha", "max_analog_value");  //
      encodingManager = std::make_unique<AlphaEncodingManager>(maxAnalogValue);
      break;
    }
  }

  switch (configuration.communicationProtocol) {
    case VRCommunicationProtocol::BTSERIAL: {
      DriverLog("Communication set to BTSerial");
      char name[248];
      vr::VRSettings()->GetString("communication_btserial",
                                  isRightHand ? "right_name" : "left_name", name, sizeof(name));
      VRBTSerialConfiguration_t btSerialSettings(name);
      communicationManager = std::make_unique<BTSerialCommunicationManager>(
          btSerialSettings, std::move(encodingManager));
      break;
    }
    default:
      DriverLog("No communication protocol set. Using serial.");
    case VRCommunicationProtocol::SERIAL:
      char port[16];
      vr::VRSettings()->GetString("communication_serial", isRightHand ? "right_port" : "left_port",
                                  port, sizeof(port));
      VRSerialConfiguration_t serialSettings(port);

      communicationManager =
          std::make_unique<SerialCommunicationManager>(serialSettings, std::move(encodingManager));
      break;
  }

  switch (configuration.deviceDriver) {
    case VRDeviceDriver::EMULATED_KNUCKLES: {
      char serialNumber[32];
      vr::VRSettings()->GetString("device_knuckles",
                                  isRightHand ? "right_serial_number" : "left_serial_number",
                                  serialNumber, sizeof(serialNumber));

      return std::make_unique<KnuckleDeviceDriver>(configuration, std::move(communicationManager),
                                                   serialNumber);
    }

    default:
      DriverLog("No device driver selected. Using lucidgloves.");
    case VRDeviceDriver::LUCIDGLOVES: {
      char serialNumber[32];
      vr::VRSettings()->GetString("device_lucidgloves",
                                  isRightHand ? "right_serial_number" : "left_serial_number",
                                  serialNumber, sizeof(serialNumber));

      return std::make_unique<LucidGloveDeviceDriver>(
          configuration, std::move(communicationManager), serialNumber);
    }
  }
}
VRDeviceConfiguration_t DeviceProvider::GetDeviceConfiguration(vr::ETrackedControllerRole role) {
  const bool isRightHand = role == vr::TrackedControllerRole_RightHand;

  const bool isEnabled = vr::VRSettings()->GetBool(c_driverSettingsSection,
                                                   isRightHand ? "right_enabled" : "left_enabled");

  const auto communicationProtocol = (VRCommunicationProtocol)vr::VRSettings()->GetInt32(
      c_driverSettingsSection, "communication_protocol");
  const auto encodingProtocol =
      (VREncodingProtocol)vr::VRSettings()->GetInt32(c_driverSettingsSection, "encoding_protocol");
  const auto deviceDriver =
      (VRDeviceDriver)vr::VRSettings()->GetInt32(c_driverSettingsSection, "device_driver");

  const float poseOffset = vr::VRSettings()->GetFloat(c_poseSettingsSection, "pose_offset");

  const float offsetXPos = vr::VRSettings()->GetFloat(
      c_poseSettingsSection, isRightHand ? "right_x_offset_position" : "left_x_offset_position");
  const float offsetYPos = vr::VRSettings()->GetFloat(
      c_poseSettingsSection, isRightHand ? "right_y_offset_position" : "left_y_offset_position");
  const float offsetZPos = vr::VRSettings()->GetFloat(
      c_poseSettingsSection, isRightHand ? "right_z_offset_position" : "left_z_offset_position");

  const float offsetXRot = vr::VRSettings()->GetFloat(
      c_poseSettingsSection, isRightHand ? "right_x_offset_degrees" : "left_x_offset_degrees");
  const float offsetYRot = vr::VRSettings()->GetFloat(
      c_poseSettingsSection, isRightHand ? "right_y_offset_degrees" : "left_y_offset_degrees");
  const float offsetZRot = vr::VRSettings()->GetFloat(
      c_poseSettingsSection, isRightHand ? "right_z_offset_degrees" : "left_z_offset_degrees");

  const bool controllerOverrideEnabled =
      vr::VRSettings()->GetBool(c_poseSettingsSection, "controller_override");
  const int controllerIdOverride =
      controllerOverrideEnabled
          ? vr::VRSettings()->GetInt32(c_poseSettingsSection, isRightHand
                                                                  ? "controller_override_right"
                                                                  : "controller_override_left")
          : -1;

  const vr::HmdVector3_t offsetVector = {offsetXPos, offsetYPos, offsetZPos};

  // Convert the rotation to a quaternion
  const vr::HmdQuaternion_t angleOffsetQuaternion =
      EulerToQuaternion(DegToRad(offsetXRot), DegToRad(offsetYRot), DegToRad(offsetZRot));

  return VRDeviceConfiguration_t(
      role, isEnabled,
      VRPoseConfiguration_t(offsetVector, angleOffsetQuaternion, poseOffset,
                            controllerOverrideEnabled, controllerIdOverride),
      encodingProtocol, communicationProtocol, deviceDriver);
}

void DeviceProvider::Cleanup() {}

const char* const* DeviceProvider::GetInterfaceVersions() { return vr::k_InterfaceVersions; }

void DeviceProvider::RunFrame() {
  if (m_leftHand && m_leftHand->IsActive()) m_leftHand->RunFrame();
  if (m_rightHand && m_rightHand->IsActive()) m_rightHand->RunFrame();
}

bool DeviceProvider::ShouldBlockStandbyMode() { return false; }

void DeviceProvider::EnterStandby() {}

void DeviceProvider::LeaveStandby() {}
