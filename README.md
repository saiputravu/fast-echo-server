# fast-echo-server

## Build

```
  mkdir build
  cd build
  cmake ..
  make
```

## TODO
- [ ] Wire up IO_URING to take benefit of multiple UDP senders via AF_XDP routing
- [ ] AF_XDP, AF_PACKET.
- [ ] BlueFlame mlx4 and mlx5+ equivalent.
