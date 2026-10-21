// After resolve, leftover 32x32 blocks stay at quant 0 and overflow pack
// on a busy 720p frame. If analyze consumed more words than the budget,
// raise those leftovers and cap very coarse values. Under-budget frames
// (desktop) keep quant 0.

cbuffer Push : register(b0)
{
    uint target_bytes;
    uint block_count;
    uint floor_q;
    uint cap_q;
};

ByteAddressBuffer payload : register(t0);
RWByteAddressBuffer quant : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint i = dtid.x;
    if (i >= block_count)
        return;
    // Sum of the 32 quant stripe bins. Same total the old single counter held.
    uint consumed = 0u;
    [unroll] for (uint b = 0u; b < 32u; b++)
        consumed += payload.Load(8u + b * 4u);
    if (consumed <= target_bytes)
        return;
    uint q = quant.Load(i * 4u);
    uint nq = q;
    if (nq == 0u)
        nq = floor_q;
    else if (nq > cap_q)
        nq = cap_q;
    if (nq != q)
        quant.Store(i * 4u, nq);
}
