# Why

In the most common case, I found that the performance of tun module is always poor when a number of packets flow in.

So that's why i try MMAP to read tun packet to avoid the copying.

However, accroding to the [benchmark result of Cloudflare](https://github.com/cloudflare/cloudflare-blog/tree/master/2019-02-tcp-splice), the zerocopy writing isn't a good idea, which is always slower than POSIX write function commonly.

As far as I'm concerned, it's not necessary to do a zerocopy write, while reading is necessary.


