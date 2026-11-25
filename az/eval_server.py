#!/usr/bin/env python3
"""GPU eval server for AZ self-play: receives bit-packed positions from
daishogi-az workers over TCP, replies with policy logits + WDL.

Ported from ../chushogi (az/eval_server.py, commit d578c81). Changes: policy
indices are u32 (115,650 > 65,535), and the network size is taken from the
checkpoint's training config.

Protocol (length-prefixed little-endian frames):
  request : u32 n | n * 3179 bytes (113 bit planes) | n * fp16 progress
  response: u32 n | n * 115650 fp16 policy logits | n * 3 fp16 wdl
Legal variant: set bit 31 of n, append n u16 counts and row-major u32 policy
indices to the request. Response echoes the tagged n and returns only those
fp16 policy entries, followed by unchanged WDL. Both variants are supported.

--watch makes the server reload the checkpoint whenever the file changes
(the training loop publishes improved checkpoints atomically).
"""

import argparse
import json
import os
import socket
import socketserver
import struct
import threading
import time

import numpy as np
import torch

import model as M
from inference_batcher import InferenceBatcher

PACKED = M.PACKED_BYTES


class Server:
    def __init__(self, checkpoint, device, dtype):
        self.profile_eval = False
        self.profile_stats = dict(calls=0,positions=0,unpack_upload_seconds=0.,forward_host_seconds=0.,download_convert_seconds=0.,worker_cpu_seconds=0.,forward_stream_ms=0.)
        self.device = device
        self.dtype = dtype
        self.checkpoint = checkpoint
        self.lock = threading.Lock()
        self.net = self.place(M.DaiNet())
        self.net_config = M.net_config({})
        self.mtime = None
        self.reload(force=True)

    def place(self, net):
        """Inference copy on the device. On CUDA the weights are stored in
        bf16 (not just autocast): the forward pass is memory-bound on the
        GB10, and this saves the per-call weight casts (about 1.65x faster
        for 256 x 6; logits within 0.1 of autocast)."""
        net = net.to(self.device).eval()
        return net.to(self.dtype) if self.device == "cuda" else net

    def reload(self, force=False):
        if not self.checkpoint:
            return
        try:
            mt = os.path.getmtime(self.checkpoint)
        except OSError:
            return
        if not force and mt == self.mtime:
            return
        state = torch.load(self.checkpoint, map_location="cpu",
                           weights_only=True)
        config = M.net_config(state)
        if config != self.net_config:  # a run trained with a non-default size
            self.net = self.place(M.DaiNet(**config))
            self.net_config = config
        M.load_weights(self.net, state["model"] if "model" in state else state)
        self.mtime = mt
        print(f"[eval] loaded {self.checkpoint} (step {state.get('step', '?')})",
              flush=True)

    def eval(self, packed, progress, indices=None):
        profile = self.profile_eval
        if profile:
            start=time.perf_counter(); cpu=time.thread_time()
        cuda = self.device == "cuda"
        planes = (M.unpack_batch_device(packed, progress, self.device, self.dtype) if cuda
                  else M.unpack_batch(packed, progress))
        if profile:
            uploaded=time.perf_counter()
            if self.device=='cuda':
                begin=torch.cuda.Event(enable_timing=True);end=torch.cuda.Event(enable_timing=True)
                begin.record()
        # No autocast: the weights are already bf16 (see place()); autocast
        # would run the norms in fp32 and cost about 40%.
        with torch.no_grad():
            policy, wdl = self.net(planes)
        if profile:
            if self.device=='cuda':end.record()
            forward=time.perf_counter()
        if indices is not None:
            # Pad ragged legal lists only within this inference batch.
            width = max(map(len, indices), default=0)
            gather = np.zeros((len(indices), width), dtype=np.int64)
            for row, values in enumerate(indices):
                gather[row, :len(values)] = values
            policy = policy.gather(1, torch.from_numpy(gather).to(self.device))
        # Convert on-device: avoid float32 D2H traffic and CPU float16 casts.
        result=(policy.to(torch.float16).cpu().numpy(),
                wdl.to(torch.float16).cpu().numpy())
        if profile:
            finish=time.perf_counter();stats=self.profile_stats
            stats['calls']+=1;stats['positions']+=len(progress)
            stats['unpack_upload_seconds']+=uploaded-start
            stats['forward_host_seconds']+=forward-uploaded
            stats['download_convert_seconds']+=finish-forward
            stats['worker_cpu_seconds']+=time.thread_time()-cpu
            if self.device=='cuda':stats['forward_stream_ms']+=begin.elapsed_time(end)
            if stats['calls']%100==0:print('[eval-profile] '+json.dumps(stats),flush=True)
        return result


def read_exact(conn, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("closed")
        buf.extend(chunk)
    return bytes(buf)


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        if self.server.tcp_nodelay:
            self.request.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        srv = self.server.srv
        while True:
            try:
                head = read_exact(self.request, 4)
                (header,) = struct.unpack("<I", head)
                legal = bool(header & 0x80000000)
                n = header & 0x7fffffff
                if not 1 <= n <= 1024:
                    return
                packed = read_exact(self.request, n * PACKED)
                prog = np.frombuffer(read_exact(self.request, n * 2),
                                     dtype=np.float16)
                indices = None
                if legal:
                    counts = np.frombuffer(read_exact(self.request, n*2), dtype='<u2')
                    if np.any(counts > M.POLICY_SIZE):
                        return
                    flat = np.frombuffer(read_exact(self.request, int(counts.sum())*4), dtype='<u4')
                    if np.any(flat >= M.POLICY_SIZE):
                        return
                    ends = np.cumsum(counts, dtype=np.int64)
                    indices = np.split(flat, ends[:-1])
            except ConnectionError:
                return
            if self.server.batcher is not None:
                policy, wdl = self.server.batcher.submit(packed, prog.astype(np.float32), indices).result()
            else:
                policy, wdl = evaluate_locked(srv, packed, prog.astype(np.float32), indices)
            policy_bytes = (b''.join(row[:int(count)].tobytes() for row, count in zip(policy, counts))
                            if legal else policy.tobytes())
            out = struct.pack("<I", header) + policy_bytes + wdl.tobytes()
            self.request.sendall(out)


def evaluate_locked(srv, packed, progress, indices=None):
    with srv.lock:
        if srv.watch:
            srv.reload()
        return srv.eval(packed, progress, indices)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--checkpoint", default="")
    ap.add_argument("--port", type=int, default=51589)
    ap.add_argument("--cpu", action="store_true")
    ap.add_argument("--bf16", action="store_true", default=True)
    ap.add_argument("--watch", action="store_true",
                    help="reload checkpoint when the file changes")
    ap.add_argument("--profile-eval", action="store_true", help="diagnostic phase timing; off by default")
    ap.add_argument("--max-batch", type=int, default=0,
                    help="combine independent requests up to this many positions (0=off)")
    ap.add_argument("--batch-wait-ms", type=float, default=1.0)
    ap.add_argument("--tcp-nodelay", action="store_true")
    ap.add_argument("--stats-interval", type=float, default=30.0)
    a = ap.parse_args()
    if a.max_batch < 0 or not np.isfinite(a.batch_wait_ms) or a.batch_wait_ms < 0:
        ap.error("batch size and wait must be finite and nonnegative")

    device = "cpu" if a.cpu or not torch.cuda.is_available() else "cuda"
    srv = Server(a.checkpoint, device, torch.bfloat16)
    srv.watch = a.watch
    srv.profile_eval = a.profile_eval

    class Server_(socketserver.ThreadingTCPServer):
        allow_reuse_address = True
        daemon_threads = True

    with Server_(("127.0.0.1", a.port), Handler) as httpd:
        httpd.srv = srv
        httpd.tcp_nodelay = a.tcp_nodelay
        httpd.batcher = (InferenceBatcher(lambda packed, progress, indices=None: evaluate_locked(srv, packed, progress, indices),
                                         a.max_batch, a.batch_wait_ms) if a.max_batch else None)
        if httpd.batcher is not None and a.stats_interval > 0:
            def stats():
                while True:
                    time.sleep(a.stats_interval)
                    print(f"[eval] batching {httpd.batcher.stats}", flush=True)
            threading.Thread(target=stats, daemon=True).start()
        print(f"[eval] serving on 127.0.0.1:{a.port} ({device})", flush=True)
        try:
            httpd.serve_forever()
        finally:
            if httpd.batcher is not None:
                httpd.batcher.close()


if __name__ == "__main__":
    main()
