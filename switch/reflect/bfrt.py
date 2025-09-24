def Namespace():
    name = bfrt.p4_programs_list[0]
    p4 = eval(f"bfrt.{name}")
    # 10.0.0.1~9 10.0.0.20~21
    inc_switch_mac = 0x020000000000
    inc_switch_ip = 0xc0a801fe # 192.168.1.254
    pcie_port = 192
    pcie_mac = 0x000200000300
    port = [180, 164, 148, 132, pcie_port]
    MAC = [0x1070fd190095, 0x1070fd2fd851, 0x1070fd2fe441, 0x1070fd2fd421, pcie_mac]
    # In doc of TNA, 192 is CPU PCIE port and 64~67 is CPU Ethernet ports for 2-pipe TF1
    # 0x000200000300 is the MAC address of bf_pci0, it may not always be this value
    # And, I found that copy_to_cpu not need to be set if we use port 192, so copy_to_cpu is useless ?

    for worker in range(len(port)):
        p4.pipe.Ingress.l2_forward_table.add_with_l2_forward(MAC[worker], port[worker])

    p4.pipe.Ingress.l2_forward_table.add_with_reflect(inc_switch_mac)

    p4.pipe.Egress.dcqcn.wred.add(0, 0, 125, 2500, 0.01)
    # DCQCN
    # 0 ~ 10KB, 0 
    # 10 ~ 200KB, 0 ~ 0.01
    # 200KB ~, 1

    node_list = []

    for index, p in enumerate(port):
        if p == pcie_port: # this port do not need to be added, and add it will cause error
            continue
        bfrt.port.port.add(p, 'BF_SPEED_100G', 'BF_FEC_TYP_RS', 4, True, 'PM_AN_FORCE_DISABLE')
        node_id = index + 1
        node_list.append(node_id)
        bfrt.pre.node.add(node_id, 0, None, [p]) # node_id, rid, lag_id, dev_port

    mgid = 1 
    bfrt.pre.mgid.add(mgid, node_list, [False] * len(node_list), [0] * len(node_list)) # mgid, node_id, L1_XID_VALID, L1_XID

    p4.pipe.Ingress.l2_forward_table.add_with_l2_multicast(0xffffffffffff, mgid)

Namespace()