# sfu/mediasoup-worker/

这个目录存放 mediasoup-worker 的**编译产物**（二进制文件），不是源码。

mediasoup-worker 是 C++ 写的，需要单独克隆 mediasoup 源码并编译：

```bash
# 1. 克隆源码（约 50MB）
git clone https://github.com/versatica/mediasoup.git ~/mediasoup-src

# 2. 编译 worker（需要 meson + ninja + python3 + g++）
cd ~/mediasoup-src/worker
meson setup build --buildtype=release
meson compile -C build

# 3. 编译产物
ls build/mediasoup-worker

# 4. 复制到本项目
mkdir -p /home/wsl/rtc-platform/sfu/mediasoup-worker/build
cp build/mediasoup-worker /home/wsl/rtc-platform/sfu/mediasoup-worker/build/
```

或者用项目脚本：

```bash
cd /home/wsl/rtc-platform
./scripts/build-worker.sh ~/mediasoup-src
```

编译完成后启动 media-gateway：

```bash
./bin/media-gateway \
  -port 9090 \
  -worker-bin ./sfu/mediasoup-worker/build/mediasoup-worker \
  -listen-ip 0.0.0.0 \
  -announced-ip <你的IP> \
  -workers 1
```
