// =============================================================================
// video_meeting 客户端入口
//
// 用法示例：
//   client.exe --url wss://192.168.1.10:4443 --room demo --name alice
//   client.exe --url ... --pattern                （无摄像头/同机多开第二个实例）
//   client.exe --url ... --svc L3T3_KEY           （VP9 SVC 分层）
//   client.exe --url ... --screen --screen-fps 15 （启动即共享屏幕）
//   client.exe --url ... --stun stun:1.2.3.4:3478 --turn turn:1.2.3.4:3478 \
//              --turn-user 1760000000 --turn-pass xxxx   （跨公网）
//
// 会中按键：m 麦克风 | v 摄像头 | c 屏幕共享 | r ICE重启 | s 统计 | q 退出
// =============================================================================
#include <windows.h>

#include <conio.h>   // _kbhit / _getch

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "media/d3d11_renderer.h"
#include "media/media_engine.h"
#include "room/room_client.h"
#include "util/log.h"

static void OnTerminate() {
    LOGT("main", "!!! std::terminate() 被调用 — 程序即将异常退出 !!!");
    try {
        throw;
    } catch (const std::exception& e) {
        LOGT("main", "未捕获异常: %s", e.what());
    } catch (...) {
        LOGT("main", "未捕获未知异常");
    }
    std::fflush(stdout);
    std::abort();
}

static LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord) {
        auto code = ep->ExceptionRecord->ExceptionCode;
        auto addr = ep->ExceptionRecord->ExceptionAddress;
        LOGT("main", "!!! Windows SEH 异常 — 进程即将终止 !!! 代码=0x%08lX 地址=%p",
             code, addr);
        if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
            auto op = ep->ExceptionRecord->ExceptionInformation[0];
            auto va = ep->ExceptionRecord->ExceptionInformation[1];
            LOGT("main", "  访问违规: %s 地址=%p", op == 0 ? "读取" : op == 1 ? "写入" : "执行", (void*)va);
        }
    } else {
        LOGT("main", "!!! Windows SEH 异常 — 进程即将终止 !!! (无异常记录)");
    }
    std::fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

static LONG CALLBACK OnVectoredException(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord) {
        auto code = ep->ExceptionRecord->ExceptionCode;
        if (code == 0x406D1388) return EXCEPTION_CONTINUE_SEARCH;
        auto addr = ep->ExceptionRecord->ExceptionAddress;
        if (code == EXCEPTION_ACCESS_VIOLATION) {
            LOGT("main", "!!! VEH 捕获访问违规 !!! 地址=%p", addr);
            if (ep->ExceptionRecord->NumberParameters >= 2) {
                auto op = ep->ExceptionRecord->ExceptionInformation[0];
                auto va = ep->ExceptionRecord->ExceptionInformation[1];
                LOGT("main", "  操作: %s 目标地址=%p", op == 0 ? "读取" : op == 1 ? "写入" : "执行", (void*)va);
            }
        } else if (code == EXCEPTION_STACK_OVERFLOW) {
            LOGT("main", "!!! VEH 捕获栈溢出 !!! 地址=%p", addr);
        } else if (code == EXCEPTION_INT_DIVIDE_BY_ZERO) {
            LOGT("main", "!!! VEH 捕获除零异常 !!! 地址=%p", addr);
        } else if (code == 0xE06D7363) {
            LOGT("main", "!!! VEH 捕获 C++ 异常(跨线程未捕获) !!! 地址=%p", addr);
        } else {
            LOGT("main", "!!! VEH 捕获异常 !!! 代码=0x%08lX 地址=%p", code, addr);
        }
        std::fflush(stdout);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static BOOL WINAPI OnConsoleCtrl(DWORD type) {
    const char* msg = "未知";
    switch (type) {
        case CTRL_C_EVENT:         msg = "Ctrl+C"; break;
        case CTRL_BREAK_EVENT:     msg = "Ctrl+Break"; break;
        case CTRL_CLOSE_EVENT:     msg = "控制台关闭"; break;
        case CTRL_LOGOFF_EVENT:    msg = "用户注销"; break;
        case CTRL_SHUTDOWN_EVENT:  msg = "系统关机"; break;
    }
    LOGT("main", "!!! 控制台信号: %s — 进程即将终止 !!!", msg);
    std::fflush(stdout);
    return FALSE;
}

static void AtExitHandler() {
    LOGT("main", "!!! atexit() 被调用 — exit() 或 main() 返回 !!!");
    std::fflush(stdout);
}

static void PrintUsage() {
    printf(
        "\nvideo_meeting 客户端 (mediasoup-demo 服务端 / mediasoupclient)\n"
        "参数:\n"
        "  --url wss://host:4443     服务端地址 (默认 wss://127.0.0.1:4443)\n"
        "  --room demo               房间号\n"
        "  --name alice              显示名\n"
        "  --camera N                摄像头序号 (默认 0)\n"
        "  --pattern                 用合成图案源（无摄像头/同机多开时用）\n"
        "  --screen [N]              启动即共享屏幕 (N=显示器序号, 默认 0)\n"
        "  --screen-fps N            屏幕共享帧率 5-30 (默认 10)\n"
        "  --no-audio / --no-video   关闭麦克风/摄像头\n"
        "  --no-hw                   禁用硬编解码（软编 VP8/VP9）\n"
        "  --svc L1T3|L1T2|L3T3_KEY  VP9 SVC 分层（需补丁；失败自动降级）\n"
        "  --stun stun:ip:3478       STUN 服务器（可多个）\n"
        "  --turn turn:ip:3478       TURN 服务器（可多个）\n"
        "  --turn-user u             TURN 用户名\n"
        "  --turn-pass p             TURN 密码\n"
        "  --force-tcp               强制 TCP 传输（排查 UDP 阻断）\n"
        "  --allow-tearing           渲染跳过 vsync（最低延迟，可能撕裂）\n"
        "  --verify-cert             校验服务端证书（默认跳过=自签名兼容）\n"
        "  --reconnect-max N         最大重连次数 (默认 30)\n"
        "会中按键: m 麦克风  v 摄像头  c 屏幕共享  r ICE重启  s 统计  q 退出\n\n");
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);

    std::set_terminate(OnTerminate);
    SetUnhandledExceptionFilter(OnUnhandledException);
    AddVectoredExceptionHandler(1, OnVectoredException);
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
    std::atexit(AtExitHandler);

    LOGT("main", "main() 入口");

    try {

    vm::RoomClient::Options opts;
    std::string turnUser, turnPass;

    for (int i = 1; i < argc; ++i) {
        auto next = [&](const char* def = "") -> std::string {
            return (i + 1 < argc) ? std::string(argv[++i]) : std::string(def);
        };
        std::string a = argv[i];
        if      (a == "--url")           opts.url = next();
        else if (a == "--origin")        opts.origin = next();
        else if (a == "--room")          opts.roomId = next("demo");
        else if (a == "--name")          opts.displayName = next("native");
        else if (a == "--camera")        opts.cameraIndex = atoi(next("0").c_str());
        else if (a == "--pattern")       opts.usePattern = true;
        else if (a == "--screen")        { opts.screenShare = true; opts.screenMonitor = atoi(next("0").c_str()); }
        else if (a == "--screen-fps")    opts.screenFps = atoi(next("10").c_str());
        else if (a == "--no-audio")      opts.noAudio = true;
        else if (a == "--no-video")      opts.noVideo = true;
        else if (a == "--no-hw")         opts.noHw = true;
        else if (a == "--svc")           opts.svc = next();
        else if (a == "--stun" || a == "--turn") {
            // IceServer 非聚合类型：逐字段赋值（两种参数同构，凭据在下方统一挂）
            webrtc::PeerConnectionInterface::IceServer s;
            s.urls.push_back(next());
            opts.iceServers.push_back(s);
        }
        else if (a == "--turn-user")     turnUser = next();
        else if (a == "--turn-pass")     turnPass = next();
        else if (a == "--force-tcp")     opts.forceTcp = true;
        else if (a == "--allow-tearing") opts.allowTearing = true;
        else if (a == "--verify-cert")   opts.insecure = false;
        else if (a == "--reconnect-max") opts.reconnectMax = atoi(next("30").c_str());
        else if (a == "--help" || a == "-h") { PrintUsage(); LOGT("main", "退出: --help"); return 0; }
        else { printf("未知参数: %s\n", a.c_str()); PrintUsage(); LOGT("main", "退出: 未知参数 %s", a.c_str()); return 1; }
    }

    // TURN 凭据挂到所有 turn: 服务器
    for (auto& s : opts.iceServers) {
        if (!s.urls.empty() && s.urls[0].rfind("turn", 0) == 0) {
            s.username = turnUser;
            s.password = turnPass;
        }
    }

    LOGT("main", "video_meeting 客户端启动 (url=%s room=%s name=%s)", opts.url.c_str(),
         opts.roomId.c_str(), opts.displayName.c_str());

    // 媒体引擎：硬编解码注入开关 = 编译开关(补丁模式) && 运行参数(--no-hw)
    bool hwInject =
#if VM_HW_INJECT
        !opts.noHw;
#else
        false;
#endif
    if (!vm::media::MediaEngine::Instance().Init(hwInject, !opts.svc.empty())) {
        LOGT("main", "退出: 媒体引擎初始化失败");
        return 1;
    }

    // 渲染器（D3D11 零拷贝）
    vm::render::RendererManager::Instance().Start(opts.allowTearing);
    LOGT("main", "渲染器已启动");

    // 会话
    vm::RoomClient room(opts);
    room.Start();
    LOGT("main", "RoomClient 已启动，进入交互循环");

    // —— 命令行交互 ——
    printf("\n[会中] m 麦克风 | v 摄像头 | c 屏幕共享 | r ICE重启 | s 统计 | q 退出\n\n");
    bool running = true;
    int  heartbeat = 0;
    while (running) {
        if (_kbhit()) {
            int c = _getch();
            switch (c) {
                case 'm': room.ToggleMic(); break;
                case 'v': room.ToggleCamera(); break;
                case 'c': room.ToggleScreenShare(); break;
                case 'r': room.RestartIceManually(); break;
                case 's': room.PrintStats(); break;
                case 'q': LOGT("main", "用户按 q 退出"); running = false; break;
                default: break;
            }
        }
        Sleep(100);
        if (++heartbeat % 50 == 0) {
            LOGT("main", "主循环心跳 (运行 %ds)", heartbeat / 10);
        }
    }

    // 逆序退出：会话 → 渲染 → 媒体引擎
    LOGT("main", "开始逆序退出...");
    room.Stop();
    LOGT("main", "RoomClient 已停止");
    vm::render::RendererManager::Instance().Shutdown();
    LOGT("main", "渲染器已关闭");
    vm::media::MediaEngine::Instance().Shutdown();
    LOGT("main", "媒体引擎已关闭，程序正常退出");
    return 0;

    } catch (const std::exception& e) {
        LOGT("main", "!!! main() 未捕获异常: %s — 程序退出 !!!", e.what());
        std::fflush(stdout);
        return 2;
    } catch (...) {
        LOGT("main", "!!! main() 未捕获未知异常 — 程序退出 !!!");
        std::fflush(stdout);
        return 2;
    }
}