#include "nhgmgr.h"
#include <string.h>
#include "logger.h"

using namespace std;

NHGMgr::NHGMgr(RedisPipeline *pipeline) :m_rib_nhg_table(pipeline, APP_NEXTHOP_GROUP_TABLE_NAME, true),
{
    m_rib_nhg_table = new RIBNhgTable(pipeline)
}
NHGMgr::~NHGMgr()
{
    if (m_rib_nhg_table != nullptr){
        delete m_rib_nhg_table;
    }
}

int NHGMgr::addNHG(const NextHopGroupFull nhg)
{
    int ret = 0;
    if (m_rib_nhg_table.isNhgExist(nhg.id))
    {
        RIBNhgEntry *entry = m_rib_nhg_table.getEntry(nhg.id);
        m_rib_nhg_table.updateNhg(nhg.id, nhg);
    }
    else
    {
        ret = m_rib_nhg_table.addNhg(nhg);
    }
    // TODO: srv6 sonic nhg create
    return ret;
}

int NHGMgr::delNHG(uint32_t id)
{
    if (!m_rib_nhg_table.isNhgExist(id))
    {
        SWSS_LOG_ERROR("NextHop group id %d not found.", id);
        return 0;
    }

    // TODO: del nhg from sonic table
    return m_rib_nhg_table.delNhg(id);
}

bool NHGMgr::getIfName(int if_index, char *if_name, size_t name_len)
{
    if (!if_name || name_len == 0)
    {
        return false;
    }

    memset(if_name, 0, name_len);

    /* Cannot get interface name. Possibly the interface gets re-created. */
    if (!rtnl_link_i2name(m_link_cache, if_index, if_name, name_len))
    {
        /* Trying to refill cache */
        nl_cache_refill(m_nl_sock, m_link_cache);
        if (!rtnl_link_i2name(m_link_cache, if_index, if_name, name_len))
        {
            return false;
        }
    }

    return true;
}


RIBNhgEntry *NHGMgr::getRIBNhgEntryByKey(string key)
{
    return m_rib_nhg_table.getEntry(key);
}

RIBNhgEntry *NHGMgr::getRIBNhgEntryByRIBID(uint32_t id)
{
    return m_rib_nhg_table.getEntry(id);
}

// TODO: add sonic object creation
SonicNHGObject *NHGMgr::getSonicNHGByKey(std::string key)
{
    return nullptr;
}

SonicNHGObject *NHGMgr::getSonicNHGByID(uint32_t id)
{
    return nullptr;
}
void NHGMgr::dump_zebra_nhg_table(string &ret) {
    m_rib_nhg_table->dump_table(ret);
}
RIBNhgTable::RIBNhgTable()
{
}

RIBNhgEntry *RIBNhgTable::getEntry(string key)
{
    auto it = m_key_2_id_map.find(key);
    if (it == m_key_2_id_map.end())
    {
        return nullptr;
    }
    it = m_nhg_map.find(it->second);
    if (it == m_nhg_map.end())
    {
        return nullptr;
    }
    return it->second;
}

RIBNhgEntry *RIBNhgTable::getEntry(uint32_t id)
{
    auto it = m_nhg_map.find(id);
    if (it == m_key_2_id_map.end())
    {
        return nullptr;
    }
    return it->second;
}

int RIBNhgTable::delNhg(uint32_t id)
{
    if (m_nhg_map.find(id) == m_nhg_map.end())
    {
        SWSS_LOG_ERROR("NextHop group id %d not found.", id);
        return 0;
    }

    RIBNhgEntry *entry = m_nhg_map[id];
    if (entry->getDependentsID().size() != 0)
    {
        SWSS_LOG_ERROR("NextHop group id %d still has dependents.", id);
        return -1;
    }

    // TODO: update depends and dependents

    m_key_2_id_map.erase(entry->getKey());
    delete entry;
    m_nhg_map.erase(id);
    return 0;
}

int RIBNhgTable::addNhg(NextHopGroupFull nhg)
{
    if (m_nhg_map.find(nhg->getId()) != m_nhg_map.end())
    {
        SWSS_LOG_ERROR("NextHop group id %d key %s already exists.", nhg->id);
        return -1;
    }

    RIBNhgEntry *entry = RIBNhgEntry::create_nhg_entry(nhg, this);
    if (entry == nullptr)
    {
        SWSS_LOG_ERROR("Failed to create nhg entry for %s", nhg->getKey());
        return -1;
    }
    if( writeToDB(nhg->getId)!=0){
        SWSS_LOG_ERROR("Failed to write to DB for %s", nhg->getKey());
        return -1;
    }

    // TODO: update depends and dependents

    m_nhg_map.insert(std::make_pair(nhg->getId(), entry));
    m_key_2_id_map.insert(std::make_pair(entry->getKey(), entry));
    return 0;
}

int RIBNhgTable::updateNhg(NextHopGroupFull nhg)
{
    if (m_nhg_map.find(nhg.getId()) == m_nhg_map.end())
    {
        SWSS_LOG_ERROR("NextHop group id %d key %s not exists.", nhg.id);
        return -1;
    }

    RIBNhgEntry *entry = m_nhg_map.find(nhg->getId())->second();

    int ret = entry->setEntry(nhg);
    if (ret != 0)
    {
        SWSS_LOG_ERROR("Failed to create nhg entry for %s", nhg->getKey());
        return -1;
    }

    ret = writeToDB(nhg->getId());
    if (ret < 0){
        SWSS_LOG_ERROR("Failed to write to DB for %s", nhg->getKey());
        return -1;
    }

    // TODO: update depends and dependents

    m_nhg_map.insert(std::make_pair(nhg.id, entry));
    m_key_2_id_map.insert(std::make_pair(entry->getKey(), entry));
    return 0;
}

void RIBNhgTable::add_nhg_dependents(RIBNhgEntry *entry)
{
    for (int i = 0; i < entry->getNhg().group.size(); i++)
    {
        m_nhg_map[entry->getNhg().group[i].first]->m_depends.push_back(entry->getNhg().getId());
    }
}

void RIBNhgTable::remove_nhg_dependents(RIBNhgEntry *entry)
{
    for (int i = 0; i < entry->getNhg().group.size(); i++)
    {
        m_nhg_map[entry->getNhg().group[i].first]->m_depends.push_back(entry->getNhg().id);
    }
}

bool RIBNhgTable::isNhgExist(string key)
{
    auto it = m_key_2_id_map.find(key);
    if (it != m_key_2_id_map.end())
    {
        return true;
    }
    return false;
}

bool RIBNhgTable::isNhgExist(uint32_t id)
{
    auto it = m_nhg_map.find(key);
    if (it != m_nhg_map.end())
    {
        return true;
    }
    return false;
}
int RIBNhgTable::writeToDB(uint32_t id) {

    auto it = m_nhg_map.find(id);
    if (it == m_nhg_map.end())
    {
        SWSS_LOG_ERROR("NextHop group id %d not found.", id);
        return -1;
    }
    if (it->second->getGroup().size() == 0){
        SWSS_DEBUG("NextHop group id %d has single path", id);
        return 0;
    }
    vector<FieldValueTuple> fvVector = it->second->getFvVector();
    if (fvVector.size() == 0)
    {
        if(it->second->syncFvVector()!=0)
        {
            SWSS_LOG_ERROR("Failed to sync fvVector for %s", it->second->getKey());
            return -1;
        }
    }

    return m_nexthop_groupTable.set(it->second->getNhg().id, it->second->getFvVector());

}
RIBNhgTable::RIBNhgTable(RedisPipeline *pipeline, string table_name, bool is_state_table): m_nexthop_groupTable(pipeline, table_name, is_state_table) {

}
RIBNhgTable::~RIBNhgTable() {
}
void RIBNhgTable::dump_table(string &ret) {
string indent = "    ";
    for (auto it = m_nhg_map.begin(); it != m_nhg_map.end(); it++)
    {
        NextHopGroupFull nhg = it->second->getNhg();
        ret += "nhg id: " + to_string(nhg.id) + ":\n";
        if (it->second->isInstalled())
        {
            ret += indent + "installed\n";
        }
        else
        {
            ret += indent + "not installed\n";
        }
        if (it->second->getGroup().size() != 0){
            ret += indent + "group: ";
            for (auto it2 = it->second->getGroup().begin(); it2 != it->second->getGroup().end(); it2++)
            {
                ret += to_string(it2->first) + ":" + to_string(it2->second) + ",";
            }
        }

        if (!it->second->getNexthop().empty())
        {
            ret += indent + "nexthop: " + it->second->getNexthop() + "\n";
        }

        switch (nhg.type)
        {
            case NEXTHOP_TYPE_IFINDEX:
                ret += indent + "type: " + "NEXTHOP_TYPE_IFINDEX" + "\n";
                ret += indent + "interface: " + it->nhg->ifname + "\n";
                break ;
            case NEXTHOP_TYPE_IPV4:
                ret += indent + "type: " + "NEXTHOP_TYPE_IPV4" + "\n";
                break;
            case NEXTHOP_TYPE_IPV4_IFINDEX:
                ret += indent + "type: " + "NEXTHOP_TYPE_IPV4_IFINDEX" + "\n";
                ret += indent + "interface: " + it->nhg->ifname + "\n";
                break;
            case NEXTHOP_TYPE_IPV6_IFINDEX:
                ret += indent + "type: " + "NEXTHOP_TYPE_IPV6_IFINDEX" + "\n";
                ret += indent + "interface: " + it->nhg->ifname + "\n";
                break;
            case NEXTHOP_TYPE_IPV6:
                ret += indent + "type: " + "NEXTHOP_TYPE_IPV6" + "\n";
                break;
            default:
                SWSS_LOG_ERROR("Unknown NextHop type %d", nhg.type);
                return -1;
        }
    }
}

RIBNhgEntry *RIBNhgEntry::create_nhg_entry(NextHopGroupFull nhg, RIBNhgTable* table)
{

    RIBNhgEntry *entry = new RIBNhgEntry(table);
    int ret = entry->setEntry(nhg);
    if (ret != 0)
    {
        delete entry;
        return nullptr;
    }
    return entry;
}

vector<pair<uint32_t, uint8_t>> RIBNhgEntry::getGroup()
{
    return m_group;
}

vector<pair<uint32_t, uint8_t>> RIBNhgEntry::getResolvedGroup()
{
    return m_resolved_group;
}

vector<RIBNhgEntry *> RIBNhgEntry::getDependsID()
{
    return m_depends;
}

vector<RIBNhgEntry *> RIBNhgEntry::getDependentsID()
{
    return m_dependents;
}

NexthopKey RIBNhgEntry::getKey()
{
    return m_key;
}

NextHopGroupFull RIBNhgEntry::getNhg()
{
    return m_nhg;
}

int RIBNhgEntry::setEntry(NexthopGroupFull nhg)
{
    m_key = NexthopKey(&nhg);
    m_nhg = nhg;
    for (int i = 0; i < nhg.group.size(); i++)
    {

        // validate group member
        if (m_map.find(nhg.group[i].first) == m_map.end())
        {
            SWSS_LOG_ERROR("NextHop id %d in group not found.", nhg_id);
            return -1;
        }

        // update resolved group
        if (nhg.group[i].type != NEXTHOP_TYPE_RECURSIVED)
        {
            entry->resolved_group.push_back(nhg.group[i]);
        }

        // TODO: add dependent and depends
    }
    switchswitch (nhg.type)
    {
        case NEXTHOP_TYPE_IFINDEX:
            m_nexthop = "";
            break;
        case NEXTHOP_TYPE_IPV4:
            char gate_str[INET_ADDRSTRLEN];
            m_af = AF_INET;
            inet_ntop(AF_INET, &nhg->gate.ipv4, gate_str, INET_ADDRSTRLEN);
            m_nexthop = string(gate_str);
            break;
        case NEXTHOP_TYPE_IPV4_IFINDEX:
            char gate_str[INET_ADDRSTRLEN];
            m_af=AF_INET;
            inet_ntop(AF_INET, &nhg->gate.ipv4, gate_str, INET_ADDRSTRLEN);
            m_nexthop = string(gate_str);
            break;
        case NEXTHOP_TYPE_IPV6_IFINDEX:
            char gate_str[INET6_ADDRSTRLEN];
            m_af=AF_INET6;
            m_nexthop = inet_ntop(AF_INET6, &nhg->gate.ipv6, gate_str, INET6_ADDRSTRLEN);
            m_nexthop = string(gate_str);
            break;
        case NEXTHOP_TYPE_IPV6:
            char gate_str[INET6_ADDRSTRLEN];
            m_af=AF_INET6;
            inet_ntop(AF_INET6, &nhg->gate.ipv6, gate_str, INET6_ADDRSTRLEN);
            m_nexthop = string(gate_str);
            break;
        default:
            SWSS_LOG_ERROR("Unknown NextHop type %d", nhg.type);
            return -1;
    }

    return 0;
}

vector<FieldValueTuple> RIBNhgEntry::getFvVector() {
    return m_fvVector;
}

int RIBNhgEntry::syncFvVector() {
    string nexthops;
    string ifnames;
    string weights;
    // TODO: sonic id manager allocate the key
    string key = to_string(m_nhg.id);
    ifnames = m_nhg.ifname;
    vector<FieldValueTuple> fvVector;
    int ret = getNextHopGroupFields(nexthops, ifnames, weights, m_nhg.af);
    if (ret <0) {
        SWSS_LOG_ERROR("Failed to get field of %d", nhg.id);
    }

    FieldValueTuple nh("nexthop", nexthops.c_str());
    FieldValueTuple ifname("ifname", ifnames.c_str());
    fvVector.push_back(nh);
    fvVector.push_back(ifname);
    if(!weights.empty())
    {
        FieldValueTuple wg("weight", weights.c_str());
        fvVector.push_back(wg);
    }
    SWSS_LOG_INFO("NextHopGroup table set: key [%s] nexthop[%s] ifname[%s] weight[%s]", key.c_str(), nexthops.c_str(), ifnames.c_str(), weights.c_str());

    m_nexthop_groupTable.set(key.c_str(), fvVector);
    return 0;
}

int RIBNhgEntry::getNextHopGroupFields( string& nexthops, string& ifnames, string& weights, uint8_t af)
{
    if(m_nhg.group.size() == 0)
    {
        if(!m_nexthop.empty())
        {
            nexthops = m_nexthop;
        }
        else
        {
            nexthops = af == AF_INET ? "0.0.0.0" : "::";
        }
        ifnames = nhg.intf;
    }
    else
    {
        int i = 0;
        for(const auto& nh : m_nhg.group)
        {
            uint32_t id = nh.id;
            if(!m_table->isNhgExist(id))
            {
                SWSS_LOG_ERROR("NextHop group is incomplete: %d", nhg.id);
                return -1;
            }

            string weight = to_string(nh.weight);
            if(i)
            {
                nexthops += NHG_DELIMITER;
                ifnames += NHG_DELIMITER;
                weights += NHG_DELIMITER;
            }
            nexthops += nh.m_nexthop.empty() ? (af == AF_INET ? "0.0.0.0" : "::") : nh.nexthop;
            ifnames += nh.m_nhg.intf;
            weights += weight;
            ++i;
        }
    }
}

RIBNhgEntry::RIBNhgEntry() {
}
bool RIBNhgEntry::isInstalled() {
    return m_installed;
}
void RIBNhgEntry::setInstalled(bool installed) {
    m_installed = installed;
}
RIBNhgEntry::RIBNhgEntry(RIBNhgTable *table):m_table(table) {
}
string RIBNhgEntry::getNexthop() {
    return m_nexthop;
}

NexthopKey::NexthopKey(const NextHopGroupFull *nhg)
{
    key = "";
    for (int i = 0; i < nhg->group.size(); i++)
    {
        if (i == 0)
        {
            key = key + "group:";
        }
        key = key + "id" + nhg->group[i].id + "weight" + nhg->group[i].weight;
    }
    switch (nhg->type)
    {
    case NEXTHOP_TYPE_IFINDEX:
    {
        key = key + "type:" + nhg->type;
        key = key + "ifindex:" + nhg->ifindex;
        key = key + "vrf_id" + nhg->vrf_id;
        break ;
    }
    case NEXTHOP_TYPE_IPV4:
    {
        key = key + "type:" + nhg->type;
        key = key + "ifindex:" + nhg->ifindex;
        key = key + "vrf_id" + nhg->vrf_id;
        key = key + "gate" + nhg->gate.ipv4;
        break ;

    }
    case NEXTHOP_TYPE_IPV4_IFINDEX:
    {
        key = key + "type:" + nhg->type;
        key = key + "ifindex:" + nhg->ifindex;
        key = key + "vrf_id" + nhg->vrf_id;
        key = key + "gate" + nhg->gate.ipv4;
        break ;

    }
    case NEXTHOP_TYPE_IPV6:
    {
        key = key + "type:" + nhg->type;
        key = key + "ifindex:" + nhg->ifindex;
        key = key + "vrf_id" + nhg->vrf_id;
        key = key + "gate" + nhg->gate.ipv6;
        break ;

    }
    case NEXTHOP_TYPE_IPV6_IFINDEX:
    {
        key = key + "type:" + nhg->type;
        key = key + "ifindex:" + nhg->ifindex;
        key = key + "vrf_id" + nhg->vrf_id;
        key = key + "gate" + nhg->gate.ipv6;
        break ;

    }
    case NEXTHOP_TYPE_BLACKHOLE:
    {
        key = key + "type:" + nhg->type;
        key = key + "blackhole_type:" + nhg->bh_type;
        break ;
    }
    default:
    }
    m_address_key = key;
}

SonicNHGTable::SonicNHGTable()
{
}

SonicNHGTable::~SonicNHGTable()
{
    return;
}

int SonicNHGTable::addNhg()
{
    return 0;
}

int SonicNHGTable::delNhg()
{
    return 0;
}

SonicNHGEntry *SonicNHGTable::getEntry(std::string key)
{
    return nullptr;
}

SonicNHGEntry *SonicNHGTable::getEntry(uint32_t id)
{
    return nullptr;
}

SonicNHGEntry::SonicNHGEntry()
{
    return;
}

SonicNHGEntry::~SonicNHGEntry()
{
    return;
}

void SonicNHGEntry::create_nhg_entry()
{
    return;
}
