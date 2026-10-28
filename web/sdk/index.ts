// RTCClient: 浏览器 SDK 封装。
// 职责: 连接 signal-gw（业务信令）+ media-gateway（媒体信令），管理 Transport/Producer/Consumer。
// 依赖: mediasoup-client（CDN 或 npm）
import { Device, types } from 'mediasoup-client';

export interface JoinOptions {
  roomId: string;
  displayName?: string;
  role?: string; // host | audience
  serverUrl?: string; // signal-gw WS URL, e.g. wss://sig.example.com/ws
  sfuUrl?: string;    // media-gateway WS URL, e.g. wss://sfu.example.com/sfu-ws
}

export interface JoinResult {
  peerId: string;
  rtpCapabilities: any;
  peers: PeerInfo[];
}

export interface PeerInfo {
  peerId: string;
  displayName: string;
  role: string;
  state: string;
  producers: { producerId: string; kind: string }[];
}

export type ClientEvent =
  | 'peerJoined' | 'peerLeft'
  | 'streamAdded' | 'streamRemoved'
  | 'producerPaused' | 'producerResumed'
  | 'reconnecting' | 'reconnected'
  | 'firstFramePlayed'
  | 'error';

type EventCb = (data: any) => void;

export class RTCClient {
  private signalWs: WebSocket | null = null;
  private sfuWs: WebSocket | null = null;
  private device: Device | null = null;
  private pubTransport: types.Transport | null = null;
  private subTransport: types.Transport | null = null;
  private producers: Map<string, types.Producer> = new Map(); // kind → Producer
  private consumers: Map<string, types.Consumer> = new Map(); // consumerId → Consumer
  private peerId = '';
  private roomId = '';
  private peers: Map<string, PeerInfo> = new Map();
  private listeners: Map<ClientEvent, EventCb[]> = new Map();
  private reqCounter = 0;
  private pending = new Map<string, { resolve: Function; reject: Function; timer: number }>();
  private connected = false;

  // ---- 事件 ----
  on(event: ClientEvent, cb: EventCb): void {
    if (!this.listeners.has(event)) this.listeners.set(event, []);
    this.listeners.get(event)!.push(cb);
  }

  private emit(event: ClientEvent, data?: any): void {
    (this.listeners.get(event) || []).forEach(cb => cb(data));
  }

  // ---- join: 一步完成连接 + 进房 + Transport 预建 ----
  async join(opts: JoinOptions): Promise<JoinResult> {
    this.roomId = opts.roomId;

    // 1. 连接 signal-gw
    await this.connectSignal(opts.serverUrl || `wss://${location.host}/ws?token=dev`);
    // 2. join 请求 → 拿 peerId + rtpCapabilities
    const resp = await this.signalRequest('join', opts.roomId, {
      displayName: opts.displayName || 'user',
      role: opts.role || 'audience',
    });
    this.peerId = resp.peerId;
    this.peers = new Map((resp.peers || []).map((p: PeerInfo) => [p.peerId, p]));

    // 3. device.load
    this.device = new Device();
    await this.device.load({ routerRtpCapabilities: resp.rtpCapabilities });

    // 4. 连接 SFU WS（并行可做，Phase 1 简化串行）
    await this.connectSfu(opts.sfuUrl || `wss://${location.host}/sfu-ws`);

    // 5. createTransport pub + sub
    const [pubInfo, subInfo] = await Promise.all([
      this.sfuRequest('createTransport', { direction: 'pub' }),
      this.sfuRequest('createTransport', { direction: 'sub' }),
    ]);
    this.pubTransport = this.device.createSendTransport(pubInfo);
    this.subTransport = this.device.createRecvTransport(subInfo);

    // transport connect 事件: mediasoup-client 触发 connectTransport 信令
    this.pubTransport.on('connect', ({ dtlsParameters }, callback, errback) => {
      this.sfuRequest('connectTransport', {
        transportId: this.pubTransport!.id, dtlsParameters,
      }).then(() => callback()).catch(errback);
    });
    this.subTransport.on('connect', ({ dtlsParameters }, callback, errback) => {
      this.sfuRequest('connectTransport', {
        transportId: this.subTransport!.id, dtlsParameters,
      }).then(() => callback()).catch(errback);
    });

    // produce 事件: mediasoup-client 触发 produce 信令
    this.pubTransport!.on('produce', ({ kind, rtpParameters, appData }, callback, errback) => {
      this.sfuRequest('produce', {
        transportId: this.pubTransport!.id, kind, rtpParameters,
      }).then(({ producerId }) => callback({ id: producerId })).catch(errback);
    });

    // 订阅已有 peers 的 producers
    for (const peer of resp.peers || []) {
      if (peer.peerId === this.peerId) continue;
      for (const prod of peer.producers || []) {
        await this.consumePeer(peer.peerId, prod.producerId, prod.kind);
      }
    }

    this.connected = true;
    return { peerId: this.peerId, rtpCapabilities: resp.rtpCapabilities, peers: resp.peers || [] };
  }

  // ---- publish: 发布音视频 ----
  async publish(camStream: MediaStream): Promise<{ audioProducerId: string; videoProducerId: string }> {
    if (!this.pubTransport) throw new Error('not joined');
    const result = { audioProducerId: '', videoProducerId: '' };
    for (const track of camStream.getTracks()) {
      const kind = track.kind as 'audio' | 'video';
      const producer = await this.pubTransport!.produce({ track });
      this.producers.set(kind, producer);
      if (kind === 'audio') result.audioProducerId = producer.id;
      else result.videoProducerId = producer.id;
    }
    return result;
  }

  // ---- unpublish ----
  async unpublish(): Promise<void> {
    for (const [kind, producer] of this.producers) {
      producer.close();
      this.sfuSend('closeProducer', { producerId: producer.id });
    }
    this.producers.clear();
  }

  // ---- 静音/关摄 ----
  async pauseProducer(kind: 'audio' | 'video'): Promise<void> {
    const p = this.producers.get(kind);
    if (!p) return;
    p.pause();
    this.sfuSend('pauseProducer', { producerId: p.id });
  }

  async resumeProducer(kind: 'audio' | 'video'): Promise<void> {
    const p = this.producers.get(kind);
    if (!p) return;
    p.resume();
    this.sfuSend('resumeProducer', { producerId: p.id });
  }

  // ---- 连麦 ----
  async requestMic(): Promise<void> {
    await this.signalRequest('requestMic', this.roomId, {});
  }

  async grantMic(targetPeerId: string): Promise<void> {
    await this.signalRequest('grantMic', this.roomId, { targetPeerId });
  }

  // ---- leave ----
  async leave(): Promise<void> {
    this.sfuSend('leave', {});
    this.signalSend('leave', {});
    for (const c of this.consumers.values()) c.close();
    for (const p of this.producers.values()) p.close();
    this.consumers.clear();
    this.producers.clear();
    this.pubTransport?.close();
    this.subTransport?.close();
    this.signalWs?.close();
    this.sfuWs?.close();
    this.connected = false;
  }

  getPeerId(): string { return this.peerId; }
  getRoomId(): string { return this.roomId; }
  getPeers(): PeerInfo[] { return Array.from(this.peers.values()); }

  // ---- 内部: signal WS ----
  private connectSignal(url: string): Promise<void> {
    return new Promise((resolve, reject) => {
      this.signalWs = new WebSocket(url);
      this.signalWs.onopen = () => resolve();
      this.signalWs.onerror = (e) => reject(new Error('signal WS error'));
      this.signalWs.onmessage = (ev) => this.onSignalMessage(JSON.parse(ev.data));
      this.signalWs.onclose = () => { if (this.connected) this.emit('reconnecting'); };
    });
  }

  private connectSfu(url: string): Promise<void> {
    return new Promise((resolve, reject) => {
      const u = url + (url.includes('?') ? '&' : '?') + `peerId=${this.peerId}&roomId=${this.roomId}`;
      this.sfuWs = new WebSocket(u);
      this.sfuWs.onopen = () => resolve();
      this.sfuWs.onerror = () => reject(new Error('SFU WS error'));
      this.sfuWs.onmessage = (ev) => this.onSfuMessage(JSON.parse(ev.data));
    });
  }

  private onSignalMessage(env: any): void {
    if (env.reqId && this.pending.has(env.reqId)) {
      const { resolve, timer } = this.pending.get(env.reqId)!;
      clearTimeout(timer);
      this.pending.delete(env.reqId);
      if (env.code === 0) resolve(env.payload || {});
      else resolve(env); // 错误也 resolve，上层判断 code
      return;
    }
    switch (env.type) {
      case 'peerJoined':
        this.peers.set(env.payload.peerId, env.payload);
        this.emit('peerJoined', env.payload);
        break;
      case 'peerLeft':
        this.peers.delete(env.payload.peerId);
        this.emit('peerLeft', env.payload);
        break;
      case 'roomUpdate':
        // Phase 1: 忽略，Phase 2 diff
        break;
    }
  }

  private onSfuMessage(env: any): void {
    if (env.reqId && this.pending.has(env.reqId)) {
      const { resolve, timer } = this.pending.get(env.reqId)!;
      clearTimeout(timer);
      this.pending.delete(env.reqId);
      if (env.code === 0) resolve(env.payload || {});
      else reject(new Error(env.msg || 'sfu error'));
      return;
    }
    switch (env.type) {
      case 'newProducer':
        this.consumePeer(env.payload.peerId, env.payload.producerId, env.payload.kind);
        break;
      case 'producerClosed':
        this.closeConsumerByProducer(env.payload.producerId);
        break;
      case 'producerPaused':
        this.emit('producerPaused', env.payload);
        break;
      case 'producerResumed':
        this.emit('producerResumed', env.payload);
        break;
      case 'rtpCapabilities':
        // 连接时收到，缓存（join 后 device.load 已用过 signal 侧的，此处忽略）
        break;
    }
  }

  // ---- 内部: 消费一路 ----
  private async consumePeer(peerId: string, producerId: string, kind: string): Promise<void> {
    if (!this.subTransport || !this.device) return;
    try {
      const resp = await this.sfuRequest('consume', {
        transportId: this.subTransport.id,
        producerId,
        rtpCapabilities: this.device.rtpCapabilities,
      });
      const consumer = await this.subTransport.consume({
        id: resp.id,
        producerId: resp.producerId,
        kind: resp.kind,
        rtpParameters: resp.rtpParameters,
      });
      this.consumers.set(consumer.id, consumer);
      consumer.on('transportclose', () => this.consumers.delete(consumer.id));

      // 等首帧再 resume
      const track = consumer.track;
      // 通知上层
      const stream = new MediaStream([track]);
      this.emit('streamAdded', { peerId, kind, stream, consumerId: consumer.id, producerId });
      // resume
      await this.sfuRequest('resumeConsumer', { consumerId: consumer.id });

      // 首帧回调
      const video = track as MediaStreamTrack;
      if (kind === 'video') {
        // 等 track 有画面（简单方案：延时 + onunmute）
        setTimeout(() => this.emit('firstFramePlayed', { peerId, kind }), 100);
      }
    } catch (e) {
      console.error('[RTCClient] consumePeer error:', e);
    }
  }

  private closeConsumerByProducer(producerId: string): void {
    for (const [cid, c] of this.consumers) {
      if (c.producerId === producerId) {
        c.close();
        this.consumers.delete(cid);
        this.emit('streamRemoved', { producerId });
        break;
      }
    }
  }

  // ---- 内部: RPC helpers ----
  private nextReqID(): string { return `r-${this.peerId}-${++this.reqCounter}`; }

  private signalSend(type: string, payload: any): void {
    if (!this.signalWs || this.signalWs.readyState !== WebSocket.OPEN) return;
    this.signalWs.send(JSON.stringify({ type, roomId: this.roomId, peerId: this.peerId, payload }));
  }

  private sfuSend(type: string, payload: any): void {
    if (!this.sfuWs || this.sfuWs.readyState !== WebSocket.OPEN) return;
    this.sfuWs.send(JSON.stringify({ type, roomId: this.roomId, peerId: this.peerId, payload }));
  }

  private signalRequest(type: string, roomId: string, payload: any): Promise<any> {
    return new Promise((resolve, reject) => {
      const reqId = this.nextReqID();
      const timer = window.setTimeout(() => {
        this.pending.delete(reqId);
        reject(new Error(`${type} timeout`));
      }, 8000);
      this.pending.set(reqId, { resolve, reject, timer });
      this.signalWs?.send(JSON.stringify({ type, roomId, reqId, payload }));
    });
  }

  private sfuRequest(type: string, payload: any): Promise<any> {
    return new Promise((resolve, reject) => {
      const reqId = this.nextReqID();
      const timer = window.setTimeout(() => {
        this.pending.delete(reqId);
        reject(new Error(`${type} timeout`));
      }, 8000);
      this.pending.set(reqId, { resolve, reject, timer });
      this.sfuWs?.send(JSON.stringify({ type, roomId: this.roomId, reqId, payload }));
    });
  }
}
