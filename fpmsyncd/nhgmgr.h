
#ifndef NHGMGR_H
#define NHGMGR_H

#include "dbconnector.h"
#include <string.h>

using namespace std;
using namespace swss;

class NHGMgr {
public:
    NHGMgr(RedisPipeline *pipeline);
    ~NHGMgr();
    static NHGMgr getInstance() {
        static NHGMgr instance;
        return instance;
    };
    int addNHG(NextHopGroupFull nhg);
    int delNHG(uint32_t id);
    RIBNhgEntry *getRIBNhgEntryByKey(string key);
    RIBNhgEntry *getRIBNhgEntryByRIBID(uint32_t id);
    bool getIfName(int if_index, char *if_name, size_t name_len);
    void dump_zebra_nhg_table(string& ret);

    // Not implemented
    void dump_sonic_nhg_table(string& ret);
    SonicNHGObject *getSonicNHGByID(uint32_t id);
    SonicNHGObject *getSonicNHGByKey(std::string key);

private:
    DBConnector *m_db;
    // Map zebra NHG id to received zebra_dplane_ctx + SONIC Context (a.k.a SONIC ZEBRA NHG)
    RIBNhgTable* m_rib_nhg_table;
    // Map SONiC NHG id to SONiC created NHG
    SonicNHGTable* m_sonic_nhg_table;
    // Map nexthops in zebra_dplane_ctx to zebra NHG ID
    map<NexthopKey, uint32_t> nexthop_key_2_rib_id_map;
    // NEXTHOP Address to SONiC NHG ID list mapping
    map<NexthopKey, uint32_t> nexthop_key_2_sonic_id_map;
};

class RIBNhgTable {

public:
    RIBNhgTable(RedisPipeline *pipeline, string table_name, bool is_state_table);
    ~RIBNhgTable();
    int addNhg(NextHopGroupFull nhg);
    int delNhg(uint32_t id);
    int updateNhg(nhg);
    RIBNhgEntry *getEntry(uint32_t id);
    RIBNhgEntry *getEntry(std::string key);
    void add_nhg_dependents(RIBNhgEntry *entry);
    void remove_nhg_dependents(RIBNhgEntry *entry);
    bool isNhgExist(string key);
    bool isNhgExist(uint32_t id);
    int writeToDB(uint32_t id);
    void dump_table(string &ret);

    // Not implemented
    vector<RIBNhgEntry *> getDepends(std::string key);
    vector<RIBNhgEntry *> getDependents(uint32_t id);

private:
    map<uint32_t, RIBNhgEntry *> m_nhg_map;
    ProducerStateTable m_nexthop_groupTable;
};

class RIBNhgEntry {
public:
    RIBNhgEntry(RIBNhgTable* m_table);
    vector<RIBNhgEntry *> getDependsID();
    vector<RIBNhgEntry *> getDependentsID();
    vector<pair<uint32_t, uint8_t>> getGroup();
    vector<pair<uint32_t, uint8_t>> getResolvedGroup();
    vector<FieldValueTuple> getFvVector();
    int syncFvVector();
    NexthopKey getKey();
    NextHopGroupFull getNhg();
    bool isInstalled();
    void setInstalled(bool installed);
    int setEntry(NexthopGroupFull);
    static RIBNhgEntry *create_nhg_entry(NextHopGroupFull nhg, RIBNhgTable* m_table);
    string getNexthop();
private:
    RIBNhgTable* m_table;
    bool isInstall ;
    vector<FieldValueTuple> m_fvVector;
    NexthopKey m_key;
    string m_nexthop;
    uint8_t af;
    NextHopGroupFull m_nhg;
    vector<pair<uint32_t, uint8_t>> m_group;
    vector<pair<uint32_t, uint8_t>> m_resolved_group;
    vector<uint32_t> m_depends;
    vector<uint32_t> m_dependents;
    uint32_t m_sonic_id = -1;
    uint32_t m_sonic_gateway_nhg_id = -1;

};
enum NEXTHOP_KEY_TYPE {
  NEXTHOP_KEY_TYPE_ZEBRA,
  NEXTHOP_KEY_TYPE_SONIC,
};
class NexthopKey {

public:
    NexthopKey(const NextHopGroupFull *nhg);
    string getNhgKey() { return key; };
    string getRIBKey(enum NEXTHOP_KEY_TYPE type) ;
    string getSonicKey() { return key; };
    string getNhgKey(enum NEXTHOP_KEY_TYPE  type);
  private:
    string m_address_key;
};

// for pic. Not implemented
class SonicNHGTable {

public:
    SonicNHGTable();
    ~SonicNHGTable();
    int addNhg();
    int delNhg();
    SonicNHGEntry *getEntry(std::string key);
    SonicNHGEntry *getEntry(uint32_t id);

private:
    map<uint32_t, RIBNhgEntry *> m_nhg_map;
    ProducerStateTable m_nexthop_groupTable;
};

class SonicNHGEntry {

public:
    SonicNHGEntry();
    void create_nhg_entry();

private:
    NexthopKey m_key;
};

// TODO
class SonicNHGObject {};

#endif// NHGMGR_H
