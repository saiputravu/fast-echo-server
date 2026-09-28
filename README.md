# fast-echo-server

Ongoing exploration on how fast UDP server replies can be. That is, for a small datagram, what is the
latency of recv, process, send. I worked on a Mellanox NIC, Linux v6.8, Xeon CPU. Details are purposefully vague.

## Method

The sender issues 256 byte payloads (32, 64-bit integers), sequentially ordered such that we send
`[0, 1, 2, ..., 32]`. Each receiving server is expected to XOR each position against the byte received and
re-send. The initiating sender will verify the zero-bytes to ensure the packet got delivered correctly. 

To remove the dependency on timing in the receiver (and prevent distractions in the recv, process, send path), we use kernel software timestamps on send-recv on the sending process. More concretely, we enable
`SO_TIMESTAMPING_NEW` socket option with flags `TX_SOFTWARE|RX_SOFTWARE` set. The sender manages sequence ids
of sent and received packets. TX timestamps are received from the socket error queue whereas RX timestamping is received on control message of the reply itself. RX to TX id mataching is done and then reduced to a mean, standard deviation and sample count over 100ms windows.
- A TODO here would be to enable hardware timestamps, as the NIC that I am using supports this.

The values seen are an over-estimate and RTT of packets, where network characteristics contribute systematic errors.

## Receive paths under test

- **Blocking syscalls.** `recvfrom`/`sendto` is used as the baseline. There is a variant that learns the peer
  address from the first datagram and `connect`s the socket so it can use the lighter `recv`/`send`. We use this as a base benchmark.
- **epoll.** An epoll event loop. The aim here is to try and reduce the latency to action.
- **Busy polling.** Socket option `SO_BUSY_POLL`, with CLI flag to toggle `SO_PREFER_BUSY_POLL`. This socket will busy loop instead of waiting for the soft-interrupt issuing the packet over the socket. There are interesting timing characteristics here to tune such as the max continuous poll time.
- **Busy polling with epoll.** Combines above.
- **io_uring.** Aiming at reducing the syscall overhead of `recvfrom`/`sendto`. Exploration done with `SQPOLL` and `SINGLE_ISSUER`, with a multishot `recvmsg` feeding off registered provided-buffer ring(s). One submission covers many receives, buffers are recycled once the matching send completes.

## Build

Needs CMake, a C++23 compiler, `liburing` and pthreads. FTXUI is fetched
automatically if it is not already installed. Builds at `-O3` with frame pointers kept so `perf`
call graphs resolve properly.

```
  mkdir build
  cd build
  cmake ..
  make
```

## Results
> TODO 

| Receive path | Mean (ns) | Stddev (ns) | Samples | Notes |
| --- | --- | --- | --- | --- |
| Blocking syscalls | | | | |
| Blocking, connected | | | | |
| epoll | | | | |
| Busy polling | | | | |
| Busy polling with epoll | | | | |
| io_uring multishot | | | | |

## Roadmap

- [ ] Wire up IO_URING to take benefit of multiple UDP senders via AF_XDP routing
- [ ] AF_XDP, AF_PACKET.
- [ ] BlueFlame mlx4 and mlx5+ equivalent.
