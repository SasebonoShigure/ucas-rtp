#!/usr/bin/python

from mininet.net import Mininet
from mininet.node import OVSBridge
from mininet.cli import CLI

def http_topology():
    net = Mininet(switch=OVSBridge, controller=None)

    h1 = net.addHost('h1', ip='10.0.0.1/24')
    h2 = net.addHost('h2', ip='10.0.0.2/24')
    s1 = net.addSwitch('s1')

    net.addLink(h1, s1)
    net.addLink(h2, s1)

    net.start()

    CLI(net)

    net.stop()

if __name__ == '__main__':
    http_topology()
