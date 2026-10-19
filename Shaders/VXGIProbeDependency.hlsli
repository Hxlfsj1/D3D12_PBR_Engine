#ifndef VXGI_PROBE_DEPENDENCY_HLSLI
#define VXGI_PROBE_DEPENDENCY_HLSLI

// Fixed world-space coarse cells. Every segment includes misses, so a new
// blocker entering previously empty space still invalidates its dependants.
struct VXGIProbeDependencyDomain
{
    float3 origin; uint generation;
    float3 cellSize; uint cellCount;
    uint3 dimensions; uint padding;
};

void VXGIProbeDependencyMarkCell(int3 cell, VXGIProbeDependencyDomain domain, inout uint mask[8])
{
    if (any(cell < 0) || any(cell >= int3(domain.dimensions))) return;
    uint index = uint(cell.x) + domain.dimensions.x * (uint(cell.y) + domain.dimensions.y * uint(cell.z));
    if (index < min(domain.cellCount, 256)) mask[index >> 5] |= 1u << (index & 31);
}

// Mark both sides of a coarse grid boundary, including parallel-on-plane rays.
void VXGIProbeDependencyMarkPoint(float3 gridPoint, VXGIProbeDependencyDomain domain, inout uint mask[8])
{
    float3 tolerance = 0.00003 * max(abs(gridPoint), 1.0);
    int3 lo = int3(floor(gridPoint - tolerance));
    int3 hi = int3(floor(gridPoint + tolerance));
    [unroll] for (uint corner = 0; corner < 8; ++corner)
        VXGIProbeDependencyMarkCell(int3((corner & 1) ? hi.x : lo.x,
            (corner & 2) ? hi.y : lo.y, (corner & 4) ? hi.z : lo.z), domain, mask);
}

void VXGIProbeDependencySegment(float3 start, float3 end, VXGIProbeDependencyDomain domain, inout uint mask[8])
{
    if (domain.cellCount == 0 || domain.cellCount > 256 || any(domain.dimensions == 0)
        || any(domain.cellSize <= 0.0)) return;
    if (!all(isfinite(start)) || !all(isfinite(end)))
    {
        [unroll] for (uint word = 0; word < 8; ++word) mask[word] = 0xffffffff;
        return;
    }
    float3 a = (start - domain.origin) / domain.cellSize;
    float3 delta = (end - start) / domain.cellSize;
    float first = 0.0, last = 1.0;
    [unroll] for (uint axis = 0; axis < 3; ++axis)
    {
        if (delta[axis] == 0.0)
        {
            if (a[axis] < 0.0 || a[axis] > float(domain.dimensions[axis])) return;
        }
        else
        {
            float t0 = -a[axis] / delta[axis];
            float t1 = (float(domain.dimensions[axis]) - a[axis]) / delta[axis];
            first = max(first, min(t0, t1));
            last = min(last, max(t0, t1));
        }
    }
    if (first > last) return;
    VXGIProbeDependencyMarkPoint(a + delta * first, domain, mask);
    if (first == last) return;
    float t = first;
    // A segment crosses at most nx+ny+nz cell planes. The loose cellCount
    // bound also handles a highly elongated 256x1x1 domain without truncation.
    [loop] for (uint iteration = 0; iteration < domain.cellCount + 4; ++iteration)
    {
        float3 p = a + delta * t;
        float next = last;
        [unroll] for (uint axis = 0; axis < 3; ++axis)
        {
            if (delta[axis] == 0.0) continue;
            float plane = delta[axis] > 0.0 ? floor(p[axis] + 0.00003) + 1.0 : ceil(p[axis] - 0.00003) - 1.0;
            float crossing = (plane - a[axis]) / delta[axis];
            if (crossing > t) next = min(next, crossing);
        }
        VXGIProbeDependencyMarkPoint(a + delta * ((t + next) * 0.5), domain, mask);
        VXGIProbeDependencyMarkPoint(a + delta * next, domain, mask);
        if (next >= last) return;
        if (next <= t) break;
        t = next;
    }
    // Never silently drop dependencies if floating-point progress stalls.
    [unroll] for (uint word = 0; word < 8; ++word) mask[word] = 0xffffffff;
}

#endif
