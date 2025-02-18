/*
 * Copyright (c) 2014 Juniper Networks, Inc. All rights reserved.
 */

extern "C" {
#include <ovsdb_wrapper.h>
};
#include <ovs_tor_agent/tor_agent_init.h>
#include <ovsdb_client.h>
#include <ovsdb_client_idl.h>
#include <ovsdb_client_session.h>
#include <physical_switch_ovsdb.h>
#include <logical_switch_ovsdb.h>
#include <physical_locator_ovsdb.h>

#include <oper/vn.h>
#include <oper/vrf.h>
#include <oper/physical_device_vn.h>
#include <ovsdb_sandesh.h>
#include <ovsdb_types.h>

using namespace OVSDB;

LogicalSwitchEntry::LogicalSwitchEntry(OvsdbDBObject *table,
                                       const std::string &name) :
    OvsdbDBEntry(table), name_(name), device_name_(), vxlan_id_(0),
    mcast_local_row_list_(), mcast_remote_row_(NULL) {
}

LogicalSwitchEntry::LogicalSwitchEntry(OvsdbDBObject *table,
        const PhysicalDeviceVn *entry) : OvsdbDBEntry(table),
    name_(UuidToString(entry->vn()->GetUuid())), mcast_local_row_list_(),
    mcast_remote_row_(NULL) {
    vxlan_id_ = entry->vxlan_id();
    device_name_ = entry->device_display_name();
}

LogicalSwitchEntry::LogicalSwitchEntry(OvsdbDBObject *table,
        const LogicalSwitchEntry *entry) : OvsdbDBEntry(table),
    mcast_local_row_list_(), mcast_remote_row_(NULL) {
    name_ = entry->name_;
    vxlan_id_ = entry->vxlan_id_;;
    device_name_ = entry->device_name_;
}

LogicalSwitchEntry::LogicalSwitchEntry(OvsdbDBObject *table,
        struct ovsdb_idl_row *entry) : OvsdbDBEntry(table, entry),
    name_(ovsdb_wrapper_logical_switch_name(entry)), device_name_(""),
    vxlan_id_(ovsdb_wrapper_logical_switch_tunnel_key(entry)),
    mcast_remote_row_(NULL) {
}

LogicalSwitchEntry::~LogicalSwitchEntry() {
    assert(pl_create_ref_.get() == NULL);
}

Ip4Address &LogicalSwitchEntry::physical_switch_tunnel_ip() {
    PhysicalSwitchEntry *p_switch =
        static_cast<PhysicalSwitchEntry *>(physical_switch_.get());
    return p_switch->tunnel_ip();
}

void LogicalSwitchEntry::AddMsg(struct ovsdb_idl_txn *txn) {
    PhysicalSwitchTable *p_table = table_->client_idl()->physical_switch_table();
    PhysicalSwitchEntry key(p_table, device_name_.c_str());
    physical_switch_ = p_table->GetReference(&key);

    if (stale()) {
        // skip add encoding for stale entry
        return;
    }
    struct ovsdb_idl_row *row =
        ovsdb_wrapper_add_logical_switch(txn, ovs_entry_, name_.c_str(),
                vxlan_id_);

    // Encode Delete for Old remote multicast entries
    DeleteOldMcastRemoteMac();

    // Add remote multicast entry if not already present
    // and if old mcast remote MAC list is empty
    if (old_mcast_remote_row_list_.empty() && mcast_remote_row_ == NULL) {
        std::string dest_ip = table_->client_idl()->tsn_ip().to_string();
        PhysicalLocatorTable *pl_table =
            table_->client_idl()->physical_locator_table();
        PhysicalLocatorEntry pl_key(pl_table, dest_ip);
        /*
         * we don't take reference to physical locator, just use if locator
         * is existing or we will create a new one.
         */
        PhysicalLocatorEntry *pl_entry =
            static_cast<PhysicalLocatorEntry *>(pl_table->Find(&pl_key));
        struct ovsdb_idl_row *pl_row = NULL;
        if (pl_entry)
            pl_row = pl_entry->ovs_entry();
        ovsdb_wrapper_add_mcast_mac_remote(txn, NULL, "unknown-dst", row,
                pl_row, dest_ip.c_str());
    }

    SendTrace(LogicalSwitchEntry::ADD_REQ);
}

void LogicalSwitchEntry::ChangeMsg(struct ovsdb_idl_txn *txn) {
    AddMsg(txn);
}

void LogicalSwitchEntry::DeleteMsg(struct ovsdb_idl_txn *txn) {
    physical_switch_ = NULL;

    // encode delete of entry if it is non-NULL
    if (mcast_remote_row_ != NULL) {
        ovsdb_wrapper_delete_mcast_mac_remote(mcast_remote_row_);
    }
    DeleteOldMcastRemoteMac();

    if (ovs_entry_ != NULL) {
        ovsdb_wrapper_delete_logical_switch(ovs_entry_);
    }

    OvsdbIdlRowList::iterator it;
    for (it = mcast_local_row_list_.begin();
         it != mcast_local_row_list_.end(); ++it) {
        ovsdb_wrapper_delete_mcast_mac_local(*it);
    }
    for (it = ucast_local_row_list_.begin();
         it != ucast_local_row_list_.end(); ++it) {
        ovsdb_wrapper_delete_ucast_mac_local(*it);
    }

    SendTrace(LogicalSwitchEntry::DEL_REQ);
}

void LogicalSwitchEntry::OvsdbChange() {
    if (!IsResolved())
        table_->NotifyEvent(this, KSyncEntry::ADD_CHANGE_REQ);
}

const std::string &LogicalSwitchEntry::name() const {
    return name_;
}

const std::string &LogicalSwitchEntry::device_name() const {
    return device_name_;
}

int64_t LogicalSwitchEntry::vxlan_id() const {
    return vxlan_id_;
}

std::string LogicalSwitchEntry::tor_service_node() const {
    return ovsdb_wrapper_mcast_mac_remote_dst_ip(mcast_remote_row_);
}

bool LogicalSwitchEntry::Sync(DBEntry *db_entry) {
    PhysicalDeviceVn *entry =
        static_cast<PhysicalDeviceVn *>(db_entry);
    bool change = false;
    if (vxlan_id_ != entry->vxlan_id()) {
        vxlan_id_ = entry->vxlan_id();
        change = true;
    }
    if (device_name_ != entry->device_display_name()) {
        device_name_ = entry->device_display_name();
        change = true;
    }
    return change;
}

bool LogicalSwitchEntry::IsLess(const KSyncEntry &entry) const {
    const LogicalSwitchEntry &ps_entry =
        static_cast<const LogicalSwitchEntry&>(entry);
    return (name_.compare(ps_entry.name_) < 0);
}

KSyncEntry *LogicalSwitchEntry::UnresolvedReference() {
    assert(pl_create_ref_.get() == NULL);

    if (stale()) {
        // while creating stale entry we should not wait for physical
        // switch object since it will not be available till config
        // comes up
        return NULL;
    }
    PhysicalSwitchTable *p_table = table_->client_idl()->physical_switch_table();
    PhysicalSwitchEntry key(p_table, device_name_.c_str());
    PhysicalSwitchEntry *p_switch =
        static_cast<PhysicalSwitchEntry *>(p_table->GetReference(&key));
    if (!p_switch->IsResolved()) {
        return p_switch;
    }

    // check if physical locator is available
    std::string dest_ip = table_->client_idl()->tsn_ip().to_string();
    PhysicalLocatorTable *pl_table =
        table_->client_idl()->physical_locator_table();
    PhysicalLocatorEntry pl_key(pl_table, dest_ip);
    PhysicalLocatorEntry *pl_entry =
        static_cast<PhysicalLocatorEntry *>(pl_table->GetReference(&pl_key));
    if (!pl_entry->IsResolved()) {
        if (!pl_entry->AcquireCreateRequest(this)) {
            // failed to Acquire Create Request, wait for physical locator
            return pl_entry;
        }
        pl_create_ref_ = pl_entry;
    }

    return NULL;
}

bool LogicalSwitchEntry::IsLocalMacsRef() const {
    return (local_mac_ref_.get() != NULL);
}

void LogicalSwitchEntry::Ack(bool success) {
    ReleaseLocatorCreateReference();
    OvsdbDBEntry::Ack(success);
}

void LogicalSwitchEntry::TxnDoneNoMessage() {
    ReleaseLocatorCreateReference();
}

void LogicalSwitchEntry::SendTrace(Trace event) const {
    SandeshLogicalSwitchInfo info;
    switch (event) {
    case ADD_REQ:
        info.set_op("Add Requested");
        break;
    case DEL_REQ:
        info.set_op("Delete Requested");
        break;
    case ADD_ACK:
        info.set_op("Add Received");
        break;
    case DEL_ACK:
        info.set_op("Delete Received");
        break;
    default:
        info.set_op("unknown");
    }
    info.set_name(name_);
    info.set_device_name(device_name_);
    info.set_vxlan(vxlan_id_);
    OVSDB_TRACE(LogicalSwitch, info);
}

void LogicalSwitchEntry::DeleteOldMcastRemoteMac() {
    OvsdbIdlRowList::iterator it;
    for (it = old_mcast_remote_row_list_.begin();
         it != old_mcast_remote_row_list_.end(); ++it) {
        ovsdb_wrapper_delete_mcast_mac_remote(*it);
    }
}

void LogicalSwitchEntry::ReleaseLocatorCreateReference() {
    // on Ack Release the physical locator create ref, if present
    if (pl_create_ref_.get() != NULL) {
        // release creator reference on txn complete
        PhysicalLocatorEntry *pl_entry =
            static_cast<PhysicalLocatorEntry *>(pl_create_ref_.get());
        pl_entry->ReleaseCreateRequest(this);
        pl_create_ref_ = NULL;
    }
}

LogicalSwitchTable::LogicalSwitchTable(OvsdbClientIdl *idl) :
    OvsdbDBObject(idl, true) {
    idl->Register(OvsdbClientIdl::OVSDB_LOGICAL_SWITCH,
                  boost::bind(&LogicalSwitchTable::OvsdbNotify, this, _1, _2));
    idl->Register(OvsdbClientIdl::OVSDB_MCAST_MAC_REMOTE,
                  boost::bind(&LogicalSwitchTable::OvsdbMcastRemoteMacNotify,
                      this, _1, _2));
}

LogicalSwitchTable::~LogicalSwitchTable() {
}

void LogicalSwitchTable::OvsdbNotify(OvsdbClientIdl::Op op,
        struct ovsdb_idl_row *row) {
    LogicalSwitchEntry key(this, row);
    if (op == OvsdbClientIdl::OVSDB_DEL) {
        NotifyDeleteOvsdb((OvsdbDBEntry*)&key, row);
        key.SendTrace(LogicalSwitchEntry::DEL_ACK);
    } else if (op == OvsdbClientIdl::OVSDB_ADD) {
        NotifyAddOvsdb((OvsdbDBEntry*)&key, row);
        key.SendTrace(LogicalSwitchEntry::ADD_ACK);
    } else {
        assert(0);
    }
}

void LogicalSwitchTable::OvsdbMcastLocalMacNotify(OvsdbClientIdl::Op op,
        struct ovsdb_idl_row *row) {
    const char *mac = ovsdb_wrapper_mcast_mac_local_mac(row);
    const char *ls = ovsdb_wrapper_mcast_mac_local_logical_switch(row);
    LogicalSwitchEntry *entry = NULL;
    if (ls) {
        LogicalSwitchEntry key(this, ls);
        entry = static_cast<LogicalSwitchEntry *>(Find(&key));
    }
    struct ovsdb_idl_row *l_set =
        ovsdb_wrapper_mcast_mac_local_physical_locator_set(row);
    // physical locator set is immutable, multicast row with NULL
    // physical locator is not valid, and it may not observe further
    // delete trigger, so trigger delete for row and wait for
    // locator set to be available
    if (op == OvsdbClientIdl::OVSDB_DEL || l_set == NULL) {
        // trigger deletion based on the entry for which add was triggered
        OvsdbIdlRowMap::iterator idl_it = idl_row_map_.find(row);
        if (idl_it != idl_row_map_.end()) {
            entry = idl_it->second;
            idl_row_map_.erase(idl_it);
        }

        OVSDB_TRACE(Trace, "Delete : Local Mcast MAC " + std::string(mac) +
                ", logical switch " + (entry != NULL ? entry->name() : ""));
        if (entry) {
            entry->mcast_local_row_list_.erase(row);
        }
    } else if (op == OvsdbClientIdl::OVSDB_ADD) {
        OVSDB_TRACE(Trace, "Add : Local Mcast MAC " + std::string(mac) +
                ", logical switch " + (ls ? std::string(ls) : ""));
        if (entry) {
            idl_row_map_[row] = entry;
            entry->mcast_local_row_list_.insert(row);
        }
    } else {
        assert(0);
    }

    if (entry) {
        if (!entry->mcast_local_row_list_.empty() ||
            !entry->ucast_local_row_list_.empty()) {
            if (entry->IsActive())
                entry->local_mac_ref_ = entry;
        } else {
            entry->local_mac_ref_ = NULL;
        }
    }
}

void LogicalSwitchTable::OvsdbMcastRemoteMacNotify(OvsdbClientIdl::Op op,
        struct ovsdb_idl_row *row) {
    const char *mac = ovsdb_wrapper_mcast_mac_remote_mac(row);
    const char *ls = ovsdb_wrapper_mcast_mac_remote_logical_switch(row);
    LogicalSwitchEntry *entry = NULL;
    if (ls) {
        LogicalSwitchEntry key(this, ls);
        entry = static_cast<LogicalSwitchEntry *>(Find(&key));
    }
    if (op == OvsdbClientIdl::OVSDB_DEL) {
        // trigger deletion based on the entry for which add was triggered
        OvsdbIdlRowMap::iterator idl_it = idl_row_map_.find(row);
        if (idl_it != idl_row_map_.end()) {
            entry = idl_it->second;
            idl_row_map_.erase(idl_it);
        }

        OVSDB_TRACE(Trace, "Delete : Remote Mcast MAC " + std::string(mac) +
                ", logical switch " + (entry != NULL ? entry->name() : ""));
        if (entry) {
            entry->old_mcast_remote_row_list_.erase(row);
            if (entry->mcast_remote_row_ == row) {
                entry->mcast_remote_row_ = NULL;
            }

            // trigger change for active entry, once we are done deleting
            // all old mcast remote rows
            if (entry->IsActive() && entry->old_mcast_remote_row_list_.empty())
                Change(entry);
        }
    } else if (op == OvsdbClientIdl::OVSDB_ADD) {
        OVSDB_TRACE(Trace, "Add : Remote Mcast MAC " + std::string(mac) +
                ", logical switch " + (ls ? std::string(ls) : ""));
        if (entry) {
            idl_row_map_[row] = entry;
            if (entry->mcast_remote_row_ != row) {
                if (entry->mcast_remote_row_) {
                    // if we already had an entry move old and current
                    // entry to old remote mac list to trigger delete
                    // for both the mcast remote mac entries.
                    // once both are deleted, we will trigger add
                    // for new entry
                    entry->old_mcast_remote_row_list_.insert(
                            entry->mcast_remote_row_);
                    entry->mcast_remote_row_ = NULL;
                    entry->old_mcast_remote_row_list_.insert(row);
                } else {
                    entry->mcast_remote_row_ = row;
                }
            }

            std::string dest_ip = ovsdb_wrapper_mcast_mac_remote_dst_ip(row);
            std::string tsn_ip = client_idl()->tsn_ip().to_string();
            if (dest_ip.compare(tsn_ip) != 0) {
                // dest ip is different from tsn ip
                // move this row to old mcast list, to delete curreny
                // entry and reprogram new entry with correct TSN IP
                entry->old_mcast_remote_row_list_.insert(row);
                entry->mcast_remote_row_ = NULL;
            }

            // trigger change on active entry to delete all old
            // mcast remote rows.
            if (entry->IsActive() && !entry->old_mcast_remote_row_list_.empty())
                Change(entry);
        }
    } else {
        assert(0);
    }
}

void LogicalSwitchTable::OvsdbUcastLocalMacNotify(OvsdbClientIdl::Op op,
        struct ovsdb_idl_row *row) {
    const char *mac = ovsdb_wrapper_ucast_mac_local_mac(row);
    const char *ls = ovsdb_wrapper_ucast_mac_local_logical_switch(row);
    LogicalSwitchEntry *entry = NULL;
    if (ls) {
        LogicalSwitchEntry key(this, ls);
        entry = static_cast<LogicalSwitchEntry *>(Find(&key));
    }
    if (op == OvsdbClientIdl::OVSDB_DEL) {
        // trigger deletion based on the entry for which add was triggered
        OvsdbIdlRowMap::iterator idl_it = idl_row_map_.find(row);
        if (idl_it != idl_row_map_.end()) {
            entry = idl_it->second;
            idl_row_map_.erase(idl_it);
        }

        OVSDB_TRACE(Trace, "Delete : Local Ucast MAC " + std::string(mac) +
                ", logical switch " + (entry != NULL ? entry->name() : ""));
        if (entry) {
            entry->ucast_local_row_list_.erase(row);
        }
    } else if (op == OvsdbClientIdl::OVSDB_ADD) {
        OVSDB_TRACE(Trace, "Add : Local Ucast MAC " + std::string(mac) +
                ", logical switch " + (ls ? std::string(ls) : ""));
        if (entry) {
            idl_row_map_[row] = entry;
            entry->ucast_local_row_list_.insert(row);
        }
    } else {
        assert(0);
    }

    if (entry) {
        if (!entry->mcast_local_row_list_.empty() ||
            !entry->ucast_local_row_list_.empty()) {
            if (entry->IsActive())
                entry->local_mac_ref_ = entry;
        } else {
            entry->local_mac_ref_ = NULL;
        }
    }
}

KSyncEntry *LogicalSwitchTable::Alloc(const KSyncEntry *key, uint32_t index) {
    const LogicalSwitchEntry *k_entry =
        static_cast<const LogicalSwitchEntry *>(key);
    LogicalSwitchEntry *entry = new LogicalSwitchEntry(this, k_entry);
    return entry;
}

KSyncEntry *LogicalSwitchTable::DBToKSyncEntry(const DBEntry* db_entry) {
    const PhysicalDeviceVn *entry =
        static_cast<const PhysicalDeviceVn *>(db_entry);
    LogicalSwitchEntry *key = new LogicalSwitchEntry(this, entry);
    return static_cast<KSyncEntry *>(key);
}

OvsdbDBEntry *LogicalSwitchTable::AllocOvsEntry(struct ovsdb_idl_row *row) {
    LogicalSwitchEntry key(this, row);
    return static_cast<OvsdbDBEntry *>(CreateStale(&key));
}

KSyncDBObject::DBFilterResp LogicalSwitchTable::OvsdbDBEntryFilter(
        const DBEntry *db_entry, const OvsdbDBEntry *ovsdb_entry) {
    const PhysicalDeviceVn *entry =
        static_cast<const PhysicalDeviceVn *>(db_entry);

    // Delete the entry which has invalid VxLAN id associated.
    if (entry->vxlan_id() == 0) {
        return DBFilterDelete;
    }
    return DBFilterAccept;
}

void LogicalSwitchTable::ProcessDeleteTableReq() {
    ProcessDeleteTableReqTask *task =
        new ProcessDeleteTableReqTask(this);
    TaskScheduler *scheduler = TaskScheduler::GetInstance();
    scheduler->Enqueue(task);
}

LogicalSwitchTable::ProcessDeleteTableReqTask::ProcessDeleteTableReqTask(
        LogicalSwitchTable *table) :
    Task((TaskScheduler::GetInstance()->GetTaskId("Agent::KSync")), 0),
    table_(table), entry_(NULL) {
}

LogicalSwitchTable::ProcessDeleteTableReqTask::~ProcessDeleteTableReqTask() {
}

bool LogicalSwitchTable::ProcessDeleteTableReqTask::Run() {
    KSyncEntry *kentry = entry_.get();
    if (kentry == NULL) {
        kentry = table_->Next(kentry);
    }

    int count = 0;
    while (kentry != NULL) {
        LogicalSwitchEntry *entry = static_cast<LogicalSwitchEntry *>(kentry);
        count++;
        kentry = table_->Next(kentry);
        // while table is set for deletion reset the local_mac_ref
        // since there will be no trigger from OVSDB database
        entry->local_mac_ref_ = NULL;

        // check for yield
        if (count == KEntriesPerIteration && kentry != NULL) {
            entry_ = kentry;
            return false;
        }
    }

    entry_ = NULL;
    // Done processing delete request, schedule delete table
    table_->DeleteTable();
    return true;
}

/////////////////////////////////////////////////////////////////////////////
// Sandesh routines
/////////////////////////////////////////////////////////////////////////////
LogicalSwitchSandeshTask::LogicalSwitchSandeshTask(
        std::string resp_ctx, AgentSandeshArguments &args) :
    OvsdbSandeshTask(resp_ctx, args), name_("") {
    if (false == args.Get("name", &name_)) {
        name_ = "";
    }
}

LogicalSwitchSandeshTask::LogicalSwitchSandeshTask(std::string resp_ctx,
                                                   const std::string &ip,
                                                   uint32_t port,
                                                   const std::string &name) :
    OvsdbSandeshTask(resp_ctx, ip, port), name_(name) {
}

LogicalSwitchSandeshTask::~LogicalSwitchSandeshTask() {
}

void LogicalSwitchSandeshTask::EncodeArgs(AgentSandeshArguments &args) {
    if (!name_.empty()) {
        args.Add("name", name_);
    }
}

OvsdbSandeshTask::FilterResp
LogicalSwitchSandeshTask::Filter(KSyncEntry *kentry) {
    if (!name_.empty()) {
        LogicalSwitchEntry *entry = static_cast<LogicalSwitchEntry *>(kentry);
        if (entry->name().find(name_) != std::string::npos) {
            return FilterAllow;
        }
        return FilterDeny;
    }
    return FilterAllow;
}

void LogicalSwitchSandeshTask::UpdateResp(KSyncEntry *kentry,
                                          SandeshResponse *resp) {
    LogicalSwitchEntry *entry = static_cast<LogicalSwitchEntry *>(kentry);
    OvsdbLogicalSwitchEntry lentry;
    lentry.set_state(entry->StateString());
    lentry.set_name(entry->name());
    lentry.set_physical_switch(entry->device_name());
    lentry.set_vxlan_id(entry->vxlan_id());
    lentry.set_tor_service_node(entry->tor_service_node());
    if (entry->IsDeleted() && entry->IsLocalMacsRef()) {
        lentry.set_message("Waiting for Local Macs Cleanup");
    }
    OvsdbLogicalSwitchResp *ls_resp =
        static_cast<OvsdbLogicalSwitchResp *>(resp);
    std::vector<OvsdbLogicalSwitchEntry> &lswitch =
        const_cast<std::vector<OvsdbLogicalSwitchEntry>&>(
                ls_resp->get_lswitch());
    lswitch.push_back(lentry);
}

SandeshResponse *LogicalSwitchSandeshTask::Alloc() {
    return static_cast<SandeshResponse *>(new OvsdbLogicalSwitchResp());
}

KSyncObject *LogicalSwitchSandeshTask::GetObject(OvsdbClientSession *session) {
    return static_cast<KSyncObject *>(
            session->client_idl()->logical_switch_table());
}

void OvsdbLogicalSwitchReq::HandleRequest() const {
    LogicalSwitchSandeshTask *task =
        new LogicalSwitchSandeshTask(context(), get_session_remote_ip(),
                                     get_session_remote_port(),
                                     get_name());
    TaskScheduler *scheduler = TaskScheduler::GetInstance();
    scheduler->Enqueue(task);
}
