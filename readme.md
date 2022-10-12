# Why

In the most common case, I found that the performance of tun module is always poor when a number of packets flow in.

So that's why i try MMAP to read tun packet to avoid the copying.

However, accroding to the [benchmark result of Cloudflare](https://blog.cloudflare.com/sockmap-tcp-splicing-of-the-future/), the zerocopy of TCP may not a good idea, which is always slower than POSIX write function when writing or receiving a large packet.



As far as I'm concerned, it's not necessary to do a zerocopy for TCP, while tun is necessary.


