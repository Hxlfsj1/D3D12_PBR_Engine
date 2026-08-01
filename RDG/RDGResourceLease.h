#ifndef RDG_RESOURCE_LEASE_H
#define RDG_RESOURCE_LEASE_H

#include <memory>

#include <d3d12.h>
#include <wrl/client.h>

// ResourceManager keeps one shared reference while an RDG resource keeps a
// second reference for as long as the graph owns the transient resource.
struct RDGTransientResourceLeaseState
{
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    bool reusable = true;
};

using RDGTransientResourceLease =
    std::shared_ptr<RDGTransientResourceLeaseState>;

#endif
