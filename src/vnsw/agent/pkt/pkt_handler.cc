/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "base/os.h"
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip6.h>
#include <netinet/icmp6.h>

#include "cmn/agent_cmn.h"
#include "net/address_util.h"
#include "oper/interface_common.h"
#include "oper/nexthop.h"
#include "oper/route_common.h"
#include "oper/vrf.h"
#include "oper/tunnel_nh.h"
#include "pkt/control_interface.h"
#include "pkt/pkt_handler.h"
#include "pkt/proto.h"
#include "pkt/flow_table.h"
#include "pkt/pkt_types.h"
#include "pkt/pkt_init.h"
#include "cmn/agent_stats.h"
#include "pkt/packet_buffer.h"

#include "vr_types.h"
#include "vr_defs.h"
#include "vr_mpls.h"

#define PKT_TRACE(obj, arg)                                              \
do {                                                                     \
    std::ostringstream _str;                                             \
    _str << arg;                                                         \
    Pkt##obj::TraceMsg(PacketTraceBuf, __FILE__, __LINE__, _str.str());  \
} while (false)                                                          \

const std::size_t PktTrace::kPktMaxTraceSize;

////////////////////////////////////////////////////////////////////////////////

PktHandler::PktHandler(Agent *agent, PktModule *pkt_module) :
    stats_(), agent_(agent), pkt_module_(pkt_module) {
    for (int i = 0; i < MAX_MODULES; ++i) {
        if (i == PktHandler::DHCP || i == PktHandler::DHCPV6 ||
            i == PktHandler::DNS)
            pkt_trace_.at(i).set_pkt_trace_size(512);
        else
            pkt_trace_.at(i).set_pkt_trace_size(128);
    }
}

PktHandler::~PktHandler() {
}

void PktHandler::Register(PktModuleName type, Proto *proto) {
    proto_list_.at(type) = proto;
}

uint32_t PktHandler::EncapHeaderLen() const {
    return agent_->pkt()->control_interface()->EncapsulationLength();
}

// Send packet to tap interface
void PktHandler::Send(const AgentHdr &hdr, const PacketBufferPtr &buff) {
    stats_.PktSent(PktHandler::PktModuleName(buff->module()));
    pkt_trace_.at(buff->module()).AddPktTrace(PktTrace::Out, buff->data_len(),
                                              buff->data(), &hdr);
    if (agent_->pkt()->control_interface()->Send(hdr, buff) <= 0) {
        PKT_TRACE(Err, "Error sending packet");
    }
    return;
}

// Process the packet received from tap interface
PktHandler::PktModuleName PktHandler::ParsePacket(const AgentHdr &hdr,
                                                  PktInfo *pkt_info,
                                                  uint8_t *pkt) {
    PktType::Type pkt_type = PktType::INVALID;
    Interface *intf = NULL;

    pkt_info->agent_hdr = hdr;
    agent_->stats()->incr_pkt_exceptions();
    if (!IsValidInterface(hdr.ifindex, &intf)) {
        return INVALID;
    }

    // Parse packet before computing forwarding mode. Packet is parsed
    // indepndent of packet forwarding mode
    if (ParseUserPkt(pkt_info, intf, pkt_type, pkt) < 0) {
        return INVALID;
    }

    if (pkt_info->agent_hdr.cmd == AgentHdr::TRAP_TOR_CONTROL_PKT) {
        // In case of a control packet from a TOR served by us, the ifindex
        // is modified to index of the VM interface; validate this interface.
        if (!IsValidInterface(pkt_info->agent_hdr.ifindex, &intf)) {
            return INVALID;
        }
    }

    // Compute L2/L3 forwarding mode for packet
    ComputeForwardingMode(pkt_info);

    if (intf->type() == Interface::VM_INTERFACE) {
        VmInterface *vm_itf = static_cast<VmInterface *>(intf);
        if (pkt_info->l3_forwarding && vm_itf->layer3_forwarding() == false) {
            PKT_TRACE(Err, "layer3 not enabled for interface index <" <<
                      pkt_info->agent_hdr.ifindex << ">");
            return INVALID;
        }

        if (pkt_info->l3_forwarding == false &&
            vm_itf->bridging() == false) {
            PKT_TRACE(Err, "bridging not enabled for interface "
                      "index <" << pkt_info->agent_hdr.ifindex << ">");
            return INVALID;
        }
    }

    pkt_info->vrf = pkt_info->agent_hdr.vrf;

    // Handle ARP packet
    if (pkt_type == PktType::ARP) {
        return ARP;
    }

    // Packets needing flow
    if (IsFlowPacket(pkt_info)) {
        if ((pkt_info->ip && pkt_info->family == Address::INET) ||
            (pkt_info->ip6 && pkt_info->family == Address::INET6)) {
            return FLOW;
        } else {
            PKT_TRACE(Err, "Flow trap for non-IP packet for interface "
                      "index <" << hdr.ifindex << ">");
            return INVALID;
        }
    }

    // Look for DHCP and DNS packets if corresponding service is enabled
    // Service processing over-rides ACL/Flow
    if (intf->dhcp_enabled() && (pkt_type == PktType::UDP)) {
        if (pkt_info->dport == DHCP_SERVER_PORT ||
            pkt_info->sport == DHCP_CLIENT_PORT) {
            return DHCP;
        }
        if (pkt_info->dport == DHCPV6_SERVER_PORT ||
            pkt_info->sport == DHCPV6_CLIENT_PORT) {
            return DHCPV6;
        }
    } 
    
    if (intf->dns_enabled() && (pkt_type == PktType::UDP)) {
        if (pkt_info->dport == DNS_SERVER_PORT) {
            return DNS;
        }
    }

    // Look for IP packets that need ARP resolution
    if (pkt_info->ip && hdr.cmd == AgentHdr::TRAP_RESOLVE) {
        return ARP;
    }

    // send time exceeded ICMP messages to diag module
    if (IsDiagPacket(pkt_info)) {
        return DIAG;
    }

    if (pkt_type == PktType::ICMP && IsGwPacket(intf, pkt_info->ip_daddr)) {
        return ICMP;
    }

    if (pkt_type == PktType::ICMPV6) {
        return ICMPV6;
    }

    if (pkt_info->ip && hdr.cmd == AgentHdr::TRAP_HANDLE_DF) {
        return ICMP_ERROR;
    }

    if (hdr.cmd == AgentHdr::TRAP_DIAG && pkt_info->ip) {
        return DIAG;
    }

    return INVALID;
}

void PktHandler::HandleRcvPkt(const AgentHdr &hdr, const PacketBufferPtr &buff){
    boost::shared_ptr<PktInfo> pkt_info(new PktInfo(buff));
    PktModuleName mod = INVALID;
    mod = ParsePacket(hdr, pkt_info.get(), pkt_info->packet_buffer()->data());
    pkt_info->packet_buffer()->set_module(mod);
    pkt_info->module = mod;
    stats_.PktRcvd(mod);
    if (mod == INVALID) {
        AddPktTrace(mod, PktTrace::In, pkt_info.get());
        agent_->stats()->incr_pkt_dropped();
        return;
    }

    Enqueue(mod, pkt_info);
    return;
}

// Compute L2/L3 forwarding mode for pacekt.
// Forwarding mode is L3 if,
// - Packet uses L3 label
// - Packet uses L2 Label and DMAC hits a route with L2-Receive NH
// Else forwarding mode is L2
void PktHandler::ComputeForwardingMode(PktInfo *pkt_info) const {
    if (pkt_info->l3_label) {
        pkt_info->l3_forwarding = true;
        return;
    }

    pkt_info->l3_forwarding = false;
    VrfTable *table = static_cast<VrfTable *>(agent_->vrf_table());
    VrfEntry *vrf = table->FindVrfFromId(pkt_info->agent_hdr.vrf);
    if (vrf == NULL) {
        return;
    }
    BridgeAgentRouteTable *l2_table = static_cast<BridgeAgentRouteTable *>
        (vrf->GetBridgeRouteTable());
    AgentRoute *rt = static_cast<AgentRoute *>
        (l2_table->FindRoute(pkt_info->dmac));
    if (rt == NULL) {
        return;
    }

    const NextHop *nh = rt->GetActiveNextHop();
    if (nh == NULL) {
        return;
    }

    if (nh->GetType() == NextHop::L2_RECEIVE) {
        pkt_info->l3_forwarding = true;
    }
    return;
}

void PktHandler::SetOuterIp(PktInfo *pkt_info, uint8_t *pkt) {
    if (pkt_info->ether_type != ETHERTYPE_IP) {
        return;
    }
    struct ip *ip_hdr = (struct ip *)pkt;
    pkt_info->tunnel.ip_saddr = ntohl(ip_hdr->ip_src.s_addr);
    pkt_info->tunnel.ip_daddr = ntohl(ip_hdr->ip_dst.s_addr);
}

static bool InterestedIPv6Protocol(uint8_t proto) {
    if (proto == IPPROTO_UDP || proto == IPPROTO_TCP ||
        proto == IPPROTO_ICMPV6) {
        return true;
    }

    return false;
}

int PktHandler::ParseEthernetHeader(PktInfo *pkt_info, uint8_t *pkt) {
    int len = 0;
    pkt_info->eth = (struct ether_header *) (pkt + len);
    pkt_info->smac = MacAddress(pkt_info->eth->ether_shost);
    pkt_info->dmac = MacAddress(pkt_info->eth->ether_dhost);
    pkt_info->ether_type = ntohs(pkt_info->eth->ether_type);
    len += sizeof(struct ether_header);

    if (pkt_info->ether_type == ETHERTYPE_VLAN) {
       pkt_info->ether_type = ntohs(*((uint16_t *)(pkt + len + 2)));
        len += VLAN_HDR_LEN;
    }

    return len;
}

int PktHandler::ParseIpPacket(PktInfo *pkt_info, PktType::Type &pkt_type,
                              uint8_t *pkt) {
    int len = 0;
    if (pkt_info->ether_type == ETHERTYPE_IP) {
        struct ip *ip = (struct ip *)(pkt + len);
        pkt_info->ip = ip;
        pkt_info->family = Address::INET;
        pkt_info->ip_saddr = IpAddress(Ip4Address(ntohl(ip->ip_src.s_addr)));
        pkt_info->ip_daddr = IpAddress(Ip4Address(ntohl(ip->ip_dst.s_addr)));
        pkt_info->ip_proto = ip->ip_p;
        len += (ip->ip_hl << 2);
    } else if (pkt_info->ether_type == ETHERTYPE_IPV6) {
        pkt_info->family = Address::INET6;
        ip6_hdr *ip = (ip6_hdr *)(pkt + len);
        pkt_info->ip6 = ip;
        pkt_info->ip = NULL;
        Ip6Address::bytes_type addr;

        for (int i = 0; i < 16; i++) {
            addr[i] = ip->ip6_src.s6_addr[i];
        }
        pkt_info->ip_saddr = IpAddress(Ip6Address(addr));

        for (int i = 0; i < 16; i++) {
            addr[i] = ip->ip6_dst.s6_addr[i];
        }
        pkt_info->ip_daddr = IpAddress(Ip6Address(addr));

        // Look for known transport headers. Fallback to the last header if
        // no known header is found
        uint8_t proto = ip->ip6_ctlun.ip6_un1.ip6_un1_nxt;
        len += sizeof(ip6_hdr);
        while (InterestedIPv6Protocol(proto) == false) {
            struct ip6_ext *ext = (ip6_ext *)(pkt + len);
            proto = ext->ip6e_nxt;
            len += (ext->ip6e_len * 8);
            if (ext->ip6e_len == 0) {
                proto = 0;
                break;
            }
            if (len >= pkt_info->len) {
                proto = 0;
                break;
            }
        }
        pkt_info->ip_proto = proto;
    } else {
        assert(0);
    }

    switch (pkt_info->ip_proto) {
    case IPPROTO_UDP : {
        pkt_info->transp.udp = (udphdr *) (pkt + len);
        len += sizeof(udphdr);
        pkt_info->data = (pkt + len);

        pkt_info->dport = ntohs(pkt_info->transp.udp->uh_dport);
        pkt_info->sport = ntohs(pkt_info->transp.udp->uh_sport);
        pkt_type = PktType::UDP;
        break;
    }

    case IPPROTO_TCP : {
        pkt_info->transp.tcp = (tcphdr *) (pkt + len);
        len += sizeof(tcphdr);
        pkt_info->data = (pkt + len);

        pkt_info->dport = ntohs(pkt_info->transp.tcp->th_dport);
        pkt_info->sport = ntohs(pkt_info->transp.tcp->th_sport);
        pkt_info->tcp_ack = pkt_info->transp.tcp->th_flags & TH_ACK;
        pkt_type = PktType::TCP;
        break;
    }

    case IPPROTO_SCTP : {
        pkt_info->transp.sctp = (sctphdr *) (pkt + len);
        len += sizeof(sctphdr);
        pkt_info->data = (pkt + len);
        pkt_info->dport = ntohs(pkt_info->transp.sctp->th_dport);
        pkt_info->sport = ntohs(pkt_info->transp.sctp->th_sport);
        pkt_type = PktType::SCTP;
        break;
    }

    case IPPROTO_ICMP: {
        pkt_info->transp.icmp = (struct icmp *) (pkt + len);
        pkt_type = PktType::ICMP;

        struct icmp *icmp = (struct icmp *)(pkt + len);

        pkt_info->dport = htons(icmp->icmp_type);
        if (icmp->icmp_type == ICMP_ECHO || icmp->icmp_type == ICMP_ECHOREPLY) {
            pkt_info->dport = ICMP_ECHOREPLY;
            pkt_info->sport = htons(icmp->icmp_id);
        } else if (IsFlowPacket(pkt_info) &&
                   icmp->icmp_type == ICMP_DEST_UNREACH) {
            //Agent has to look at inner payload
            //and recalculate the parameter
            //Handle this only for packets requiring flow miss
            ParseIpPacket(pkt_info, pkt_type, pkt + len + sizeof(icmp));
            //Swap the key parameter, which would be used as key
            IpAddress src_ip = pkt_info->ip_saddr;
            pkt_info->ip_saddr = pkt_info->ip_daddr;
            pkt_info->ip_daddr = src_ip;
            if (pkt_info->ip_proto != IPPROTO_ICMP) {
                uint16_t port = pkt_info->sport;
                pkt_info->sport = pkt_info->dport;
                pkt_info->dport = port;
            }
        } else {
            pkt_info->sport = 0;
        }
        break;
    }

    case IPPROTO_ICMPV6: {
        pkt_type = PktType::ICMPV6;
        icmp6_hdr *icmp = (icmp6_hdr *)(pkt + len);
        pkt_info->transp.icmp6 = icmp;

        pkt_info->dport = htons(icmp->icmp6_type);
        if (icmp->icmp6_type == ICMP6_ECHO_REQUEST ||
            icmp->icmp6_type == ICMP6_ECHO_REPLY) {
            pkt_info->dport = ICMP6_ECHO_REPLY;
            pkt_info->sport = htons(icmp->icmp6_id);
        } else {
            pkt_info->sport = 0;
        }
        break;
    }

    default: {
        pkt_type = PktType::IP;
        pkt_info->dport = 0;
        pkt_info->sport = 0;
        break;
    }
    }

    return len;
}

int PktHandler::ParseMplsHdr(PktInfo *pkt_info, uint8_t *pkt) {
    MplsHdr *hdr = (MplsHdr *)(pkt);

    // MPLS Header validation. Check for,
    // - There is single label
    uint32_t mpls_host = ntohl(hdr->hdr);
    pkt_info->tunnel.label = (mpls_host & 0xFFFFF000) >> 12;

    if ((mpls_host & 0x100) == 0) {
        pkt_info->tunnel.label = MplsTable::kInvalidLabel;
        PKT_TRACE(Err, "Unexpected MPLS Label Stack. Ignoring");
        return -1;
    }

    uint32_t label = pkt_info->tunnel.label;
    const MplsLabel *mpls = agent_->mpls_table()->FindMplsLabel(label);
    if (mpls == NULL) {
        PKT_TRACE(Err, "Invalid MPLS Label <" << label << ">. Ignoring");
        pkt_info->tunnel.label = MplsTable::kInvalidLabel;
        return -1;
    }

    pkt_info->l3_label = true;
    const InterfaceNH *nh = dynamic_cast<const InterfaceNH *>(mpls->nexthop());
    if (nh && nh->IsBridge()) {
        pkt_info->l3_label = false;
    }

    return sizeof(MplsHdr);
}

// Parse MPLSoGRE header
int PktHandler::ParseMPLSoGRE(PktInfo *pkt_info, uint8_t *pkt) {
    GreHdr *gre = (GreHdr *)(pkt);
    if (gre->protocol != ntohs(VR_GRE_PROTO_MPLS)) {
        PKT_TRACE(Err, "Non-MPLS protocol <" << ntohs(gre->protocol) <<
                  "> in GRE header");
        return -1;
    }

    int len = sizeof(GreHdr);

    int tmp;
    tmp = ParseMplsHdr(pkt_info, (pkt + len));
    if (tmp < 0) {
        return tmp;
    }
    len += tmp;

    if (pkt_info->l3_label == false) {
        tmp = ParseEthernetHeader(pkt_info, (pkt + len));
        if (tmp < 0)
            return tmp;
        len += tmp;
    }

    pkt_info->tunnel.type.SetType(TunnelType::MPLS_GRE);
    return (len);
}

int PktHandler::ParseMPLSoUDP(PktInfo *pkt_info, uint8_t *pkt) {
    int len = ParseMplsHdr(pkt_info, pkt);
    if (len < 0) {
        return len;
    }

    if (pkt_info->l3_label == false) {
        len += ParseEthernetHeader(pkt_info, (pkt + len));
    }

    pkt_info->tunnel.type.SetType(TunnelType::MPLS_UDP);
    return len;
}

int PktHandler::ParseVxlan(PktInfo *pkt_info, uint8_t *pkt) {
    VxlanHdr *vxlan = (VxlanHdr *)(pkt);
    pkt_info->tunnel.vxlan_id = htonl(vxlan->vxlan_id) >> 8;

    int len = sizeof(vxlan);
    len += ParseEthernetHeader(pkt_info, (pkt + len));

    pkt_info->tunnel.type.SetType(TunnelType::VXLAN);
    pkt_info->l3_label = false;
    return len;
}

int PktHandler::ParseUDPTunnels(PktInfo *pkt_info, uint8_t *pkt) {
    int len = 0;
    if (pkt_info->dport == VXLAN_UDP_DEST_PORT)
        len = ParseVxlan(pkt_info, (pkt + len));
    else if (pkt_info->dport == MPLS_OVER_UDP_DEST_PORT)
        len = ParseMPLSoUDP(pkt_info, (pkt + len));

    return len;
}

int PktHandler::ParseUserPkt(PktInfo *pkt_info, Interface *intf,
                             PktType::Type &pkt_type, uint8_t *pkt) {
    int len = 0;

    // get to the actual packet header
    len += ParseEthernetHeader(pkt_info, (pkt + len));

    // Parse payload
    if (pkt_info->ether_type == ETHERTYPE_ARP) {
        pkt_info->arp = (ether_arp *) (pkt + len);
        pkt_type = PktType::ARP;
        return len;
    }

    // Identify NON-IP Packets
    if (pkt_info->ether_type != ETHERTYPE_IP &&
        pkt_info->ether_type != ETHERTYPE_IPV6) {
        pkt_info->data = (pkt + len);
        pkt_type = PktType::NON_IP;
        return len;
    }

    // Copy IP fields from outer header assuming tunnel is present. If tunnel
    // is not present, the values here will be ignored
    SetOuterIp(pkt_info, (pkt + len));

    // IP Packets
    len += ParseIpPacket(pkt_info, pkt_type, (pkt + len));

    // If packet is an IP fragment and not flow trap, ignore it
    if (IgnoreFragmentedPacket(pkt_info)) {
        agent_->stats()->incr_pkt_fragments_dropped();
        return -1;
    }

    // If it is a packet from TOR that we serve, dont parse any further
    if (IsManagedTORPacket(intf, pkt_info, pkt_type, (pkt + len))) {
        return len;
    }

    // If tunneling is not enabled on interface or if it is a DHCP packet,
    // dont parse any further
    if (intf->IsTunnelEnabled() == false || IsDHCPPacket(pkt_info) ||
        IsDiagPacket(pkt_info)) {
        return len;
    }

    // Tunneling is enabled on interface. Reset pkt_type and decode payload
    pkt_type = PktType::INVALID;

    // Decap only IP-DA is ours
    if (pkt_info->family != Address::INET ||
        pkt_info->ip_daddr.to_v4() != agent_->router_id()) {
        PKT_TRACE(Err, "Tunnel packet not destined to me. Ignoring");
        return -1;
    }

    int tunnel_len = 0;
    // Look for supported headers
    switch (pkt_info->ip_proto) {
    case IPPROTO_GRE :
        // Parse MPLSoGRE tunnel
        tunnel_len = ParseMPLSoGRE(pkt_info, (pkt + len));
        break;

    case IPPROTO_UDP:
        // Parse MPLSoUDP tunnel
        tunnel_len = ParseUDPTunnels(pkt_info, (pkt + len));
        break;

    default:
        break;
    }

    if (tunnel_len < 0) {
       pkt_type = PktType::INVALID;
       return tunnel_len;
    }
    len += tunnel_len;

    // Find IPv4/IPv6 Packet based on first nibble in payload
    if (((pkt + len)[0] & 0x60) == 0x60) {
        pkt_info->ether_type = ETHERTYPE_IPV6;
    } else {
        pkt_info->ether_type = ETHERTYPE_IP;
    }

    len += ParseIpPacket(pkt_info, pkt_type, (pkt + len));
    return len;
}

// Enqueue an inter-task message to the specified module
void PktHandler::SendMessage(PktModuleName mod, InterTaskMsg *msg) {
    if (mod < MAX_MODULES) {
        boost::shared_ptr<PktInfo> pkt_info(new PktInfo(mod, msg));
        if (!(proto_list_.at(mod)->Enqueue(pkt_info))) {
            PKT_TRACE(Err, "Threshold exceeded while enqueuing IPC Message <" <<
                      mod << ">");
        }
    }
}

bool PktHandler::IgnoreFragmentedPacket(PktInfo *pkt_info) {
    if (!pkt_info->ip)
        return false;

    uint16_t offset = htons(pkt_info->ip->ip_off);
    if (((offset & IP_MF) || (offset & IP_OFFMASK)) &&
        !IsFlowPacket(pkt_info))
        return true;
    return false;
}

bool PktHandler::IsDHCPPacket(PktInfo *pkt_info) {
    if (pkt_info->dport == DHCP_SERVER_PORT || 
        pkt_info->sport == DHCP_CLIENT_PORT) {
        // we dont handle DHCPv6 coming from fabric
        return true;
    }
    return false;
}

bool PktHandler::IsToRDevice(uint32_t vrf_id, const IpAddress &ip) {
    if (agent()->tsn_enabled() == false)
        return false;

    if (ip.is_v4() == false)
        return false;
    Ip4Address ip4 = ip.to_v4();

    VrfEntry *vrf = agent()->vrf_table()->FindVrfFromId(vrf_id);
    if (vrf == NULL)
        return false;

    BridgeAgentRouteTable *table = static_cast<BridgeAgentRouteTable *>
        (vrf->GetBridgeRouteTable());
    if (table == NULL)
        return false;

    BridgeRouteEntry *rt = table->FindRoute(MacAddress::BroadcastMac());
    if (rt == NULL)
        return false;

    const CompositeNH *nh = dynamic_cast<const CompositeNH *>
        (rt->GetActiveNextHop());
    if (nh == NULL)
        return false;

    ComponentNHList::const_iterator it = nh->begin();
    while (it != nh->end()) {
        const CompositeNH *tor_nh = dynamic_cast<const CompositeNH *>
            ((*it)->nh());
        if (tor_nh == NULL) {
            it++;
            continue;
        }

        if (tor_nh->composite_nh_type() != Composite::TOR) {
            it++;
            continue;
        }

        ComponentNHList::const_iterator tor_it = tor_nh->begin();
        while (tor_it != tor_nh->end()) {
            const TunnelNH *tun_nh = dynamic_cast<const TunnelNH *>
                ((*tor_it)->nh());
            if (tun_nh == NULL) {
                tor_it++;
                continue;
            }
            if (*tun_nh->GetDip() == ip4) {
                return true;
            }
            tor_it++;
        }

        it++;
    }

    return false;
}

// We can receive DHCP / DNS packets on physical port from TOR ports managed
// by a TOR services node. Check if the source mac is the mac address of a
// VM interface available in the node.
bool PktHandler::IsManagedTORPacket(Interface *intf, PktInfo *pkt_info,
                                    PktType::Type &pkt_type, uint8_t *pkt) {
    if (intf->type() != Interface::PHYSICAL) {
        return false;
    }

    if (pkt_type != PktType::UDP || pkt_info->dport != VXLAN_UDP_DEST_PORT)
        return false;

    // Get VXLAN id and point to original L2 frame after the VXLAN header
    pkt += 8;

    // get to the actual packet header
    pkt += ParseEthernetHeader(pkt_info, pkt);

    ether_addr addr;
    memcpy(addr.ether_addr_octet, pkt_info->eth->ether_shost, ETH_ALEN);
    MacAddress address(addr);
    const VrfEntry *vrf = agent_->vrf_table()->
        FindVrfFromId(pkt_info->agent_hdr.vrf);
    if (vrf == NULL)
        return false;
   BridgeAgentRouteTable *bridge_table =
        dynamic_cast<BridgeAgentRouteTable *>(vrf->GetBridgeRouteTable());
    assert(bridge_table != NULL);
    const VmInterface *vm_intf = bridge_table->FindVmFromDhcpBinding(address);
    if (vm_intf == NULL) {
        return false;
    }

    if (IsToRDevice(vm_intf->vrf_id(), pkt_info->ip_saddr) == false) {
        return false;
    }

    // update agent_hdr to reflect the VM interface data
    // cmd_param is set to physical interface id
    pkt_info->agent_hdr.cmd = AgentHdr::TRAP_TOR_CONTROL_PKT;
    pkt_info->agent_hdr.cmd_param = pkt_info->agent_hdr.ifindex;
    pkt_info->agent_hdr.ifindex = vm_intf->id();

    // Parse payload
    if (pkt_info->ether_type == ETHERTYPE_ARP) {
        pkt_info->arp = (ether_arp *) pkt;
        pkt_type = PktType::ARP;
        return true;
    }

    // Identify NON-IP Packets
    if (pkt_info->ether_type != ETHERTYPE_IP &&
        pkt_info->ether_type != ETHERTYPE_IPV6) {
        pkt_info->data = pkt;
        pkt_type = PktType::NON_IP;
        return true;
    }

    // IP Packets
    ParseIpPacket(pkt_info, pkt_type, pkt);
    return true;
}

bool PktHandler::IsFlowPacket(PktInfo *pkt_info) {
    if (pkt_info->agent_hdr.cmd == AgentHdr::TRAP_FLOW_MISS ||
        pkt_info->agent_hdr.cmd == AgentHdr::TRAP_ECMP_RESOLVE ||
        pkt_info->agent_hdr.cmd == AgentHdr::TRAP_HOLD_ACTION) {
        return true;
    }
    return false;
}

bool PktHandler::IsDiagPacket(PktInfo *pkt_info) {
    if (pkt_info->agent_hdr.cmd == AgentHdr::TRAP_ZERO_TTL ||
        pkt_info->agent_hdr.cmd == AgentHdr::TRAP_ICMP_ERROR)
        return true;
    return false;
}

// Check if the packet is destined to the VM's default GW
bool PktHandler::IsGwPacket(const Interface *intf, const IpAddress &dst_ip) {
    if (!intf || intf->type() != Interface::VM_INTERFACE)
        return false;

    const VmInterface *vm_intf = static_cast<const VmInterface *>(intf);
    if (vm_intf->vmi_type() != VmInterface::GATEWAY) {
        //Gateway interface doesnt have IP address
        if (dst_ip.is_v6() && vm_intf->primary_ip6_addr().is_unspecified())
            return false;
        else if (dst_ip.is_v4() && vm_intf->primary_ip_addr().is_unspecified())
            return false;
    }
    const VnEntry *vn = vm_intf->vn();
    if (vn) {
        const std::vector<VnIpam> &ipam = vn->GetVnIpam();
        for (unsigned int i = 0; i < ipam.size(); ++i) {
            if (dst_ip.is_v4()) {
                if (!ipam[i].IsV4()) {
                    continue;
                }
                IpAddress src_ip = vm_intf->primary_ip_addr();
                if (vm_intf->vmi_type() == VmInterface::GATEWAY) {
                    src_ip = vm_intf->subnet();
                }

                if (ipam[i].default_gw == dst_ip ||
                        ipam[i].dns_server == dst_ip) {
                    return true;
                }
            } else {
                if (!ipam[i].IsV6()) {
                    continue;
                }
                if (ipam[i].default_gw == dst_ip ||
                        ipam[i].dns_server == dst_ip) {
                    return true;
                }
            }

        }
    }

    return false;
}

bool PktHandler::IsValidInterface(uint32_t ifindex, Interface **interface) {
    Interface *intf = agent_->interface_table()->FindInterface(ifindex);
    if (intf == NULL) {
        PKT_TRACE(Err, "Invalid interface index <" << ifindex << ">");
        agent_->stats()->incr_pkt_invalid_interface();
        return false;
    }

    *interface = intf;
    return true;
}

void PktHandler::PktTraceIterate(PktModuleName mod, PktTraceCallback cb) {
    if (cb) {
        PktTrace &pkt(pkt_trace_.at(mod));
        pkt.Iterate(cb);
    }
}

void PktHandler::PktStats::PktRcvd(PktModuleName mod) {
    if (mod < MAX_MODULES)
        received[mod]++;
}

void PktHandler::PktStats::PktSent(PktModuleName mod) {
    if (mod < MAX_MODULES)
        sent[mod]++;
}

void PktHandler::PktStats::PktQThresholdExceeded(PktModuleName mod) {
    if (mod < MAX_MODULES)
        q_threshold_exceeded[mod]++;
}

///////////////////////////////////////////////////////////////////////////////

PktInfo::PktInfo(const PacketBufferPtr &buff) :
    module(PktHandler::INVALID),
    pkt(buff->data()), len(buff->data_len()), max_pkt_len(buff->buffer_len()),
    data(), ipc(), family(Address::UNSPEC), type(PktType::INVALID), agent_hdr(),
    ether_type(-1), ip_saddr(), ip_daddr(), ip_proto(), sport(), dport(),
    tcp_ack(false), tunnel(), l3_forwarding(false), l3_label(false), eth(),
    arp(), ip(), ip6(), packet_buffer_(buff) {
    transp.tcp = 0;
}

PktInfo::PktInfo(Agent *agent, uint32_t buff_len, PktHandler::PktModuleName mod,
                 uint32_t mdata) :
    module(mod),
    len(), max_pkt_len(), data(), ipc(), family(Address::UNSPEC),
    type(PktType::INVALID), agent_hdr(), ether_type(-1), ip_saddr(), ip_daddr(),
    ip_proto(), sport(), dport(), tcp_ack(false), tunnel(),
    l3_forwarding(false), l3_label(false), eth(), arp(), ip(), ip6() {

    packet_buffer_ = agent->pkt()->packet_buffer_manager()->Allocate
        (module, buff_len, mdata);
    pkt = packet_buffer_->data();
    len = packet_buffer_->data_len();
    max_pkt_len = packet_buffer_->buffer_len();

    transp.tcp = 0;
}

PktInfo::PktInfo(PktHandler::PktModuleName mod, InterTaskMsg *msg) :
    module(mod),
    pkt(), len(), max_pkt_len(0), data(), ipc(msg), family(Address::UNSPEC),
    type(PktType::MESSAGE), agent_hdr(), ether_type(-1), ip_saddr(), ip_daddr(),
    ip_proto(), sport(), dport(), tcp_ack(false), tunnel(),
    l3_forwarding(false), l3_label(false), eth(), arp(), ip(), ip6(),
    packet_buffer_() {
    transp.tcp = 0;
}

PktInfo::~PktInfo() {
}

const AgentHdr &PktInfo::GetAgentHdr() const {
    return agent_hdr;
}

void PktInfo::reset_packet_buffer() {
    packet_buffer_.reset();
}

void PktInfo::AllocPacketBuffer(Agent *agent, uint32_t module, uint16_t len,
                                uint32_t mdata) {
    packet_buffer_ = agent->pkt()->packet_buffer_manager()->Allocate
        (module, len, mdata);
    pkt = packet_buffer_->data();
    len = packet_buffer_->data_len();
    max_pkt_len = packet_buffer_->buffer_len();
}

void PktInfo::set_len(uint32_t x) {
    packet_buffer_->set_len(x);
    len = x;
}

void PktInfo::UpdateHeaderPtr() {
    eth = (struct ether_header *)(pkt);
    ip = (struct ip *)(eth + 1);
    transp.tcp = (struct tcphdr *)(ip + 1);
}

std::size_t PktInfo::hash() const {
    std::size_t seed = 0;
    if (family == Address::INET) {
        boost::hash_combine(seed, ip_saddr.to_v4().to_ulong());
        boost::hash_combine(seed, ip_daddr.to_v4().to_ulong());
    } else if (family == Address::INET6) {
        uint32_t *words;

        words = (uint32_t *) (ip_saddr.to_v6().to_bytes().c_array());
        boost::hash_combine(seed, words[0]);
        boost::hash_combine(seed, words[1]);
        boost::hash_combine(seed, words[2]);
        boost::hash_combine(seed, words[3]);

        words = (uint32_t *) (ip_daddr.to_v6().to_bytes().c_array());
        boost::hash_combine(seed, words[0]);
        boost::hash_combine(seed, words[1]);
        boost::hash_combine(seed, words[2]);
        boost::hash_combine(seed, words[3]);
    } else {
        assert(0);
    }
    boost::hash_combine(seed, ip_proto);
    boost::hash_combine(seed, sport);
    boost::hash_combine(seed, dport);
    return seed;
}

void PktHandler::AddPktTrace(PktModuleName module, PktTrace::Direction dir,
                             const PktInfo *pkt) {
    pkt_trace_.at(module).AddPktTrace(PktTrace::In, pkt->len, pkt->pkt,
                                      &pkt->agent_hdr);
}

void PktHandler::Enqueue(PktModuleName module,
                         boost::shared_ptr<PktInfo> pkt_info) {
    if (!(proto_list_.at(module)->Enqueue(pkt_info))) {
        stats_.PktQThresholdExceeded(module);
    }
    return;
}

///////////////////////////////////////////////////////////////////////////////
void PktTrace::Pkt::Copy(Direction d, std::size_t l, uint8_t *msg,
                         std::size_t pkt_trace_size, const AgentHdr *hdr) {
    uint16_t hdr_len = sizeof(AgentHdr);
    dir = d;
    len = l + hdr_len;
    memcpy(pkt, hdr, hdr_len);
    memcpy(pkt + hdr_len, msg, std::min(l, (pkt_trace_size - hdr_len)));
}

void PktTrace::AddPktTrace(Direction dir, std::size_t len, uint8_t *msg,
                           const AgentHdr *hdr) {
    if (num_buffers_) {
        end_ = (end_ + 1) % num_buffers_;
        pkt_buffer_[end_].Copy(dir, len, msg, pkt_trace_size_, hdr);
        count_ = std::min((count_ + 1), (uint32_t) num_buffers_);
    }
}
