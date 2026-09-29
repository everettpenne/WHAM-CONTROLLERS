#include "fake_nvstore.h"
#include "nvstore.h"
#include <string.h>

static uint8_t  g_mem[NVSTORE_CAPACITY_BYTES];
static uint8_t  g_dead;
static uint8_t  g_haveBudget;
static uint32_t g_budget;
static uint32_t g_used;

void FakeNv_Reset(void)
{
    memset(g_mem, 0xFF, sizeof(g_mem));
    g_dead = 0U;
    g_haveBudget = 0U;
    g_budget = 0U;
    g_used = 0U;
}

void FakeNv_CutPowerAfter(uint32_t units)
{
    g_haveBudget = 1U;
    g_budget = units;
    g_used = 0U;
}

void FakeNv_RestorePower(void)
{
    g_dead = 0U;
    g_haveBudget = 0U;
}

uint32_t FakeNv_UnitsUsed(void)
{
    return g_used;
}

void FakeNv_Poke(uint32_t addr, uint8_t xorMask)
{
    g_mem[addr] ^= xorMask;
}

/* 1 if a unit is available (and consumes it); 0 if power is (now) gone. */
static uint8_t Spend(void)
{
    if (g_dead != 0U)
    {
        return 0U;
    }
    if (g_haveBudget != 0U && g_budget == 0U)
    {
        g_dead = 1U;
        return 0U;
    }
    if (g_haveBudget != 0U)
    {
        g_budget--;
    }
    g_used++;
    return 1U;
}

const char *NvStore_StatusText(nvstore_status_t status)
{
    return (status == NVSTORE_OK) ? "ok" : "error";
}

nvstore_status_t NvStore_Probe(void)
{
    return (g_dead != 0U) ? NVSTORE_ERR_IO : NVSTORE_OK;
}

nvstore_status_t NvStore_ReadStatusRegs(uint8_t sr[3])
{
    sr[0] = 0U;
    sr[1] = 2U;
    sr[2] = 0x60U;
    return (g_dead != 0U) ? NVSTORE_ERR_IO : NVSTORE_OK;
}

nvstore_status_t NvStore_Read(uint32_t addr, void *buf, uint32_t len)
{
    if (g_dead != 0U)
    {
        return NVSTORE_ERR_IO;
    }
    if (addr > NVSTORE_CAPACITY_BYTES || len > (NVSTORE_CAPACITY_BYTES - addr))
    {
        return NVSTORE_ERR_RANGE;
    }
    memcpy(buf, &g_mem[addr], len);
    return NVSTORE_OK;
}

nvstore_status_t NvStore_EraseSector(uint32_t addr)
{
    if ((addr % NVSTORE_SECTOR_BYTES) != 0U || addr > NVSTORE_CAPACITY_BYTES - NVSTORE_SECTOR_BYTES)
    {
        return NVSTORE_ERR_RANGE;
    }
    if (Spend() == 0U)
    {
        /* Cut during the erase: the first half is already blank, the rest untouched. */
        if (g_dead != 0U && g_haveBudget != 0U)
        {
            memset(&g_mem[addr], 0xFF, NVSTORE_SECTOR_BYTES / 2U);
        }
        return NVSTORE_ERR_IO;
    }
    memset(&g_mem[addr], 0xFF, NVSTORE_SECTOR_BYTES);
    return NVSTORE_OK;
}

nvstore_status_t NvStore_Program(uint32_t addr, const void *data, uint32_t len)
{
    const uint8_t *src = (const uint8_t *)data;

    if (addr > NVSTORE_CAPACITY_BYTES || len > (NVSTORE_CAPACITY_BYTES - addr))
    {
        return NVSTORE_ERR_RANGE;
    }
    for (uint32_t i = 0U; i < len; i++)
    {
        if (Spend() == 0U)
        {
            return NVSTORE_ERR_IO;   /* bytes before i are already written */
        }
        g_mem[addr + i] &= src[i];
    }
    /* The real driver reads back what it programmed. */
    return (memcmp(&g_mem[addr], src, len) == 0) ? NVSTORE_OK : NVSTORE_ERR_VERIFY;
}
