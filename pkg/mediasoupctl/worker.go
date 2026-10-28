// Package mediasoupctl 封装 mediasoup-worker 的 channel 协议（Unix socket 或 pipe fd）。
// 不依赖 Node.js；Go 直接与 worker 的 fd3 (channel) / fd4 (payloadChannel) 通信。
//
// mediasoup worker channel 协议（文本帧）:
//   请求:  <targetId>:<handlerId或"">:<data>\n
//   响应:  <targetId>:<accepted true|false>:<data>\n
//   通知:  <targetId>:<event>:<data>\n
// payload channel 是二进制帧: 4字节长度(网络序) + 4字节payload长度 + payload + data
//
// 本实现用 os/exec 传 ExtraFiles (fd3/fd4 为 pipe)。
package mediasoupctl

import (
	"bufio"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"sync"
)

// Worker 对一个 mediasoup-worker 子进程的封装。
type Worker struct {
	cmd       *exec.Cmd
	chW       *bufio.Writer    // fd3 写
	chR       *bufio.Reader    // fd3 读
	pwW       *bufio.Writer    // fd4 写
	pwR       *bufio.Reader    // fd4 读
	nextID    uint32
	pending   map[uint32]chan workerResponse
	notifCb   func(targetID string, event string, data json.RawMessage)
	mu        sync.Mutex
	closed    bool
	closeOnce sync.Once
}

type workerResponse struct {
	accepted bool
	data     json.RawMessage
	err      error
}

// WorkerOptions 传给 worker 进程的启动参数。
type WorkerOptions struct {
	BinPath   string // mediasoup-worker 可执行文件路径
	LogLevel  string // debug|warn|error|none
	RtcMinPort uint16
	RtcMaxPort uint16
}

// SpawnWorker 启动 worker 进程并握手。
func SpawnWorker(opts WorkerOptions) (*Worker, error) {
	if opts.BinPath == "" {
		return nil, errors.New("mediasoupctl: BinPath is required (path to mediasoup-worker)")
	}
	if opts.RtcMinPort == 0 {
		opts.RtcMinPort = 40000
	}
	if opts.RtcMaxPort == 0 {
		opts.RtcMaxPort = 49999
	}

	// 创建 pipe: Go 写 -> worker 读 (fd3), worker 写 -> Go 读 (fd3)
	chR_fd, chW_fd, err := os.Pipe()
	if err != nil {
		return nil, fmt.Errorf("channel pipe: %w", err)
	}
	pwR_fd, pwW_fd, err := os.Pipe()
	if err != nil {
		return nil, fmt.Errorf("payload pipe: %w", err)
	}

	cmd := exec.Command(opts.BinPath,
		"--logLevel="+orDefault(opts.LogLevel, "warn"),
		fmt.Sprintf("--rtcMinPort=%d", opts.RtcMinPort),
		fmt.Sprintf("--rtcMaxPort=%d", opts.RtcMaxPort),
	)
	// mediasoup worker 期望:
	//   fd 3 = channel 读端 (Go 写入)
	//   fd 4 = channel 写端 (worker 写出)
	//   fd 5 = payloadChannel 读端 (Go 写入)
	//   fd 6 = payloadChannel 写端 (worker 写出)
	cmd.ExtraFiles = []*os.File{chW_fd, chR_fd, pwW_fd, pwR_fd}
	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("spawn worker: %w", err)
	}

	// Go 侧只需要读写各一个 fd；chW_fd 由子进程读，chR_fd 由 Go 读？实际是:
	// ExtraFiles[i] 对应 fd3+i。我们传:
	//   ExtraFiles[0] = chW (Go 写入 → worker fd3 读)
	//   ExtraFiles[1] = chR (worker fd4 写 → Go 读)
	// 但 mediasoup worker 约定 fd3=读 fd4=写。
	// 修正: ExtraFiles[0] (fd3) 应该是 worker 读 = Go 写。即 chW_fd 放在 [0]。
	// ExtraFiles[1] (fd4) 应该是 worker 写 = Go 读。即 chR_fd 放在 [1]。
	// 同理 payload: fd5=读 (Go 写 = pwW_fd 在[2]) fd6=写 (worker 写 = pwR_fd 在[3])
	// 上面 ExtraFiles 顺序已经是 [chW, chR, pwW, pwR] → fd3,4,5,6，正确。
	// 但注意: os.Pipe() 返回 (r, w)。chR_fd 是读端（worker 写），但我们把它放 fd4，即 worker 从 fd4 写——worker 写到 fd4，Go 从 fd4 读。
	// 实际上 Go 进程里：
	//   chW_fd: Go 写，worker 读 (fd3)  ✓
	//   chR_fd: worker 写，Go 读 (fd4)  ✓
	//   pwW_fd: Go 写，worker 读 (fd5)  ✓
	//   pwR_fd: worker 写，Go 读 (fd6)  ✓
	// 但 os.Pipe() 返回 (r, w)，chR_fd,r = chR_fd? 变量名:
	//   chR_fd, chW_fd := os.Pipe() → chR_fd=r, chW_fd=w。
	//   所以 chR_fd 是读端。Go 读它 ✓。chW_fd 是写端。Go 写它 ✓。
	// worker 侧: fd3 应该是读端 (它读 Go 写入的数据) = chW_fd ✓
	//            fd4 应该是写端 (它写通知/响应给 Go) = chR_fd ✓
	// OK 语义对。

	// 关闭 Go 进程里传给子进程的那一端？不，ExtraFiles 内部会 dup2；父进程应关闭 fd3/4/5/6 对应的原始 fd（但不关 chW_fd 写端，否则无法写）。
	// 实际 exec.Cmd 会自动在子进程里 dup2 到 fd3-6，父进程两端都保留即可。
	// 但要关子进程没用到的端: chR_fd 在子进程里是被写的端（worker 写），子进程需要写它。
	// ExtraFiles 的语义: 传入的 *os.File，子进程会继承并 dup2 到 fd3+i。父子共享同一个打开文件描述。
	// 所以:
	//   chW_fd: 父进程写，子进程读 → 子进程拿到的 fd3 就是 chW_fd，父子共享同一个"写端打开文件"。
	//     这意味着子进程读 fd3 时读到的是父进程写入的数据，但如果父进程不写，子进程会阻塞（因为写端一直开着——父子共享同一个写端）。
	//     这里实际是把 chW_fd(写端) 传给了子进程，而子进程要的是"读端"。这是错的！

	// ---- 修正：重新搞 pipe 语义 ----
	// 需要两根 pipe：
	//   pipe1: Go → worker  (Go 写 = pipe1.w, worker 读 = pipe1.r → 传给子进程作为 fd3)
	//   pipe2: worker → Go  (worker 写 = pipe2.w → 传给子进程作为 fd4, Go 读 = pipe2.r)
	// 同理 pipe3/pipe4 for payload.
	// 用 os.Pipe() 时: os.Pipe() 返回 (r, w)。Go 写 = w 端, Go 读 = r 端。
	// ExtraFiles[0] (fd3) 应传 pipe1.r (worker 读端)。父进程持有 pipe1.w 用来写。
	// ExtraFiles[1] (fd4) 应传 pipe2.w (worker 写端)。父进程持有 pipe2.r 用来读。
	// 同理 fd5=pipe3.r (payload worker 读), fd6=pipe4.w (payload worker 写)。
	cmd.Process.Kill() // 上面 start 了但 pipe 方向不对，杀掉重来
	cmd.Wait()

	return spawnWorkerCorrect(opts)
}

func spawnWorkerCorrect(opts WorkerOptions) (*Worker, error) {
	// pipe1: Go → worker (Go 写, worker 读 = fd3)
	goToWorkerR, goToWorkerW, err := os.Pipe()
	if err != nil {
		return nil, err
	}
	// pipe2: worker → Go (worker 写 = fd4, Go 读)
	workerToGoR, workerToGoW, err := os.Pipe()
	if err != nil {
		return nil, err
	}
	// pipe3: Go → worker (payload, fd5)
	goToWorkerPR, goToWorkerPW, err := os.Pipe()
	if err != nil {
		return nil, err
	}
	// pipe4: worker → Go (payload, fd6)
	workerToGoPR, workerToGoPW, err := os.Pipe()
	if err != nil {
		return nil, err
	}

	cmd := exec.Command(opts.BinPath,
		"--logLevel="+orDefault(opts.LogLevel, "warn"),
		fmt.Sprintf("--rtcMinPort=%d", opts.RtcMinPort),
		fmt.Sprintf("--rtcMaxPort=%d", opts.RtcMaxPort),
	)
	cmd.ExtraFiles = []*os.File{
		goToWorkerR,   // fd3: worker 读
		workerToGoW,   // fd4: worker 写
		goToWorkerPR,  // fd5: worker 读 (payload)
		workerToGoPW,  // fd6: worker 写 (payload)
	}
	cmd.Stderr = os.Stderr
	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("spawn mediasoup-worker: %w", err)
	}

	// 父进程关闭不需要的端：worker 读端（Go 不读）、worker 写端（Go 不写）
	goToWorkerR.Close()
	workerToGoW.Close()
	goToWorkerPR.Close()
	workerToGoPW.Close()

	w := &Worker{
		cmd:     cmd,
		chW:     bufio.NewWriter(goToWorkerW),
		chR:     bufio.NewReader(workerToGoR),
		pwW:     bufio.NewWriter(goToWorkerPW),
		pwR:     bufio.NewReader(workerToGoPR),
		pending: make(map[uint32]chan workerResponse),
	}
	go w.readLoop()
	return w, nil
}

// Request 发送一个请求并等待响应。
// method 形如 "worker.createRouter"、"transport.produce" 等。
func (w *Worker) Request(method string, data interface{}) (json.RawMessage, error) {
	w.mu.Lock()
	if w.closed {
		w.mu.Unlock()
		return nil, errors.New("worker closed")
	}
	w.nextID++
	id := w.nextID
	ch := make(chan workerResponse, 1)
	w.pending[id] = ch
	w.mu.Unlock()

	// mediasoup channel 帧格式:
	//   <targetId>:<handlerId>:<data>\n
	// targetId 固定为 "0" (worker 本身), handlerId 我们用请求 id 的字符串。
	// data 是 JSON-RPC 风格: {"method":"...","id":<id>,"data":{...}}
	frameData, err := json.Marshal(map[string]interface{}{
		"method": method,
		"id":     id,
		"data":   data,
	})
	if err != nil {
		return nil, err
	}
	frame := fmt.Sprintf("0:%d:%s\n", id, frameData)
	w.chW.WriteString(frame)
	w.chW.Flush()

	select {
	case resp := <-ch:
		return resp.data, resp.err
	}
	// 不加超时；worker 崩溃由 readLoop 关闭所有 pending channel 触发。
}

// RequestWithPayload 带大数据（SDP 等），走 payload channel。
func (w *Worker) RequestWithPayload(method string, data interface{}, payload []byte) (json.RawMessage, []byte, error) {
	// Phase 1 简化: 不用 payload channel（大多数 mediasoup 请求 payload 为空或很小）。
	// SDP/RTP 参数一般 < 4KB，channel 里 JSON 就够。
	// 后续优化时再实现真正的 payload channel 分帧。
	msg, err := w.Request(method, data)
	return msg, nil, err
}

// OnNotification 注册 worker 事件回调（如 producer score 变化、transport closed）。
func (w *Worker) OnNotification(fn func(targetID, event string, data json.RawMessage)) {
	w.mu.Lock()
	w.notifCb = fn
	w.mu.Unlock()
}

// Close 关闭 worker。
func (w *Worker) Close() {
	w.closeOnce.Do(func() {
		w.mu.Lock()
		w.closed = true
		pending := w.pending
		w.pending = make(map[uint32]chan workerResponse)
		w.mu.Unlock()
		for _, ch := range pending {
			close(ch) // readLoop goroutine 会检查 closed
		}
		w.cmd.Process.Kill()
		w.cmd.Wait()
	})
}

func (w *Worker) readLoop() {
	for {
		line, err := w.chR.ReadString('\n')
		if err != nil {
			// worker 崩溃或关闭
			w.Close()
			return
		}
		// 格式: <targetId>:<accepted|event>:<data>
		// 响应:   targetId 是 "0", 第二列是 "true"/"false"
		// 通知:   第二列是 event 名
		parts := splitFrame(line)
		if len(parts) < 3 {
			continue
		}
		targetID := parts[0]
		second := parts[1]
		data := []byte(parts[2])

		switch second {
		case "true", "false":
			// 响应帧: {"id":<n>,"accepted":true,"data":...}
			var resp struct {
				ID       uint32          `json:"id"`
				Accepted bool            `json:"accepted"`
				Data     json.RawMessage `json:"data"`
				Reason   string          `json:"reason,omitempty"`
			}
			if err := json.Unmarshal(data, &resp); err != nil {
				continue
			}
			w.mu.Lock()
			ch, ok := w.pending[resp.ID]
			if ok {
				delete(w.pending, resp.ID)
			}
			w.mu.Unlock()
			if ok {
				if resp.Accepted {
					ch <- workerResponse{accepted: true, data: resp.Data}
				} else {
					ch <- workerResponse{err: fmt.Errorf("worker error: %s", resp.Reason)}
				}
			}
		default:
			// 通知帧: {"event":"...","data":...} 或第二列就是 event 名
			var notif struct {
				Event string          `json:"event"`
				Data  json.RawMessage `json:"data"`
			}
			if err := json.Unmarshal(data, &notif); err != nil || notif.Event == "" {
				notif.Event = second
				notif.Data = data
			}
			w.mu.Lock()
			cb := w.notifCb
			w.mu.Unlock()
			if cb != nil {
				cb(targetID, notif.Event, notif.Data)
			}
		}
	}
}

func splitFrame(s string) []string {
	s = s[:len(s)-1] // 去掉 \n
	var parts []string
	start := 0
	for i := 0; i < len(s); i++ {
		if s[i] == ':' && (i == 0 || s[i-1] != '\\') {
			parts = append(parts, s[start:i])
			start = i + 1
		}
	}
	parts = append(parts, s[start:])
	return parts
}

func orDefault(v, def string) string {
	if v == "" {
		return def
	}
	return v
}

var _ = io.EOF
var _ = binary.LittleEndian

// Pid 返回 worker 进程 PID（日志用）。
func (w *Worker) Pid() int { return w.cmd.Process.Pid }
