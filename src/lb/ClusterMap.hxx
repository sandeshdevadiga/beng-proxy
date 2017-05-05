/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_LB_CLUSTER_MAP_HXX
#define BENG_LB_CLUSTER_MAP_HXX

#include "Cluster.hxx"

#include <string>
#include <map>

class MyAvahiClient;
struct LbTranslationHandlerConfig;

class LbClusterMap {
    std::map<std::string, LbCluster> clusters;

public:
    void Scan(const LbConfig &config, MyAvahiClient &avahi_client);
    void Scan(const LbGoto &g, MyAvahiClient &avahi_client);

    LbCluster *Find(const std::string &name) {
        auto i = clusters.find(name);
        return i != clusters.end()
            ? &i->second
            : nullptr;
    }

private:
    void Scan(const LbTranslationHandlerConfig &config,
              MyAvahiClient &avahi_client);
    void Scan(const LbGotoIfConfig &config, MyAvahiClient &avahi_client);
    void Scan(const LbBranchConfig &config, MyAvahiClient &avahi_client);
    void Scan(const LbListenerConfig &config, MyAvahiClient &avahi_client);

    void Scan(const LbClusterConfig &config, MyAvahiClient &avahi_client);
};

#endif
