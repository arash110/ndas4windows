#include "port.h"
#ifdef RUN_WPP
#include "ndasport.tmh"
#endif

//
// DllInitialize and DllUnload is called if the extension drivers
// are loaded before ndasport is loaded.
// Since the extension drivers do not (and must not) call any 
// exported functions before actually ndasport calls to initialize
// the extension drivers, there is not need to perform any 
// initialization or cleanup here.
// All initialization and cleanup codes are in DriverEntry and DriverUnload
//
NTSTATUS
DllInitialize(
	__in PUNICODE_STRING RegistryPath)
{
	NdasPortTrace(NDASPORT_GENERAL, TRACE_LEVEL_VERBOSE,
		"ndasport.sys (dll) loaded successfully.\n");

	KdPrint(("ndasport.sys (dll) loaded successfully.\n"));

	return STATUS_SUCCESS;
}

NTSTATUS
DllUnload()
{
	NdasPortTrace(NDASPORT_GENERAL, TRACE_LEVEL_VERBOSE,
		"ndasport.sys (dll) unloaded successfully.\n");

	KdPrint(("ndasport.sys (dll) unloaded successfully.\n"));

	return STATUS_SUCCESS;
}

FORCEINLINE
NTSTATUS
NdasPortpRetrieveSrbDataBuffer(
	PSCSI_REQUEST_BLOCK Srb,
	PMDL Mdl,
	MM_PAGE_PRIORITY Priority);

FORCEINLINE
VOID
NdasPortpFreeSrbData(
	PNDASPORT_PDO_EXTENSION PdoExtension,
	PSCSI_REQUEST_BLOCK Srb,
	PIRP* Irp);

FORCEINLINE
NTSTATUS
NdasPortpAllocateSrbData(
	PNDASPORT_PDO_EXTENSION PdoExtension,
	PIRP Irp,
	PSCSI_REQUEST_BLOCK Srb,
	PVOID OriginalDataBuffer);

PVOID
NdasPortGetLogicalUnit(
	__in PNDAS_LOGICALUNIT_EXTENSION LogicalUnitExtension,
	__in UCHAR Reserved,
	__in UCHAR Reserved2,
	__in UCHAR Reserved3)
{
#if DBG
	ASSERT(
		(PVOID)NdasPortLogicalUnitGetPdoExtension(LogicalUnitExtension)->CommonExtension == 
		(PVOID)NdasPortLogicalUnitGetPdoExtension(LogicalUnitExtension));
#endif
	return LogicalUnitExtension;
}

VOID
NpDumpSrb(PSCSI_REQUEST_BLOCK Srb)
{
	if (Srb->Function == SRB_FUNCTION_EXECUTE_SCSI)
	{
		NdasPortTrace(NDASPORT_PDO_SCSIEX, TRACE_LEVEL_VERBOSE,
			"SrbCompletion(%p): SRB_FUNCTION_EXECUTE_SCSI(00), CDB=%s(%02X), "
			"SrbStatus=%s(%02X), ScsiStatus=%X, InternalStatus=%X, "
			"DataTransferLength=%X\n",
			Srb,
			DbgScsiOpStringA(Srb->Cdb[0]), Srb->Cdb[0],
			DbgSrbStatusStringA(Srb->SrbStatus), Srb->SrbStatus,
			Srb->ScsiStatus,
			Srb->InternalStatus,
			Srb->DataTransferLength);

		switch (Srb->Cdb[0])
		{
		case SCSIOP_READ_CAPACITY:
			{
				PREAD_CAPACITY_DATA capacity;
				capacity = (PREAD_CAPACITY_DATA) Srb->DataBuffer;
				NdasPortTrace(NDASPORT_PDO_SCSI, TRACE_LEVEL_VERBOSE,
					"READ_CAPACITY: LBA=%08Xh, BytesPerBlock=%08Xh\n", 
					RtlUlongByteSwap(capacity->LogicalBlockAddress),
					RtlUlongByteSwap(capacity->BytesPerBlock));
			}
			break;
		}
	}
	else
	{
		NdasPortTrace(NDASPORT_PDO_SCSI, TRACE_LEVEL_INFORMATION,
			"SrbCompletion(%p): %s(%02X), "
			"SrbStatus=%s(%02X), ScsiStatus=%X, InternalStatus=%X, "
			"DataTransferLength=%X\n",
			Srb,
			DbgSrbFunctionStringA(Srb->Function), Srb->Function,
			DbgSrbStatusStringA(Srb->SrbStatus), Srb->SrbStatus,
			Srb->ScsiStatus,
			Srb->InternalStatus,
			Srb->DataTransferLength);
	}
}

VOID
NdasPortCompleteRequest(
	__in PNDAS_LOGICALUNIT_EXTENSION LogicalUnitExtension,
	__in UCHAR PathId,
	__in UCHAR TargetId,
	__in UCHAR Lun,
	__in UCHAR SrbStatus)
{
	PNDASPORT_PDO_EXTENSION pdoExtension;
	
	//
	// completing all pending request
	//

	pdoExtension = NdasPortLogicalUnitGetPdoExtension(LogicalUnitExtension);

	InterlockedIncrement(&pdoExtension->ActiveRequestNumber);
	SetFlag(&pdoExtension->PdoFlags, NDASPORT_PDO_FLAG_COMPLETE_ACTIVE_REQUEST);
	pdoExtension->CompletingSrbStatus = SrbStatus;

	pdoExtension->ActiveSrb->SrbStatus = SrbStatus;
	pdoExtension->ActiveSrb->DataTransferLength = 0;

	NdasPortNotification(
		RequestComplete,
		LogicalUnitExtension,
		pdoExtension->ActiveSrb);

	NdasPortNotification(
		NextLuRequest,
		LogicalUnitExtension);

}

VOID
NdasPortNotification(
	__in SCSI_NOTIFICATION_TYPE NotificationType, 
	__in PNDAS_LOGICALUNIT_EXTENSION LogicalUnitExtension,
	...)
{
	PNDASPORT_PDO_EXTENSION pdoExtension;
	PSCSI_REQUEST_BLOCK srb;
	PNDASPORT_SRB_DATA srbData;
	PIRP irp;
	va_list ap;
	KIRQL oldIrql;

	pdoExtension = NdasPortLogicalUnitGetPdoExtension(LogicalUnitExtension);

	va_start(ap, LogicalUnitExtension);
	switch (NotificationType)
	{
	case NextRequest:
	case NextLuRequest:
		{
			oldIrql = KeRaiseIrqlToDpcLevel();
			IoStartNextPacket(pdoExtension->DeviceObject, FALSE);
			KeLowerIrql(oldIrql);
		}
		break;
	case RequestComplete:
		{
			srb = va_arg(ap, PSCSI_REQUEST_BLOCK);

			ASSERT(sizeof(SCSI_REQUEST_BLOCK) == srb->Length);
			if (sizeof(SCSI_REQUEST_BLOCK) != srb->Length)
			{
				break;
			}

			ASSERT(pdoExtension->ActiveSrb == srb);

			ASSERT(TestFlag(srb->SrbFlags, SRB_FLAGS_IS_ACTIVE));
			ClearFlag(&srb->SrbFlags, SRB_FLAGS_IS_ACTIVE);

			srbData = (PNDASPORT_SRB_DATA) srb->OriginalRequest;

			NdasPortpFreeSrbData(pdoExtension, srb, &irp);
			
			irp->IoStatus.Status = NpTranslateScsiStatus(srb);
			if (srb->SrbFlags & (SRB_FLAGS_DATA_IN | SRB_FLAGS_DATA_OUT))
			{
				irp->IoStatus.Information = srb->DataTransferLength;
			}
			else
			{
				irp->IoStatus.Information = 0;
			}

			NpDumpSrb(srb);

			NpReleaseRemoveLock(pdoExtension->CommonExtension, irp);
			IoCompleteRequest(irp, IO_DISK_INCREMENT);

			pdoExtension->ActiveSrb = NULL;
		}
		break;
	case ResetDetected:
	case BusChangeDetected:
		{
			//
			// Disconnected!
			//
			pdoExtension->Present = FALSE;

			IoInvalidateDeviceRelations(
				pdoExtension->ParentFDOExtension->LowerPdo,
				BusRelations);
		}
		break;
	case RequestTimerCall:
	case WMIEvent:
	case WMIReregister:
	default:
		ASSERT(FALSE); // not implemented yet
		;
	}
	va_end(ap);
}

PIO_WORKITEM
NdasPortExAllocateWorkItem(
	__in PNDAS_LOGICALUNIT_EXTENSION LogicalUnitExtension)
{
	PNDASPORT_PDO_EXTENSION pdoExtension;
	pdoExtension = NdasPortLogicalUnitGetPdoExtension(LogicalUnitExtension);
	return IoAllocateWorkItem(pdoExtension->DeviceObject);
}

PDEVICE_OBJECT
NdasPortExGetWdmDeviceObject(
	__in PNDAS_LOGICALUNIT_EXTENSION LogicalUnitExtension)
{
	PNDASPORT_PDO_EXTENSION pdoExtension;
	pdoExtension = NdasPortLogicalUnitGetPdoExtension(LogicalUnitExtension);
	return pdoExtension->DeviceObject;
}

FORCEINLINE
PNDASPORT_SRB_DATA
NpGetSrbData(PSCSI_REQUEST_BLOCK Srb)
{
	PNDASPORT_SRB_DATA srbData;
	srbData = (PNDASPORT_SRB_DATA) Srb->OriginalRequest;
	ASSERT(srbData->Size == sizeof(NDASPORT_SRB_DATA));
	return srbData;
}

VOID
NdasPortPdoStartIo(
	__in PDEVICE_OBJECT DeviceObject,
	__in PIRP Irp)
{
	PNDASPORT_PDO_EXTENSION pdoExtension;
	PIO_STACK_LOCATION irpStack;
	PSCSI_REQUEST_BLOCK srb;

	NdasPortTrace(NDASPORT_GENERAL, TRACE_LEVEL_VERBOSE,
		"Starting Irp=%p\n", Irp);

	pdoExtension = NdasPortPdoGetExtension(DeviceObject);
	irpStack = IoGetCurrentIrpStackLocation(Irp);
	srb = irpStack->Parameters.Scsi.Srb;

	ASSERT(
		SRB_FUNCTION_EXECUTE_SCSI == srb->Function ||
		SRB_FUNCTION_IO_CONTROL == srb->Function ||
		SRB_FUNCTION_ABORT_COMMAND == srb->Function ||
		SRB_FUNCTION_RESET_BUS == srb->Function ||
		SRB_FUNCTION_RESET_DEVICE == srb->Function ||
		SRB_FUNCTION_RESET_LOGICAL_UNIT == srb->Function ||
		SRB_FUNCTION_WMI == srb->Function || 
		SRB_FUNCTION_SHUTDOWN == srb->Function ||
		SRB_FUNCTION_FLUSH == srb->Function);

	pdoExtension->ActiveSrb = srb;

	//
	// If CompleteRequest is issued, we should complete
	// all of the active request without calling SrbStartIo.
	//

	if (TestFlag(pdoExtension->PdoFlags, NDASPORT_PDO_FLAG_COMPLETE_ACTIVE_REQUEST))
	{
		//
		// ActiveRequestNumber can be rolled over to 0 after 0xFFFFFFFF.
		// We normalize the ActiveRequestNumbers to the center (0x7FFFFFFF)
		// and compare by the distance.
		//
		static const ULONG center = ((ULONG)(-1)) / 2 ;
		ULONG arnOfSrb = NpGetSrbData(srb)->ActiveRequestNumber;
		// ARN has been already increased by NdasPortCompleteRequest!
		ULONG arnToComplete = pdoExtension->ActiveRequestNumber - 1;
		ULONG distance;
		//
		// Comparison window is 0x7FFFFFFF
		//
		if (arnToComplete >= center)
		{
			distance = arnToComplete - center;
		}
		else
		{
			distance = center - arnToComplete;
		}
		arnOfSrb += distance;
		arnToComplete += distance;

		if (arnOfSrb <= arnToComplete)
		{
			srb->SrbStatus = pdoExtension->CompletingSrbStatus;
			srb->DataTransferLength = 0;

			NdasPortNotification(
				RequestComplete,
				NdasPortPdoGetLogicalUnitExtension(pdoExtension),
				srb);

			NdasPortNotification(
				NextLuRequest, 
				NdasPortPdoGetLogicalUnitExtension(pdoExtension));

			return;
		}
		else
		{
			ClearFlag(&pdoExtension->PdoFlags, NDASPORT_PDO_FLAG_COMPLETE_ACTIVE_REQUEST);
		}
	}

	ASSERT(!TestFlag(srb->SrbFlags, SRB_FLAGS_IS_ACTIVE));
	SetFlag(&srb->SrbFlags, SRB_FLAGS_IS_ACTIVE);

	pdoExtension->LogicalUnitInterface.StartIo(
		NdasPortPdoGetLogicalUnitExtension(pdoExtension),
		srb);
}

FORCEINLINE
NTSTATUS
NdasPortpAllocateSrbData(
	PNDASPORT_PDO_EXTENSION PdoExtension,
	PIRP Irp,
	PSCSI_REQUEST_BLOCK Srb,
	PVOID OriginalDataBuffer)
{
	PNDASPORT_SRB_DATA srbData;

	srbData = (PNDASPORT_SRB_DATA) ExAllocateFromNPagedLookasideList(
		&PdoExtension->SrbDataLookasideList);

	if (NULL == srbData)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	srbData->Size = sizeof(NDASPORT_SRB_DATA);
	srbData->ActiveRequestNumber = PdoExtension->ActiveRequestNumber;
	srbData->OriginalIrp = Irp;
	srbData->OriginalRequest = Srb->OriginalRequest;
	srbData->OriginalBuffer = OriginalDataBuffer;

	if (PdoExtension->SrbDataExtensionSize > 0)
	{
		Srb->SrbExtension = NdasPortOffsetOf(srbData, sizeof(NDASPORT_SRB_DATA));
	}
	else
	{
		Srb->SrbExtension = NULL;
	}

	Srb->OriginalRequest = srbData;

	return STATUS_SUCCESS;
}

FORCEINLINE
VOID
NdasPortpFreeSrbData(
	PNDASPORT_PDO_EXTENSION PdoExtension,
	PSCSI_REQUEST_BLOCK Srb,
	PIRP* Irp)
{
	PNDASPORT_SRB_DATA srbData;
	
	srbData = (PNDASPORT_SRB_DATA) Srb->OriginalRequest;

	ASSERT(NULL != srbData);
	ASSERT(sizeof(NDASPORT_SRB_DATA) == srbData->Size);

	Srb->OriginalRequest = srbData->OriginalRequest;
	Srb->DataBuffer = srbData->OriginalBuffer;
	*Irp = srbData->OriginalIrp;

	ExFreeToNPagedLookasideList(
		&PdoExtension->SrbDataLookasideList,
		srbData);

}

FORCEINLINE
NTSTATUS
NdasPortpRetrieveSrbDataBuffer(
	PSCSI_REQUEST_BLOCK Srb,
	PMDL Mdl,
	MM_PAGE_PRIORITY Priority)
{
	ULONG_PTR offset;
	PVOID requestBuffer;
	UCHAR readChar;

	if (NULL == Mdl)
	{
		return STATUS_SUCCESS;
	}

	offset = (ULONG_PTR) ((ULONG_PTR) Srb->DataBuffer - 
		(ULONG_PTR) MmGetMdlVirtualAddress(Mdl));

	NdasPortTrace(NDASPORT_GENERAL, TRACE_LEVEL_VERBOSE,
		"Srb DataBuffer=%p, Offset into the MDL=%d\n",
		Srb->DataBuffer, (ULONG)offset);

	requestBuffer = MmGetSystemAddressForMdlSafe(Mdl, Priority);
	if (NULL == requestBuffer)
	{
		NdasPortTrace(NDASPORT_GENERAL, TRACE_LEVEL_ERROR,
			"Failed to get System Address for MDL=%p\n", Mdl);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	Srb->DataBuffer = (PVOID) ((ULONG_PTR) requestBuffer + (ULONG_PTR) offset);

	//
	// This is for catching the case where the Srb DataBuffer
	// we have generated is not valid
	//
	readChar = *((PUCHAR)(Srb->DataBuffer));

	return STATUS_SUCCESS;
}

FORCEINLINE
NTSTATUS
NdasPortpCompleteInvalidRequest(
	PNDASPORT_PDO_EXTENSION PdoExtension,
	PIRP Irp,
	PSCSI_REQUEST_BLOCK Srb)
{
	Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
	Srb->DataTransferLength = 0;
	return NpReleaseRemoveLockAndCompleteIrpEx(
		PdoExtension->CommonExtension,
		Irp,
		STATUS_INVALID_DEVICE_REQUEST,
		Srb->DataTransferLength,
		IO_NO_INCREMENT);
}

FORCEINLINE
NTSTATUS
NdasPortpCompleteRequestWithInternalError(
	PNDASPORT_PDO_EXTENSION PdoExtension,
	PIRP Irp,
	PSCSI_REQUEST_BLOCK Srb,
	NTSTATUS Status)
{
	Srb->SrbStatus = SRB_STATUS_INTERNAL_ERROR;
	Srb->InternalStatus = Status;
	Srb->DataTransferLength = 0;

	return NpReleaseRemoveLockAndCompleteIrpEx(
		PdoExtension->CommonExtension,
		Irp,
		Status,
		Srb->DataTransferLength,
		IO_NO_INCREMENT);
}

NTSTATUS
NdasPortPdoClaimLogicalUnit(
    __in PNDASPORT_PDO_EXTENSION PdoExtension,
    __in PIRP Irp)
{
    PIO_STACK_LOCATION irpStack;
    PSCSI_REQUEST_BLOCK srb;
    PDEVICE_OBJECT saveDevice;

    PAGED_CODE();

	//
	// See Windows DDK Storage Devices:
	// Topic: Negotiating Claim and Release Device Requests with SCSI Port
	// 

    //
    // Get SRB address from current IRP stack.
    //

    irpStack = IoGetCurrentIrpStackLocation(Irp);

    srb = (PSCSI_REQUEST_BLOCK) irpStack->Parameters.Scsi.Srb;

    if (srb->Function == SRB_FUNCTION_RELEASE_DEVICE) 
    {
		NdasPortTrace(NDASPORT_PDO_PNP, TRACE_LEVEL_INFORMATION,
			"Device released, Pdo=%p\n",
			PdoExtension->DeviceObject);

        PdoExtension->IsClaimed = FALSE;
        srb->SrbStatus = SRB_STATUS_SUCCESS;
        return STATUS_SUCCESS;
    }

    //
    // Check for a claimed device.
    //

    if (PdoExtension->IsClaimed) 
    {
		NdasPortTrace(NDASPORT_PDO_PNP, TRACE_LEVEL_WARNING,
			"Device already claimed, Pdo=%p\n",
			PdoExtension->DeviceObject);

        srb->SrbStatus = SRB_STATUS_BUSY;
        return STATUS_DEVICE_BUSY;
    }

    //
    // Save the current device object.
    //

    saveDevice = PdoExtension->DeviceObject;

    //
    // Update the lun information based on the operation type.
    //

    if (srb->Function == SRB_FUNCTION_CLAIM_DEVICE) 
    {
        PdoExtension->IsClaimed = TRUE;
    }

    if (srb->Function == SRB_FUNCTION_ATTACH_DEVICE) 
    {
        ASSERT(FALSE);
        PdoExtension->DeviceObject = (PDEVICE_OBJECT) srb->DataBuffer;
    }

    srb->DataBuffer = saveDevice;

    srb->SrbStatus = SRB_STATUS_SUCCESS;

	NdasPortTrace(NDASPORT_PDO_PNP, TRACE_LEVEL_INFORMATION,
		"Device claimed, Pdo=%p\n", 
		PdoExtension->DeviceObject);

	return STATUS_SUCCESS;
}

#if 0
NTSTATUS
NdasPortPdoDispatchScsi(
	__in PDEVICE_OBJECT LogicalUnit,
	__in PIRP Irp)
{
	PNDASPORT_PDO_EXTENSION pdoExtension;
	PNDASPORT_COMMON_EXTENSION CommonExtension;
	PIO_STACK_LOCATION irpStack;
	PSCSI_REQUEST_BLOCK srb;
	NTSTATUS status;
	ULONG isRemoved = NO_REMOVE;
	BOOLEAN forwardToStartIo;
	PVOID originalDataBuffer;

	pdoExtension = NdasPortPdoGetExtension(LogicalUnit);
	CommonExtension = pdoExtension->CommonExtension;
	irpStack = IoGetCurrentIrpStackLocation(Irp);
	srb = irpStack->Parameters.Scsi.Srb;

	ASSERT(srb->OriginalRequest == Irp);

	if (srb->Function == SRB_FUNCTION_EXECUTE_SCSI)
	{
		NdasPortTrace(NDASPORT_PDO_SCSI, TRACE_LEVEL_INFORMATION,
			"PDO: SCSI %s(%02Xh): %s(%02Xh), Pdo=%p, Irp=%p, "
			"QueueTag=%x, QueueAction=%x, SrbFlags=%x, QueueSortKey=%x, Timeout=%x\n",
			DbgSrbFunctionStringA(srb->Function),
			srb->Function,
			DbgScsiOpStringA(srb->Cdb[0]),
			srb->Cdb[0],
			LogicalUnit,
			Irp,
			srb->QueueTag,
			srb->QueueAction,
			srb->SrbFlags,
			srb->QueueSortKey,
			srb->TimeOutValue);
	}
	else
	{
		NdasPortTrace(NDASPORT_PDO_SCSI, TRACE_LEVEL_INFORMATION,
			"PDO: SCSI %s(%02Xh), Pdo=%p, Irp=%p\n",
			DbgSrbFunctionStringA(srb->Function),
			srb->Function,
			pdoExtension,
			Irp);
	}

	isRemoved = NpAcquireRemoveLock(CommonExtension, Irp);
	if (isRemoved &&
		!IsCleanupRequest(irpStack) &&
		(srb->Function != SRB_FUNCTION_CLAIM_DEVICE)) 
	{
		return NpReleaseRemoveLockAndCompleteIrp(
			CommonExtension, 
			Irp, 
			STATUS_DEVICE_DOES_NOT_EXIST);
	}

	forwardToStartIo = FALSE;

	originalDataBuffer = srb->DataBuffer;

	if (Irp->MdlAddress)
	{
		status = NpRetrieveSrbDataBuffer(
			srb, 
			Irp->MdlAddress, 
			((Irp->RequestorMode == KernelMode) ? HighPagePriority : NormalPagePriority));

		if (!NT_SUCCESS(status))
		{
			return NpCompleteRequestWithInternalError(
				pdoExtension, Irp, srb, status);
		}

	}

	switch (srb->Function)
	{
	case SRB_FUNCTION_EXECUTE_SCSI:

		if (TestFlag(pdoExtension->PdoFlags, 
			NDASPORT_PDO_FLAG_REMOVED |
			NDASPORT_PDO_FLAG_PNP_STOPPED |
			NDASPORT_PDO_FLAG_DEVICE_FAILED))
		{
			//
			// We cannot accept requests any more
			//
			return NpCompleteRequestWithInternalError(
				pdoExtension,
				Irp,
				srb,
				STATUS_DEVICE_DOES_NOT_EXIST);
		}

		if (TestFlag(srb->SrbFlags, SRB_FLAGS_NO_KEEP_AWAKE))
		{
			if (PowerDeviceD3 == pdoExtension->CommonExtension->DevicePowerState)
			{
				srb->SrbStatus = SRB_STATUS_NOT_POWERED;
				return NpReleaseRemoveLockAndCompleteIrp(
					pdoExtension->CommonExtension,
					Irp,
					STATUS_UNSUCCESSFUL);
			}
		}

		IoMarkIrpPending(Irp);

		if (TestFlag(pdoExtension->PdoFlags, NDASPORT_PDO_FLAG_QUEUE_LOCKED) &&
			TestFlag(srb->SrbFlags, SRB_FLAGS_BYPASS_LOCKED_QUEUE))
		{
			IoStartPacket()
		}
		else
		{

		}

		return STATUS_PENDING;
	}
}
#endif

NTSTATUS
NdasPortPdoDispatchScsi(
	__in PDEVICE_OBJECT LogicalUnit,
	__in PIRP Irp)
{
	PNDASPORT_PDO_EXTENSION pdoExtension;
	PNDASPORT_COMMON_EXTENSION CommonExtension;
	PIO_STACK_LOCATION irpStack;
	PSCSI_REQUEST_BLOCK srb;
	NTSTATUS status;
	ULONG isRemoved = NO_REMOVE;
	BOOLEAN forwardToStartIo;
	PVOID originalDataBuffer;

	pdoExtension = NdasPortPdoGetExtension(LogicalUnit);
	CommonExtension = pdoExtension->CommonExtension;
	irpStack = IoGetCurrentIrpStackLocation(Irp);
	srb = irpStack->Parameters.Scsi.Srb;

	ASSERT(srb->OriginalRequest == Irp);

	if (srb->Function == SRB_FUNCTION_EXECUTE_SCSI)
	{
		switch (srb->Cdb[0])
		{
		case SCSIOP_READ6:
		case SCSIOP_READ:
		case SCSIOP_READ16:
		case SCSIOP_WRITE6:
		case SCSIOP_WRITE:
		case SCSIOP_WRITE16:
		case SCSIOP_VERIFY6:
		case SCSIOP_VERIFY:
		case SCSIOP_VERIFY16:
			NdasPortTrace(NDASPORT_PDO_SCSIEX, TRACE_LEVEL_INFORMATION,
				"PDO: SCSI %s(%02Xh): %s(%02Xh), Pdo=%p, Irp=%p, "
				"QueueTag=%x, QueueAction=%x, SrbFlags=%x, QueueSortKey=%x, Timeout=%x\n",
				DbgSrbFunctionStringA(srb->Function),
				srb->Function,
				DbgScsiOpStringA(srb->Cdb[0]),
				srb->Cdb[0],
				LogicalUnit,
				Irp,
				srb->QueueTag,
				srb->QueueAction,
				srb->SrbFlags,
				srb->QueueSortKey,
				srb->TimeOutValue);
			break;
		default:
			NdasPortTrace(NDASPORT_PDO_SCSI, TRACE_LEVEL_INFORMATION,
				"PDO: SCSI %s(%02Xh): %s(%02Xh), Pdo=%p, Irp=%p, "
				"QueueTag=%x, QueueAction=%x, SrbFlags=%x, QueueSortKey=%x, Timeout=%x\n",
				DbgSrbFunctionStringA(srb->Function),
				srb->Function,
				DbgScsiOpStringA(srb->Cdb[0]),
				srb->Cdb[0],
				LogicalUnit,
				Irp,
				srb->QueueTag,
				srb->QueueAction,
				srb->SrbFlags,
				srb->QueueSortKey,
				srb->TimeOutValue);
		}
	}
	else
	{
		NdasPortTrace(NDASPORT_PDO_SCSI, TRACE_LEVEL_INFORMATION,
			"PDO: SCSI %s(%02Xh), Pdo=%p, Irp=%p\n",
			DbgSrbFunctionStringA(srb->Function),
			srb->Function,
			LogicalUnit,
			Irp);
	}

	isRemoved = NpAcquireRemoveLock(CommonExtension, Irp);
	if (isRemoved &&
		!IsCleanupRequest(irpStack) &&
		(srb->Function != SRB_FUNCTION_CLAIM_DEVICE)) 
	{
		return NpReleaseRemoveLockAndCompleteIrp(
			CommonExtension, 
			Irp, 
			STATUS_DEVICE_DOES_NOT_EXIST);
	}

	forwardToStartIo = FALSE;

	originalDataBuffer = srb->DataBuffer;

	if (Irp->MdlAddress)
	{
		status = NdasPortpRetrieveSrbDataBuffer(
			srb, 
			Irp->MdlAddress, 
			((Irp->RequestorMode == KernelMode) ? HighPagePriority : NormalPagePriority));

		if (!NT_SUCCESS(status))
		{
			return NdasPortpCompleteRequestWithInternalError(
				pdoExtension, Irp, srb, status);
		}

	}

	switch (srb->Function) 
	{
	//
	// A SCSI device I/O request should be executed on the target logical unit. 
	//
	case SRB_FUNCTION_EXECUTE_SCSI: 
	case SRB_FUNCTION_ABORT_COMMAND: 
	case SRB_FUNCTION_RESET_BUS:
	case SRB_FUNCTION_RESET_DEVICE:
	case SRB_FUNCTION_RESET_LOGICAL_UNIT:
	case SRB_FUNCTION_FLUSH:
	case SRB_FUNCTION_SHUTDOWN:
	case SRB_FUNCTION_IO_CONTROL:
	case SRB_FUNCTION_WMI:
		forwardToStartIo = TRUE;
		break;
	//
	// The NT-based operating system SCSI port driver processes requests 
	// with the following SRB Function values without calling any 
	// SCSI miniport driver: 
	//
	case SRB_FUNCTION_CLAIM_DEVICE:
	case SRB_FUNCTION_REMOVE_DEVICE:
	case SRB_FUNCTION_RELEASE_DEVICE:
	case SRB_FUNCTION_ATTACH_DEVICE:

		status = NdasPortPdoClaimLogicalUnit(
			pdoExtension,
			Irp);

		break;
	//
	// We won't handle these functions on the client
	// side for the time being.
	//
	case SRB_FUNCTION_RELEASE_QUEUE:
	case SRB_FUNCTION_FLUSH_QUEUE: 
	case SRB_FUNCTION_LOCK_QUEUE:
	case SRB_FUNCTION_UNLOCK_QUEUE:
		status = STATUS_SUCCESS;
		srb->SrbStatus = SRB_STATUS_SUCCESS;
		break;
	//
	// The following SRB Function values are defined for use in future 
	// versions of the operating system: 
	//
	case SRB_FUNCTION_RECEIVE_EVENT:
	case SRB_FUNCTION_RELEASE_RECOVERY:
	case SRB_FUNCTION_TERMINATE_IO:
	default: 
		//
		// Unsupported SRB function.
		//
		NdasPortTrace(NDASPORT_PDO_SCSI, TRACE_LEVEL_WARNING,
			"PdoDispatch: Unsupported function, SRB %p\n",
			srb);

		srb->DataBuffer = originalDataBuffer;
		return NdasPortpCompleteInvalidRequest(
			pdoExtension,
			Irp,
			srb);
	}

	//
	// Immediate completion for non-io commands
	//
	if (!forwardToStartIo)
	{
		srb->DataBuffer = originalDataBuffer;
		return NpReleaseRemoveLockAndCompleteIrp(
			CommonExtension,
			Irp,
			status);
	}

	status = NdasPortpAllocateSrbData(
		pdoExtension, 
		Irp,
		srb, 
		originalDataBuffer);

	if (!NT_SUCCESS(status))
	{
		return NdasPortpCompleteRequestWithInternalError(
			pdoExtension, Irp, srb, status);
	}

	//
	// Invoke Build IO routine
	//
	IoMarkIrpPending(Irp);

	if (pdoExtension->LogicalUnitInterface.BuildIo)
	{
		forwardToStartIo = pdoExtension->LogicalUnitInterface.BuildIo(
			NdasPortPdoGetLogicalUnitExtension(pdoExtension),
			srb);
	}

	//
	// Queue the request
	//
	if (forwardToStartIo)
	{
		IoStartPacket(
			LogicalUnit,
			Irp,
			&srb->QueueSortKey,
			NULL);
	}

	return STATUS_PENDING;
}
