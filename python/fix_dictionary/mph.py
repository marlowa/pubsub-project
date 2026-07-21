"""Build a perfect hash over a static set of integer FIX tags.

FIX validation looks up every field's tag many times (dense index, data format,
enumerated values). Those lookups are currently binary searches over the ~6000
known tags. Because the tag set is fixed at generation time, a perfect hash gives
each tag a distinct slot with no collision chains, so a lookup is two integer hash
evaluations plus one array probe plus one verify compare.

This is a compress-hash-displace (CHD) perfect hash. It is *not* minimal: the slot
table is padded to a power of two so the modulo becomes a bitmask, which matters
more for a hot-path lookup than saving the unused slots. The same integer mix is
implemented identically in the emitted C++.
"""

from __future__ import annotations

from typing import Dict, List, NamedTuple, Tuple

# A fixed bucket seed keeps generation deterministic; it only needs to decorrelate
# bucket assignment from within-bucket slot placement.
_BUCKET_SEED = 0x9747B28C

_MASK32 = 0xFFFFFFFF


def mix(key: int, seed: int) -> int:
    """A 32-bit integer hash of ``key`` under ``seed`` (fmix32-style avalanche).

    Reproduced byte-for-byte by ``mph_mix`` in the generated header.
    """
    value = (key ^ ((seed * 0x9E3779B1) & _MASK32)) & _MASK32
    value ^= value >> 16
    value = (value * 0x7FEB352D) & _MASK32
    value ^= value >> 15
    value = (value * 0x846CA68B) & _MASK32
    value ^= value >> 16
    return value


def _next_power_of_two(value: int) -> int:
    result = 1
    while result < value:
        result <<= 1
    return result


class PerfectHash(NamedTuple):
    """The parameters of a built perfect hash: sizes, displacements, key->slot map."""

    size: int
    bucket_count: int
    bucket_seed: int
    displacements: List[int]
    slot_of_key: Dict[int, int]


def hash_slot(perfect_hash: PerfectHash, key: int) -> int:
    """Return the slot a key hashes to (mirrors the emitted C++ field_index)."""
    bucket = mix(key, perfect_hash.bucket_seed) & (perfect_hash.bucket_count - 1)
    return mix(key, perfect_hash.displacements[bucket]) & (perfect_hash.size - 1)


def build_perfect_hash(keys: List[int]) -> PerfectHash:
    """Build a CHD perfect hash over the distinct integer ``keys``.

    Raises RuntimeError only if a bucket cannot be placed within the displacement
    search limit, which does not happen for the FIX tag set at the sizes chosen.
    """
    distinct = sorted(set(keys))
    if not distinct:
        return PerfectHash(1, 1, _BUCKET_SEED, [0], {})

    size = _next_power_of_two(len(distinct))
    if len(distinct) > size * 3 // 4:  # keep the load factor comfortable for CHD
        size <<= 1
    bucket_count = max(1, _next_power_of_two(len(distinct) // 4))

    buckets: List[List[int]] = [[] for _ in range(bucket_count)]
    for key in distinct:
        buckets[mix(key, _BUCKET_SEED) & (bucket_count - 1)].append(key)

    displacements = [0] * bucket_count
    slot_of_key: Dict[int, int] = {}
    occupied = [False] * size

    for bucket_index in sorted(range(bucket_count), key=lambda index: len(buckets[index]), reverse=True):
        if not buckets[bucket_index]:
            continue
        displacement, placed = _place_bucket(buckets[bucket_index], size, occupied)
        displacements[bucket_index] = displacement
        for key, slot in placed.items():
            occupied[slot] = True
            slot_of_key[key] = slot

    return PerfectHash(size, bucket_count, _BUCKET_SEED, displacements, slot_of_key)


def _place_bucket(bucket: List[int], size: int, occupied: List[bool]) -> Tuple[int, Dict[int, int]]:
    """Find a displacement that maps every key in ``bucket`` to a distinct free slot."""
    displacement = 0
    while displacement < 1_000_000:
        placed: Dict[int, int] = {}
        used_here: set = set()
        clash = False
        for key in bucket:
            slot = mix(key, displacement) & (size - 1)
            if occupied[slot] or slot in used_here:
                clash = True
                break
            used_here.add(slot)
            placed[key] = slot
        if not clash:
            return displacement, placed
        displacement += 1
    raise RuntimeError("perfect hash construction exceeded displacement limit")
