#!/usr/bin/python

from mininet.net import Mininet
from mininet.node import OVSBridge
from mininet.cli import CLI
from mininet.link import TCLink

def udp_topology():
    net = Mininet(switch=OVSBridge, controller=None, link=TCLink)

    h1 = net.addHost('h1', ip='10.0.0.1/24')
    h2 = net.addHost('h2', ip='10.0.0.2/24')
    s1 = net.addSwitch('s1')

    link_opts = {'bw': 10, 'delay': '20ms', 'loss': 5} # 10Mbps, 20ms延迟, 5%丢包
    net.addLink(h1, s1)
    net.addLink(h2, s1, **link_opts)

    net.start()
    
    CLI(net)

    net.stop()

if __name__ == '__main__':
    udp_topology()
