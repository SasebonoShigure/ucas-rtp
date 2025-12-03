#!/usr/bin/python

from mininet.net import Mininet
from mininet.node import OVSBridge
from mininet.cli import CLI
from mininet.link import TCLink

def tcp_analysis_topo():
    net = Mininet(switch=OVSBridge, controller=None, link=TCLink)

    h1 = net.addHost('h1', ip='10.0.0.1/24')
    h2 = net.addHost('h2', ip='10.0.0.2/24')
    s1 = net.addSwitch('s1')

    # 带宽10Mbps，延迟10ms，队列大小100个数据包
    link_opts = {'bw': 10, 'delay': '10ms', 'max_queue_size': 100}
    net.addLink(h1, s1)
    net.addLink(h2, s1, **link_opts)

    net.start()

    CLI(net)

    net.stop()

if __name__ == '__main__':
    tcp_analysis_topo()
