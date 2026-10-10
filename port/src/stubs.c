// Stand-ins for hardware the browser port doesn't have: the wireless adapter
// (librfu), the GameCube link (libgcnmultiboot) and the cartridge header.

#include "port_internal.h"
#include "main.h"
#include "librfu.h"
#include "libgcnmultiboot.h"

// --------------------------------------------------------------------------
// Cartridge header fields read by the game.

const u8 RomHeaderGameCode[GAME_CODE_LENGTH] = {'B', 'P', 'E', 'E'};
const u8 RomHeaderSoftwareVersion = 0;
vu16 GPIOPortData;
vu16 GPIOPortDirection;
vu16 GPIOPortReadEnable;

// --------------------------------------------------------------------------
// Wireless adapter: report that none is connected.

static struct RfuLinkStatus sRfuLinkStatus;
static struct RfuSlotStatusNI sRfuSlotStatusNI[RFU_CHILD_MAX];
static struct RfuSlotStatusUNI sRfuSlotStatusUNI[RFU_CHILD_MAX];
static u8 sRfuRecvBuffer[0x120];

struct RfuLinkStatus *gRfuLinkStatus = &sRfuLinkStatus;
struct RfuSlotStatusNI *gRfuSlotStatusNI[RFU_CHILD_MAX] = {
    &sRfuSlotStatusNI[0], &sRfuSlotStatusNI[1], &sRfuSlotStatusNI[2], &sRfuSlotStatusNI[3],
};
struct RfuSlotStatusUNI *gRfuSlotStatusUNI[RFU_CHILD_MAX] = {
    &sRfuSlotStatusUNI[0], &sRfuSlotStatusUNI[1], &sRfuSlotStatusUNI[2], &sRfuSlotStatusUNI[3],
};

u16 rfu_CHILD_getConnectRecoveryStatus(u8 *status) { *status = 0; return 0; }
u16 rfu_NI_CHILD_setSendGameName(u8 slotNo, u8 subFrameSize) { return 0; }
u16 rfu_NI_setSendData(u8 bmSendSlot, u8 subFrameSize, const void *src, u32 size) { return 0; }
u16 rfu_NI_stopReceivingData(u8 slotStatusIndex) { return 0; }
u32 rfu_REQBN_softReset_and_checkID(void) { return 0; }
u16 rfu_REQBN_watchLink(u16 reqCommandId, u8 *bmLinkLossSlot, u8 *linkLossReason, u8 *parentBmLinkRecoverySlot)
{
    *bmLinkLossSlot = 0;
    *linkLossReason = 0;
    *parentBmLinkRecoverySlot = 0;
    return 0;
}
void rfu_REQ_CHILD_endConnectRecovery(void) {}
void rfu_REQ_CHILD_pollConnectRecovery(void) {}
void rfu_REQ_CHILD_startConnectRecovery(u8 bmRecoverySlot) {}
void rfu_REQ_PARENT_resumeRetransmitAndChange(void) {}
void rfu_REQ_RFUStatus(void) {}
void rfu_REQ_changeMasterSlave(void) {}
void rfu_REQ_configGameData(u8 mbootFlag, u16 serialNo, const u8 *gname, const u8 *uname) {}
void rfu_REQ_configSystem(u16 availSlotFlag, u8 maxMFrame, u8 mcTimer) {}
void rfu_REQ_disconnect(u8 bmDisconnectSlot) {}
void rfu_REQ_endConnectParent(void) {}
void rfu_REQ_endSearchChild(void) {}
void rfu_REQ_endSearchParent(void) {}
void rfu_REQ_pollConnectParent(void) {}
void rfu_REQ_pollSearchChild(void) {}
void rfu_REQ_pollSearchParent(void) {}
void rfu_REQ_recvData(void) {}
void rfu_REQ_reset(void) {}
void rfu_REQ_sendData(bool8 clockChangeFlag) {}
void rfu_REQ_startConnectParent(u16 pid) {}
void rfu_REQ_startSearchChild(void) {}
void rfu_REQ_startSearchParent(void) {}
void rfu_REQ_stopMode(void) {}
u16 rfu_UNI_PARENT_getDRAC_ACK(u8 *ackFlag) { *ackFlag = 0; return 0; }
void rfu_UNI_clearRecvNewDataFlag(u8 slotStatusIndex) {}
void rfu_UNI_readySendData(u8 slotStatusIndex) {}
u16 rfu_UNI_setSendData(u8 bmSendSlot, const void *src, u8 size) { return 0; }
u16 rfu_changeSendTarget(u8 connType, u8 slotStatusIndex, u8 bmNewTgtSlot) { return 0; }
void rfu_clearAllSlot(void) {}
u16 rfu_clearSlot(u8 connTypeFlag, u8 slotStatusIndex) { return 0; }
u16 rfu_getConnectParentStatus(u8 *status, u8 *connectSlotNo) { *status = 0xFF; *connectSlotNo = 0; return 0; }
bool8 rfu_getMasterSlave(void) { return TRUE; }
u16 rfu_getRFUStatus(u8 *rfuState) { *rfuState = 0; return 0; }
u8 *rfu_getSTWIRecvBuffer(void) { return sRfuRecvBuffer; }
u16 rfu_initializeAPI(u32 *APIBuffer, u16 buffByteSize, IntrFunc *sioIntrTable_p, bool8 copyInterruptToRam) { return 0; }
void rfu_setMSCCallback(void (*callback)(u16 reqCommandId)) {}
void rfu_setREQCallback(void (*callback)(u16 reqCommandId, u16 reqResult)) {}
u16 rfu_setRecvBuffer(u8 connType, u8 slotNo, void *buffer, u32 buffSize) { return 0; }
void rfu_setTimerInterrupt(u8 timerNo, IntrFunc *timerIntrTable_p) {}
u16 rfu_syncVBlank(void) { return 0; }
u16 rfu_waitREQComplete(void) { return 0; }

// --------------------------------------------------------------------------
// GameCube multiboot (Pokemon Colosseum / Box): never connected.

void GameCubeMultiBoot_Main(struct GcmbStruct *pStruct) {}
void GameCubeMultiBoot_ExecuteProgram(struct GcmbStruct *pStruct) {}
void GameCubeMultiBoot_Init(struct GcmbStruct *pStruct) {}
void GameCubeMultiBoot_HandleSerialInterrupt(struct GcmbStruct *pStruct) {}
void GameCubeMultiBoot_Quit(void) {}
