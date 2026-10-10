"""
AstraOS Physical Memory Manager (PMM) Host-Side Unit Tests
Simulates and verifies the exact PMM region-based bitmap allocator logic
against synthetic Limine memory maps.
"""

import sys

PAGE_SIZE = 4096

PMM_SUCCESS = 0
PMM_ERR_NOMEM = -1
PMM_ERR_UNALIGNED = -2
PMM_ERR_PAGE_ZERO = -3
PMM_ERR_UNMANAGED = -4
PMM_ERR_DOUBLE_FREE = -5
PMM_ERR_INVALID_ARG = -6

LIMINE_MEMMAP_USABLE = 0
LIMINE_MEMMAP_RESERVED = 1
LIMINE_MEMMAP_ACPI_RECLAIMABLE = 2
LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE = 5
LIMINE_MEMMAP_KERNEL_AND_MODULES = 6
LIMINE_MEMMAP_FRAMEBUFFER = 7

class PMMRegion:
    def __init__(self, base, end, pages, bmp_bytes):
        self.base = base
        self.end = end
        self.total_pages = pages
        self.free_pages = pages
        self.last_search_idx = 0
        self.bitmap = bytearray(bmp_bytes)

class PMMHostSimulator:
    def __init__(self):
        self.regions = []
        self.total_managed_pages = 0
        self.allocated_pages = 0
        self.free_pages = 0
        self.metadata_pages = 0

    def init(self, memmap_entries):
        self.regions = []
        self.total_managed_pages = 0
        self.allocated_pages = 0
        self.free_pages = 0
        self.metadata_pages = 0

        candidates = []
        total_bitmap_bytes = 0

        for base, length, mtype in memmap_entries:
            if mtype != LIMINE_MEMMAP_USABLE:
                continue

            # Arithmetic overflow check (64-bit unsigned)
            if (base + length) & 0xFFFFFFFFFFFFFFFF < base:
                continue

            start = (base + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1)
            end = (base + length) & ~(PAGE_SIZE - 1)

            # Reserve page zero
            if start == 0:
                start = PAGE_SIZE

            if end <= start:
                continue

            pages = (end - start) // PAGE_SIZE
            bmp_bytes = (pages + 7) // 8

            candidates.append((start, end, pages, bmp_bytes))
            total_bitmap_bytes += bmp_bytes

        if not candidates:
            return PMM_ERR_NOMEM

        # Metadata requirements (56 bytes per struct pmm_region on 64-bit)
        metadata_bytes = len(candidates) * 56 + total_bitmap_bytes
        metadata_pages = (metadata_bytes + (PAGE_SIZE - 1)) // PAGE_SIZE

        # Find host region
        host_idx = -1
        for i, (_, _, pages, _) in enumerate(candidates):
            if pages >= metadata_pages + 1:
                host_idx = i
                break

        if host_idx < 0:
            return PMM_ERR_NOMEM

        for i, (start, end, pages, bmp_bytes) in enumerate(candidates):
            reg = PMMRegion(start, end, pages, bmp_bytes)
            self.regions.append(reg)
            self.total_managed_pages += pages

        # Reserve metadata in host region bitmap
        host_reg = self.regions[host_idx]
        for p in range(metadata_pages):
            host_reg.bitmap[p // 8] |= (1 << (p % 8))
        host_reg.free_pages -= metadata_pages
        host_reg.last_search_idx = metadata_pages

        self.metadata_pages = metadata_pages
        self.allocated_pages = metadata_pages
        self.free_pages = self.total_managed_pages - metadata_pages

        return PMM_SUCCESS

    def alloc_page(self):
        if self.free_pages == 0:
            return PMM_ERR_NOMEM, 0

        for reg in self.regions:
            if reg.free_pages == 0:
                continue

            total = reg.total_pages
            start = reg.last_search_idx

            for offset in range(total):
                idx = (start + offset) % total
                byte_idx = idx // 8
                bit_mask = 1 << (idx % 8)

                if (reg.bitmap[byte_idx] & bit_mask) == 0:
                    reg.bitmap[byte_idx] |= bit_mask
                    reg.free_pages -= 1
                    reg.last_search_idx = (idx + 1) % total

                    self.allocated_pages += 1
                    self.free_pages -= 1

                    paddr = reg.base + (idx * PAGE_SIZE)
                    return PMM_SUCCESS, paddr

        return PMM_ERR_NOMEM, 0

    def free_page(self, paddr):
        if (paddr & (PAGE_SIZE - 1)) != 0:
            return PMM_ERR_UNALIGNED

        if paddr == 0:
            return PMM_ERR_PAGE_ZERO

        for reg in self.regions:
            if reg.base <= paddr < reg.end:
                idx = (paddr - reg.base) // PAGE_SIZE
                byte_idx = idx // 8
                bit_mask = 1 << (idx % 8)

                if (reg.bitmap[byte_idx] & bit_mask) == 0:
                    return PMM_ERR_DOUBLE_FREE

                reg.bitmap[byte_idx] &= ~bit_mask
                reg.free_pages += 1
                if idx < reg.last_search_idx:
                    reg.last_search_idx = idx

                self.allocated_pages -= 1
                self.free_pages += 1
                return PMM_SUCCESS

        return PMM_ERR_UNMANAGED


def test_unaligned_and_tiny_regions():
    print("[HOST TEST] Running test_unaligned_and_tiny_regions...")
    pmm = PMMHostSimulator()
    entries = [
        (0x100500, 0x4800, LIMINE_MEMMAP_USABLE), # 3 pages: 0x101000..0x104000
        (0x200000, 2048, LIMINE_MEMMAP_USABLE),   # Tiny < 1 page -> discarded
        (0x300800, 100, LIMINE_MEMMAP_USABLE),    # Subpage -> discarded
        (0x400000, 0x200000, LIMINE_MEMMAP_USABLE)# 2 MB (512 pages)
    ]
    res = pmm.init(entries)
    assert res == PMM_SUCCESS
    assert pmm.total_managed_pages == 515 # 3 + 512
    assert len(pmm.regions) == 2
    assert pmm.metadata_pages > 0
    assert pmm.free_pages == pmm.total_managed_pages - pmm.metadata_pages
    print("  PASS: Unaligned and tiny regions correctly aligned and filtered.")


def test_sparse_high_address_regions():
    print("[HOST TEST] Running test_sparse_high_address_regions...")
    pmm = PMMHostSimulator()
    entries = [
        (0x100000, 0x100000, LIMINE_MEMMAP_USABLE),  # 1 MB
        (0x200000, 0x600000, LIMINE_MEMMAP_RESERVED),# 6 MB gap
        (0x800000, 0x200000, LIMINE_MEMMAP_USABLE),  # 2 MB
        (0xFD00000000, 0x300000000, LIMINE_MEMMAP_RESERVED) # 12 GB MMIO
    ]
    res = pmm.init(entries)
    assert res == PMM_SUCCESS
    assert pmm.total_managed_pages == 768 # 256 + 512
    assert len(pmm.regions) == 2
    assert pmm.metadata_pages == 1 # 768 bits = 96 bytes, fits in 1 page!
    print("  PASS: Sparse regions handled; high MMIO ignored without bloat.")


def test_overflow_and_invalid_ranges():
    print("[HOST TEST] Running test_overflow_and_invalid_ranges...")
    pmm = PMMHostSimulator()
    entries = [
        (0xFFFFFFFFFFFFF000, 0x2000, LIMINE_MEMMAP_USABLE), # Wraparound overflow
        (0x500000, 0, LIMINE_MEMMAP_USABLE),               # Zero length
        (0x100000, 0x100000, LIMINE_MEMMAP_USABLE)         # Valid 256 pages
    ]
    res = pmm.init(entries)
    assert res == PMM_SUCCESS
    assert len(pmm.regions) == 1
    assert pmm.total_managed_pages == 256
    print("  PASS: Overflow and zero-length ranges safely rejected.")


def test_exhaustion_and_recovery():
    print("[HOST TEST] Running test_exhaustion_and_recovery...")
    pmm = PMMHostSimulator()
    entries = [
        (0x100000, 10 * PAGE_SIZE, LIMINE_MEMMAP_USABLE)
    ]
    res = pmm.init(entries)
    assert res == PMM_SUCCESS
    free_init = pmm.free_pages
    assert free_init > 0

    allocated = []
    for _ in range(free_init):
        r, paddr = pmm.alloc_page()
        assert r == PMM_SUCCESS
        assert 0x100000 <= paddr < 0x10A000
        allocated.append(paddr)

    # Exhaustion check
    r, _ = pmm.alloc_page()
    assert r == PMM_ERR_NOMEM
    assert pmm.free_pages == 0

    # Free one page
    assert pmm.free_page(allocated[0]) == PMM_SUCCESS
    assert pmm.free_pages == 1

    # Recovery: allocation succeeds
    r, rec_addr = pmm.alloc_page()
    assert r == PMM_SUCCESS
    assert rec_addr == allocated[0]

    # Clean up
    pmm.free_page(rec_addr)
    for addr in allocated[1:]:
        assert pmm.free_page(addr) == PMM_SUCCESS

    assert pmm.free_pages == free_init
    print("  PASS: Exhaustion correctly detected; recovery functional.")


def test_invalid_and_double_frees():
    print("[HOST TEST] Running test_invalid_and_double_frees...")
    pmm = PMMHostSimulator()
    entries = [
        (0x100000, 16 * PAGE_SIZE, LIMINE_MEMMAP_USABLE)
    ]
    res = pmm.init(entries)
    assert res == PMM_SUCCESS

    r, p = pmm.alloc_page()
    assert r == PMM_SUCCESS

    # Unaligned
    assert pmm.free_page(p + 1) == PMM_ERR_UNALIGNED
    assert pmm.free_page(p + 1024) == PMM_ERR_UNALIGNED

    # Page zero
    assert pmm.free_page(0) == PMM_ERR_PAGE_ZERO

    # Unmanaged
    assert pmm.free_page(0x200000) == PMM_ERR_UNMANAGED
    assert pmm.free_page(0xFED00000) == PMM_ERR_UNMANAGED

    # Valid free
    assert pmm.free_page(p) == PMM_SUCCESS

    # Double free
    assert pmm.free_page(p) == PMM_ERR_DOUBLE_FREE
    print("  PASS: Unaligned, page zero, unmanaged, and double frees correctly rejected.")


def main():
    print("======================================================")
    print("       AstraOS Physical Memory Manager Host Tests     ")
    print("======================================================")

    test_unaligned_and_tiny_regions()
    test_sparse_high_address_regions()
    test_overflow_and_invalid_ranges()
    test_exhaustion_and_recovery()
    test_invalid_and_double_frees()

    print("------------------------------------------------------")
    print("[SUCCESS] All host-side PMM unit tests passed!")
    print("======================================================")


if __name__ == '__main__':
    main()
