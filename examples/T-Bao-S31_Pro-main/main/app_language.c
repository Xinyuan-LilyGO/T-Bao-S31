/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_language.h"

#include <string.h>
#include <stdint.h>
#include "nvs.h"
#include "esp_log.h"

#define APP_LANGUAGE_NAMESPACE "app_settings"
#define APP_LANGUAGE_KEY "language"

static const char *TAG = "APP_LANGUAGE";
static app_language_t s_language = APP_LANGUAGE_ZH_CN;

typedef struct {
    const char *zh;
    const char *en;
} language_entry_t;

static const language_entry_t s_common_text[] = {
    {"对话", "Chat"}, {"摄像头", "Camera"}, {"音乐", "Music"},
    {"移动", "Motion"}, {"游戏", "Games"}, {"计时器", "Timer"},
    {"视频", "Video"}, {"表情", "Expressions"}, {"小智AI", "Xiaozhi AI"},
    {"小智 AI", "Xiaozhi AI"}, {"设置", "Settings"}, {"人脸跟随", "Face Follow"},
    {"返回", "Home"}, {"控制中心", "Control Center"}, {"设备快捷控制", "Quick Controls"},
    {"音量", "Volume"}, {"亮度", "Brightness"}, {"首页", "Home"},
    {"语言", "Language"}, {"中文", "Chinese"}, {"English", "English"},
    {"选择界面语言", "Choose interface language"},
    {"你", "You"}, {"喵伴", "Miaoban"}, {"系统", "System"}, {"就绪", "Ready"},
    {"系统", "System"}, {"蓝牙", "Bluetooth"}, {"显示与声音", "Display & Sound"},
    {"电池与充电", "Battery & Charging"}, {"存储与设备", "Storage & Device"},
    {"无线网络", "Wireless Network"}, {"重启设备", "Restart Device"},
    {"恢复设置默认值", "Restore Defaults"}, {"取消", "Cancel"}, {"确认", "Confirm"},
    {"检测中", "Checking..."}, {"读取中", "Loading..."}, {"准备中", "Ready"},
    {"已连接", "Connected"}, {"未连接", "Disconnected"}, {"已挂载", "Mounted"},
    {"未挂载", "Not mounted"}, {"重新配置 WiFi", "Configure WiFi"},
    {"管理功能未接入", "Management unavailable"}, {"重启与恢复选项", "Restart and recovery"},
    {"设备偏好与连接状态", "Device preferences and connectivity"},
    {"设置详情", "Settings details"}, {"界面提示音", "Interface sounds"},
    {"点击与页面切换反馈", "Tap and page transition feedback"},
    {"将清除已保存的 WiFi，并重启进入配网。", "Saved WiFi will be cleared and the device will restart into setup."},
    {"设备将立即重启。", "The device will restart immediately."},
    {"将恢复音量、亮度、提示音和小智开关。WiFi 信息会保留。", "Volume, brightness, sounds, and Xiaozhi will be reset. WiFi is kept."},
    {"电量：--", "Battery: --"}, {"电池电压：--", "Battery voltage: --"},
    {"输入电压：--", "Input voltage: --"}, {"充电阶段：--", "Charge phase: --"},
    {"故障码：--", "Fault code: --"}, {"等待充电监测数据", "Waiting for battery data"},
    {"SD 卡", "SD card"}, {"固件版本：未知", "Firmware: unknown"},
    {"构建时间：", "Build time: "}, {"WiFi 检测中", "Checking WiFi"},
    {"WiFi 未连接", "WiFi disconnected"}, {"上滑收起", "Swipe up to close"},
    {"WiFi 已连接", "WiFi connected"}, {"再次确认", "Tap again to confirm"},
    {"重置", "Reset"}, {"重置中", "Resetting"}, {"请稍候", "Please wait"},
    {"设备状态", "Device status"}, {"拖动调整音量", "Drag to adjust volume"},
    {"拖动调整亮度", "Drag to adjust brightness"},
    {"实时预览", "Live preview"}, {"喵伴 S31", "Miaoban S31"},
    {"动画选择", "Animation selector"}, {"实时记录", "Live transcript"},
    {"手动驾驶", "Manual driving"}, {"环形钟面", "Circular timer"},
    {"选择玩法", "Choose a game"}, {"SD 媒体库", "SD media library"},
    {"你的专属语音伙伴", "Your voice companion"}, {"音乐播放器", "Music player"},
    {"离线跟随控制", "Offline follow control"}, {"关于设备", "About device"},
    {"专注计时", "Focus timer"},
    {"绿色框为当前目标", "Green box marks the current target"},
    {"启动中", "Starting"}, {"等待人脸", "Waiting for face"},
    {"跟随中", "Following"}, {"人脸丢失，已停车", "Face lost; stopped"},
    {"跟随故障", "Follow fault"}, {"已停止", "Stopped"},
    {"开始", "Start"}, {"停止", "Stop"}, {"停止跟随", "Stop follow"},
    {"后台运行", "Background"}, {"知道了", "OK"},
    {"继续进入", "Continue"}, {"预测", "Predicted"},
    {"框", "Box"}, {"当前动画", "Current animation"},
    {"无活动", "No activity"}, {"轻微", "Low"}, {"活跃", "Active"},
    {"运动", "Motion"}, {"位置", "Position"}, {"朝向", "Heading"},
    {"电机", "Motor"}, {"执行器故障，已锁定；重启后检查接线", "Actuator fault locked; check wiring after restart"},
    {"关闭", "Off"}, {"等待", "Waiting"}, {"第一次呼叫：你好小智", "First call: Hello Xiaozhi"},
    {"第二次：你好小智，讲个笑话", "Second: Hello Xiaozhi, tell me a joke"},
    {"仅唤醒小智", "Wake Xiaozhi only"}, {"就可以开始正常对话啦", "Then start a normal conversation"},
    {"运动:%d%%", "Motion: %d%%"}, {"播放结束", "Playback finished"},
    {"屏幕亮度", "Screen brightness"}, {"界面提示音", "Interface sounds"},
    {"提示音设置保存失败", "Failed to save sound setting"},
    {"提示音已开启", "Interface sounds on"}, {"提示音已关闭", "Interface sounds off"},
    {"运行中", "Running"}, {"关闭中", "Stopping"}, {"已关闭", "Disabled"},
    {"启动失败", "Start failed"}, {"预充电", "Precharge"}, {"快速充电", "Fast charge"},
    {"恒压收尾", "Taper"}, {"补充充电", "Top-off"}, {"涓流", "Trickle"},
    {"已完成", "Done"},
    {"未充电", "Not charging"}, {"状态正常", "Status normal"},
    {"暂不可用", "Unavailable"}, {"已充满", "Full"}, {"正在充电", "Charging"},
    {"已连接电源", "Power connected"}, {"电池供电", "Battery power"},
    {"插入 SD 卡后可播放本地音乐与视频", "Insert an SD card to play local music and videos"},
    {"容量信息暂不可用", "Capacity unavailable"}, {"打开 AI 管理", "Open AI management"},
    {"蓝牙管理功能未接入当前应用", "Bluetooth management is unavailable in this app"},
    {"恢复默认设置失败", "Failed to restore defaults"}, {"亮度", "Brightness"},
    {"请先关闭小智 AI，再开启人脸跟随，避免运行卡顿", "Disable Xiaozhi AI before enabling face follow to avoid lag."},
    {"设置已保存", "Settings saved"}, {"设置保存失败", "Failed to save settings"},
    {"充电阶段", "Charge phase"}, {"系统", "System"},
    {"固件版本：未知", "Firmware: unknown"}, {"屏幕亮度", "Screen brightness"},
    {"拖动调整", "Drag to adjust"}, {"提示音设置保存失败", "Failed to save sound setting"},
    {"提示音已开启", "Interface sounds on"}, {"提示音已关闭", "Interface sounds off"},
    {"检测中", "Checking..."}, {"读取中", "Loading..."},
    {"WiFi 检测中", "Checking WiFi"}, {"管理功能未接入", "Management unavailable"},
    {"重启设备", "Restart device"}, {"恢复设置默认值", "Restore defaults"},
    {"重新配置 WiFi", "Configure WiFi"}, {"无线网络", "Wireless Network"},
    {"存储与设备", "Storage & Device"}, {"蓝牙", "Bluetooth"},
    {"显示与声音", "Display & Sound"}, {"重启与恢复选项", "Restart and recovery"},
    {"状态正常", "Status normal"}, {"充电阶段：--", "Charge phase: --"},
    {"故障码：--", "Fault code: --"}, {"输入电压：--", "Input voltage: --"},
    {"电池电压：--", "Battery voltage: --"}, {"电量：--", "Battery: --"},
    {"屏幕", "Screen"}, {"触摸", "Touch"}, {"摄像头", "Camera"},
    {"音频", "Audio"}, {"应用", "Apps"}, {"关于设备", "About device"},
    {"自然", "Neutral"}, {"开心", "Happy"}, {"难过", "Sad"},
    {"哭泣", "Crying"}, {"困倦", "Sleepy"}, {"生气", "Angry"},
    {"震惊", "Shocked"}, {"困惑", "Confused"}, {"眨眼", "Winking"},
    {"前进", "Forward"}, {"左转", "Turn left"}, {"右转", "Turn right"},
    {"后退", "Reverse"}, {"急停", "Emergency stop"}, {"转一圈", "Spin"},
    {"回中心", "Reset pose"}, {"人脸跟随: 开", "Face follow: On"},
    {"人脸跟随: 关", "Face follow: Off"}, {"运动", "Motion"},
    {"不可用", "Unavailable"}, {"故障锁定", "Fault locked"},
    {"保护停车", "Failsafe stop"}, {"动作中", "Action running"},
    {"运动中", "Moving"}, {"前进", "Forward"},
    {"未找到 AVI/MJPEG 视频", "No AVI/MJPEG videos found"},
    {"随机播放", "Random"}, {"顺序播放", "Sequential"}, {"单曲循环", "Repeat one"},
    {"正在播放", "Playing"}, {"选择视频播放", "Select a video"},
    {"显示屏切换失败", "Display switch failed"}, {"暂不支持该视频格式", "Video format not supported"},
    {"视频无法播放", "Video cannot be played"},
    {"请将 MJPEG AVI 放入 SD 卡根目录", "Put an MJPEG AVI in the SD card root"},
    {"视频控件初始化失败", "Video controls failed to initialize"},
    {"视频自动播放失败", "Video autoplay failed"},
    {"未选择曲目", "No track selected"}, {"正在播放中…", "Playing…"},
    {"已暂停", "Paused"}, {"点击曲库选择音乐", "Choose music from the library"},
    {"播放失败", "Playback failed"}, {"SD卡中没有音乐", "No music on SD card"},
    {"曲库", "Library"}, {"选择音乐", "Choose music"},
    {"设置专注时长", "Set focus duration"}, {"上下滑动刻度调整 · 每格 1 分钟", "Swipe the scale; each step is 1 minute"},
    {"25 分", "25 min"}, {"5 分", "5 min"}, {"15 分", "15 min"}, {"1 小时", "1 hour"},
    {"确认时长", "Apply duration"}, {"时间到", "Time is up"},
    {"准备开始", "Ready to start"}, {"已暂停", "Paused"},
    {"专注进行中", "Focus in progress"}, {"重新开始", "Restart"},
    {"开始专注", "Start focus"}, {"继续专注", "Resume focus"},
    {"暂停专注", "Pause focus"}, {"选择一个游戏", "Choose a game"},
    {"专注", "Focus"},
    {"敲木鱼", "Wooden fish"}, {"功德无量", "Merit"}, {"功德", "Merit"},
    {"功德 +1", "Merit +1"}, {"点击木鱼 功德+1", "Tap the fish for +1 merit"},
    {"游戏结束", "Game over"}, {"分数", "Score"}, {"返回", "Back"},
    {"2048 达成！", "2048 reached!"}, {"选择一个表情", "Choose an expression"},
};

esp_err_t app_language_init(void)
{
    s_language = APP_LANGUAGE_ZH_CN;
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(APP_LANGUAGE_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "open language failed: %s", esp_err_to_name(ret));
        return ret;
    }
    uint8_t value = 0;
    if (nvs_get_u8(handle, APP_LANGUAGE_KEY, &value) == ESP_OK && value <= 1) {
        s_language = (app_language_t)value;
    }
    nvs_close(handle);
    return ESP_OK;
}

app_language_t app_language_get(void) { return s_language; }
bool app_language_is_english(void) { return s_language == APP_LANGUAGE_EN_US; }

esp_err_t app_language_set(app_language_t language)
{
    if (language != APP_LANGUAGE_ZH_CN && language != APP_LANGUAGE_EN_US) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(APP_LANGUAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_u8(handle, APP_LANGUAGE_KEY, (uint8_t)language);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    if (ret == ESP_OK) {
        s_language = language;
    }
    return ret;
}

esp_err_t app_language_toggle(void)
{
    return app_language_set(app_language_is_english() ? APP_LANGUAGE_ZH_CN : APP_LANGUAGE_EN_US);
}

const char *app_language_name(void)
{
    return app_language_is_english() ? "English" : "中文";
}

const char *app_language_code(void)
{
    return app_language_is_english() ? "en-US" : "zh-CN";
}

const char *app_language_text(const char *zh, const char *en)
{
    if (zh == NULL) {
        return en != NULL ? en : "";
    }
    if (!app_language_is_english()) {
        return zh;
    }
    if (en != NULL) {
        return en;
    }
    for (size_t i = 0; i < sizeof(s_common_text) / sizeof(s_common_text[0]); ++i) {
        if (strcmp(zh, s_common_text[i].zh) == 0) {
            return s_common_text[i].en;
        }
    }
    return zh;
}
