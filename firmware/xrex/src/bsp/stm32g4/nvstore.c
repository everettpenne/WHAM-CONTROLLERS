/*
 * nvstore.c -- STM32G4 implementation of drivers/nvstore.h: W25Q128JVS
 * byte storage over the QUADSPI peripheral that qspi_test.c brings up.
 *
 * Everything uses plain single-line SPI commands (read 0x03, page
 * program 0x02, 4 KiB sector erase 0x20, status 0x05/0x35/0x15, write
 * enable 0x06) with 24-bit addresses -- the same 1-line mode the
 * already-verified JEDEC Read ID uses, so it does not depend on the
 * chip's Quad Enable bit and needs no dummy cycles. At the 5 MHz
 * QSPI_CLOCK_PRESCALER in qspi_test.c that is ~600 KB/s, which is far
 * more than a few hundred bytes of settings will ever need.
 */
#include "nvstore.h"

#if (NVSTORE_FEATURE_ENABLED != 0)

#include "qspi_test_hw.h"
#include <string.h>

#define CMD_WRITE_ENABLE    (0x06U)
#define CMD_READ_SR1        (0x05U)
#define CMD_READ_SR2        (0x35U)
#define CMD_READ_SR3        (0x15U)
#define CMD_READ_DATA       (0x03U)
#define CMD_READ_UNIQUE_ID  (0x4BU)   /* then 4 dummy bytes, then the 64-bit ID (see NvStore_ReadUniqueId) */
#define CMD_PAGE_PROGRAM    (0x02U)
#define CMD_SECTOR_ERASE_4K (0x20U)

#define SR1_BUSY            (0x01U)
#define SR1_WEL             (0x02U)
#define SR1_BP_MASK         (0x1CU)   /* BP0..BP2: any set = part of the array is write-protected */
#define SR3_WPS             (0x04U)   /* individual block locks in use (all locked after power-up) */

/* Datasheet maxima with margin: page program 3 ms, 4 KiB sector erase
   400 ms. Individual QUADSPI transactions get a fixed short timeout;
   HAL's default is 5 s, which would hang the command link on a wedged bus. */
#define TIMEOUT_XFER_MS     (100U)
#define TIMEOUT_PROGRAM_MS  (10U)
#define TIMEOUT_ERASE_MS    (500U)

#define VERIFY_CHUNK        (64U)

static uint8_t g_chipOk = 0U;
static uint8_t g_identityWrite = 0U;   /* see NvStore_IdentityWriteEnable() */

const char *NvStore_StatusText(nvstore_status_t status)
{
    switch (status)
    {
    case NVSTORE_OK:            return "ok";
    case NVSTORE_ERR_NOT_READY: return "not ready";
    case NVSTORE_ERR_RANGE:     return "out of range";
    case NVSTORE_ERR_TIMEOUT:   return "timeout";
    case NVSTORE_ERR_IO:        return "transfer failed";
    case NVSTORE_ERR_PROTECTED: return "write-protected";
    case NVSTORE_ERR_VERIFY:    return "verify failed";
    case NVSTORE_ERR_RESERVED:  return "reserved region";
    default:                    return "unknown";
    }
}

static void InitCmd(QSPI_CommandTypeDef *c, uint8_t instruction)
{
    memset(c, 0, sizeof(*c));
    c->InstructionMode   = QSPI_INSTRUCTION_1_LINE;
    c->Instruction       = instruction;
    c->AddressMode       = QSPI_ADDRESS_NONE;
    c->AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
    c->DummyCycles       = 0U;
    c->DataMode          = QSPI_DATA_NONE;
    c->DdrMode           = QSPI_DDR_MODE_DISABLE;
    c->SIOOMode          = QSPI_SIOO_INST_EVERY_CMD;
}

static void SetAddress(QSPI_CommandTypeDef *c, uint32_t addr)
{
    c->AddressMode = QSPI_ADDRESS_1_LINE;
    c->AddressSize = QSPI_ADDRESS_24_BITS;
    c->Address     = addr;
}

static void SetData(QSPI_CommandTypeDef *c, uint32_t n)
{
    c->DataMode = QSPI_DATA_1_LINE;
    c->NbData   = n;
}

static nvstore_status_t ReadReg(QSPI_HandleTypeDef *h, uint8_t instruction, uint8_t *value)
{
    QSPI_CommandTypeDef cmd;

    InitCmd(&cmd, instruction);
    SetData(&cmd, 1U);
    if (HAL_QSPI_Command(h, &cmd, TIMEOUT_XFER_MS) != HAL_OK ||
        HAL_QSPI_Receive(h, value, TIMEOUT_XFER_MS) != HAL_OK)
    {
        return NVSTORE_ERR_IO;
    }
    return NVSTORE_OK;
}

/* Waits for the chip's BUSY bit to clear. */
static nvstore_status_t WaitReady(QSPI_HandleTypeDef *h, uint32_t timeoutMs)
{
    uint32_t start = HAL_GetTick();

    for (;;)
    {
        uint8_t sr1 = 0U;
        nvstore_status_t st = ReadReg(h, CMD_READ_SR1, &sr1);

        if (st != NVSTORE_OK)
        {
            return st;
        }
        if ((sr1 & SR1_BUSY) == 0U)
        {
            return NVSTORE_OK;
        }
        if ((HAL_GetTick() - start) > timeoutMs)
        {
            return NVSTORE_ERR_TIMEOUT;
        }
    }
}

/* Sets the write-enable latch and checks it took (a chip that ignores
   the command, or is wedged, leaves it clear). */
static nvstore_status_t WriteEnable(QSPI_HandleTypeDef *h)
{
    QSPI_CommandTypeDef cmd;
    uint8_t sr1 = 0U;
    nvstore_status_t st;

    InitCmd(&cmd, CMD_WRITE_ENABLE);
    if (HAL_QSPI_Command(h, &cmd, TIMEOUT_XFER_MS) != HAL_OK)
    {
        return NVSTORE_ERR_IO;
    }
    st = ReadReg(h, CMD_READ_SR1, &sr1);
    if (st != NVSTORE_OK)
    {
        return st;
    }
    return ((sr1 & SR1_WEL) != 0U) ? NVSTORE_OK : NVSTORE_ERR_IO;
}

/* Common front end: the peripheral exists, and the chip has answered as
   a W25Q128 at least once (re-probed until it does). */
static nvstore_status_t Prepare(QSPI_HandleTypeDef **h)
{
    uint8_t id[3] = {0U, 0U, 0U};

    *h = QspiTest_Handle();
    if (*h == NULL)
    {
        return NVSTORE_ERR_NOT_READY;
    }
    if (g_chipOk == 0U)
    {
        if (QspiTest_ReadId(id) == 0U || id[0] != 0xEFU || id[1] != 0x40U || id[2] != 0x18U)
        {
            return NVSTORE_ERR_NOT_READY;
        }
        g_chipOk = 1U;
    }
    return NVSTORE_OK;
}

/* Refuses to write when any part of the array is protected -- otherwise
   the chip would silently ignore the erase/program and we would only
   find out from the verify step. */
static nvstore_status_t CheckNotProtected(QSPI_HandleTypeDef *h)
{
    uint8_t sr1 = 0U;
    uint8_t sr3 = 0U;
    nvstore_status_t st = ReadReg(h, CMD_READ_SR1, &sr1);

    if (st == NVSTORE_OK)
    {
        st = ReadReg(h, CMD_READ_SR3, &sr3);
    }
    if (st != NVSTORE_OK)
    {
        return st;
    }
    if ((sr1 & SR1_BP_MASK) != 0U || (sr3 & SR3_WPS) != 0U)
    {
        return NVSTORE_ERR_PROTECTED;
    }
    return NVSTORE_OK;
}

static uint8_t RangeOk(uint32_t addr, uint32_t len)
{
    return (addr <= NVSTORE_CAPACITY_BYTES && len <= (NVSTORE_CAPACITY_BYTES - addr)) ? 1U : 0U;
}

/* 1 when [addr, addr+len) overlaps the identity sectors (the range is already known to be in the chip). */
static uint8_t TouchesIdentity(uint32_t addr, uint32_t len)
{
    return (addr < (NVSTORE_IDENTITY_ADDR + NVSTORE_IDENTITY_BYTES) && (addr + len) > NVSTORE_IDENTITY_ADDR) ? 1U : 0U;
}

void NvStore_IdentityWriteEnable(uint8_t enable)
{
    g_identityWrite = (enable != 0U) ? 1U : 0U;
}

static nvstore_status_t ReadRaw(QSPI_HandleTypeDef *h, uint32_t addr, uint8_t *buf, uint32_t len)
{
    QSPI_CommandTypeDef cmd;

    if (len == 0U)
    {
        return NVSTORE_OK;
    }
    InitCmd(&cmd, CMD_READ_DATA);
    SetAddress(&cmd, addr);
    SetData(&cmd, len);
    /* ~1.6 ms per KiB at 5 MHz single-line; allow generously more. */
    if (HAL_QSPI_Command(h, &cmd, TIMEOUT_XFER_MS) != HAL_OK ||
        HAL_QSPI_Receive(h, buf, TIMEOUT_XFER_MS + ((len / 1024U) * 4U)) != HAL_OK)
    {
        return NVSTORE_ERR_IO;
    }
    return NVSTORE_OK;
}

/* Compares flash contents against `expect` (or against all-0xFF when
   expect is NULL), a few bytes at a time to keep the stack small. */
static nvstore_status_t VerifyRegion(QSPI_HandleTypeDef *h, uint32_t addr, const uint8_t *expect,
                                     uint32_t len)
{
    uint8_t buf[VERIFY_CHUNK];

    for (uint32_t off = 0U; off < len; off += VERIFY_CHUNK)
    {
        uint32_t n = ((len - off) < VERIFY_CHUNK) ? (len - off) : VERIFY_CHUNK;
        nvstore_status_t st = ReadRaw(h, addr + off, buf, n);

        if (st != NVSTORE_OK)
        {
            return st;
        }
        for (uint32_t i = 0U; i < n; i++)
        {
            uint8_t want = (expect != NULL) ? expect[off + i] : 0xFFU;

            if (buf[i] != want)
            {
                return NVSTORE_ERR_VERIFY;
            }
        }
    }
    return NVSTORE_OK;
}

nvstore_status_t NvStore_Probe(void)
{
    QSPI_HandleTypeDef *h;

    g_chipOk = 0U;   /* force a real ID read */
    return Prepare(&h);
}

nvstore_status_t NvStore_ReadStatusRegs(uint8_t sr[3])
{
    static const uint8_t kRegCmd[3] = { CMD_READ_SR1, CMD_READ_SR2, CMD_READ_SR3 };
    QSPI_HandleTypeDef *h;
    nvstore_status_t st = Prepare(&h);

    for (uint32_t i = 0U; st == NVSTORE_OK && i < 3U; i++)
    {
        st = ReadReg(h, kRegCmd[i], &sr[i]);
    }
    return st;
}

nvstore_status_t NvStore_ReadUniqueId(uint8_t id[8])
{
    QSPI_HandleTypeDef *h;
    QSPI_CommandTypeDef cmd;
    uint8_t raw[12];
    nvstore_status_t st = Prepare(&h);

    if (st != NVSTORE_OK)
    {
        return st;
    }
    /* The chip sends four dummy bytes (32 clocks) after the instruction, then the ID, which it
       then repeats. The QUADSPI's dummy-cycle field holds only 0..31, so 32 silently wrapped to 0
       (found on hardware 2026-10-07: the first four "ID" bytes changed from boot to boot, the
       rest never did). Read the dummy bytes as data and drop them. */
    InitCmd(&cmd, CMD_READ_UNIQUE_ID);
    SetData(&cmd, 12U);
    if (HAL_QSPI_Command(h, &cmd, TIMEOUT_XFER_MS) != HAL_OK ||
        HAL_QSPI_Receive(h, raw, TIMEOUT_XFER_MS) != HAL_OK)
    {
        return NVSTORE_ERR_IO;
    }
    memcpy(id, &raw[4], 8U);
    return NVSTORE_OK;
}

nvstore_status_t NvStore_Read(uint32_t addr, void *buf, uint32_t len)
{
    QSPI_HandleTypeDef *h;
    nvstore_status_t st = Prepare(&h);

    if (st != NVSTORE_OK)
    {
        return st;
    }
    if (RangeOk(addr, len) == 0U)
    {
        return NVSTORE_ERR_RANGE;
    }
    return ReadRaw(h, addr, (uint8_t *)buf, len);
}

nvstore_status_t NvStore_EraseSector(uint32_t addr)
{
    QSPI_HandleTypeDef *h;
    QSPI_CommandTypeDef cmd;
    nvstore_status_t st = Prepare(&h);

    if (st != NVSTORE_OK)
    {
        return st;
    }
    if ((addr % NVSTORE_SECTOR_BYTES) != 0U || RangeOk(addr, NVSTORE_SECTOR_BYTES) == 0U)
    {
        return NVSTORE_ERR_RANGE;
    }
    if (TouchesIdentity(addr, NVSTORE_SECTOR_BYTES) != 0U && g_identityWrite == 0U)
    {
        return NVSTORE_ERR_RESERVED;
    }
    if ((st = CheckNotProtected(h)) != NVSTORE_OK || (st = WaitReady(h, TIMEOUT_ERASE_MS)) != NVSTORE_OK ||
        (st = WriteEnable(h)) != NVSTORE_OK)
    {
        return st;
    }
    InitCmd(&cmd, CMD_SECTOR_ERASE_4K);
    SetAddress(&cmd, addr);
    if (HAL_QSPI_Command(h, &cmd, TIMEOUT_XFER_MS) != HAL_OK)
    {
        return NVSTORE_ERR_IO;
    }
    if ((st = WaitReady(h, TIMEOUT_ERASE_MS)) != NVSTORE_OK)
    {
        return st;
    }
    return VerifyRegion(h, addr, NULL, NVSTORE_SECTOR_BYTES);
}

nvstore_status_t NvStore_Program(uint32_t addr, const void *data, uint32_t len)
{
    QSPI_HandleTypeDef *h;
    QSPI_CommandTypeDef cmd;
    const uint8_t *src = (const uint8_t *)data;
    uint32_t done = 0U;
    nvstore_status_t st = Prepare(&h);

    if (st != NVSTORE_OK)
    {
        return st;
    }
    if (RangeOk(addr, len) == 0U)
    {
        return NVSTORE_ERR_RANGE;
    }
    if (len == 0U)
    {
        return NVSTORE_OK;
    }
    if (TouchesIdentity(addr, len) != 0U && g_identityWrite == 0U)
    {
        return NVSTORE_ERR_RESERVED;
    }
    if ((st = CheckNotProtected(h)) != NVSTORE_OK)
    {
        return st;
    }

    while (done < len)
    {
        /* One page program must not cross a 256-byte page boundary. */
        uint32_t a     = addr + done;
        uint32_t room  = NVSTORE_PAGE_BYTES - (a % NVSTORE_PAGE_BYTES);
        uint32_t chunk = ((len - done) < room) ? (len - done) : room;

        if ((st = WaitReady(h, TIMEOUT_PROGRAM_MS)) != NVSTORE_OK || (st = WriteEnable(h)) != NVSTORE_OK)
        {
            return st;
        }
        InitCmd(&cmd, CMD_PAGE_PROGRAM);
        SetAddress(&cmd, a);
        SetData(&cmd, chunk);
        if (HAL_QSPI_Command(h, &cmd, TIMEOUT_XFER_MS) != HAL_OK ||
            HAL_QSPI_Transmit(h, (uint8_t *)&src[done], TIMEOUT_XFER_MS) != HAL_OK)
        {
            return NVSTORE_ERR_IO;
        }
        if ((st = WaitReady(h, TIMEOUT_PROGRAM_MS)) != NVSTORE_OK)
        {
            return st;
        }
        done += chunk;
    }
    return VerifyRegion(h, addr, src, len);
}

#else /* NVSTORE_FEATURE_ENABLED == 0 */

const char *NvStore_StatusText(nvstore_status_t status)
{
    return (status == NVSTORE_OK) ? "ok" : "not ready";
}

nvstore_status_t NvStore_Probe(void)
{
    return NVSTORE_ERR_NOT_READY;
}

nvstore_status_t NvStore_Read(uint32_t addr, void *buf, uint32_t len)
{
    (void)addr;
    (void)buf;
    (void)len;
    return NVSTORE_ERR_NOT_READY;
}

nvstore_status_t NvStore_EraseSector(uint32_t addr)
{
    (void)addr;
    return NVSTORE_ERR_NOT_READY;
}

nvstore_status_t NvStore_Program(uint32_t addr, const void *data, uint32_t len)
{
    (void)addr;
    (void)data;
    (void)len;
    return NVSTORE_ERR_NOT_READY;
}

nvstore_status_t NvStore_ReadStatusRegs(uint8_t sr[3])
{
    (void)sr;
    return NVSTORE_ERR_NOT_READY;
}

nvstore_status_t NvStore_ReadUniqueId(uint8_t id[8])
{
    (void)id;
    return NVSTORE_ERR_NOT_READY;
}

void NvStore_IdentityWriteEnable(uint8_t enable)
{
    (void)enable;
}

#endif /* NVSTORE_FEATURE_ENABLED */
