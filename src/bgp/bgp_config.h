/*
 * Copyright (c) 2014 Juniper Networks, Inc. All rights reserved.
 */
#ifndef BGP_BGP_CONFIG_H__
#define BGP_BGP_CONFIG_H__

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/util.h"
#include "net/address.h"

class BgpServer;

struct AuthenticationKey {
    AuthenticationKey() : id(-1), start_time(0) {
    }

    bool operator<(const AuthenticationKey &) const;
    bool operator==(const AuthenticationKey &) const;
    void Reset() {
        value = "";
        start_time = 0;
    }

    int id;
    std::string value;
    time_t start_time;
};

typedef std::vector<AuthenticationKey> AuthenticationKeyChain;

class AuthenticationData {
public:
    enum KeyType {
        NIL = 0,
        MD5,
    };
    typedef AuthenticationKeyChain::iterator iterator;
    typedef AuthenticationKeyChain::const_iterator const_iterator;

    iterator begin() { return key_chain_.begin(); }
    iterator end() { return key_chain_.end(); }
    const_iterator begin() const { return key_chain_.begin(); }
    const_iterator end() const { return key_chain_.end(); }

    AuthenticationData();
    AuthenticationData(KeyType type, const AuthenticationKeyChain &chain);

    bool operator==(const AuthenticationData &rhs) const;
    bool operator<(const AuthenticationData &rhs) const;
    AuthenticationData &operator=(const AuthenticationData &rhs);

    const AuthenticationKey *Find(int key_id) const;
    bool IsMd5() const { return key_type_ == MD5; }
    bool Empty() const;
    void Clear();
    std::string KeyTypeToString() const;
    static std::string KeyTypeToString(KeyType key_type);
    void AddKeyToKeyChain(const AuthenticationKey &key);

    KeyType key_type() const { return key_type_; }
    void set_key_type(KeyType in_type) {
        key_type_ = in_type;
    }
    const AuthenticationKeyChain &key_chain() const { return key_chain_; }
    void set_key_chain(const AuthenticationKeyChain &in_chain) {
        key_chain_ = in_chain;
    }
    std::vector<std::string> KeysToString() const;
    std::vector<std::string> KeysToStringDetail() const;

private:
    KeyType key_type_;
    AuthenticationKeyChain key_chain_;
};

// The configuration associated with a BGP neighbor.
//
// BgpNeighborConfig represents a single session between the local bgp-router
// and a remote bgp-router.  There may be multiple BgpNeighborConfigs for one
// BgpIfmapPeeringConfig, though there's typically just one.
//
// Each BgpNeighborConfig causes creation of a BgpPeer. BgpPeer has a pointer
// to the BgpNeighborConfig. This pointer is invalidated and cleared when the
// BgpPeer is undergoing deletion.
//
// BgpNeighborConfigs get created/updated/deleted when the
// BgpIfmapPeeringConfig is created/updated/deleted.
//
// The uuid_ field is used is there are multiple sessions to the same remote
// bgp-router i.e. there's multiple BgpNeighborConfigs for a BgpPeeringConfig.
//
// The name_ field contains the fully qualified name for the peer.  If uuid_
// is non-empty i.e. there's multiple sessions to the same remote bgp-router,
// the uuid_ is appended to the remote peer's name to make it unique.
//
class BgpNeighborConfig {
public:
    typedef std::vector<std::string> AddressFamilyList;
    enum Type {
        UNSPECIFIED,
        IBGP,
        EBGP,
    };

    BgpNeighborConfig();

    void CopyValues(const BgpNeighborConfig &rhs);

    const std::string &name() const { return name_; }
    void set_name(const std::string &name) { name_ = name; }

    const std::string &uuid() const { return uuid_; }
    void set_uuid(const std::string &uuid) { uuid_ = uuid; }

    const std::string &instance_name() const { return instance_name_; }
    void set_instance_name(const std::string &instance_name) {
        instance_name_ = instance_name;
    }

    const std::string &group_name() const { return group_name_; }
    void set_group_name(const std::string &group_name) {
        group_name_ = group_name;
    }

    Type peer_type() const { return type_; }
    void set_peer_type(Type type) { type_ = type; }
 
    uint32_t peer_as() const { return peer_as_; }
    void set_peer_as(uint32_t peer_as) { peer_as_ = peer_as; }

    const IpAddress &peer_address() const { return address_; }
    void set_peer_address(const IpAddress &address) { address_ = address; }

    uint32_t peer_identifier() const { return identifier_; }
    void set_peer_identifier(uint32_t identifier) {
        identifier_ = identifier;
    }

    int port() const { return port_; }
    void set_port(int port) { port_ = port; }

    int hold_time() const { return hold_time_; }
    void set_hold_time(int hold_time) { hold_time_ = hold_time; }

    uint32_t local_as() const { return local_as_; }
    void set_local_as(uint32_t local_as) { local_as_ = local_as; }

    uint32_t local_identifier() const { return local_identifier_; }
    void set_local_identifier(uint32_t identifier) {
        local_identifier_ = identifier;
    }

    const AuthenticationData &auth_data() const {
        return auth_data_;
    }
    void set_keydata(const AuthenticationData &in_auth_data) {
        auth_data_ = in_auth_data;
    }

    const AddressFamilyList &address_families() const {
        return address_families_;
    }

    void set_address_families(const AddressFamilyList &address_families) {
        address_families_ = address_families;
    }

    uint64_t last_change_at() const { return last_change_at_; }
    void set_last_change_at(uint64_t tstamp) const { last_change_at_ = tstamp; }

    std::string AuthKeyTypeToString() const;
    std::vector<std::string> AuthKeysToString() const;

    int CompareTo(const BgpNeighborConfig &rhs) const;
    bool operator!=(const BgpNeighborConfig &rhs) const {
        return CompareTo(rhs) != 0;
    }

private:
    std::string name_;
    std::string uuid_;
    std::string instance_name_;
    std::string group_name_;
    Type type_;
    uint32_t peer_as_;
    uint32_t identifier_;
    IpAddress address_;
    int port_;
    int hold_time_;
    uint32_t local_as_;
    uint32_t local_identifier_;
    mutable uint64_t last_change_at_;
    AuthenticationData auth_data_;
    AddressFamilyList address_families_;

    DISALLOW_COPY_AND_ASSIGN(BgpNeighborConfig);
};

struct ServiceChainConfig {
    Address::Family family;
    std::string routing_instance;
    std::vector<std::string> prefix;
    std::string service_chain_address;
    std::string service_instance;
    std::string source_routing_instance;
};

struct StaticRouteConfig {
    IpAddress address;
    int prefix_length;
    IpAddress nexthop;
    std::vector<std::string> route_target;
};

// Instance configuration.
class BgpInstanceConfig {
public:
    typedef std::set<std::string> RouteTargetList;
    typedef std::vector<StaticRouteConfig> StaticRouteList;
    typedef std::vector<ServiceChainConfig> ServiceChainList;

    explicit BgpInstanceConfig(const std::string &name);
    virtual ~BgpInstanceConfig();

    const std::string &name() const { return name_; }

    const RouteTargetList &import_list() const { return import_list_; }
    void set_import_list(const RouteTargetList &import_list) {
        import_list_ = import_list;
    }
    const RouteTargetList &export_list() const { return export_list_; }
    void set_export_list(const RouteTargetList &export_list) {
        export_list_ = export_list;
    }

    bool has_pnf() const { return has_pnf_; }
    void set_has_pnf(bool has_pnf) { has_pnf_ = has_pnf; }

    const std::string &virtual_network() const { return virtual_network_; }
    void set_virtual_network(const std::string &virtual_network) {
        virtual_network_ = virtual_network;
    }

    int virtual_network_index() const { return virtual_network_index_; }
    void set_virtual_network_index(int virtual_network_index) {
        virtual_network_index_ = virtual_network_index;
    }

    bool virtual_network_allow_transit() const {
        return virtual_network_allow_transit_;
    }
    void set_virtual_network_allow_transit(bool allow_transit) {
        virtual_network_allow_transit_ = allow_transit;
    }

    int vxlan_id() const { return vxlan_id_; }
    void set_vxlan_id(int vxlan_id) { vxlan_id_ = vxlan_id; }

    uint64_t last_change_at() const { return last_change_at_; }
    void set_last_change_at(uint64_t tstamp) const { last_change_at_ = tstamp; }

    const StaticRouteList &static_routes(Address::Family family) const;
    void swap_static_routes(Address::Family family, StaticRouteList *list);

    const ServiceChainList &service_chain_list() const {
        return service_chain_list_;
    }
    void swap_service_chain_list(ServiceChainList *list) {
        std::swap(service_chain_list_, *list);
    }
    const ServiceChainConfig *service_chain_info(Address::Family family) const;

    void Clear();

private:
    friend class BgpInstanceConfigTest;

    std::string name_;
    RouteTargetList import_list_;
    RouteTargetList export_list_;
    bool has_pnf_;
    std::string virtual_network_;
    int virtual_network_index_;
    bool virtual_network_allow_transit_;
    int vxlan_id_;
    mutable uint64_t last_change_at_;
    StaticRouteList inet_static_routes_;
    StaticRouteList inet6_static_routes_;
    ServiceChainList service_chain_list_;

    DISALLOW_COPY_AND_ASSIGN(BgpInstanceConfig);
};

// Local configuration.
class BgpProtocolConfig {
public:
    explicit BgpProtocolConfig(const std::string &instance_name);
    const std::string &instance_name() const {
        return instance_name_;
    }

    int CompareTo(const BgpProtocolConfig &rhs) const;

    uint32_t identifier() const { return identifier_; }
    void set_identifier(uint32_t identifier) { identifier_ = identifier; }
    uint32_t autonomous_system() const { return autonomous_system_; }
    void set_autonomous_system(uint32_t autonomous_system) {
        autonomous_system_ = autonomous_system;
    }

    uint32_t local_autonomous_system() const {
        return local_autonomous_system_;
    }
    void set_local_autonomous_system(uint32_t local_autonomous_system) {
        local_autonomous_system_ = local_autonomous_system;
    }

    int port() const { return port_; }
    void set_port(int port) { port_ = port; }

    int hold_time() const { return hold_time_; }
    void set_hold_time(int hold_time) { hold_time_ = hold_time; }

    uint64_t last_change_at() const { return last_change_at_; }
    void set_last_change_at(uint64_t tstamp) const { last_change_at_ = tstamp; }

private:
    std::string instance_name_;
    uint32_t autonomous_system_;
    uint32_t local_autonomous_system_;
    uint32_t identifier_;
    int port_;
    int hold_time_;
    mutable uint64_t last_change_at_;

    DISALLOW_COPY_AND_ASSIGN(BgpProtocolConfig);
};

/*
 * BgpConfigManager defines the interface between the BGP server and the
 * configuration sub-system. Multiple configuration sub-systems are
 * supported.
 */
class BgpConfigManager {
public:
    enum EventType {
        CFG_NONE,
        CFG_ADD,
        CFG_CHANGE,
        CFG_DELETE
    };

    typedef boost::function<void(const BgpProtocolConfig *, EventType)>
        BgpProtocolObserver;
    typedef boost::function<void(const BgpInstanceConfig *, EventType)>
        BgpInstanceObserver;
    typedef boost::function<void(const BgpNeighborConfig *, EventType)>
        BgpNeighborObserver;

    struct Observers {
        BgpProtocolObserver protocol;
        BgpInstanceObserver instance;
        BgpNeighborObserver neighbor;
    };

    typedef std::map<std::string, BgpInstanceConfig *> InstanceMap;
    typedef std::pair<InstanceMap::const_iterator,
            InstanceMap::const_iterator> InstanceMapRange;
    typedef std::map<std::string, BgpNeighborConfig *> NeighborMap;
    typedef std::pair<NeighborMap::const_iterator,
            NeighborMap::const_iterator> NeighborMapRange;

    static const char *kMasterInstance;
    static const int kDefaultPort;
    static const uint32_t kDefaultAutonomousSystem;

    BgpConfigManager(BgpServer *server);
    virtual ~BgpConfigManager();

    void RegisterObservers(const Observers &obs) { obs_ = obs; }

    virtual void Terminate() = 0;
    virtual const std::string &localname() const = 0;

    virtual InstanceMapRange InstanceMapItems(
        const std::string &start_instance = std::string()) const = 0;
    virtual NeighborMapRange NeighborMapItems(
        const std::string &instance_name) const = 0;

    virtual int NeighborCount(const std::string &instance_name) const = 0;

    virtual const BgpInstanceConfig *FindInstance(
        const std::string &name) const = 0;
    virtual const BgpProtocolConfig *GetProtocolConfig(
        const std::string &instance_name) const = 0;
    virtual const BgpNeighborConfig *FindNeighbor(
        const std::string &instance_name, const std::string &name) const = 0;

    // Invoke registered observer
    template <typename BgpConfigObject>
    void Notify(const BgpConfigObject *, EventType);

    const BgpServer *server() { return server_; }

private:
    BgpServer *server_;
    Observers obs_;

    DISALLOW_COPY_AND_ASSIGN(BgpConfigManager);
};

#endif  // BGP_BGP_CONFIG_H__
