/*
 * Copyright (c) 2014 Travis Geiselbrecht
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <trace.h>
#include <assert.h>
#include <err.h>
#include <string.h>
#include <lib/console.h>
#include <kernel/vm.h>
#include "vm_priv.h"

#define LOCAL_TRACE 0

static struct list_node aspace_list = LIST_INITIAL_VALUE(aspace_list);

vmm_aspace_t _kernel_aspace;

static void dump_aspace(const vmm_aspace_t *a);
static void dump_region(const vmm_region_t *r);

void vmm_init(void)
{
    /* initialize the kernel address space */
    strlcpy(_kernel_aspace.name, "kernel", sizeof(_kernel_aspace.name));
    _kernel_aspace.base = KERNEL_ASPACE_BASE,
    _kernel_aspace.size = KERNEL_ASPACE_SIZE,
    list_initialize(&_kernel_aspace.region_list);

    list_add_head(&aspace_list, &_kernel_aspace.node);
}

static inline bool is_inside_aspace(const vmm_aspace_t *aspace, vaddr_t vaddr)
{
    return (vaddr >= aspace->base && vaddr <= aspace->base + aspace->size - 1);
}

static bool is_region_inside_aspace(const vmm_aspace_t *aspace, vaddr_t vaddr, size_t size)
{
    /* is the starting address within the address space*/
    if (!is_inside_aspace(aspace, vaddr))
        return false;

    if (size == 0)
        return true;

    /* see if the size is enough to wrap the integer */
    if (vaddr + size - 1 < vaddr)
        return false;

    /* test to see if the end address is within the address space's */
    if (vaddr + size - 1 > aspace->base + aspace->size - 1)
        return false;

    return true;
}

static size_t trim_to_aspace(const vmm_aspace_t *aspace, vaddr_t vaddr, size_t size)
{
    DEBUG_ASSERT(is_inside_aspace(aspace, vaddr));

    if (size == 0)
        return size;

    size_t offset = vaddr - aspace->base;

    //LTRACEF("vaddr 0x%lx size 0x%zx offset 0x%zx aspace base 0x%lx aspace size 0x%zx\n",
    //        vaddr, size, offset, aspace->base, aspace->size);

    if (offset + size < offset)
        size = ULONG_MAX - offset - 1;

    //LTRACEF("size now 0x%zx\n", size);

    if (offset + size >= aspace->size - 1)
        size = aspace->size - offset;

    //LTRACEF("size now 0x%zx\n", size);

    return size;
}

static vmm_region_t *alloc_region_struct(const char *name, vaddr_t base, size_t size, uint flags, uint arch_mmu_flags)
{
    DEBUG_ASSERT(name);

    vmm_region_t *r = malloc(sizeof(vmm_region_t));
    if (!r)
        return NULL;

    strlcpy(r->name, name, sizeof(r->name));
    r->base = base;
    r->size = size;
    r->flags = flags;
    r->arch_mmu_flags = arch_mmu_flags;
    list_initialize(&r->page_list);

    return r;
}

/* add a region to the appropriate spot in the address space list,
 * testing to see if there's a space */
static status_t add_region_to_aspace(vmm_aspace_t *aspace, vmm_region_t *r)
{
    DEBUG_ASSERT(aspace);
    DEBUG_ASSERT(r);

    LTRACEF("aspace %p base 0x%lx size 0x%zx r %p base 0x%lx size 0x%zx\n",
            aspace, aspace->base, aspace->size, r, r->base, r->size);

    /* only try if the region will at least fit in the address space */
    if (r->size == 0 || !is_region_inside_aspace(aspace, r->base, r->size)) {
        LTRACEF("region was out of range\n");
        return ERR_OUT_OF_RANGE;
    }

    vaddr_t r_end = r->base + r->size - 1;

    /* does it fit in front */
    vmm_region_t *last;
    last = list_peek_head_type(&aspace->region_list, vmm_region_t, node);
    if (!last || r_end < last->base) {
        /* empty list or not empty and fits before the first element */
        list_add_head(&aspace->region_list, &r->node);
        return NO_ERROR;
    }

    /* walk the list, finding the right spot to put it */
    list_for_every_entry(&aspace->region_list, last, vmm_region_t, node) {
        /* does it go after last? */
        if (r->base > last->base + last->size - 1) {
            /* get the next element in the list */
            vmm_region_t *next = list_next_type(&aspace->region_list, &last->node, vmm_region_t, node);
            if (!next || (r_end < next->base)) {
                /* end of the list or next exists and it goes between them */
                list_add_after(&last->node, &r->node);
                return NO_ERROR;
            }
        }
    }

    LTRACEF("couldn't find spot\n");
    return ERR_NO_MEMORY;
}

static vaddr_t alloc_spot(vmm_aspace_t *aspace, size_t size, uint8_t align_pow2, struct list_node **before)
{
    DEBUG_ASSERT(aspace);
    DEBUG_ASSERT(size > 0 && IS_PAGE_ALIGNED(size));

    LTRACEF("aspace %p size 0x%zx align %hhu\n", aspace, size, align_pow2);

    if (align_pow2 < PAGE_SIZE_SHIFT)
        align_pow2 = PAGE_SIZE_SHIFT;
    vaddr_t align = 1UL << align_pow2;

    /* start our search */
    vaddr_t spot = ALIGN(aspace->base, align);
    if (!is_inside_aspace(aspace, spot)) {
        /* the alignment is so big, we can't even allocate in this address space */
        return -1;
    }

    vmm_region_t *r = list_peek_head_type(&aspace->region_list, vmm_region_t, node);
    if (r) {
        /* does it fit before the first element? */
        if (spot < r->base && r->base - spot >= size) {
            if (before)
                *before = &aspace->region_list;
            return spot;
        }
    } else {
        /* nothing is in the list, does it fit in the aspace? */
        if (aspace->base + aspace->size - spot >= size) {
            if (before)
                *before = &aspace->region_list;
            return spot;
        }
    }

    /* search the middle of the list */
    list_for_every_entry(&aspace->region_list, r, vmm_region_t, node) {
        /* calculate the aligned spot after r */
        spot = ALIGN(r->base + r->size, align);
        if (!is_inside_aspace(aspace, spot))
            break;

        /* get the next element in the list */
        vmm_region_t *next = list_next_type(&aspace->region_list, &r->node, vmm_region_t, node);

        if (next) {
            /* see if the aligned spot is between current and next */
            if (spot >= next->base)
                continue;

            /* see if it'll fit between the current item and the next */
            if (next->base - spot >= size) {
                /* it'll fit here */
                if (before)
                    *before = &r->node;
                return spot;
            }
        } else {
            /* we're at the end of the list, will it fit between us and the end of the aspace? */
            if ((aspace->base + aspace->size) - spot >= size) {
                /* it'll fit here */
                if (before)
                    *before = &r->node;
                return spot;
            }
        }
    }

    /* couldn't find anything */
    return -1;
}

/* allocate a region structure and stick it in the address space */
static vmm_region_t *alloc_region(vmm_aspace_t *aspace, const char *name, size_t size,
        vaddr_t vaddr, uint8_t align_pow2,
        uint vmm_flags, uint region_flags, uint arch_mmu_flags)
{
    /* make a region struct for it and stick it in the list */
    vmm_region_t *r = alloc_region_struct(name, vaddr, size, region_flags, arch_mmu_flags);
    if (!r)
        return NULL;

    /* if they ask us for a specific spot, put it there */
    if (vmm_flags & VMM_FLAG_VALLOC_SPECIFIC) {
        /* stick it in the list, checking to see if it fits */
        if (add_region_to_aspace(aspace, r) < 0) {
            /* didn't fit */
            free(r);
            return NULL;
        }
    } else {
        /* allocate a virtual slot for it */
        struct list_node *before = NULL;
        vaddr = alloc_spot(aspace, size, align_pow2, &before);
        LTRACEF("alloc_spot returns 0x%lx, before %p\n", vaddr, before);

        if (vaddr == (vaddr_t)-1) {
            LTRACEF("failed to find spot\n");
            free(r);
            return NULL;
        }

        DEBUG_ASSERT(before != NULL);

        r->base = (vaddr_t)vaddr;

        /* add it to the region list */
        list_add_after(before, &r->node);
    }

    return r;
}

status_t vmm_reserve_space(vmm_aspace_t *aspace, const char *name, size_t size, vaddr_t vaddr)
{
    LTRACEF("aspace %p name '%s' size 0x%zx vaddr 0x%lx\n", aspace, name, size, vaddr);

    DEBUG_ASSERT(aspace);
    DEBUG_ASSERT(IS_PAGE_ALIGNED(vaddr));
    DEBUG_ASSERT(IS_PAGE_ALIGNED(size));

    if (!name)
        name = "";

    if (!aspace)
        return ERR_INVALID_ARGS;
    if (size == 0)
        return NO_ERROR;
    if (!IS_PAGE_ALIGNED(vaddr) || !IS_PAGE_ALIGNED(size))
        return ERR_INVALID_ARGS;

    if (!is_inside_aspace(aspace, vaddr))
        return ERR_OUT_OF_RANGE;

    /* trim the size */
    size = trim_to_aspace(aspace, vaddr, size);

    /* lookup how it's already mapped */
    uint arch_mmu_flags = 0;
    arch_mmu_query(vaddr, NULL, &arch_mmu_flags);

    /* build a new region structure */
    vmm_region_t *r = alloc_region(aspace, name, size, vaddr, 0, VMM_FLAG_VALLOC_SPECIFIC, VMM_REGION_FLAG_RESERVED, arch_mmu_flags);
    if (!r)
        return ERR_NO_MEMORY;

    return NO_ERROR;
}

status_t vmm_alloc_physical(vmm_aspace_t *aspace, const char *name, size_t size, void **ptr, paddr_t paddr, uint vmm_flags, uint arch_mmu_flags)
{
    LTRACEF("aspace %p name '%s' size 0x%zx ptr %p paddr 0x%lx vmm_flags 0x%x arch_mmu_flags 0x%x\n",
            aspace, name, size, ptr ? *ptr : 0, paddr, vmm_flags, arch_mmu_flags);

    DEBUG_ASSERT(aspace);
    DEBUG_ASSERT(IS_PAGE_ALIGNED(paddr));
    DEBUG_ASSERT(IS_PAGE_ALIGNED(size));

    if (!name)
        name = "";

    if (!aspace)
        return ERR_INVALID_ARGS;
    if (size == 0)
        return NO_ERROR;
    if (!IS_PAGE_ALIGNED(paddr) || !IS_PAGE_ALIGNED(size))
        return ERR_INVALID_ARGS;

    vaddr_t vaddr = 0;

    /* if they're asking for a specific spot, copy the address */
    if (vmm_flags & VMM_FLAG_VALLOC_SPECIFIC) {
        /* can't ask for a specific spot and then not provide one */
        if (!ptr) {
            return ERR_INVALID_ARGS;
        }
        vaddr = (vaddr_t)*ptr;
    }

    /* allocate a region and put it in the aspace list */
    vmm_region_t *r = alloc_region(aspace, name, size, vaddr, 0, vmm_flags, VMM_REGION_FLAG_PHYSICAL, arch_mmu_flags);
    if (!r)
        return ERR_NO_MEMORY;

    /* return the vaddr if requested */
    if (ptr)
        *ptr = (void *)r->base;

    /* map all of the pages */
    int err = arch_mmu_map(r->base, paddr, size / PAGE_SIZE, arch_mmu_flags);
    LTRACEF("arch_mmu_map returns %d\n", err);

    return NO_ERROR;
}

status_t vmm_alloc_contiguous(vmm_aspace_t *aspace, const char *name, size_t size, void **ptr, uint8_t align_pow2, uint vmm_flags, uint arch_mmu_flags)
{
    status_t err = NO_ERROR;

    LTRACEF("aspace %p name '%s' size 0x%zx ptr %p align %hhu vmm_flags 0x%x arch_mmu_flags 0x%x\n",
            aspace, name, size, ptr ? *ptr : 0, align_pow2, vmm_flags, arch_mmu_flags);

    DEBUG_ASSERT(aspace);

    size = ROUNDUP(size, PAGE_SIZE);
    if (size == 0)
        return ERR_INVALID_ARGS;

    if (!name)
        name = "";

    vaddr_t vaddr = 0;

    /* if they're asking for a specific spot, copy the address */
    if (vmm_flags & VMM_FLAG_VALLOC_SPECIFIC) {
        /* can't ask for a specific spot and then not provide one */
        if (!ptr) {
            err = ERR_INVALID_ARGS;
            goto err;
        }
        vaddr = (vaddr_t)*ptr;
    }

    /* allocate physical memory up front, in case it cant be satisfied */
    struct list_node page_list;
    list_initialize(&page_list);

    paddr_t pa = 0;
    /* allocate a run of physical pages */
    uint count = pmm_alloc_contiguous(size / PAGE_SIZE, align_pow2, &pa, &page_list);
    if (count < size / PAGE_SIZE) {
        err = ERR_NO_MEMORY;
        goto err;
    }

    /* allocate a region and put it in the aspace list */
    vmm_region_t *r = alloc_region(aspace, name, size, vaddr, align_pow2, vmm_flags, VMM_REGION_FLAG_PHYSICAL, arch_mmu_flags);
    if (!r) {
        err = ERR_NO_MEMORY;
        goto err1;
    }

    /* return the vaddr if requested */
    if (ptr)
        *ptr = (void *)r->base;

    /* map all of the pages */
    arch_mmu_map(r->base, pa, size / PAGE_SIZE, arch_mmu_flags);
    // XXX deal with error mapping here

    vm_page_t *p;
    while ((p = list_remove_head_type(&page_list, vm_page_t, node))) {
        list_add_tail(&r->page_list, &p->node);
    }

    return NO_ERROR;

err1:
    pmm_free(&page_list);
err:
    return err;
}

status_t vmm_alloc(vmm_aspace_t *aspace, const char *name, size_t size, void **ptr, uint8_t align_pow2, uint vmm_flags, uint arch_mmu_flags)
{
    status_t err = NO_ERROR;

    LTRACEF("aspace %p name '%s' size 0x%zx ptr %p align %hhu vmm_flags 0x%x arch_mmu_flags 0x%x\n",
            aspace, name, size, ptr ? *ptr : 0, align_pow2, vmm_flags, arch_mmu_flags);

    DEBUG_ASSERT(aspace);

    size = ROUNDUP(size, PAGE_SIZE);
    if (size == 0)
        return ERR_INVALID_ARGS;

    if (!name)
        name = "";

    vaddr_t vaddr = 0;

    /* if they're asking for a specific spot, copy the address */
    if (vmm_flags & VMM_FLAG_VALLOC_SPECIFIC) {
        /* can't ask for a specific spot and then not provide one */
        if (!ptr) {
            err = ERR_INVALID_ARGS;
            goto err;
        }
        vaddr = (vaddr_t)*ptr;
    }

    /* allocate physical memory up front, in case it cant be satisfied */

    /* allocate a random pile of pages */
    struct list_node page_list;
    list_initialize(&page_list);

    uint count = pmm_alloc_pages(size / PAGE_SIZE, &page_list);
    DEBUG_ASSERT(count <= size);
    if (count < size / PAGE_SIZE) {
        LTRACEF("failed to allocate enough pages (asked for %u, got %u)\n", size / PAGE_SIZE, count);
        err = ERR_NO_MEMORY;
        goto err1;
    }

    /* allocate a region and put it in the aspace list */
    vmm_region_t *r = alloc_region(aspace, name, size, vaddr, align_pow2, vmm_flags, VMM_REGION_FLAG_PHYSICAL, arch_mmu_flags);
    if (!r) {
        err = ERR_NO_MEMORY;
        goto err1;
    }

    /* return the vaddr if requested */
    if (ptr)
        *ptr = (void *)r->base;

    /* map all of the pages */
    /* XXX use smarter algorithm that tries to build runs */
    vm_page_t *p;
    vaddr_t va = r->base;
    DEBUG_ASSERT(IS_PAGE_ALIGNED(va));
    while ((p = list_remove_head_type(&page_list, vm_page_t, node))) {
        DEBUG_ASSERT(va < r->base + r->size);

        paddr_t pa = page_to_address(p);
        DEBUG_ASSERT(IS_PAGE_ALIGNED(pa));

        arch_mmu_map(va, pa, 1, arch_mmu_flags);
        // XXX deal with error mapping here

        list_add_tail(&r->page_list, &p->node);

        va += PAGE_SIZE;
    }

    return NO_ERROR;

err1:
    pmm_free(&page_list);
err:
    return err;
}

static void dump_region(const vmm_region_t *r)
{
    printf("\tregion %p: name '%s' range 0x%lx - 0x%lx size 0x%zx flags 0x%x mmu_flags 0x%x\n",
            r, r->name, r->base, r->base + r->size - 1, r->size, r->flags, r->arch_mmu_flags);
}

static void dump_aspace(const vmm_aspace_t *a)
{
    printf("aspace %p: name '%s' range 0x%lx - 0x%lx size 0x%zx flags 0x%x\n",
            a, a->name, a->base, a->base + a->size - 1, a->size, a->flags);

    printf("regions:\n");
    vmm_region_t *r;
    list_for_every_entry(&a->region_list, r, vmm_region_t, node) {
        dump_region(r);
    }
}

static int cmd_vmm(int argc, const cmd_args *argv)
{
    if (argc < 2) {
notenoughargs:
        printf("not enough arguments\n");
usage:
        printf("usage:\n");
        printf("%s aspaces\n", argv[0].str);
        printf("%s alloc <size> <align_pow2>\n", argv[0].str);
        printf("%s alloc_physical <paddr> <size>\n", argv[0].str);
        printf("%s alloc_contig <size> <align_pow2>\n", argv[0].str);
        return ERR_GENERIC;
    }

    if (!strcmp(argv[1].str, "aspaces")) {
        vmm_aspace_t *a;
        list_for_every_entry(&aspace_list, a, vmm_aspace_t, node) {
            dump_aspace(a);
        }
    } else if (!strcmp(argv[1].str, "alloc")) {
        if (argc < 4) goto notenoughargs;

        void *ptr = (void *)0x99;
        status_t err = vmm_alloc(vmm_get_kernel_aspace(), "alloc test", argv[2].u, &ptr, argv[3].u, 0, 0);
        printf("vmm_alloc returns %d, ptr %p\n", err, ptr);
    } else if (!strcmp(argv[1].str, "alloc_physical")) {
        if (argc < 4) goto notenoughargs;

        void *ptr = (void *)0x99;
        status_t err = vmm_alloc_physical(vmm_get_kernel_aspace(), "physical test", argv[3].u, &ptr, argv[2].u, 0, ARCH_MMU_FLAG_UNCACHED_DEVICE);
        printf("vmm_alloc_physical returns %d, ptr %p\n", err, ptr);
    } else if (!strcmp(argv[1].str, "alloc_contig")) {
        if (argc < 4) goto notenoughargs;

        void *ptr = (void *)0x99;
        status_t err = vmm_alloc_contiguous(vmm_get_kernel_aspace(), "contig test", argv[2].u, &ptr, argv[3].u, 0, 0);
        printf("vmm_alloc_contig returns %d, ptr %p\n", err, ptr);
    } else {
        printf("unknown command\n");
        goto usage;
    }

    return NO_ERROR;
}

STATIC_COMMAND_START
#if LK_DEBUGLEVEL > 0
STATIC_COMMAND("vmm", "virtual memory manager", &cmd_vmm)
#endif
STATIC_COMMAND_END(vmm);

