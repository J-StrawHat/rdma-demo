# RDMA Sample Codes

## Prerequisites

1. Check the network interface corresponding to the available(state: `ACTIVE`) Mellanox RDMA NIC

    ```bash
    $ rdma link
    link mlx5_0/1 state ACTIVE physical_state LINK_UP netdev ens3f0np0 
    link mlx5_1/1 subnet_prefix fe80:0000:0000:0000 lid 65535 sm_lid 0 lmc 0 state DOWN physical_state DISABLED
    ```
    We can see that the network interface corresponding to the active device is `ens3f0np0`

2. Check the addresses of network interface from `/etc/netplan/xx-installer-config.yaml`

    ```bash
    $ sudo vim /etc/netplan/xx-installer-config.yaml
    network:
    ethernets:
            ens3f0np0:
            addresses:
            - xxx
            xxx:
            addresses:
            - xxx
    ...
    ```

## Usage

01_basic-client-server:
- For server: `./server`
- For client: `./client <server inet IP> <server random port>`

02_read-write:
- For writing:
    - server:  `./rdma-server write`
    - client: `./rdma-client write <server inet IP> <server random port>`
- For reading:
    - server: `./rdma-server read`
    - client: `./rdma-client read <server inet IP> <server random port>`
    
04_gpu-direct-rdma:
- For server (.164):
  ```bash
  ./server -a <local host inet IP> -n <iterations> -D <debug mask> -s <data size> -p <port> [-u <GPU BDF>]
  # ./gpu_server -a 192.168.0.210 -n 10000 -D 1 -s 10000000 -p 17788 -u 17:00.0
  ```
- For client (.163):
  ```bash
  ./client -t <RDMA operation type> -a <local host inet IP> <remote server inet IP> -n <iterations> -D <debug mask> -s <data size> -p <port> [-u <GPU BDF>]
  # ./client -t 0 -a 192.168.0.208 192.168.0.210 -n 10000 -D 1 -s 10000000 -p 17788 -u ca:00.0
  ```

## References:
- [Basic codes from thegeekinthecorner.com](https://github.com/tarickb/the-geek-in-the-corner.git)
- [Tutorial](https://thegeekinthecorner.wordpress.com/2013/02/02/rdma-tutorial-pdfs/)
    - [Building an RDMA-Capable Application with IB Verbs](http://www.hpcadvisorycouncil.com/pdf/building-an-rdma-capable-application-with-ib-verbs.pdf)
    - [RDMA Read and Write with IB Verbs](http://www.hpcadvisorycouncil.com/pdf/rdma-read-and-write-with-ib-verbs.pdf)
    - [Basic Flow Control for RDMA Transfers](http://www.hpcadvisorycouncil.com/pdf/vendor_content/basic-flow-control-for-rdma-transfers.pdf)
- [GPU Direct RDMA Access example code](https://github.com/Mellanox/gpu_direct_rdma_access.git)

