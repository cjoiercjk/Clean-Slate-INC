+ find out whether we can post kernel for any device in stream 0: yes

把bcast和reduce都当成allreduce可以省去所有ACK
这个可以当论文的novelty

+ pkt header fmt: 
    + group id
    + rank
    + addr
    + psn
    + 



+ forward situation 
    + old packet
        + full
            + bcast
        + not full
            + drop
    + expected packet
        + full 
            + bcast
        + not full
            + drop 
    + out ordered packet
        + drop or NAK

ACK address: increase 1 for every K packets (must increase for last packet)
ACK address只要约定好大家都一样就行

主要依赖NAK进行重传而非超时，先只考虑NAK
如果采用counter而非bitmap，那么生成NAK时，几乎不会所有接收方同时生成NAK
通常只有一个receiver生成NAK，并且NAK的包一定在交换机上聚合好了
那么，这个NAK只需要交给一个sender处理，而不需发给所有sender？


+ ACK forward situation 
    + old packet
        + full
            + bcast
        + not full
            + drop
    + expected packet
        + full 
            + bcast
        + not full
            + drop 
    + out ordered packet
        + drop or NAK

+ aggregate situation
    + old packet
        + full
            + bcast
        + not full
            + drop
    + expected packet
        + full 
            + bcast
        + not full
            + drop 
    + out ordered packet