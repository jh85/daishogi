"""Legal-logit wire protocol: ragged batches, the full-policy variant, and
rejected indices. Ported from ../chushogi (tests/az_legal_policy_test.py,
commit d578c81) for 115,650 policy slots and 32-bit indices."""
import socket
import socketserver
import struct
import sys
import threading
import unittest
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'az'))
import model as M  # noqa: E402
from eval_server import Handler, PACKED, read_exact  # noqa: E402
from inference_batcher import InferenceBatcher  # noqa: E402

N = M.POLICY_SIZE


class ProtocolTest(unittest.TestCase):
    def test_mixed_protocols_and_rejected_indices(self):
        def evaluate(raw, progress, indices=None):
            # fp16-exact distinct values per index: index mod 2048 plus progress.
            policy = (np.arange(N) % 2048).astype(np.float32)[None, :] + progress[:, None]
            policy = policy.astype(np.float16)
            if indices is not None:
                out = np.zeros((len(indices), max(1, max(map(len, indices)))), dtype=np.float16)
                for i, row in enumerate(indices):
                    out[i, :len(row)] = policy[i, row]
                policy = out
            return policy, np.repeat(progress[:, None], 3, axis=1).astype(np.float16)

        class TCP(socketserver.ThreadingTCPServer):
            allow_reuse_address = True
            daemon_threads = True

        with TCP(('127.0.0.1', 0), Handler) as server:
            server.srv = None
            server.tcp_nodelay = True
            server.batcher = InferenceBatcher(evaluate, 128, 5)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                def request(rows):
                    n = 2
                    header = n if rows is None else n | 0x80000000
                    raw = (struct.pack('<I', header) + bytes(n * PACKED)
                           + np.array([1, 2], dtype='<f2').tobytes())
                    if rows is not None:
                        raw += np.array(list(map(len, rows)), dtype='<u2').tobytes()
                        raw += np.array([v for row in rows for v in row], dtype='<u4').tobytes()
                    with socket.create_connection(server.server_address, timeout=3) as sock:
                        sock.sendall(raw)
                        self.assertEqual(struct.unpack('<I', read_exact(sock, 4))[0], header)
                        count = n * N if rows is None else sum(map(len, rows))
                        values = np.frombuffer(read_exact(sock, (count + 6) * 2), dtype='<f2')
                    return values[:count], values[count:]

                full, wdl = request(None)
                # Duplicate, unordered, boundary (incl. > 65,535) and empty rows keep their order.
                for rows in [[[N - 1, 3, 3, 0, 70000], [1, 65536]], [[], [9, 0]], [[], []]]:
                    sparse, got_wdl = request(rows)
                    expected = np.concatenate(
                        [full.reshape(2, -1)[i, row] for i, row in enumerate(rows)])
                    self.assertEqual(sparse.tobytes(), expected.tobytes())
                    self.assertEqual(got_wdl.tobytes(), wdl.tobytes())
                # An index past the policy closes the connection without a reply.
                with socket.create_connection(server.server_address, timeout=3) as sock:
                    sock.sendall(struct.pack('<I', 0x80000001) + bytes(PACKED + 2)
                                 + struct.pack('<HI', 1, N))
                    self.assertEqual(sock.recv(1), b'')
            finally:
                server.shutdown()
                thread.join()
                server.batcher.close()


if __name__ == '__main__':
    unittest.main()
