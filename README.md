# Distributed_System_Labs

## Index

Lab的说明文档见Documents目录。

[Lab1 Reliable Data Transport Protocol](#lab-1-reliable-data-transport-protocol)

[Lab2 Send and receive packets with DPDK](#lab-2-send-and-receive-packets-with-dpdk)

[Lab3 QoS Implementation with DPDK](#lab-3-qos-implementation-with-dpdk)

[Lab4 MapReduce](#lab-4-mapreduce)

## Lab 1 Reliable Data Transport Protocol

### 设计策略

* **策略选择**

  * 采用Selective Repeat策略。

* **Packet设计**

  * 原则：只增加必要的信息进包头，选择满足条件的占用最少的数据类型。

  * 无条件在头部增加两字节存放校验码，因使用16位校验，选取unsigned int作为数据类型。

  * 尝试后发现测试用例的发包大概在几万这个数量级，因此新增的包的编号选择了unsigned int。

  * packet number在发送方为全局变量，因此保证了message的顺序，不需要添加message的信息。

  * Data Packet

    | Checksum | Packet number | Payload size | Payload  |
    | -------- | ------------- | ------------ | -------- |
    | 2 Bytes  | 2 Bytes       | 1 Byte       | The rest |

  * ACK Packet

    | Checksum | ACK number | Nothing  |
    | -------- | ---------- | -------- |
    | 2 Bytes  | 2 Bytes    | The rest |

### 实现策略

* **Sender**
  * packet number初始化为0，新包取当前packet number作为包号，并增加packet number。
  * 维护三个队列，分别对应包、包号、是否受到ACK，所有新包直接排进队列末尾，但活跃区仅为前WINDOWSIZE个元素。
  * 加入新进队的包处在活跃区，则发出这个包并（重新）开始计时。
  * 收到某个包的ACK时，如果这个包在队列中间，则仅标记其ACK为true；如果这个包在队首，则弹出队首开始连续的已收到ACK的包，然后依次发送新进入活跃区的包，并重新开始计时。**(滑动窗口)**
  * 一旦超时，若队列为空说明包发完了，直接返回；若队列不为空则依次发送活跃区内ACK状态为false的包，并重新开始计时。**(选择重传)**
  * 整个过程没有用到停止计时的功能。
* **Receiver**
  * next number初始化为0，表示当前等待的包的包号，收到该包后递增。
  * 维护两个队列，分别对应包、包号，符合条件的新收到的包会进入队列。
  * 收到包号小于next number的包，说明之前该包的ACK未送达，重发ACK后丢弃该包。
  * 收到包号在next number到next number + BUFFERSIZE - 1之间的包，检查队列内是否存在该包，存在则说明之前该包的ACK未送达，重发ACK后丢弃该包；不存在则放进队首，维护队列，发送ACK。
  * 收到包号超过next number + BUFFERSIZE - 1的包，说明滑窗出现了bug，但说不定还有救，直接丢弃并且不发ACK，万一滑窗又好了它会再次发送该包的。
  * 维护队列仅在新包进入队列时发生，放入新包后整理队列保持升序，若队首包号为next number，则将队首开始连续包号的包依次弹出传递给上层，并修改next number为最后一个弹出的后一位，如果弹空了则表示某一阶段的数据传输完毕。

### 参数选取

| TIMEOUT | BUFFER SIZE |
| ------- | ----------- |
| 0.3     | 16          |

首先用TA推荐的0.3作为超时限制，依次测试以2 ~ 16作为缓冲区大小的结果，得到如下趋势：

![buffersize](Reliable%20Data%20Transport%20Protocol/charts/buffersize.png)

可见在超时限制不变的前提下缓冲区越大，吞吐量越高，传输耗时越低。但大容量缓冲区在实际情况下会增加对硬件的要求，因此适可而止地加到16就没再上探了。

然后以16作为缓冲区的大小，又依次测试了0.1 ~ 0.7作为超时限制的结果，得到如下趋势：

![timeout](Reliable%20Data%20Transport%20Protocol/charts/timeout.png)

可见虽然超时时限越低吞吐量越高，传输耗时越短，但从传输数据与超时时长关系中会发现，过短的超时限制带来的性能提升背后是相同数据被反复重传的巨大开支，而大于0.3的超时限制不会再减少传输数据量，因此选择TA给出的0.3确实是比较合适的超时限制。**(详细测试数据参见test_data.xlsx)**

### 遇到的坑

* 一开始试图给message编号，再给message里的packet单独编号，后来发现这种设计没有必要，而且增加复杂度和开支，具体表现在需要事先告知接收方某条message的包数，而这条信息本身也可能崩掉。
* 因为有队列单独维护ACK状态和next number，在连续弹出的时候用队列的front()作为循环体的判断条件，忽略了队列可能在弹出过程中弹空导致下一次循环判断调用front()时出现无法预估的错误。

## Lab 2 Send and receive packets with DPDK

### Part 0 Set up environment

基础环境：~~Ubuntu 18.04 LTS~~降级DPDK后各种报错又把Ubuntu 16.04 LTS装回来了

安装依赖：

```shell
$ sudo apt install libpcap-dev
```

~~下载DPDK，我选了目前LTS分支的最新版本19\.11~~根据TA的要求更换为指定版本16\.11\.11，解压后执行：

```shell
 $ make config T=x86_64-native-linuxapp-gcc
 $ sed -ri 's,(PMD_PCAP=).*,\1y,' build/.config
 $ make
```

经过漫长的等待编译完毕，然后配置DPDK的运行环境，以root用户运行：

```shell
$ mkdir -p /mnt/huge
$ mount -t hugetlbfs nodev /mnt/huge
$ echo 64 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
$ modprobe uio
$ insmod ./build/kmod/igb_uio.ko
$ ./tools/dpdk-devbind.py -s
$ ./tools/dpdk-devbind.py --bind=igb_uio 02:00.0
$ export RTE_SDK=/home/richard/Desktop/dpdk
$ export RTE_TARGET=build
```

绑定网卡那一步会报警告可以无视无伤大雅，然后测试一下DPDK是否成功安装：

```shell
$ cd $RTE_SDK/examples/helloworld
$ make
$ ./build/helloworld
```

得到输出：

```shell
...
hello from core 1
hello from core 2
hello from core 3
hello from core 0
```

### Part 1 Get familiar with DPDK

Q1: What’s the purpose of using hugepage?

* 在内存容量不变的条件下，页越大，页表项越少，页表占用的内存越少。更少的页表项意味着缺页的情况更不容易发生，缺页中断的次数会减少，TLB的miss次数也会减少。

Q2: Take examples/helloworld as an example, describe the execution flow of DPDK programs?

* 初始化Environment Abstraction Layer \(EAL\)，在main\(\)函数中：

  ```c++
  ret = rte_eal_init(argc, argv);
  if (ret < 0)
  	rte_panic("Cannot init EAL\n");
  ```

  在Linux环境下该调用在main\(\)被调用前完成初始化流程（每个lcore的初始化等），返回的是参数的个数。

* 在从属核心上调用lcore\_hello\(\)：

  ```c++
  RTE_LCORE_FOREACH_SLAVE(lcore_id) {
  	rte_eal_remote_launch(lcore_hello, NULL, lcore_id);
  }
  ```

* 在主核心上调用lcore\_hello\(\)：

  ```c++
  lcore_hello(NULL);
  ```

* 最后等待所有线程执行结束：

  ```c++
  rte_eal_mp_wait_lcore();
  ```

Q3: Read the codes of examples/skeleton, describe DPDK APIs related to sending and receiving packets\.

* rte\_eth\_tx\_burst\(\)，定义如下：

  ```c++
  static uint16_t rte_eth_tx_burst (
      uint16_t port_id,
      uint16_t queue_id,
      struct rte_mbuf ** tx_pkts,
      uint16_t nb_pkts
  )
  ```

  用于在参数port\_id指定的以太网设备上发送参数queue\_id指定的输出队列上的数据包，参数nb\_pkts指定了要发送的数据包的数量，这些数据包由rte\_mbuf结构的参数tx\_pkts提供，函数返回实际发送的包的数量。

* rte\_eth\_rx\_burst\(\)，定义如下：

  ```c++
  static uint16_t rte_eth_rx_burst (
      uint16_t port_id,
      uint16_t queue_id,
      struct rte_mbuf ** rx_pkts,
      const uint16_t nb_pkts
  )	
  ```

  用于在参数port\_id指定的以太网设备上的由参数queue\_id指定的接收队列上循环解析RX ring，最多nb\_pkts个包，并且对于每一个完整的RX descriptor，初始化一个rte\_mbuf结构的数据，并存放到参数tx\_pkts的下一个目录。

Q4: Describe the data structure of ‘rte\_mbuf’\.

* 在rte\_mbuf\_core\.h的第467行找到其定义如下：

  ```c++
  struct rte_mbuf {
      RTE_MARKER cacheline0;
  
      void *buf_addr;           
      RTE_STD_C11
      union {
          rte_iova_t buf_iova;
          rte_iova_t buf_physaddr; 
      } __rte_aligned(sizeof(rte_iova_t));
  
      /* next 8 bytes are initialised on RX descriptor rearm */
      RTE_MARKER64 rearm_data;
      uint16_t data_off;
  
      RTE_STD_C11
      union {
          rte_atomic16_t refcnt_atomic; 
          uint16_t refcnt;
      };
      uint16_t nb_segs;         
      uint16_t port;
  
      uint64_t ol_flags;        
      /* remaining bytes are set on RX when pulling packet from descriptor */
      RTE_MARKER rx_descriptor_fields1;
  
      /*
       * The packet type, which is the combination of outer/inner L2, L3, L4
       * and tunnel types. The packet_type is about data really present in the
       * mbuf. Example: if vlan stripping is enabled, a received vlan packet
       * would have RTE_PTYPE_L2_ETHER and not RTE_PTYPE_L2_VLAN because the
       * vlan is stripped from the data.
       */
      RTE_STD_C11
      union {
          uint32_t packet_type; 
          struct {
              uint32_t l2_type:4; 
              uint32_t l3_type:4; 
              uint32_t l4_type:4; 
              uint32_t tun_type:4; 
              RTE_STD_C11
              union {
                  uint8_t inner_esp_next_proto;
                  __extension__
                  struct {
                      uint8_t inner_l2_type:4;
                      uint8_t inner_l3_type:4;
                  };
              };
              uint32_t inner_l4_type:4; 
          };
      };
  
      uint32_t pkt_len;         
      uint16_t data_len;        
      uint16_t vlan_tci;
  
      RTE_STD_C11
      union {
          union {
              uint32_t rss;     
              struct {
                  union {
                      struct {
                          uint16_t hash;
                          uint16_t id;
                      };
                      uint32_t lo;
                  };
                  uint32_t hi;
              } fdir; 
              struct rte_mbuf_sched sched;
              struct {
                  uint32_t reserved1;
                  uint16_t reserved2;
                  uint16_t txq;
              } txadapter; 
              uint32_t usr;
          } hash;                   
      };
  
      uint16_t vlan_tci_outer;
  
      uint16_t buf_len;         
      uint64_t timestamp;
  
      /* second cache line - fields only used in slow path or on TX */
      RTE_MARKER cacheline1 __rte_cache_min_aligned;
  
      RTE_STD_C11
      union {
          void *userdata;   
          uint64_t udata64; 
      };
  
      struct rte_mempool *pool; 
      struct rte_mbuf *next;    
      /* fields to support TX offloads */
      RTE_STD_C11
      union {
          uint64_t tx_offload;       
          __extension__
          struct {
              uint64_t l2_len:RTE_MBUF_L2_LEN_BITS;
              uint64_t l3_len:RTE_MBUF_L3_LEN_BITS;
              uint64_t l4_len:RTE_MBUF_L4_LEN_BITS;
              uint64_t tso_segsz:RTE_MBUF_TSO_SEGSZ_BITS;
              /*
               * Fields for Tx offloading of tunnels.
               * These are undefined for packets which don't request
               * any tunnel offloads (outer IP or UDP checksum,
               * tunnel TSO).
               *
               * PMDs should not use these fields unconditionally
               * when calculating offsets.
               *
               * Applications are expected to set appropriate tunnel
               * offload flags when they fill in these fields.
               */
              uint64_t outer_l3_len:RTE_MBUF_OUTL3_LEN_BITS;
              uint64_t outer_l2_len:RTE_MBUF_OUTL2_LEN_BITS;
              /* uint64_t unused:RTE_MBUF_TXOFLD_UNUSED_BITS; */
          };
      };
  
      uint16_t priv_size;
  
      uint16_t timesync;
  
      uint32_t seqn;
  
      struct rte_mbuf_ext_shared_info *shinfo;
  
      uint64_t dynfield1[2]; 
  } __rte_cache_aligned;
  ```

  * buf\_addr: 当前mbuf的虚拟地址
  * union \{\.\.\.\}: mbuf对应的物理地址
  * data\_off: 标识mbuf的data room开始地址到报文起始位置的偏移
  * refcnt: mbuf被引用的次数
  * nb\_segs, next: 当前的mbuf报文有多少个分段和下一个分段的地址，单向链表连接
  * port: 输入输出端口号
  * ol_flags: 卸载特性标识
  * packet_type: 报文类型
  * pkt\_len, data\_len, buf\_len: 报文长度信息
  * vlan\_tci, vlan\_tci\_outer: vlan信息
  * union \{\.\.\.\}: 报文的hash信息
  * timestamp: 时间戳，只有ol\_flags设置了PKT\_RX\_TIMESTAMP才有意义
  * pool: mbuf从这个pool申请来的，释放mbuf的时候用到
  * union \{\.\.\.\}: tx offload的信息

### Part 2 Send packets with DPDK

1. **Construct UDP packets**

   | 最外面    | 第二外面 | 里面    | 最里面 |
   | --------- | -------- | ------- | ------ |
   | ETHER_HDR | IPV4_HDR | UDP_HDR | DATA   |

   由于markdown表格必须两行无奈加上了奇怪的第一行，定义如下：

   ```c++
   struct Packet {
   	struct rte_ether_hdr ether_hdr;
   	struct rte_ipv4_hdr ipv4_hdr;
   	struct rte_udp_hdr udp_hdr;
   	struct rte_data data;
   };
   ```

   其中数据部分定义如下：

   ```c++
   struct rte_data {
   	char content[16];
   };
   ```

2. **Write a DPDK application to construct and send UDP packets**

   ~~配置Virtual NIC之前一定要先确保ifconfig可用，配置完成后会断网没法下载安装~~降级后无此问题

   具体代码参见main\.c文件，发包的内容为“Hello,Wireshark!”。

3. **Verify the correctness**

   用Wireshark捕获到包的内容如下：

   ![wireshark](Send%20and%20receive%20packets%20with%20DPDK/wireshark.png)

## Lab 3 QoS Implementation with DPDK

### Task 1A  Implement a meter

首先给qos\.c补上缺失的头文件，否则没法用uint32\_t和uint64\_t：

```c++
#include <stdint.h>
```

需要初始化的内容有：

* 流的信息，每个流各一个

  ```c++
  struct rte_meter_srtcm app_flow[APP_FLOWS_MAX];
  ```

* 流的初始时间，每个流各一个：

  ```c++
  uint64_t cpu_time_stamp_reference[APP_FLOWS_MAX];
  ```

然后基本上就是从qos\_meter移植代码了，函数对应关系如下：

| qos\_meter                      | qos                  |
| ------------------------------- | -------------------- |
| app\_configure\_flow\_table\(\) | qos\_meter\_init\(\) |
| app\_pkt\_handle\(\)            | qos\_meter\_run\(\)  |

* **关于Point 1**：看了官方文档中函数rte\_meter\_srtcm\_color\_blind\_check的定义注意到time指的是以cpu cycles为单位的当前cpu time stamp，传进来的time是ns为单位，需要做ns到cpu cycles的转换

  ```c++
  uint64_t tsc_frequency = rte_get_tsc_hz();
  uint64_t cpu_time_stamp_offset = time * tsc_frequency / 1000000000;
  ```

* **关于Point 2**：从函数定义和qos\_meter里的实现来看逻辑上这个time不应该作为参数传入，就应该去取当前的cpu time stamp，但实验中却是从main里传了一个从0开始每次循环增加1毫秒的变量进来，然后又提示说不应该从0开始计时，所以猜测应该是想模拟出两批调用间隔1毫秒的场景，传进来的time只是偏移量，于是在初始化的时候为每个流单独记录了初始化时的cpu time stamp作为基本量。

  ```c++
  uint64_t cpu_time_stamp_reference[APP_FLOWS_MAX];
  cpu_time_stamp_reference[i] = rte_rdtsc();
  time = cpu_time_stamp_reference[i] + cpu_time_stamp_offset;
  ```

另外原代码中兼容不同模式的代码在移植过程中改写为仅针对本实验的代码，暂时保留了原代码中的流参数，在main中增加了单个流的三色统计功能便于调试，详见代码。

### Task 1B  Implement a dropper

需要初始化的内容有：

* 运行时数据，每个流的每种颜色各一个

  ```c++
  struct rte_red app_red[APP_FLOWS_MAX][e_RTE_METER_COLORS];
  ```

* 配置信息，每个流的每种颜色各一个

  ```c++
  struct rte_red_config app_red_config[APP_FLOWS_MAX][e_RTE_METER_COLORS];
  ```

* 队列大小，每个流各一个

  ```c++
  unsigned queue_size[APP_FLOWS_MAX];
  ```

最开始没注意随意取值结果初始化每次都返回\-2后来发现原来数字不能乱填。

基本就是给每一个流的每一种颜色初始化数据，然后初始化它们的队列大小为0。关于超时清空队列，通过输出发现q\_time一开始是0，而time本身就是ns计算的，因此此处没有转换和加参考量，直接用time和q\_time进行了比较，但是传给rte\_red\_enqueue的time依旧是加了参考量的，详见代码。

### Task 2 Deduce parameters

首先要尽可能正确地认识那些参数的含义：

* srTCM
  * cir：往桶里投放令牌的速率，注意一个令牌对应的是一个字节
  * cbs：突发令牌桶容量，桶中的令牌数表示当前可标绿色通过的包最大为多少字节
  * ebs：超额突发令牌桶容量，桶中的令牌数表示当前可标黄色通过的包最大为多少字节

* WRED
  * wq\_log2：计算平均队列长度时对输入流量变化的反应程度
  * min\_th：最小队列长度，队列长度在该值内不会发生丢包
  * max\_th：最大队列长度，队列长度超过该值一定丢包
  * maxp\_inv：队列长度介于最小与最大之间时的丢包概率

由于flow 0要求的带宽与总带宽一致，因此flow 0发送的所有的包都要顺利通过，于是针对flow 0的参数选择的目标定为在meter中所有包被标记为绿色，在dropper中所有的包被判定通过。限制包不被标记为绿色的因素有：

* cir不够大导致即使桶容量大于包大小，但是填充速度过慢，令牌数不够最终标黄或标红
* cbs不够大导致即使填充够快，但是桶容量小于包大小最终标黄或标红

解决限制1，cir应不小于流单位时间发送的字节量，通过阅读main\.c得到信息：时间间隔1000000ns内，平均发1000个包，平均每个包640字节，所以流单位时间发送字节量平均为：

```
cir = packets total / number of flows x packet size in Byte x scale to 1s
    = 1000 / 4 x 640 x (10^9 / 10^6)
    = 160000000
```

解决限制2，cbs应不小于瞬间涌入包的大小总和最大值，即某一时刻发送的包全来自flow 0：

```
cbs = packets total x packet size
    = 1000 x 640
    = 640000
```

然后ebs随便取一个大数字即可，理论上flow 0的ebs桶第一次充满就不会再变了。测试了一下发现确实全部标记为绿包，并且dropper中随便选的flow 0绿包参数已经能让绿包全部通过了因此没有再动，flow 0不会有黄包红包所以参数选了和绿包一样的，看上去整整齐齐一家人。

接下来为了控制4个flow分到的带宽比为8:4:2:1，让flow 1到3分别只有1/2、1/4、1/8的包标记为绿色，然后dropper控制flow 1到3各颜色丢包数量，可以用上述的两种限制来减少flow 1到3被标绿的包数。

对于flow 1，为了每一时刻都只标记一半的包为绿色，考虑设置cbs为同一时刻包大小总和的一半，设置cir为保证下一时刻前cbs能重新充满的值：

```
cbs = 640000 / 4 / 2 = 80000
cir = 80000 x (10 ^ 9 / 10 ^ 6) = 80000000
```

同理可以推出flow 2和flow 3的cir与cbs分别为40000与40000000和20000与20000000。然后调dropper让flow 1到3的黄绿红包各丢一部分，绿包少丢，黄包丢大部分，红包几乎不留。main\.c中增加统计了每个流每种颜色通过了多少字节，最后输出如下：

```
fid: 0, green: 1643910 of 1643910, yellow: 0 of 0 red: 0 of 0
fid: 1, green: 798209 of 799701, yellow: 12728 of 164161 red: 27682 of 646780
fid: 2, green: 395213 of 399244, yellow: 5775 of 81864 red: 30087 of 1095207
fid: 3, green: 189757 of 199282, yellow: 1284 of 41198 red: 48902 of 1299588
fid: 0, send: 1643910, pass: 1643910
fid: 1, send: 1610642, pass: 838619
fid: 2, send: 1576315, pass: 431075
fid: 3, send: 1540068, pass: 239943
```

发现通过率接近8:4:2:1，且各种颜色的包都有通过，也符合绿包通过率大于黄包大于红包的设计。

## Lab 4 MapReduce

### Part I: Map/Reduce input and output

* doMap
  * 使用inputStream读取inFile的内容
  * 转换为字符串之后调用mapF中的map函数获得inFile中的键值对
  * doMap要做的事情可以概括为把1个文件中的nTotal个键值对拆分到nReduce个中间文件中，平均每个文件有nTotal/nReduce个键值对
  * 考虑到键值对和对应的中间文件的映射关系里有hashcode操作，无法在对nReduce个文件的迭代中分别计算自己的键值对，因此在写文件前先开好了nReduce个键值对组
  * 开nReduce个键值对组不能只靠初始化的时候给一个size，那个指定的其实是capacity，真实size仍为0会导致下标越界，手动循环了nReduce次完成键值对组的初始化
  * 遍历nTotal个键值对完成分配工作
  * 再迭代nReduce次把分配好的键值对组以JSON格式写进中间文件，doMap工作完毕
* doReduce
  * 使用hashmap自动排序
  * 读取中间文件并将JSON格式的数据转回键值对组，此时仍是多对多
  * 遍历键值对组整理进hashmap，转为一对多
  * 遍历hashmap对每个key对应的多个value执行reduceF中的reduce函数得到一个输出value
  * 生成最终的键值对组，写入outFile，doReduce工作完毕

### Part II: Single-worker word count

* mapFunc
  * 使用Pattern和Matcher取下文章中的word作为key，value设为1，表示又出现了1次
  * 返回生成的键值对组
* reduceFunc
  * 对单个word的出现次数进行累加
  * 返回总和

### Part III: Distributing MapReduce tasks

* schedule
  * 初始化CountDownLatch，计数器需计数nTasks次
  * 初始化nTasks个线程
    * 在循环体内读registerChan
    * registerChan内部实现采用了BlockingQueue，不需要自己折腾锁什么的
    * 当有workers起得比schedule晚时registerChan会读着读着就空了，再去读就会抛异常，异常不需要做任何处理接着循环回去读即可
    * 读到RPC地址后就照着文档提示的流程准备参数，调RPC做任务等返回
    * 由于task比worker多，而read操作用到了take，worker会从registerChan中被移除，为了复用数量不够的worker需要把它write回去
    * 此时跳出循环，工作完成，执行countDown后返回
  * 启动nTasks个线程后await等待它们完成工作，最后结束调度任务
  * 疑问：目前没有用到interrupt，但是暂时没有想到用例，或许和后面的部分有关？

### Part IV: Handling worker failures

* schedule
  * 文档说如果worker出错call最终会返回false，然而并没有
  * 裸跑测试发现当worker出错时会抛出SofaRpcException，所以给call补上try catch
  * 捕获到SofaRpcException表示当前worker出错了，应直接continue到下一次循环
    * 因为跳过了write，所以故障worker不会再回到registerChan中
    * 这样保证了故障worker不会再被分配做别的task，直到它重启恢复后再把自己加回registerChan
    * 于是保证了在之后的循环会获得一个别的worker来做当前的task
* 疑问：至今没有用到interrupt，或许还有别的用到它的实现方式？

### Part V: Inverted index generation

* mapFunc
  * 和WordCount几乎无差，value修改为所属文件名
* reduceFunc
  * 收到的key和values分别是word和所有出现过它的文件
  * 由于个word可以在一个file中多次出现，因此values中可能存在大量重复的file
  * 使用Set来自动去除重复的file，把values全部add进Set里得到不重复的file集合
  * 集合的大小即为出现该word的总文件数
  * 最后将Set里的file逐个添加到retStr末即可
  * 注意：word本身不需要被加到retStr中，reduce也确实没必要做这个没意义的事情
