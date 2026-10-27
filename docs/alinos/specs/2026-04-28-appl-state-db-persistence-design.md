# APPL_STATE_DB Persistence Enhancement Design

## Overview

This document describes the extension of APPL_STATE_DB persistence at SIGTERM to cover additional fpmsyncd state. Currently, NHG RIB/FIB state is persisted via `NHGWarmRestartAssist`. This design introduces a generic state persistence framework (`WarmRestartStateManager` + `WarmRestartStateAdapter` interface) so that additional subsystems can be persisted with shared infrastructure and independent deployment.

The current iteration focuses on NHG only (RIB/FIB + NextHopGroup map + SonicIDAllocator). SRv6 and other subsystems will be added as separate adapters in future iterations.

### Goals

- Zero traffic loss during warm reboot for all covered state types
- Independent deployment per subsystem — each adapter can be enabled/disabled separately
- Full backward compatibility with existing warm reboot mechanisms
- Shared infrastructure to avoid code duplication across subsystems

### Scope

| Subsystem | Implementation | Notes |
|-----------|---------------|-------|
| NHG RIB/FIB + NextHopGroup map + SonicIDAllocator | `NHGWarmRestartAssist` implements `WarmRestartStateAdapter` | Extends existing class with adapter interface; adds NextHopGroup map persistence and ID allocator recovery |

Out of scope (future iterations):
- SRv6 MySid / SID list persistence
- Non-IP route types (label, VNet, EVPN)

## Architecture

```
┌──────────────────────────────────────────────┐
│          WarmRestartStateManager              │
│                                              │
│  - registerAdapter(adapter)                  │
│  - saveAll() / restoreAll() / reconcileAll() │
│  - timeout/integrity/per-adapter enable      │
└──────────────────┬───────────────────────────┘
                   │
             ┌─────▼──────────────────────┐
             │  NHGWarmRestartAssist      │
             │  (implements               │
             │   WarmRestartStateAdapter) │
             └────────────────────────────┘
```

**WarmRestartStateManager** is the single orchestrator, owned by `RouteSync`. Adapters are registered at startup; the manager calls them in order.

**NHGWarmRestartAssist** implements `WarmRestartStateAdapter` directly (Option 3 from design discussion). No separate `NHGAdapter` class — the existing class gains the adapter interface via multiple inheritance. This preserves `NHGMgr`'s ownership of `NHGWarmRestartAssist` and avoids containment/circular dependency issues.

**Future extension:** When SRv6 is added, a separate `SRv6Adapter` class implements `WarmRestartStateAdapter` and registers with the same manager. Reconcile order: NHG first (ID space + NHG objects must exist before SRv6 references them), then SRv6.

## Adapter Interface

```cpp
class WarmRestartStateAdapter {
public:
    virtual ~WarmRestartStateAdapter() = default;

    // Identity
    virtual std::string name() const = 0;

    // Lifecycle
    virtual void saveState() = 0;       // SIGTERM: persist to APPL_STATE_DB
    virtual void restoreState() = 0;    // Startup: load from APPL_STATE_DB
    virtual void reconcile() = 0;       // Warm reboot: compare + sync

    // Integrity
    virtual bool checkIntegrity() const = 0;
    virtual void clearState() = 0;

    // Configuration
    virtual bool isEnabled() const = 0;
};
```

**Design decisions:**

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Virtual interface | Abstract base class with pure virtual functions | C++ idiom; equivalent to interface |
| `name()` | Per-adapter | Used for logging, CONFIG_DB key lookup, debugging |
| `checkIntegrity()` | Separate method | Manager can verify saved state is complete before restore |
| `clearState()` | Separate method | Clean up APPL_STATE_DB after successful reconcile or on corruption |
| `isEnabled()` | Adapter checks its own CONFIG_DB flag | Decentralized config; each adapter reads its own flag |

**NHGWarmRestartAssist with adapter interface:**

```cpp
class NHGWarmRestartAssist : public AppRestartAssist,
                             public WarmRestartStateAdapter {
public:
    // WarmRestartStateAdapter interface
    std::string name() const override { return "NHG"; }
    void saveState() override;       // delegates to saveStateToDB()
    void restoreState() override;    // delegates to readNHGTablesToMap()
    void reconcile() override;       // delegates to reconcileAll()
    bool checkIntegrity() const override;
    void clearState() override;
    bool isEnabled() const override;

    // Existing public interface preserved
    void saveStateToDB(...);
    void readNHGTablesToMap();
    void reconcileAll();
    // ... rest unchanged
};
```

**Manager registration and execution:**

```cpp
class WarmRestartStateManager {
public:
    WarmRestartStateManager(RedisPipeline *pipeline);

    void registerAdapter(std::shared_ptr<WarmRestartStateAdapter> adapter);

    void saveAll();
    void restoreAll();
    void reconcileAll();

    bool isAnyAdapterEnabled() const;

private:
    RedisPipeline *m_pipeline;
    std::vector<std::shared_ptr<WarmRestartStateAdapter>> m_adapters;

    static constexpr int SAVE_TIMEOUT_SEC = 5;
};
```

**Mapping from current code:**

| Current | New |
|---------|-----|
| `NHGWarmRestartAssist::saveStateToDB()` | `NHGWarmRestartAssist::saveState()` (delegates) |
| `NHGWarmRestartAssist::readNHGTablesToMap()` | `NHGWarmRestartAssist::restoreState()` (delegates) |
| `NHGWarmRestartAssist::reconcileAll()` | `NHGWarmRestartAssist::reconcile()` (delegates) |
| `SonicIDAllocator::recoverSonicIDMapFromDB()` | Part of `NHGWarmRestartAssist::restoreState()` |
| `sigterm_handler` + `saveNHGStateForWarmReboot()` | `WarmRestartStateManager::saveAll()` |

## SIGTERM Flow

**Replaces current `saveNHGStateForWarmReboot()`:**

```
SIGTERM signal
    → sigterm_handler() sets g_sigterm_received = 1  (unchanged)

Main loop detects g_sigterm_received
    → WarmRestartStateManager::saveAll()
        for each adapter in registration order:
            if adapter.isEnabled():
                adapter.saveState()
        pipeline->flush()
    → exit(0)
```

**Key differences from current code:**

| Current | New |
|---------|-----|
| `saveNHGStateForWarmReboot()` — hardcoded for NHG only | `saveAll()` — iterates all registered adapters |
| One pipeline flush inside `saveNHGStateForWarmReboot()` | One pipeline flush after all adapters save |
| 5-second timeout inside `NHGWarmRestartAssist` | 5-second timeout managed by `WarmRestartStateManager` |

**Timeout handling:**

`saveAll()` tracks elapsed time across all adapters. If total exceeds `SAVE_TIMEOUT_SEC`, it logs a warning and stops. Each adapter's `saveState()` should be fast (batch Redis writes), but the manager guards the total budget.

```cpp
void WarmRestartStateManager::saveAll() {
    auto start = std::chrono::steady_clock::now();
    for (auto& adapter : m_adapters) {
        if (!adapter->isEnabled()) continue;

        auto elapsed = std::chrono::steady_clock::now() - start;
        if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count()
            >= SAVE_TIMEOUT_SEC) {
            SWSS_LOG_ERROR("saveAll timeout after %d sec, skipping %s",
                           SAVE_TIMEOUT_SEC, adapter->name().c_str());
            break;
        }
        adapter->saveState();
    }
    m_pipeline->flush();
}
```

## Startup & Recovery Flow

**Startup sequence (after fpmsyncd starts and detects warm start):**

```
fpmsyncd starts
    → check WarmStart::isWarmStart()
    → if warm start:
        1. WarmRestartStateManager::restoreAll()
            for each registered adapter:
                if adapter.isEnabled():
                    if !adapter.checkIntegrity():
                        SWSS_LOG_WARN("State corrupt for %s, clearing", name)
                        adapter->clearState()
                        continue   // adapter enters cold-start path
                    adapter->restoreState()
        2. Existing WarmStartHelper::runRestoration()  (routes, unchanged)
        3. Start warm restart timer + EOIU timer       (unchanged)
    → if not warm start:
        adapters do not restore, normal startup
```

**NHGWarmRestartAssist::restoreState() detail:**

```
NHGWarmRestartAssist::restoreState()
    → read APPL_STATE_DB NHG_RIB_STATE_TABLE      (existing logic)
    → read APPL_STATE_DB NHG_SONIC_STATE_TABLE    (existing logic)
    → read APPL_STATE_DB NHG_GROUP_STATE_TABLE    (NEW — NextHopGroup map)
    → load APP_DB Sonic NHG TABLE                 (existing logic)
    → load APP_DB Route TABLE                     (existing logic, builds ref count)
    → recover SonicIDAllocator from restored sonic NHG entries
    → populate internal temp tables with restored entries
    → rebuild m_nh_groups from NHG_GROUP_STATE_TABLE
```

**Integrity checks:**

| Adapter | checkIntegrity() |
|---------|-------------------|
| NHG | Entry count > 0; verify key fields (zebra_id, sonic_obj_id) exist; verify cross-reference consistency between RIB and Sonic tables |

**Failure paths:**

| Scenario | Behavior |
|----------|----------|
| APPL_STATE_DB empty (fresh install) | `checkIntegrity()` returns false, `clearState()` is no-op, adapter cold-starts |
| Data corruption (missing fields) | `checkIntegrity()` returns false, cleared, adapter cold-starts |
| Adapter disabled | `isEnabled()` is false, restore completely skipped |
| Partial save (timeout) | Some tables written, others missing; `checkIntegrity()` catches inconsistency |

## Reconcile Flow

**Triggered when warm restart timer or EOIU hold timer expires:**

```
warmStartTimer / eoiuHoldTimer expires
    → sync.onWarmStartEnd(applStateDb)
        → WarmRestartStateManager::reconcileAll()
            for each adapter in registration order:
                if adapter.isEnabled():
                    adapter.reconcile()
                    adapter.clearState()    // clean up APPL_STATE_DB
        → existing route reconciliation (WarmStartHelper, unchanged)
```

### NHGWarmRestartAssist::reconcile()

Extends current reconcile logic:

```
NHGWarmRestartAssist::reconcile()
    → reconcileNHG()
        for each entry in temp_rib_table:
            SAME:  preserve sonic_obj_id, update zebra_id mapping
            NEW:   allocate new sonic_obj_id via SonicIDAllocator
            DELETE + ref_count==0: remove from APPL_DB
            DELETE + ref_count>0:  preserve, log WARN
        → migrateToRIBTable()   // write into NHGMgr's live tables
    → reconcileRoutes()
        resolve zebra_id → sonic_obj_id from NHGMgr RIBNHGTable
        write updated ROUTE_TABLE entries
    → NextHopGroup map reconcile (NEW)
        compare saved m_nh_groups with Zebra re-pushed NHG messages
        SAME:  preserve installed state
        NEW:   install NHG
        DELETE: remove from m_nh_groups, deleteNextHopGroupDb()
```

**Reconcile state machine** (same STALE/SAME/NEW/DELETE/PENDING states):

```
  STALE ──(Zebra re-push matches)──► SAME
  STALE ──(Zebra re-push, different)──► NEW
  STALE ──(no Zebra re-push)──► DELETE
  NEW  ──(depends resolved)──► process
  PENDING ──(depends resolved)──► process
```

## Data Model

### NHG Adapter Tables

**NHG_RIB_STATE_TABLE** (existing, unchanged)

Key = `<zebra_id>`

| Field | Description |
|-------|-------------|
| `sonic_obj_id` | Sonic NHG ID (0 for single hop) |
| `sonic_obj_type` | 0=NORMAL, 1=SRv6_GATEWAY |
| `sonic_gateway_nhg_id` | SRv6 PIC Context ID |
| `af` | Address family |
| `nexthop` | Compare field |
| `ifname` | Compare field |
| `weight` | Compare field |
| `vpn_sid` | SRv6 VPN SID |
| `seg_src` | SRv6 Segment Source |
| `group` | Member list: `<zebra_id>:<weight>[,...]` |

**NHG_SONIC_STATE_TABLE** (existing, unchanged)

Key = `<type>:<sonic_id>`

| Field | Description |
|-------|-------------|
| `nexthop` | SonicNHGObjectKey field |
| `ifname` | SonicNHGObjectKey field |
| `vpn_sid` | VPN SID |
| `seg_src` | Segment Source |
| `group_members` | `<zebra_id>:<weight>[,...]` |

**NHG_GROUP_STATE_TABLE** (NEW — for NextHopGroup map)

Key = `<nhg_id>` (uint32 as string)

| Field | Description |
|-------|-------------|
| `nexthop` | Nexthop IP string |
| `intf` | Interface name |
| `installed` | 0 or 1 |

### schema.h Additions

```cpp
// NHG NextHopGroup map state
#define APP_NHG_GROUP_STATE_TABLE_NAME   "NHG_GROUP_STATE_TABLE"
```

## Configuration

| Flag | Location | Default | Description |
|------|----------|---------|-------------|
| `nhg-warm-reboot-enabled` | `CONFIG_DB` `DEVICE_METADATA\|localhost` | `"enabled"` | NHG adapter (existing, unchanged) |
| `rib-fib-nhg-warm-reboot` | `CONFIG_DB` `DEVICE_METADATA\|localhost` | `"disabled"` | RIB/FIB path (existing, unchanged) |

Each adapter reads its own flag in `isEnabled()`. No new global flag — the manager just checks per-adapter.

## Backward Compatibility

| Scenario | Behavior |
|----------|----------|
| `nhg-warm-reboot-enabled=disabled` | NHG adapter skips save/restore/reconcile, existing WarmStartHelper NHG path unaffected |
| `rib-fib-nhg-warm-reboot=disabled` | NHG adapter falls back to legacy WarmStartHelper NHG path (existing behavior) |
| Old fpmsyncd reads new APPL_STATE_DB tables | Old code ignores unknown tables — no impact |
| New fpmsyncd without saved NextHopGroup map state | `checkIntegrity()` returns false for NHG_GROUP_STATE_TABLE, adapter cold-starts NHG map, no error |
| `WarmStartHelper` route restoration | Completely unchanged — manager does not touch route restoration |

**Key principle**: `WarmRestartStateManager` is additive. It only adds new save/restore paths. Existing `WarmStartHelper` for routes and legacy NHG path remain untouched.

## Error Handling & Degradation

Each adapter's save/restore/reconcile is independent — a failure in one adapter does not block others.

### Failure Handling by Stage

| Stage | Failure | Handling |
|-------|---------|----------|
| saveAll() | Single adapter throws | Catch exception, log error, continue to next adapter. Partial save for failed adapter is acceptable. |
| saveAll() | Timeout before all adapters save | Skip remaining adapters, flush what we have. Skipped adapters will cold-start. |
| restoreAll() | checkIntegrity() fails | Clear state table, adapter cold-starts. Other adapters unaffected. |
| restoreAll() | restoreState() throws | Catch exception, clear state, adapter cold-starts. |
| reconcileAll() | Single adapter reconcile fails | Catch exception, log error, clear state, continue to next adapter. |

### Degradation Matrix

| Failure | NHG | Routes (WarmStartHelper) |
|---------|-----|--------------------------|
| NHG save fails | Cold start | Unaffected |
| NHG state corrupt | Cold start | Unaffected |
| SIGTERM timeout (5s) | Partial or none | N/A (not in saveAll) |

### Error Handling Pattern (all stages)

```cpp
void WarmRestartStateManager::saveAll() {
    auto start = std::chrono::steady_clock::now();
    for (auto& adapter : m_adapters) {
        if (!adapter->isEnabled()) continue;

        auto elapsed = std::chrono::steady_clock::now() - start;
        if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count()
            >= SAVE_TIMEOUT_SEC) {
            SWSS_LOG_ERROR("saveAll timeout, skipping %s", adapter->name().c_str());
            break;
        }
        try {
            adapter->saveState();
        } catch (const std::exception& e) {
            SWSS_LOG_ERROR("saveState failed for %s: %s",
                           adapter->name().c_str(), e.what());
        }
    }
    m_pipeline->flush();
}
```

Same try/catch-per-adapter pattern for `restoreAll()` and `reconcileAll()`.
