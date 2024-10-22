#ifndef __ROUTESYNC__
#define __ROUTESYNC__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "netmsg.h"
#include "warmRestartHelper.h"
#include <string.h>
#include <bits/stdc++.h>
#include <linux/version.h>
#include <linux/seg6.h>

#include <netlink/route/route.h>

#if (LINUX_VERSION_CODE > KERNEL_VERSION(5,3,0))
#define HAVE_NEXTHOP_GROUP
#endif

// Add RTM_F_OFFLOAD define if it is not there.
// Debian buster does not provide one but it is neccessary for compilation.
#ifndef RTM_F_OFFLOAD
#define RTM_F_OFFLOAD 0x4000 /* route is offloaded */
#endif

using namespace std;

/* Parse the Raw netlink msg */
extern void netlink_parse_rtattr(struct rtattr **tb, int max, struct rtattr *rta,
                                                int len);
extern void netlink_parse_rtattr_nested(struct rtattr **tb, int max, const struct rtattr *rta);

namespace swss {

#ifdef HAVE_NEXTHOP_GROUP
struct NextHopGroup {
    uint32_t id;
    vector<pair<uint32_t,uint8_t>> group;
    string nexthop;
    string intf;
    bool installed;
    string vni_label;
    string vpn_sid;
    string segment;
    string seg_src;
    NextHopGroup(uint32_t id, const string& nexthop, const string& interface) : installed(false), id(id), nexthop(nexthop), intf(interface) {};
    NextHopGroup(uint32_t id, const vector<pair<uint32_t,uint8_t>>& group) : installed(false), id(id), group(group) {};
    NextHopGroup(uint32_t id, const string& nexthop, const string& interface,
        const string& vpnsid, const string& segsrc) : id(id), nexthop(nexthop), intf(interface), vpn_sid(vpnsid), seg_src(segsrc) {};
    NextHopGroup(uint32_t id, const string& nexthop, const string& interface,
        const string& vpnsid, const string& segsrc,  const string& segment) : id(id), nexthop(nexthop), intf(interface), vpn_sid(vpnsid), seg_src(segsrc), segment(segment) {};
};
#endif


struct seg6_iptunnel_encap_pri {
    int mode;
    char segment_name[64];
	struct in6_addr src;
	struct ipv6_sr_hdr srh[0];
};

/* Path to protocol name database provided by iproute2 */
constexpr auto DefaultRtProtoPath = "/etc/iproute2/rt_protos";

class RouteSync : public NetMsg
{
public:
    enum { MAX_ADDR_SIZE = 64 };

    RouteSync(RedisPipeline *pipeline);

    virtual void onMsg(int nlmsg_type, struct nl_object *obj);

    virtual void onMsgRaw(struct nlmsghdr *obj);
    WarmStartHelper  m_warmStartHelper;

private:
    /* regular route table */
    ProducerStateTable  m_routeTable;
    /* label route table */
    ProducerStateTable  m_label_routeTable;
    /* vnet route table */
    ProducerStateTable  m_vnet_routeTable;
    /* vnet vxlan tunnel table */  
    ProducerStateTable  m_vnet_tunnelTable;
    /* srv6 local sid table */
    ProducerStateTable m_srv6LocalSidTable; 
    /* srv6 sid list table */
    ProducerStateTable m_srv6SidListTable; 

    struct nl_cache    *m_link_cache;
    struct nl_sock     *m_nl_sock;
#ifdef HAVE_NEXTHOP_GROUP
    /* nexthop group table */
    ProducerStateTable  m_nexthop_groupTable;
    ProducerStateTable  m_pic_context_groupTable;
    map<uint32_t,NextHopGroup> m_nh_groups;
#endif

    /* Handle regular route (include VRF route) */
    void onRouteMsg(int nlmsg_type, struct nl_object *obj, char *vrf);

    /* Handle label route */
    void onLabelRouteMsg(int nlmsg_type, struct nl_object *obj);

    void parseEncap(struct rtattr *tb, uint32_t &encap_value, string &rmac);

    void parseEncapSrv6SteerRoute(struct rtattr *tb, string &vpn_sid, string &src_addr);
    void parseEncapSrv6SidList(struct rtattr *tb, string &sidlist_name, vector<string> &segments, uint32_t &sidlistlen);
    bool parseEncapSrv6VpnRoute(struct rtattr *tb, uint32_t &pic_id, uint32_t &nhg_id);

    bool parseSrv6LocalSid(struct rtattr *tb[], string &block_len,
                           string &node_len, string &func_len,
                           string &arg_len, string &action, string &vrf,
                           string &adj, string &ifname);

    bool parseSrv6LocalSidFormat(struct rtattr *tb, string &block_len,
                                 string &node_len, string &func_len,
                                 string &arg_len);

    void parseRtAttrNested(struct rtattr **tb, int max,
                 struct rtattr *rta);

    char *prefixMac2Str(char *mac, char *buf, int size);


    /* Handle prefix route */
    void onEvpnRouteMsg(struct nlmsghdr *h, int len);

    /* Handle routes containing an SRv6 nexthop */
    void onSrv6SteerRouteMsg(struct nlmsghdr *h, int len);

    /* Handle sidlist*/    
    void onSrv6SidListMsg(struct nlmsghdr *h, int len);

    /* Handle SRv6 Local SID */
    void onSrv6LocalSidMsg(struct nlmsghdr *h, int len);

    /* Handle vpn route */
    void onSrv6VpnRouteMsg(struct nlmsghdr *h, int len);

    /* Handle vnet route */
    void onVnetRouteMsg(int nlmsg_type, struct nl_object *obj, string vnet);

    /* Get interface name based on interface index */
    bool getIfName(int if_index, char *if_name, size_t name_len);

    void getEvpnNextHopSep(string& nexthops, string& vni_list,  
                       string& mac_list, string& intf_list);

    void getEvpnNextHopGwIf(char *gwaddr, int vni_value,
                          string& nexthops, string& vni_list,
                          string& mac_list, string& intf_list,
                          string rmac, string vlan_id);

    bool getEvpnNextHop(struct nlmsghdr *h, int received_bytes, struct rtattr *tb[],
                        string& nexthops, string& vni_list, string& mac_list,
                        string& intf_list);

    bool getSrv6SteerRouteNextHop(struct nlmsghdr *h, int received_bytes,
                        struct rtattr *tb[], string &vpn_sid, string &src_addr);
    bool getSrv6VpnRouteNextHop(struct nlmsghdr *h, int received_bytes,
                               struct rtattr *tb[], uint32_t &pic_id,uint32_t &nhg_id);

    /* Get next hop list */
    void getNextHopList(struct rtnl_route *route_obj, string& gw_list,
                        string& mpls_list, string& intf_list);

    /* Get next hop gateway IP addresses */
    string getNextHopGw(struct rtnl_route *route_obj);

    /* Get next hop interfaces */
    string getNextHopIf(struct rtnl_route *route_obj);

    /* Get next hop weights*/
    string getNextHopWt(struct rtnl_route *route_obj);


#ifdef HAVE_NEXTHOP_GROUP
    /* Handle Nexthop message */
    void onNextHopMsg(struct nlmsghdr *h, int len);
    void onPicContextMsg(struct nlmsghdr *h, int len);
    int parse_encap_seg6(const struct rtattr *tb, struct in6_addr *segs, struct in6_addr *src, char *segment_name);
    int parse_encap_ip6(const struct rtattr *tb, struct in6_addr *src);
    /* Get next hop group key */
    const string getNextHopGroupKeyAsString(uint32_t id) const;
    void updateNextHopGroup(uint32_t nh_id);
    void deleteNextHopGroup(uint32_t nh_id);
    void deletePicContextGroup(uint32_t nh_id);
    void updateNextHopGroupDb(const NextHopGroup& nhg);
    void updatePicContextGroupDb(const NextHopGroup& nhg);
    void getNextHopGroupFields(const NextHopGroup& nhg, struct NextHopField& nhField, uint8_t af = AF_INET);
    void getPicContextGroupFields(const NextHopGroup& nhg, struct NextHopField& nhField, uint8_t af = AF_INET);
#endif
    /* Get encap type */
    uint16_t getEncapType(struct nlmsghdr *h);

    const char *localSidAction2Str(uint32_t action);
};
struct NextHopField {
    string nexthops;
    string ifnames;
    string vni_label;
    string vpn_sid;
    string segments;
    string mpls_nh;
    string weights;
    string seg_srcs;
};

}

#endif
