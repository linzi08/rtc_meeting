#include "camera_capturer.h"

#include "../util/log.h"

namespace vm::media {

static const char* TAG = "camera";

std::vector<std::string> CameraCapturer::EnumDevices() {
    std::vector<std::string> names;
    // m140：VideoCaptureFactory 直接在 webrtc 命名空间（无 videocapturemodule 前缀）
    std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(
        webrtc::VideoCaptureFactory::CreateDeviceInfo());
    if (!info) return names;

    uint32_t n = info->NumberOfDevices();
    for (uint32_t i = 0; i < n; ++i) {
        char name[256] = {0}, guid[256] = {0};
        if (info->GetDeviceName(i, name, sizeof(name), guid, sizeof(guid)) == 0) {
            names.push_back(name);
        }
    }
    return names;
}

bool CameraCapturer::Start(int index, webrtc::scoped_refptr<LocalVideoSource> source) {
    Stop();
    source_ = source;

    std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(
        webrtc::VideoCaptureFactory::CreateDeviceInfo());
    if (!info || (int)info->NumberOfDevices() <= index) {
        LOGT(TAG, "摄像头 %d 不存在（--camera 序号越界）", index);
        return false;
    }

    char name[256] = {0}, guid[256] = {0};
    info->GetDeviceName(index, name, sizeof(name), guid, sizeof(guid));

    // Create(deviceUniqueId)：DirectShow 后端按唯一 id 打开
    module_ = webrtc::VideoCaptureFactory::Create(guid);
    if (!module_) {
        LOGT(TAG, "打开摄像头失败: %s（独占被占? 同机多开请用 --pattern）", name);
        return false;
    }

    module_->RegisterCaptureDataCallback(this);

    // 请求能力：优先 640x480@30（DirectShow 协商最接近的能力）
    webrtc::VideoCaptureCapability cap;
    cap.width     = 640;
    cap.height    = 480;
    cap.maxFPS    = 30;
    cap.videoType = webrtc::VideoType::kI420;
    cap.interlaced = false;
    if (module_->StartCapture(cap) != 0) {
        LOGT(TAG, "启动采集失败: %s", name);
        Stop();
        return false;
    }

    LOGT(TAG, "摄像头采集已启动: %s (640x480@30 起)", name);
    return true;
}

void CameraCapturer::Stop() {
    if (module_) {
        module_->StopCapture();
        module_->DeRegisterCaptureDataCallback();
        module_ = nullptr;
    }
    source_ = nullptr;
}

}  // namespace vm::media
